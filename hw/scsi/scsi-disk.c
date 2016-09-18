/*
 * SCSI Device emulation
 *
 * Copyright (c) 2006 CodeSourcery.
 * Based on code by Fabrice Bellard
 *
 * Written by Paul Brook
 * Modifications:
 *  2009-Dec-12 Artyom Tarasenko : implemented stamdard inquiry for the case
 *                                 when the allocation length of CDB is smaller
 *                                 than 36.
 *  2009-Oct-13 Artyom Tarasenko : implemented the block descriptor in the
 *                                 MODE SENSE response.
 *
 * This code is licensed under the LGPL.
 *
 * Note that this file only handles the SCSI architecture model and device
 * commands.  Emulation of interface/link layer protocols is handled by
 * the host adapter emulator.
 */

//#define DEBUG_SCSI

#ifdef DEBUG_SCSI
#define DPRINTF(fmt, ...) \
do { printf("scsi-disk: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do {} while(0)
#endif

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/scsi/scsi.h"
#include "block/cdrom.h"
#include "scsi/common.h"
#include "scsi/emulate.h"
#include "sysemu/sysemu.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "hw/block/block.h"
#include "sysemu/dma.h"
#include "qemu/cutils.h"

#ifdef __linux
#include <scsi/sg.h>
#endif

#define SCSI_WRITE_SAME_MAX         524288
#define SCSI_DMA_BUF_SIZE           131072
#define SCSI_MAX_INQUIRY_LEN        256
#define SCSI_MAX_MODE_LEN           256

#define DEFAULT_DISCARD_GRANULARITY 4096
#define DEFAULT_MAX_UNMAP_SIZE      (1 << 30)   /* 1 GB */
#define DEFAULT_MAX_IO_SIZE         INT_MAX     /* 2 GB - 1 block */

#define TYPE_SCSI_DISK_BASE         "scsi-disk-base"

#define SCSI_DISK_BASE(obj) \
     OBJECT_CHECK(SCSIDiskState, (obj), TYPE_SCSI_DISK_BASE)
#define SCSI_DISK_BASE_CLASS(klass) \
     OBJECT_CLASS_CHECK(SCSIDiskClass, (klass), TYPE_SCSI_DISK_BASE)
#define SCSI_DISK_BASE_GET_CLASS(obj) \
     OBJECT_GET_CLASS(SCSIDiskClass, (obj), TYPE_SCSI_DISK_BASE)

typedef struct SCSIDiskClass {
    SCSIDeviceClass parent_class;
    DMAIOFunc       *dma_readv;
    DMAIOFunc       *dma_writev;
    bool            ignore_fua;
} SCSIDiskClass;

typedef struct SCSIDiskReq {
    SCSIRequest req;
    /* Both sector and sector_count are in terms of qemu 512 byte blocks.  */
    uint64_t sector;
    uint32_t sector_count;
    uint32_t buflen;
    bool started;
    bool need_fua_emulation;
    struct iovec iov;
    QEMUIOVector qiov;
    BlockAcctCookie acct;
    unsigned char *status;
    SCSIEmuReq *er;
} SCSIDiskReq;

#define SCSI_DISK_F_REMOVABLE             0
#define SCSI_DISK_F_DPOFUA                1
#define SCSI_DISK_F_NO_REMOVABLE_DEVOPS   2

typedef struct SCSIDiskState
{
    SCSIDevice qdev;
    uint32_t features;
    bool media_changed;
    bool media_event;
    bool eject_request;
    uint16_t port_index;
    uint64_t max_unmap_size;
    uint64_t max_io_size;
    QEMUBH *bh;
    char *version;
    char *serial;
    char *vendor;
    char *product;
    bool tray_open;
    bool tray_locked;
    SCSIEmu *emu;
} SCSIDiskState;

static int scsi_handle_rw_error(SCSIDiskReq *r, int error, bool acct_failed);

static void scsi_free_request(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    qemu_vfree(r->iov.iov_base);
}

/* Helper function for command completion with sense.  */
static void scsi_check_condition(SCSIDiskReq *r, SCSISense sense)
{
    DPRINTF("Command complete tag=0x%x sense=%d/%d/%d\n",
            r->req.tag, sense.key, sense.asc, sense.ascq);
    scsi_req_build_sense(&r->req, sense);
    scsi_req_complete(&r->req, CHECK_CONDITION);
}

static void scsi_init_iovec(SCSIDiskReq *r, size_t size)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    if (!r->iov.iov_base) {
        r->buflen = size;
        r->iov.iov_base = blk_blockalign(s->qdev.conf.blk, r->buflen);
    }
    r->iov.iov_len = MIN(r->sector_count * 512, r->buflen);
    qemu_iovec_init_external(&r->qiov, &r->iov, 1);
}

static void scsi_disk_save_request(QEMUFile *f, SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    qemu_put_be64s(f, &r->sector);
    qemu_put_be32s(f, &r->sector_count);
    qemu_put_be32s(f, &r->buflen);
    if (r->buflen) {
        if (r->req.cmd.mode == QEMU_SCSI_XFER_TO_DEV) {
            qemu_put_buffer(f, r->iov.iov_base, r->iov.iov_len);
        } else if (!req->retry) {
            uint32_t len = r->iov.iov_len;
            qemu_put_be32s(f, &len);
            qemu_put_buffer(f, r->iov.iov_base, r->iov.iov_len);
        }
    }
}

static void scsi_disk_load_request(QEMUFile *f, SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    qemu_get_be64s(f, &r->sector);
    qemu_get_be32s(f, &r->sector_count);
    qemu_get_be32s(f, &r->buflen);
    if (r->buflen) {
        scsi_init_iovec(r, r->buflen);
        if (r->req.cmd.mode == QEMU_SCSI_XFER_TO_DEV) {
            qemu_get_buffer(f, r->iov.iov_base, r->iov.iov_len);
        } else if (!r->req.retry) {
            uint32_t len;
            qemu_get_be32s(f, &len);
            r->iov.iov_len = len;
            assert(r->iov.iov_len <= r->buflen);
            qemu_get_buffer(f, r->iov.iov_base, r->iov.iov_len);
        }
    }

    qemu_iovec_init_external(&r->qiov, &r->iov, 1);
}

static bool scsi_disk_req_check_error(SCSIDiskReq *r, int ret, bool acct_failed)
{
    if (r->req.io_canceled) {
        scsi_req_cancel_complete(&r->req);
        return true;
    }

    if (ret < 0) {
        return scsi_handle_rw_error(r, -ret, acct_failed);
    }

    if (r->status && *r->status) {
        if (acct_failed) {
            SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
            block_acct_failed(blk_get_stats(s->qdev.conf.blk), &r->acct);
        }
        scsi_req_complete(&r->req, *r->status);
        return true;
    }

    return false;
}

static void scsi_aio_complete(void *opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    assert(r->req.aiocb != NULL);
    r->req.aiocb = NULL;
    if (scsi_disk_req_check_error(r, ret, true)) {
        goto done;
    }

    block_acct_done(blk_get_stats(s->qdev.conf.blk), &r->acct);
    scsi_req_complete(&r->req, GOOD);

done:
    scsi_req_unref(&r->req);
}

static void scsi_write_do_fua(SCSIDiskReq *r)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    assert(r->req.aiocb == NULL);
    assert(!r->req.io_canceled);

    if (r->need_fua_emulation) {
        block_acct_start(blk_get_stats(s->qdev.conf.blk), &r->acct, 0,
                         BLOCK_ACCT_FLUSH);
        r->req.aiocb = blk_aio_flush(s->qdev.conf.blk, scsi_aio_complete, r);
        return;
    }

    scsi_req_complete(&r->req, GOOD);
    scsi_req_unref(&r->req);
}

