/*
 * QEMU SCSI Emulation
 *
 * Copyright 2016 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef QEMU_SCSI_EMULATE_H
#define QEMU_SCSI_EMULATE_H

#include "sysemu/block-backend.h"
#include "scsi/sense.h"
#include "hw/block/block.h"
#include "sysemu/dma.h"

#define SCSI_MAX_INQUIRY_LEN        256

#define SCSI_DISK_F_REMOVABLE             0
#define SCSI_DISK_F_DPOFUA                1
#define SCSI_DISK_F_NO_REMOVABLE_DEVOPS   2

typedef struct {
    BlockConf *conf;
    int scsi_type;
    bool media_changed;
    bool media_event;
    bool eject_request;
    bool tray_open;
    bool tray_locked;
    char *version;
    char *serial;
    char *vendor;
    char *product;
    uint64_t wwn;
    uint64_t port_wwn;
    uint16_t port_index;
    int blocksize;
    uint64_t max_unmap_size;
    uint64_t max_io_size;
    uint32_t features;
    uint64_t *max_lba;
    bool tcq;
    bool skip_fua;
} SCSIEmu;

typedef struct {
    SCSIEmu *s;
    /* Both sector and sector_count are in terms of qemu 512 byte blocks.  */
    uint64_t sector;
    uint32_t sector_count;
    uint32_t buflen;
    bool started;
    bool need_fua_emulation;
    bool no_account_failed;
    struct iovec iov;
    QEMUIOVector qiov;
    BlockAcctCookie acct;
    unsigned char *status;
    BlockCompletionFunc *cb;
    void *opaque;
    bool is_read;
    int error;
    uint8_t command;
} SCSIEmuReq;

SCSIEmu *scsi_emu_new(BlockConf *conf, int scsi_type,
                      bool tcq, uint64_t *max_lba,
                      const char *version, const char *serial,
                      const char *vendor, const char *product);
void scsi_emu_free(SCSIEmu *s);
void scsi_emu_reset(SCSIEmu *s);
void scsi_emu_sync_cmd(SCSIEmu *s, uint8_t *cdb,
                       uint8_t *outbuf, int buflen,
                       const SCSISense **sense);
SCSIEmuReq *scsi_emu_async_cmd_begin(SCSIEmu *s, uint8_t *cdb,
                                     const SCSISense **sense);
BlockAIOCB *scsi_emu_req_continue(SCSIEmuReq *r, DMAIOFunc iofunc,
                                  void *iofunc_opaque,
                                  QEMUIOVector *qiov, QEMUSGList *sg,
                                  BlockCompletionFunc *cb, void *opaque);

#endif