static void scsi_dma_complete_noio(SCSIDiskReq *r, int ret)
{
    assert(r->req.aiocb == NULL);
    if (scsi_disk_req_check_error(r, ret, false)) {
        goto done;
    }

    r->sector += r->sector_count;
    r->sector_count = 0;
    if (r->req.cmd.mode == QEMU_SCSI_XFER_TO_DEV) {
        scsi_write_do_fua(r);
        return;
    } else {
        scsi_req_complete(&r->req, GOOD);
    }

done:
    scsi_req_unref(&r->req);
}

static void scsi_dma_complete(void *opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    assert(r->req.aiocb != NULL);
    r->req.aiocb = NULL;

    if (ret < 0) {
        block_acct_failed(blk_get_stats(s->qdev.conf.blk), &r->acct);
    } else {
        block_acct_done(blk_get_stats(s->qdev.conf.blk), &r->acct);
    }
    scsi_dma_complete_noio(r, ret);
}

static void scsi_read_complete(void * opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    int n;

    assert(r->req.aiocb != NULL);
    r->req.aiocb = NULL;
    if (scsi_disk_req_check_error(r, ret, true)) {
        goto done;
    }

    block_acct_done(blk_get_stats(s->qdev.conf.blk), &r->acct);
    DPRINTF("Data ready tag=0x%x len=%zd\n", r->req.tag, r->qiov.size);

    n = r->qiov.size / 512;
    r->sector += n;
    r->sector_count -= n;
    scsi_req_data(&r->req, r->qiov.size);

done:
    scsi_req_unref(&r->req);
}

/* Actually issue a read to the block device.  */
static void scsi_do_read(SCSIDiskReq *r, int ret)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    SCSIDiskClass *sdc = (SCSIDiskClass *) object_get_class(OBJECT(s));

    assert (r->req.aiocb == NULL);
    if (scsi_disk_req_check_error(r, ret, false)) {
        goto done;
    }

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);

    if (r->req.sg) {
        dma_acct_start(s->qdev.conf.blk, &r->acct, r->req.sg, BLOCK_ACCT_READ);
        r->req.resid -= r->req.sg->size;
        r->req.aiocb = dma_blk_io(blk_get_aio_context(s->qdev.conf.blk),
                                  r->req.sg, r->sector << BDRV_SECTOR_BITS,
                                  sdc->dma_readv, r, scsi_dma_complete, r,
                                  DMA_DIRECTION_FROM_DEVICE);
    } else {
        scsi_init_iovec(r, SCSI_DMA_BUF_SIZE);
        block_acct_start(blk_get_stats(s->qdev.conf.blk), &r->acct,
                         r->qiov.size, BLOCK_ACCT_READ);
        r->req.aiocb = sdc->dma_readv(r->sector << BDRV_SECTOR_BITS, &r->qiov,
                                      scsi_read_complete, r, r);
    }

done:
    scsi_req_unref(&r->req);
}

static void scsi_do_read_cb(void *opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    assert (r->req.aiocb != NULL);
    r->req.aiocb = NULL;

    if (ret < 0) {
        block_acct_failed(blk_get_stats(s->qdev.conf.blk), &r->acct);
    } else {
        block_acct_done(blk_get_stats(s->qdev.conf.blk), &r->acct);
    }
    scsi_do_read(opaque, ret);
}

/* Read more data from scsi device into buffer.  */
static void scsi_read_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    bool first;

    DPRINTF("Read sector_count=%d\n", r->sector_count);
    if (r->sector_count == 0) {
        /* This also clears the sense buffer for REQUEST SENSE.  */
        scsi_req_complete(&r->req, GOOD);
        return;
    }

    /* No data transfer may already be in progress */
    assert(r->req.aiocb == NULL);

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);
    if (r->req.cmd.mode == QEMU_SCSI_XFER_TO_DEV) {
        DPRINTF("Data transfer direction invalid\n");
        scsi_read_complete(r, -EINVAL);
        return;
    }

    if (!blk_is_available(req->dev->conf.blk)) {
        scsi_read_complete(r, -ENOMEDIUM);
        return;
    }

    first = !r->started;
    r->started = true;
    if (first && r->need_fua_emulation) {
        block_acct_start(blk_get_stats(s->qdev.conf.blk), &r->acct, 0,
                         BLOCK_ACCT_FLUSH);
        r->req.aiocb = blk_aio_flush(s->qdev.conf.blk, scsi_do_read_cb, r);
    } else {
        scsi_do_read(r, 0);
    }
}

/*
 * scsi_handle_rw_error has two return values.  0 means that the error
 * must be ignored, 1 means that the error has been processed and the
 * caller should not do anything else for this request.  Note that
 * scsi_handle_rw_error always manages its reference counts, independent
 * of the return value.
 */
static int scsi_handle_rw_error(SCSIDiskReq *r, int error, bool acct_failed)
{
    bool is_read = (r->req.cmd.mode == QEMU_SCSI_XFER_FROM_DEV);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    BlockErrorAction action = blk_get_error_action(s->qdev.conf.blk,
                                                   is_read, error);

    if (action == BLOCK_ERROR_ACTION_REPORT) {
        if (acct_failed) {
            block_acct_failed(blk_get_stats(s->qdev.conf.blk), &r->acct);
        }
        switch (error) {
        case ENOMEDIUM:
            scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
            break;
        case ENOMEM:
            scsi_check_condition(r, SENSE_CODE(TARGET_FAILURE));
            break;
        case EINVAL:
            scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
            break;
        case ENOSPC:
            scsi_check_condition(r, SENSE_CODE(SPACE_ALLOC_FAILED));
            break;
        default:
            scsi_check_condition(r, SENSE_CODE(IO_ERROR));
            break;
        }
    }
    blk_error_action(s->qdev.conf.blk, action, is_read, error);
    if (action == BLOCK_ERROR_ACTION_STOP) {
        scsi_req_retry(&r->req);
    }
    return action != BLOCK_ERROR_ACTION_IGNORE;
}

static void scsi_write_complete_noio(SCSIDiskReq *r, int ret)
{
    uint32_t n;

    assert (r->req.aiocb == NULL);
    if (scsi_disk_req_check_error(r, ret, false)) {
        goto done;
    }

    n = r->qiov.size / 512;
    r->sector += n;
    r->sector_count -= n;
    if (r->sector_count == 0) {
        scsi_write_do_fua(r);
        return;
    } else {
        scsi_init_iovec(r, SCSI_DMA_BUF_SIZE);
        DPRINTF("Write complete tag=0x%x more=%zd\n", r->req.tag, r->qiov.size);
        scsi_req_data(&r->req, r->qiov.size);
    }

done:
    scsi_req_unref(&r->req);
}

static void scsi_write_complete(void * opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;

    assert (r->req.aiocb != NULL);
    r->req.aiocb = NULL;
    scsi_write_complete_noio(r, ret);
}

static void scsi_write_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    SCSIDiskClass *sdc = (SCSIDiskClass *) object_get_class(OBJECT(s));

    /* No data transfer may already be in progress */
    assert(r->req.aiocb == NULL);

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);
    if (r->req.cmd.mode != QEMU_SCSI_XFER_TO_DEV) {
        DPRINTF("Data transfer direction invalid\n");
        scsi_write_complete_noio(r, -EINVAL);
        return;
    }

    if (!r->req.sg && !r->qiov.size) {
        /* Called for the first time.  Ask the driver to send us more data.  */
        r->started = true;
        scsi_write_complete_noio(r, 0);
        return;
    }
    if (!blk_is_available(req->dev->conf.blk)) {
        scsi_write_complete_noio(r, -ENOMEDIUM);
        return;
    }

    if (r->req.cmd.buf[0] == VERIFY_10 || r->req.cmd.buf[0] == VERIFY_12 ||
        r->req.cmd.buf[0] == VERIFY_16) {
        if (r->req.sg) {
            scsi_dma_complete_noio(r, 0);
        } else {
            scsi_write_complete_noio(r, 0);
        }
        return;
    }

    if (r->req.sg) {
        dma_acct_start(s->qdev.conf.blk, &r->acct, r->req.sg, BLOCK_ACCT_WRITE);
        r->req.resid -= r->req.sg->size;
        r->req.aiocb = dma_blk_io(blk_get_aio_context(s->qdev.conf.blk),
                                  r->req.sg, r->sector << BDRV_SECTOR_BITS,
                                  sdc->dma_writev, r, scsi_dma_complete, r,
                                  DMA_DIRECTION_TO_DEVICE);
    } else {
        block_acct_start(blk_get_stats(s->qdev.conf.blk), &r->acct,
                         r->qiov.size, BLOCK_ACCT_WRITE);
        r->req.aiocb = sdc->dma_writev(r->sector << BDRV_SECTOR_BITS, &r->qiov,
                                       scsi_write_complete, r, r);
    }
}

/* Return a pointer to the data buffer.  */
static uint8_t *scsi_get_buf(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    return (uint8_t *)r->iov.iov_base;
}

static inline bool media_is_dvd(SCSIDiskState *s)
{
    uint64_t nb_sectors;
    if (s->qdev.type != TYPE_ROM) {
        return false;
    }
    if (!blk_is_available(s->qdev.conf.blk)) {
        return false;
    }
    blk_get_geometry(s->qdev.conf.blk, &nb_sectors);
    return nb_sectors > CD_MAX_SECTORS;
}

static inline bool media_is_cd(SCSIDiskState *s)
{
    uint64_t nb_sectors;
    if (s->qdev.type != TYPE_ROM) {
        return false;
    }
    if (!blk_is_available(s->qdev.conf.blk)) {
        return false;
    }
    blk_get_geometry(s->qdev.conf.blk, &nb_sectors);
    return nb_sectors <= CD_MAX_SECTORS;
}

static inline bool check_lba_range(SCSIDiskState *s,
                                   uint64_t sector_num, uint32_t nb_sectors)
{
    /*
     * The first line tests that no overflow happens when computing the last
     * sector.  The second line tests that the last accessed sector is in
     * range.
     *
     * Careful, the computations should not underflow for nb_sectors == 0,
     * and a 0-block read to the first LBA beyond the end of device is
     * valid.
     */
    return (sector_num <= sector_num + nb_sectors &&
            sector_num + nb_sectors <= s->qdev.max_lba + 1);
}

/* Execute a scsi command.  Returns the length of the data expected by the
   command.  This will be Positive for data transfers from the device
   (eg. disk reads), negative for transfers to the device (eg. disk writes),
   and zero if the command does not transfer any data.  */

static int32_t scsi_disk_dma_command(SCSIRequest *req, uint8_t *buf)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    uint32_t len;
    uint8_t command;

    command = buf[0];

    if (!blk_is_available(s->qdev.conf.blk)) {
        scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
        return 0;
    }

    len = scsi_data_cdb_xfer(r->req.cmd.buf);
    switch (command) {
    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
        DPRINTF("Read (sector %" PRId64 ", count %u)\n", r->req.cmd.lba, len);
        if (r->req.cmd.buf[1] & 0xe0) {
            goto illegal_request;
        }
        if (!check_lba_range(s, r->req.cmd.lba, len)) {
            goto illegal_lba;
        }
        r->sector = r->req.cmd.lba * (s->qdev.blocksize / 512);
        r->sector_count = len * (s->qdev.blocksize / 512);
        break;
    case WRITE_6:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
    case WRITE_VERIFY_10:
    case WRITE_VERIFY_12:
    case WRITE_VERIFY_16:
        if (blk_is_read_only(s->qdev.conf.blk)) {
            scsi_check_condition(r, SENSE_CODE(WRITE_PROTECTED));
            return 0;
        }
        DPRINTF("Write %s(sector %" PRId64 ", count %u)\n",
                (command & 0xe) == 0xe ? "And Verify " : "",
                r->req.cmd.lba, len);
        if (r->req.cmd.buf[1] & 0xe0) {
            goto illegal_request;
        }
        if (!check_lba_range(s, r->req.cmd.lba, len)) {
            goto illegal_lba;
        }
        r->sector = r->req.cmd.lba * (s->qdev.blocksize / 512);
        r->sector_count = len * (s->qdev.blocksize / 512);
        break;
    default:
        abort();
    illegal_request:
        scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
        return 0;
    illegal_lba:
        scsi_check_condition(r, SENSE_CODE(LBA_OUT_OF_RANGE));
        return 0;
    }
    if (r->sector_count == 0) {
        scsi_req_complete(&r->req, GOOD);
    }
    assert(r->iov.iov_len == 0);
    if (r->req.cmd.mode == QEMU_SCSI_XFER_TO_DEV) {
        return -r->sector_count * 512;
    } else {
        return r->sector_count * 512;
    }
}

static void scsi_disk_reset(DeviceState *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev.qdev, dev);
    uint64_t nb_sectors;

    scsi_device_purge_requests(&s->qdev, SENSE_CODE(RESET));

    blk_get_geometry(s->qdev.conf.blk, &nb_sectors);
    nb_sectors /= s->qdev.blocksize / 512;
    if (nb_sectors) {
        nb_sectors--;
    }
    s->qdev.max_lba = nb_sectors;
    /* reset tray statuses */
    s->tray_locked = 0;
    s->tray_open = 0;
}

static void scsi_disk_resize_cb(void *opaque)
{
    SCSIDiskState *s = opaque;

    /* SPC lists this sense code as available only for
     * direct-access devices.
     */
    if (s->qdev.type == TYPE_DISK) {
        scsi_device_report_change(&s->qdev, SENSE_CODE(CAPACITY_CHANGED));
    }
}

static void scsi_cd_change_media_cb(void *opaque, bool load)
{
    SCSIDiskState *s = opaque;

    /*
     * When a CD gets changed, we have to report an ejected state and
     * then a loaded state to guests so that they detect tray
     * open/close and media change events.  Guests that do not use
     * GET_EVENT_STATUS_NOTIFICATION to detect such tray open/close
     * states rely on this behavior.
     *
     * media_changed governs the state machine used for unit attention
     * report.  media_event is used by GET EVENT STATUS NOTIFICATION.
     */
    s->media_changed = load;
    s->tray_open = !load;
    scsi_device_set_ua(&s->qdev, SENSE_CODE(UNIT_ATTENTION_NO_MEDIUM));
    s->media_event = true;
    s->eject_request = false;
}

static void scsi_cd_eject_request_cb(void *opaque, bool force)
{
    SCSIDiskState *s = opaque;

    s->eject_request = true;
    if (force) {
        s->tray_locked = false;
    }
}

static bool scsi_cd_is_tray_open(void *opaque)
{
    return ((SCSIDiskState *)opaque)->tray_open;
}

static bool scsi_cd_is_medium_locked(void *opaque)
{
    return ((SCSIDiskState *)opaque)->tray_locked;
}

static const BlockDevOps scsi_disk_removable_block_ops = {
    .change_media_cb = scsi_cd_change_media_cb,
    .eject_request_cb = scsi_cd_eject_request_cb,
    .is_tray_open = scsi_cd_is_tray_open,
    .is_medium_locked = scsi_cd_is_medium_locked,

    .resize_cb = scsi_disk_resize_cb,
};

static const BlockDevOps scsi_disk_block_ops = {
    .resize_cb = scsi_disk_resize_cb,
};

static void scsi_disk_unit_attention_reported(SCSIDevice *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    if (s->media_changed) {
        s->media_changed = false;
        scsi_device_set_ua(&s->qdev, SENSE_CODE(MEDIUM_CHANGED));
    }
}

static void scsi_realize(SCSIDevice *dev, Error **errp)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    SCSIBus *bus = DO_UPCAST(SCSIBus, qbus, dev->qdev.parent_bus);
    Error *err = NULL;

    if (!s->qdev.conf.blk) {
        error_setg(errp, "drive property not set");
        return;
    }

    if (!(s->features & (1 << SCSI_DISK_F_REMOVABLE)) &&
        !blk_is_inserted(s->qdev.conf.blk)) {
        error_setg(errp, "Device needs media, but drive is empty");
        return;
    }

    blkconf_serial(&s->qdev.conf, &s->serial);
    blkconf_blocksizes(&s->qdev.conf);
    if (dev->type == TYPE_DISK) {
        blkconf_geometry(&dev->conf, NULL, 65535, 255, 255, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    }
    blkconf_apply_backend_options(&dev->conf);

    if (s->qdev.conf.discard_granularity == -1) {
        s->qdev.conf.discard_granularity =
            MAX(s->qdev.conf.logical_block_size, DEFAULT_DISCARD_GRANULARITY);
    }

    if (!s->version) {
        s->version = g_strdup(qemu_hw_version());
    }
    if (!s->vendor) {
        s->vendor = g_strdup("QEMU");
    }

    if (blk_is_sg(s->qdev.conf.blk)) {
        error_setg(errp, "unwanted /dev/sg*");
        return;
    }

    if ((s->features & (1 << SCSI_DISK_F_REMOVABLE)) &&
            !(s->features & (1 << SCSI_DISK_F_NO_REMOVABLE_DEVOPS))) {
        blk_set_dev_ops(s->qdev.conf.blk, &scsi_disk_removable_block_ops, s);
    } else {
        blk_set_dev_ops(s->qdev.conf.blk, &scsi_disk_block_ops, s);
    }
    blk_set_guest_block_size(s->qdev.conf.blk, s->qdev.blocksize);

    blk_iostatus_enable(s->qdev.conf.blk);
    s->emu = scsi_emu_new(&s->qdev.conf, dev->type,
                          bus->info->tcq, &dev->max_lba,
                          s->version, s->serial,
                          s->vendor, s->product);
}

static void scsi_hd_realize(SCSIDevice *dev, Error **errp)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    /* can happen for devices without drive. The error message for missing
     * backend will be issued in scsi_realize
     */
    if (s->qdev.conf.blk) {
        blkconf_blocksizes(&s->qdev.conf);
    }
    s->qdev.blocksize = s->qdev.conf.logical_block_size;
    s->qdev.type = TYPE_DISK;
    if (!s->product) {
        s->product = g_strdup("QEMU HARDDISK");
    }
    scsi_realize(&s->qdev, errp);
}

static void scsi_cd_realize(SCSIDevice *dev, Error **errp)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);

    if (!dev->conf.blk) {
        dev->conf.blk = blk_new();
    }

    s->qdev.blocksize = 2048;
    s->qdev.type = TYPE_ROM;
    s->features |= 1 << SCSI_DISK_F_REMOVABLE;
    if (!s->product) {
        s->product = g_strdup("QEMU CD-ROM");
    }
    scsi_realize(&s->qdev, errp);
}

static void scsi_disk_realize(SCSIDevice *dev, Error **errp)
{
    DriveInfo *dinfo;
    Error *local_err = NULL;

    if (!dev->conf.blk) {
        scsi_realize(dev, &local_err);
        assert(local_err);
        error_propagate(errp, local_err);
        return;
    }

    dinfo = blk_legacy_dinfo(dev->conf.blk);
    if (dinfo && dinfo->media_cd) {
        scsi_cd_realize(dev, errp);
    } else {
        scsi_hd_realize(dev, errp);
    }
}

static int32_t scsi_disk_sync_command(SCSIRequest *req, uint8_t *buf)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    uint8_t *outbuf;
    const SCSISense *sense = NULL;

    /*
     * FIXME: we shouldn't return anything bigger than 4k, but the code
     * requires the buffer to be as big as req->cmd.xfer in several
     * places.  So, do not allow CDBs with a very large ALLOCATION
     * LENGTH.  The real fix would be to modify scsi_read_data and
     * dma_buf_read, so that they return data beyond the buflen
     * as all zeros.
     */
    if (req->cmd.xfer > 65536) {
        sense = &SENSE_CODE(INVALID_FIELD);
        goto out;
    }
    r->buflen = MAX(4096, req->cmd.xfer);

    if (!r->iov.iov_base) {
        r->iov.iov_base = blk_blockalign(s->qdev.conf.blk, r->buflen);
    }

    outbuf = r->iov.iov_base;
    memset(outbuf, 0, r->buflen);

    scsi_emu_sync_cmd(s->emu, buf, outbuf, r->buflen, &sense);

    assert(!r->req.aiocb);
    r->iov.iov_len = MIN(r->buflen, req->cmd.xfer);
    if (r->iov.iov_len == 0) {
        scsi_req_complete(&r->req, GOOD);
    }
    if (r->req.cmd.mode == QEMU_SCSI_XFER_TO_DEV) {
        assert(r->iov.iov_len == req->cmd.xfer);
        return -r->iov.iov_len;
    } else {
        return r->iov.iov_len;
    }

out:
    if (!sense) {
        scsi_req_complete(&r->req, GOOD);
    } else {
        scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
    }
    return 0;
}

static void scsi_disk_sync_read_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    int buflen = r->iov.iov_len;

    if (buflen) {
        DPRINTF("Read buf_len=%d\n", buflen);
        r->iov.iov_len = 0;
        r->started = true;
        scsi_req_data(&r->req, buflen);
        return;
    }

    /* This also clears the sense buffer for REQUEST SENSE.  */
    scsi_req_complete(&r->req, GOOD);
}

static void scsi_disk_sync_write_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    if (r->iov.iov_len) {
        int buflen = r->iov.iov_len;
        DPRINTF("Write buf_len=%d\n", buflen);
        r->iov.iov_len = 0;
        scsi_req_data(&r->req, buflen);
        return;
    }
}

static int32_t scsi_disk_async_command(SCSIRequest *req, uint8_t *buf)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    const SCSISense *sense = NULL;
    
    r->er = scsi_emu_async_cmd_begin(s->emu, buf, &sense);
    if (!r->er) {
        if (sense) {
            scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
            return 0;
        } else {
            scsi_req_complete(&r->req, GOOD);
            return 0;
        }
    }
    return r->er->sector_count * 512;
}

static void scsi_disk_async_read_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    SCSIDiskClass *sdc = (SCSIDiskClass *) object_get_class(OBJECT(s));

    DPRINTF("Read sector_count=%d\n", r->sector_count);
    if (r->er->sector_count == 0) {
        /* This also clears the sense buffer for REQUEST SENSE.  */
        scsi_req_complete(&r->req, GOOD);
        return;
    }

    /* No data transfer may already be in progress */
    assert(r->req.aiocb == NULL);

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);
    if (r->req.cmd.mode == QEMU_SCSI_XFER_TO_DEV) {
        DPRINTF("Data transfer direction invalid\n");
        scsi_read_complete(r, -EINVAL);
        return;
    }

    if (r->req.sg) {
        r->req.resid -= r->req.sg->size;
        r->req.aiocb = scsi_emu_req_continue(r->er, sdc->dma_readv,
                                             r,
                                             NULL, r->req.sg,
                                             scsi_do_read_cb, r);
    } else {
        scsi_init_iovec(r, SCSI_DMA_BUF_SIZE);
        r->req.aiocb = scsi_emu_req_continue(r->er, sdc->dma_readv,
                                             r,
                                             &r->qiov, NULL,
                                             scsi_read_complete, r);
    }

    if (!r->req.aiocb) {
        if (r->er->error) {
            scsi_read_complete(r, r->er->error);
        } else {
            scsi_req_complete(&r->req, GOOD);
        }
    }
}

static void scsi_disk_async_write_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    SCSIDiskClass *sdc = (SCSIDiskClass *) object_get_class(OBJECT(s));

    /* No data transfer may already be in progress */
    assert(r->req.aiocb == NULL);

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);
    if (r->req.cmd.mode != QEMU_SCSI_XFER_TO_DEV) {
        DPRINTF("Data transfer direction invalid\n");
        scsi_write_complete_noio(r, -EINVAL);
        return;
    }

    if (!r->req.sg && !r->qiov.size) {
        /* Called for the first time.  Ask the driver to send us more data.  */
        r->started = true;
        scsi_write_complete_noio(r, 0);
        return;
    }

    if (r->req.sg) {
        dma_acct_start(s->qdev.conf.blk, &r->acct, r->req.sg, BLOCK_ACCT_WRITE);
        r->req.resid -= r->req.sg->size;
        r->req.aiocb = scsi_emu_req_continue(r->er, sdc->dma_writev, r,
                                             NULL, r->req.sg,
                                             scsi_dma_complete, r);
    } else {
        scsi_init_iovec(r, SCSI_DMA_BUF_SIZE);
        r->req.aiocb = scsi_emu_req_continue(r->er, sdc->dma_writev, r,
                                             &r->qiov, NULL,
                                             scsi_write_complete, r);
    }
}

static const SCSIReqOps scsi_disk_em_sync_reqops = {
    .size         = sizeof(SCSIDiskReq),
    .free_req     = scsi_free_request,
    .send_command = scsi_disk_sync_command,
    .read_data    = scsi_disk_sync_read_data,
    .write_data   = scsi_disk_sync_write_data,
    .get_buf      = scsi_get_buf,
};

static const SCSIReqOps scsi_disk_em_async_reqops = {
    .size         = sizeof(SCSIDiskReq),
    .free_req     = scsi_free_request,
    .send_command = scsi_disk_async_command,
    .read_data    = scsi_disk_async_read_data,
    .write_data   = scsi_disk_async_write_data,
    .get_buf      = scsi_get_buf,
    .load_request = scsi_disk_load_request,
    .save_request = scsi_disk_save_request,
};

static const SCSIReqOps *const scsi_disk_reqops_dispatch[256] = {
    [TEST_UNIT_READY]                 = &scsi_disk_em_sync_reqops,
    [INQUIRY]                         = &scsi_disk_em_sync_reqops,
    [MODE_SENSE]                      = &scsi_disk_em_sync_reqops,
    [MODE_SENSE_10]                   = &scsi_disk_em_sync_reqops,
    [START_STOP]                      = &scsi_disk_em_sync_reqops,
    [ALLOW_MEDIUM_REMOVAL]            = &scsi_disk_em_sync_reqops,
    [READ_CAPACITY_10]                = &scsi_disk_em_sync_reqops,
    [READ_TOC]                        = &scsi_disk_em_sync_reqops,
    [READ_DVD_STRUCTURE]              = &scsi_disk_em_sync_reqops,
    [READ_DISC_INFORMATION]           = &scsi_disk_em_sync_reqops,
    [GET_CONFIGURATION]               = &scsi_disk_em_sync_reqops,
    [GET_EVENT_STATUS_NOTIFICATION]   = &scsi_disk_em_sync_reqops,
    [MECHANISM_STATUS]                = &scsi_disk_em_sync_reqops,
    [SERVICE_ACTION_IN_16]            = &scsi_disk_em_sync_reqops,
    [REQUEST_SENSE]                   = &scsi_disk_em_sync_reqops,
    [SEEK_10]                         = &scsi_disk_em_sync_reqops,

    [MODE_SELECT]                     = &scsi_disk_em_async_reqops,
    [MODE_SELECT_10]                  = &scsi_disk_em_async_reqops,
    [SYNCHRONIZE_CACHE]               = &scsi_disk_em_async_reqops,
    [UNMAP]                           = &scsi_disk_em_async_reqops,
    [WRITE_SAME_10]                   = &scsi_disk_em_async_reqops,
    [WRITE_SAME_16]                   = &scsi_disk_em_async_reqops,
    [VERIFY_10]                       = &scsi_disk_em_async_reqops,
    [VERIFY_12]                       = &scsi_disk_em_async_reqops,
    [VERIFY_16]                       = &scsi_disk_em_async_reqops,
    [READ_6]                          = &scsi_disk_em_async_reqops,
    [READ_10]                         = &scsi_disk_em_async_reqops,
    [READ_12]                         = &scsi_disk_em_async_reqops,
    [READ_16]                         = &scsi_disk_em_async_reqops,
    [WRITE_6]                         = &scsi_disk_em_async_reqops,
    [WRITE_10]                        = &scsi_disk_em_async_reqops,
    [WRITE_12]                        = &scsi_disk_em_async_reqops,
    [WRITE_16]                        = &scsi_disk_em_async_reqops,
    [WRITE_VERIFY_10]                 = &scsi_disk_em_async_reqops,
    [WRITE_VERIFY_12]                 = &scsi_disk_em_async_reqops,
    [WRITE_VERIFY_16]                 = &scsi_disk_em_async_reqops,
};

static SCSIRequest *scsi_new_request(SCSIDevice *d, uint32_t tag, uint32_t lun,
                                     uint8_t *buf, void *hba_private)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, d);
    SCSIRequest *req;
    const SCSIReqOps *ops;
    uint8_t command;

    command = buf[0];
    ops = scsi_disk_reqops_dispatch[command];
    if (!ops) {
        ops = &scsi_disk_em_sync_reqops;
    }
    req = scsi_req_alloc(ops, &s->qdev, tag, lun, hba_private);

#ifdef DEBUG_SCSI
    DPRINTF("Command: lun=%d tag=0x%x data=0x%02x", lun, tag, buf[0]);
    {
        int i;
        for (i = 1; i < scsi_cdb_length(buf); i++) {
            printf(" 0x%02x", buf[i]);
        }
        printf("\n");
    }
#endif

    return req;
}

#ifdef __linux__
static int get_device_type(SCSIDiskState *s)
{
    uint8_t cmd[16];
    uint8_t buf[36];
    uint8_t sensebuf[8];
    sg_io_hdr_t io_header;
    int ret;

    memset(cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    cmd[0] = INQUIRY;
    cmd[4] = sizeof(buf);

    memset(&io_header, 0, sizeof(io_header));
    io_header.interface_id = 'S';
    io_header.dxfer_direction = SG_DXFER_FROM_DEV;
    io_header.dxfer_len = sizeof(buf);
    io_header.dxferp = buf;
    io_header.cmdp = cmd;
    io_header.cmd_len = sizeof(cmd);
    io_header.mx_sb_len = sizeof(sensebuf);
    io_header.sbp = sensebuf;
    io_header.timeout = 6000; /* XXX */

    ret = blk_ioctl(s->qdev.conf.blk, SG_IO, &io_header);
    if (ret < 0 || io_header.driver_status || io_header.host_status) {
        return -1;
    }
    s->qdev.type = buf[0];
    if (buf[1] & 0x80) {
        s->features |= 1 << SCSI_DISK_F_REMOVABLE;
    }
    return 0;
}

static void scsi_block_realize(SCSIDevice *dev, Error **errp)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    int sg_version;
    int rc;

    if (!s->qdev.conf.blk) {
        error_setg(errp, "drive property not set");
        return;
    }

    /* check we are using a driver managing SG_IO (version 3 and after) */
    rc = blk_ioctl(s->qdev.conf.blk, SG_GET_VERSION_NUM, &sg_version);
    if (rc < 0) {
        error_setg(errp, "cannot get SG_IO version number: %s.  "
                     "Is this a SCSI device?",
                     strerror(-rc));
        return;
    }
    if (sg_version < 30000) {
        error_setg(errp, "scsi generic interface too old");
        return;
    }

    /* get device type from INQUIRY data */
    rc = get_device_type(s);
    if (rc < 0) {
        error_setg(errp, "INQUIRY failed");
        return;
    }

    /* Make a guess for the block size, we'll fix it when the guest sends.
     * READ CAPACITY.  If they don't, they likely would assume these sizes
     * anyway. (TODO: check in /sys).
     */
    if (s->qdev.type == TYPE_ROM || s->qdev.type == TYPE_WORM) {
        s->qdev.blocksize = 2048;
    } else {
        s->qdev.blocksize = 512;
    }

    /* Makes the scsi-block device not removable by using HMP and QMP eject
     * command.
     */
    s->features |= (1 << SCSI_DISK_F_NO_REMOVABLE_DEVOPS);

    scsi_realize(&s->qdev, errp);
    scsi_generic_read_device_identification(&s->qdev);
}

typedef struct SCSIBlockReq {
    SCSIDiskReq req;
    sg_io_hdr_t io_header;

    /* Selected bytes of the original CDB, copied into our own CDB.  */
    uint8_t cmd, cdb1, group_number;

    /* CDB passed to SG_IO.  */
    uint8_t cdb[16];
} SCSIBlockReq;

static BlockAIOCB *scsi_block_do_sgio(SCSIBlockReq *req,
                                      int64_t offset, QEMUIOVector *iov,
                                      int direction,
                                      BlockCompletionFunc *cb, void *opaque)
{
    sg_io_hdr_t *io_header = &req->io_header;
    SCSIDiskReq *r = &req->req;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    int nb_logical_blocks;
    uint64_t lba;
    BlockAIOCB *aiocb;

    /* This is not supported yet.  It can only happen if the guest does
     * reads and writes that are not aligned to one logical sectors
     * _and_ cover multiple MemoryRegions.
     */
    assert(offset % s->qdev.blocksize == 0);
    assert(iov->size % s->qdev.blocksize == 0);

    io_header->interface_id = 'S';

    /* The data transfer comes from the QEMUIOVector.  */
    io_header->dxfer_direction = direction;
    io_header->dxfer_len = iov->size;
    io_header->dxferp = (void *)iov->iov;
    io_header->iovec_count = iov->niov;
    assert(io_header->iovec_count == iov->niov); /* no overflow! */

    /* Build a new CDB with the LBA and length patched in, in case
     * DMA helpers split the transfer in multiple segments.  Do not
     * build a CDB smaller than what the guest wanted, and only build
     * a larger one if strictly necessary.
     */
    io_header->cmdp = req->cdb;
    lba = offset / s->qdev.blocksize;
    nb_logical_blocks = io_header->dxfer_len / s->qdev.blocksize;

    if ((req->cmd >> 5) == 0 && lba <= 0x1ffff) {
        /* 6-byte CDB */
        stl_be_p(&req->cdb[0], lba | (req->cmd << 24));
        req->cdb[4] = nb_logical_blocks;
        req->cdb[5] = 0;
        io_header->cmd_len = 6;
    } else if ((req->cmd >> 5) <= 1 && lba <= 0xffffffffULL) {
        /* 10-byte CDB */
        req->cdb[0] = (req->cmd & 0x1f) | 0x20;
        req->cdb[1] = req->cdb1;
        stl_be_p(&req->cdb[2], lba);
        req->cdb[6] = req->group_number;
        stw_be_p(&req->cdb[7], nb_logical_blocks);
        req->cdb[9] = 0;
        io_header->cmd_len = 10;
    } else if ((req->cmd >> 5) != 4 && lba <= 0xffffffffULL) {
        /* 12-byte CDB */
        req->cdb[0] = (req->cmd & 0x1f) | 0xA0;
        req->cdb[1] = req->cdb1;
        stl_be_p(&req->cdb[2], lba);
        stl_be_p(&req->cdb[6], nb_logical_blocks);
        req->cdb[10] = req->group_number;
        req->cdb[11] = 0;
        io_header->cmd_len = 12;
    } else {
        /* 16-byte CDB */
        req->cdb[0] = (req->cmd & 0x1f) | 0x80;
        req->cdb[1] = req->cdb1;
        stq_be_p(&req->cdb[2], lba);
        stl_be_p(&req->cdb[10], nb_logical_blocks);
        req->cdb[14] = req->group_number;
        req->cdb[15] = 0;
        io_header->cmd_len = 16;
    }

    /* The rest is as in scsi-generic.c.  */
    io_header->mx_sb_len = sizeof(r->req.sense);
    io_header->sbp = r->req.sense;
    io_header->timeout = UINT_MAX;
    io_header->usr_ptr = r;
    io_header->flags |= SG_FLAG_DIRECT_IO;

    aiocb = blk_aio_ioctl(s->qdev.conf.blk, SG_IO, io_header, cb, opaque);
    assert(aiocb != NULL);
    return aiocb;
}

static BlockAIOCB *scsi_block_dma_readv(int64_t offset,
                                        QEMUIOVector *iov,
                                        BlockCompletionFunc *cb, void *cb_opaque,
                                        void *opaque)
{
    SCSIBlockReq *r = opaque;
    return scsi_block_do_sgio(r, offset, iov,
                              SG_DXFER_FROM_DEV, cb, cb_opaque);
}

static BlockAIOCB *scsi_block_dma_writev(int64_t offset,
                                         QEMUIOVector *iov,
                                         BlockCompletionFunc *cb, void *cb_opaque,
                                         void *opaque)
{
    SCSIBlockReq *r = opaque;
    return scsi_block_do_sgio(r, offset, iov,
                              SG_DXFER_TO_DEV, cb, cb_opaque);
}

static bool scsi_block_is_passthrough(SCSIDiskState *s, uint8_t *buf)
{
    switch (buf[0]) {
    case VERIFY_10:
    case VERIFY_12:
    case VERIFY_16:
        /* Check if BYTCHK == 0x01 (data-out buffer contains data
         * for the number of logical blocks specified in the length
         * field).  For other modes, do not use scatter/gather operation.
         */
        if ((buf[1] & 6) != 2) {
            return false;
        }
        break;

    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
    case WRITE_6:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
    case WRITE_VERIFY_10:
    case WRITE_VERIFY_12:
    case WRITE_VERIFY_16:
        /* MMC writing cannot be done via DMA helpers, because it sometimes
         * involves writing beyond the maximum LBA or to negative LBA (lead-in).
         * We might use scsi_disk_dma_reqops as long as no writing commands are
         * seen, but performance usually isn't paramount on optical media.  So,
         * just make scsi-block operate the same as scsi-generic for them.
         */
        if (s->qdev.type != TYPE_ROM) {
            return false;
        }
        break;

    default:
        break;
    }

    return true;
}


static int32_t scsi_block_dma_command(SCSIRequest *req, uint8_t *buf)
{
    SCSIBlockReq *r = (SCSIBlockReq *)req;
    r->cmd = req->cmd.buf[0];
    switch (r->cmd >> 5) {
    case 0:
        /* 6-byte CDB.  */
        r->cdb1 = r->group_number = 0;
        break;
    case 1:
        /* 10-byte CDB.  */
        r->cdb1 = req->cmd.buf[1];
        r->group_number = req->cmd.buf[6];
        break;
    case 4:
        /* 12-byte CDB.  */
        r->cdb1 = req->cmd.buf[1];
        r->group_number = req->cmd.buf[10];
        break;
    case 5:
        /* 16-byte CDB.  */
        r->cdb1 = req->cmd.buf[1];
        r->group_number = req->cmd.buf[14];
        break;
    default:
        abort();
    }

    if (r->cdb1 & 0xe0) {
        /* Protection information is not supported.  */
        scsi_check_condition(&r->req, SENSE_CODE(INVALID_FIELD));
        return 0;
    }

    r->req.status = &r->io_header.status;
    return scsi_disk_dma_command(req, buf);
}

static const SCSIReqOps scsi_block_dma_reqops = {
    .size         = sizeof(SCSIBlockReq),
    .free_req     = scsi_free_request,
    .send_command = scsi_block_dma_command,
    .read_data    = scsi_read_data,
    .write_data   = scsi_write_data,
    .get_buf      = scsi_get_buf,
    .load_request = scsi_disk_load_request,
    .save_request = scsi_disk_save_request,
};

static SCSIRequest *scsi_block_new_request(SCSIDevice *d, uint32_t tag,
                                           uint32_t lun, uint8_t *buf,
                                           void *hba_private)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, d);

    if (scsi_block_is_passthrough(s, buf)) {
        return scsi_req_alloc(&scsi_generic_req_ops, &s->qdev, tag, lun,
                              hba_private);
    } else {
        return scsi_req_alloc(&scsi_block_dma_reqops, &s->qdev, tag, lun,
                              hba_private);
    }
}

static int scsi_block_parse_cdb(SCSIDevice *d, SCSICommand *cmd,
                                  uint8_t *buf, void *hba_private)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, d);

    if (scsi_block_is_passthrough(s, buf)) {
        return scsi_bus_parse_cdb(&s->qdev, cmd, buf, hba_private);
    } else {
        return scsi_req_parse_cdb(&s->qdev, cmd, buf);
    }
}

#endif

static
BlockAIOCB *scsi_dma_readv(int64_t offset, QEMUIOVector *iov,
                           BlockCompletionFunc *cb, void *cb_opaque,
                           void *opaque)
{
    SCSIDiskReq *r = opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    return blk_aio_preadv(s->qdev.conf.blk, offset, iov, 0, cb, cb_opaque);
}

static
BlockAIOCB *scsi_dma_writev(int64_t offset, QEMUIOVector *iov,
                            BlockCompletionFunc *cb, void *cb_opaque,
                            void *opaque)
{
    SCSIDiskReq *r = opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    return blk_aio_pwritev(s->qdev.conf.blk, offset, iov, 0, cb, cb_opaque);
}

static void scsi_disk_base_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDiskClass *sdc = SCSI_DISK_BASE_CLASS(klass);

    dc->fw_name = "disk";
    dc->reset = scsi_disk_reset;
    sdc->dma_readv = scsi_dma_readv;
    sdc->dma_writev = scsi_dma_writev;
    sdc->ignore_fua = false;
}

static const TypeInfo scsi_disk_base_info = {
    .name          = TYPE_SCSI_DISK_BASE,
    .parent        = TYPE_SCSI_DEVICE,
    .class_init    = scsi_disk_base_class_initfn,
    .instance_size = sizeof(SCSIDiskState),
    .class_size    = sizeof(SCSIDiskClass),
    .abstract      = true,
};

#define DEFINE_SCSI_DISK_PROPERTIES()                                \
    DEFINE_BLOCK_PROPERTIES(SCSIDiskState, qdev.conf),               \
    DEFINE_BLOCK_ERROR_PROPERTIES(SCSIDiskState, qdev.conf),         \
    DEFINE_PROP_STRING("ver", SCSIDiskState, version),               \
    DEFINE_PROP_STRING("serial", SCSIDiskState, serial),             \
    DEFINE_PROP_STRING("vendor", SCSIDiskState, vendor),             \
    DEFINE_PROP_STRING("product", SCSIDiskState, product)

static Property scsi_hd_properties[] = {
    DEFINE_SCSI_DISK_PROPERTIES(),
    DEFINE_PROP_BIT("removable", SCSIDiskState, features,
                    SCSI_DISK_F_REMOVABLE, false),
    DEFINE_PROP_BIT("dpofua", SCSIDiskState, features,
                    SCSI_DISK_F_DPOFUA, false),
    DEFINE_PROP_UINT64("wwn", SCSIDiskState, qdev.wwn, 0),
    DEFINE_PROP_UINT64("port_wwn", SCSIDiskState, qdev.port_wwn, 0),
    DEFINE_PROP_UINT16("port_index", SCSIDiskState, port_index, 0),
    DEFINE_PROP_UINT64("max_unmap_size", SCSIDiskState, max_unmap_size,
                       DEFAULT_MAX_UNMAP_SIZE),
    DEFINE_PROP_UINT64("max_io_size", SCSIDiskState, max_io_size,
                       DEFAULT_MAX_IO_SIZE),
    DEFINE_BLOCK_CHS_PROPERTIES(SCSIDiskState, qdev.conf),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_scsi_disk_state = {
    .name = "scsi-disk",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_SCSI_DEVICE(qdev, SCSIDiskState),
        VMSTATE_BOOL(media_changed, SCSIDiskState),
        VMSTATE_BOOL(media_event, SCSIDiskState),
        VMSTATE_BOOL(eject_request, SCSIDiskState),
        VMSTATE_BOOL(tray_open, SCSIDiskState),
        VMSTATE_BOOL(tray_locked, SCSIDiskState),
        VMSTATE_END_OF_LIST()
    }
};

static void scsi_hd_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->realize      = scsi_hd_realize;
    sc->alloc_req    = scsi_new_request;
    sc->unit_attention_reported = scsi_disk_unit_attention_reported;
    dc->desc = "virtual SCSI disk";
    dc->props = scsi_hd_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static const TypeInfo scsi_hd_info = {
    .name          = "scsi-hd",
    .parent        = TYPE_SCSI_DISK_BASE,
    .class_init    = scsi_hd_class_initfn,
};

static Property scsi_cd_properties[] = {
    DEFINE_SCSI_DISK_PROPERTIES(),
    DEFINE_PROP_UINT64("wwn", SCSIDiskState, qdev.wwn, 0),
    DEFINE_PROP_UINT64("port_wwn", SCSIDiskState, qdev.port_wwn, 0),
    DEFINE_PROP_UINT16("port_index", SCSIDiskState, port_index, 0),
    DEFINE_PROP_UINT64("max_io_size", SCSIDiskState, max_io_size,
                       DEFAULT_MAX_IO_SIZE),
    DEFINE_PROP_END_OF_LIST(),
};

static void scsi_cd_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->realize      = scsi_cd_realize;
    sc->alloc_req    = scsi_new_request;
    sc->unit_attention_reported = scsi_disk_unit_attention_reported;
    dc->desc = "virtual SCSI CD-ROM";
    dc->props = scsi_cd_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static const TypeInfo scsi_cd_info = {
    .name          = "scsi-cd",
    .parent        = TYPE_SCSI_DISK_BASE,
    .class_init    = scsi_cd_class_initfn,
};

#ifdef __linux__
static Property scsi_block_properties[] = {
    DEFINE_PROP_DRIVE("drive", SCSIDiskState, qdev.conf.blk),
    DEFINE_PROP_END_OF_LIST(),
};

static void scsi_block_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);
    SCSIDiskClass *sdc = SCSI_DISK_BASE_CLASS(klass);

    sc->realize      = scsi_block_realize;
    sc->alloc_req    = scsi_block_new_request;
    sc->parse_cdb    = scsi_block_parse_cdb;
    sdc->dma_readv   = scsi_block_dma_readv;
    sdc->dma_writev  = scsi_block_dma_writev;
    sdc->ignore_fua  = true;
    dc->desc = "SCSI block device passthrough";
    dc->props = scsi_block_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static const TypeInfo scsi_block_info = {
    .name          = "scsi-block",
    .parent        = TYPE_SCSI_DISK_BASE,
    .class_init    = scsi_block_class_initfn,
};
#endif

static Property scsi_disk_properties[] = {
    DEFINE_SCSI_DISK_PROPERTIES(),
    DEFINE_PROP_BIT("removable", SCSIDiskState, features,
                    SCSI_DISK_F_REMOVABLE, false),
    DEFINE_PROP_BIT("dpofua", SCSIDiskState, features,
                    SCSI_DISK_F_DPOFUA, false),
    DEFINE_PROP_UINT64("wwn", SCSIDiskState, qdev.wwn, 0),
    DEFINE_PROP_UINT64("port_wwn", SCSIDiskState, qdev.port_wwn, 0),
    DEFINE_PROP_UINT16("port_index", SCSIDiskState, port_index, 0),
    DEFINE_PROP_UINT64("max_unmap_size", SCSIDiskState, max_unmap_size,
                       DEFAULT_MAX_UNMAP_SIZE),
    DEFINE_PROP_UINT64("max_io_size", SCSIDiskState, max_io_size,
                       DEFAULT_MAX_IO_SIZE),
    DEFINE_PROP_END_OF_LIST(),
};

static void scsi_disk_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->realize      = scsi_disk_realize;
    sc->alloc_req    = scsi_new_request;
    sc->unit_attention_reported = scsi_disk_unit_attention_reported;
    dc->fw_name = "disk";
    dc->desc = "virtual SCSI disk or CD-ROM (legacy)";
    dc->reset = scsi_disk_reset;
    dc->props = scsi_disk_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static const TypeInfo scsi_disk_info = {
    .name          = "scsi-disk",
    .parent        = TYPE_SCSI_DISK_BASE,
    .class_init    = scsi_disk_class_initfn,
};

static void scsi_disk_register_types(void)
{
    type_register_static(&scsi_disk_base_info);
    type_register_static(&scsi_hd_info);
    type_register_static(&scsi_cd_info);
#ifdef __linux__
    type_register_static(&scsi_block_info);
#endif
    type_register_static(&scsi_disk_info);
}

type_init(scsi_disk_register_types)
