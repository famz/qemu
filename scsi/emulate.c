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

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "scsi/common.h"
#include "scsi/emulate.h"
#include "qemu/cutils.h"
#include "sysemu/block-backend.h"
#include "block/cdrom.h"

#ifdef DEBUG_SCSI
#define DEBUG_SCSI_PRINT 1
#else
#define DEBUG_SCSI_PRINT 0
#endif

#define DPRINTF(fmt, ...) \
do { \
    if (DEBUG_SCSI_PRINT) { printf("scsi-disk-em: " fmt , ## __VA_ARGS__); } \
} while (0)

void scsi_disk_em_init(SCSIDiskEm *s, BlockConf *conf,
                       int scsi_type, bool tcq, uint64_t *max_lba)
{
    *s = (SCSIDiskEm) {
        .conf = conf,
        .scsi_type = scsi_type,
        .tcq = tcq,
        .max_lba = max_lba,
    };
    blk_ref(conf->blk);
}

void scsi_disk_em_reset(SCSIDiskEm *s)
{
    /* reset tray statuses */
    s->tray_locked = 0;
    s->tray_open = 0;
}

void scsi_disk_em_finalize(SCSIDiskEm *s)
{
    blk_unref(s->conf->blk);
}

static int scsi_disk_em_inquiry(SCSIDiskEmReq *req, uint8_t *cdb,
                                uint8_t *outbuf, int outbuflen)
{
    SCSIDiskEm *s = req->em;
    int buflen = 0;
    int start;

    if (cdb[1] & 0x1) {
        /* Vital product data */
        uint8_t page_code = cdb[2];

        outbuf[buflen++] = s->scsi_type & 0x1f;
        outbuf[buflen++] = page_code ; // this page
        outbuf[buflen++] = 0x00;
        outbuf[buflen++] = 0x00;
        start = buflen;

        switch (page_code) {
        case 0x00: /* Supported page codes, mandatory */
        {
            DPRINTF("Inquiry EVPD[Supported pages] "
                    "buffer size %d\n", buflen);
            outbuf[buflen++] = 0x00; // list of supported pages (this page)
            if (s->serial) {
                outbuf[buflen++] = 0x80; // unit serial number
            }
            outbuf[buflen++] = 0x83; // device identification
            if (s->scsi_type == TYPE_DISK) {
                outbuf[buflen++] = 0xb0; // block limits
                outbuf[buflen++] = 0xb2; // thin provisioning
            }
            break;
        }
        case 0x80: /* Device serial number, optional */
        {
            int l;

            if (!s->serial) {
                DPRINTF("Inquiry (EVPD[Serial number] not supported\n");
                return -1;
            }

            l = strlen(s->serial);
            if (l > 20) {
                l = 20;
            }

            DPRINTF("Inquiry EVPD[Serial number] "
                    "buffer size %d\n", buflen);
            memcpy(outbuf + buflen, s->serial, l);
            buflen += l;
            break;
        }

        case 0x83: /* Device identification page, mandatory */
        {
            const char *str = s->serial ?: blk_name(s->conf->blk);
            int max_len = s->serial ? 20 : 255 - 8;
            int id_len = strlen(str);

            if (id_len > max_len) {
                id_len = max_len;
            }
            DPRINTF("Inquiry EVPD[Device identification] "
                    "buffer size %d\n", buflen);

            outbuf[buflen++] = 0x2; // ASCII
            outbuf[buflen++] = 0;   // not officially assigned
            outbuf[buflen++] = 0;   // reserved
            outbuf[buflen++] = id_len; // length of data following
            memcpy(outbuf + buflen, str, id_len);
            buflen += id_len;

            if (s->wwn) {
                outbuf[buflen++] = 0x1; // Binary
                outbuf[buflen++] = 0x3; // NAA
                outbuf[buflen++] = 0;   // reserved
                outbuf[buflen++] = 8;
                stq_be_p(&outbuf[buflen], s->wwn);
                buflen += 8;
            }

            if (s->port_wwn) {
                outbuf[buflen++] = 0x61; // SAS / Binary
                outbuf[buflen++] = 0x93; // PIV / Target port / NAA
                outbuf[buflen++] = 0;    // reserved
                outbuf[buflen++] = 8;
                stq_be_p(&outbuf[buflen], s->port_wwn);
                buflen += 8;
            }

            if (s->port_index) {
                outbuf[buflen++] = 0x61; // SAS / Binary
                outbuf[buflen++] = 0x94; // PIV / Target port / relative target port
                outbuf[buflen++] = 0;    // reserved
                outbuf[buflen++] = 4;
                stw_be_p(&outbuf[buflen + 2], s->port_index);
                buflen += 4;
            }
            break;
        }
        case 0xb0: /* block limits */
        {
            unsigned int unmap_sectors =
                    s->conf->discard_granularity / s->blocksize;
            unsigned int min_io_size =
                    s->conf->min_io_size / s->blocksize;
            unsigned int opt_io_size =
                    s->conf->opt_io_size / s->blocksize;
            unsigned int max_unmap_sectors =
                    s->max_unmap_size / s->blocksize;
            unsigned int max_io_sectors =
                    s->max_io_size / s->blocksize;

            if (s->scsi_type == TYPE_ROM) {
                DPRINTF("Inquiry (EVPD[%02X] not supported for CDROM\n",
                        page_code);
                return -1;
            }
            /* required VPD size with unmap support */
            buflen = 0x40;
            memset(outbuf + 4, 0, buflen - 4);

            outbuf[4] = 0x1; /* wsnz */

            /* optimal transfer length granularity */
            outbuf[6] = (min_io_size >> 8) & 0xff;
            outbuf[7] = min_io_size & 0xff;

            /* maximum transfer length */
            outbuf[8] = (max_io_sectors >> 24) & 0xff;
            outbuf[9] = (max_io_sectors >> 16) & 0xff;
            outbuf[10] = (max_io_sectors >> 8) & 0xff;
            outbuf[11] = max_io_sectors & 0xff;

            /* optimal transfer length */
            outbuf[12] = (opt_io_size >> 24) & 0xff;
            outbuf[13] = (opt_io_size >> 16) & 0xff;
            outbuf[14] = (opt_io_size >> 8) & 0xff;
            outbuf[15] = opt_io_size & 0xff;

            /* max unmap LBA count, default is 1GB */
            outbuf[20] = (max_unmap_sectors >> 24) & 0xff;
            outbuf[21] = (max_unmap_sectors >> 16) & 0xff;
            outbuf[22] = (max_unmap_sectors >> 8) & 0xff;
            outbuf[23] = max_unmap_sectors & 0xff;

            /* max unmap descriptors, 255 fit in 4 kb with an 8-byte header.  */
            outbuf[24] = 0;
            outbuf[25] = 0;
            outbuf[26] = 0;
            outbuf[27] = 255;

            /* optimal unmap granularity */
            outbuf[28] = (unmap_sectors >> 24) & 0xff;
            outbuf[29] = (unmap_sectors >> 16) & 0xff;
            outbuf[30] = (unmap_sectors >> 8) & 0xff;
            outbuf[31] = unmap_sectors & 0xff;

            /* max write same size */
            outbuf[36] = 0;
            outbuf[37] = 0;
            outbuf[38] = 0;
            outbuf[39] = 0;

            outbuf[40] = (max_io_sectors >> 24) & 0xff;
            outbuf[41] = (max_io_sectors >> 16) & 0xff;
            outbuf[42] = (max_io_sectors >> 8) & 0xff;
            outbuf[43] = max_io_sectors & 0xff;
            break;
        }
        case 0xb2: /* thin provisioning */
        {
            buflen = 8;
            outbuf[4] = 0;
            outbuf[5] = 0xe0; /* unmap & write_same 10/16 all supported */
            outbuf[6] = s->conf->discard_granularity ? 2 : 1;
            outbuf[7] = 0;
            break;
        }
        default:
            return -1;
        }
        /* done with EVPD */
        assert(buflen - start <= 255);
        outbuf[start - 1] = buflen - start;
        return buflen;
    }

    /* Standard INQUIRY data */
    if (cdb[2] != 0) {
        return -1;
    }

    /* PAGE CODE == 0 */
    buflen = outbuflen;
    if (buflen > SCSI_MAX_INQUIRY_LEN) {
        buflen = SCSI_MAX_INQUIRY_LEN;
    }

    outbuf[0] = s->scsi_type & 0x1f;
    outbuf[1] = (s->features & (1 << SCSI_DISK_F_REMOVABLE)) ? 0x80 : 0;

    strpadcpy((char *) &outbuf[16], 16, s->product, ' ');
    strpadcpy((char *) &outbuf[8], 8, s->vendor, ' ');

    memset(&outbuf[32], 0, 4);
    memcpy(&outbuf[32], s->version, MIN(4, strlen(s->version)));
    /*
     * We claim conformance to SPC-3, which is required for guests
     * to ask for modern features like READ CAPACITY(16) or the
     * block characteristics VPD page by default.  Not all of SPC-3
     * is actually implemented, but we're good enough.
     */
    outbuf[2] = 5;
    outbuf[3] = 2 | 0x10; /* Format 2, HiSup */

    if (buflen > 36) {
        outbuf[4] = buflen - 5; /* Additional Length = (Len - 1) - 4 */
    } else {
        /* If the allocation length of CDB is too small,
               the additional length is not adjusted */
        outbuf[4] = 36 - 5;
    }

    /* Sync data transfer and TCQ.  */
    outbuf[7] = 0x10 | (s->tcq ? 0x02 : 0);
    return buflen;
}

static int mode_sense_page(SCSIDiskEm *s, int page, uint8_t **p_outbuf,
                           int page_control)
{
    static const int mode_sense_valid[0x3f] = {
        [MODE_PAGE_HD_GEOMETRY]            = (1 << TYPE_DISK),
        [MODE_PAGE_FLEXIBLE_DISK_GEOMETRY] = (1 << TYPE_DISK),
        [MODE_PAGE_CACHING]                = (1 << TYPE_DISK) | (1 << TYPE_ROM),
        [MODE_PAGE_R_W_ERROR]              = (1 << TYPE_DISK) | (1 << TYPE_ROM),
        [MODE_PAGE_AUDIO_CTL]              = (1 << TYPE_ROM),
        [MODE_PAGE_CAPABILITIES]           = (1 << TYPE_ROM),
    };

    uint8_t *p = *p_outbuf + 2;
    int length;

    if ((mode_sense_valid[page] & (1 << s->scsi_type)) == 0) {
        return -1;
    }

    /*
     * If Changeable Values are requested, a mask denoting those mode parameters
     * that are changeable shall be returned. As we currently don't support
     * parameter changes via MODE_SELECT all bits are returned set to zero.
     * The buffer was already menset to zero by the caller of this function.
     *
     * The offsets here are off by two compared to the descriptions in the
     * SCSI specs, because those include a 2-byte header.  This is unfortunate,
     * but it is done so that offsets are consistent within our implementation
     * of MODE SENSE and MODE SELECT.  MODE SELECT has to deal with both
     * 2-byte and 4-byte headers.
     */
    switch (page) {
    case MODE_PAGE_HD_GEOMETRY:
        length = 0x16;
        if (page_control == 1) { /* Changeable Values */
            break;
        }
        /* if a geometry hint is available, use it */
        p[0] = (s->conf->cyls >> 16) & 0xff;
        p[1] = (s->conf->cyls >> 8) & 0xff;
        p[2] = s->conf->cyls & 0xff;
        p[3] = s->conf->heads & 0xff;
        /* Write precomp start cylinder, disabled */
        p[4] = (s->conf->cyls >> 16) & 0xff;
        p[5] = (s->conf->cyls >> 8) & 0xff;
        p[6] = s->conf->cyls & 0xff;
        /* Reduced current start cylinder, disabled */
        p[7] = (s->conf->cyls >> 16) & 0xff;
        p[8] = (s->conf->cyls >> 8) & 0xff;
        p[9] = s->conf->cyls & 0xff;
        /* Device step rate [ns], 200ns */
        p[10] = 0;
        p[11] = 200;
        /* Landing zone cylinder */
        p[12] = 0xff;
        p[13] =  0xff;
        p[14] = 0xff;
        /* Medium rotation rate [rpm], 5400 rpm */
        p[18] = (5400 >> 8) & 0xff;
        p[19] = 5400 & 0xff;
        break;

    case MODE_PAGE_FLEXIBLE_DISK_GEOMETRY:
        length = 0x1e;
        if (page_control == 1) { /* Changeable Values */
            break;
        }
        /* Transfer rate [kbit/s], 5Mbit/s */
        p[0] = 5000 >> 8;
        p[1] = 5000 & 0xff;
        /* if a geometry hint is available, use it */
        p[2] = s->conf->heads & 0xff;
        p[3] = s->conf->secs & 0xff;
        p[4] = s->blocksize >> 8;
        p[6] = (s->conf->cyls >> 8) & 0xff;
        p[7] = s->conf->cyls & 0xff;
        /* Write precomp start cylinder, disabled */
        p[8] = (s->conf->cyls >> 8) & 0xff;
        p[9] = s->conf->cyls & 0xff;
        /* Reduced current start cylinder, disabled */
        p[10] = (s->conf->cyls >> 8) & 0xff;
        p[11] = s->conf->cyls & 0xff;
        /* Device step rate [100us], 100us */
        p[12] = 0;
        p[13] = 1;
        /* Device step pulse width [us], 1us */
        p[14] = 1;
        /* Device head settle delay [100us], 100us */
        p[15] = 0;
        p[16] = 1;
        /* Motor on delay [0.1s], 0.1s */
        p[17] = 1;
        /* Motor off delay [0.1s], 0.1s */
        p[18] = 1;
        /* Medium rotation rate [rpm], 5400 rpm */
        p[26] = (5400 >> 8) & 0xff;
        p[27] = 5400 & 0xff;
        break;

    case MODE_PAGE_CACHING:
        length = 0x12;
        if (page_control == 1 || /* Changeable Values */
            blk_enable_write_cache(s->conf->blk)) {
            p[0] = 4; /* WCE */
        }
        break;

    case MODE_PAGE_R_W_ERROR:
        length = 10;
        if (page_control == 1) { /* Changeable Values */
            break;
        }
        p[0] = 0x80; /* Automatic Write Reallocation Enabled */
        if (s->scsi_type == TYPE_ROM) {
            p[1] = 0x20; /* Read Retry Count */
        }
        break;

    case MODE_PAGE_AUDIO_CTL:
        length = 14;
        break;

    case MODE_PAGE_CAPABILITIES:
        length = 0x14;
        if (page_control == 1) { /* Changeable Values */
            break;
        }

        p[0] = 0x3b; /* CD-R & CD-RW read */
        p[1] = 0; /* Writing not supported */
        p[2] = 0x7f; /* Audio, composite, digital out,
                        mode 2 form 1&2, multi session */
        p[3] = 0xff; /* CD DA, DA accurate, RW supported,
                        RW corrected, C2 errors, ISRC,
                        UPC, Bar code */
        p[4] = 0x2d | (s->tray_locked ? 2 : 0);
        /* Locking supported, jumper present, eject, tray */
        p[5] = 0; /* no volume & mute control, no
                     changer */
        p[6] = (50 * 176) >> 8; /* 50x read speed */
        p[7] = (50 * 176) & 0xff;
        p[8] = 2 >> 8; /* Two volume levels */
        p[9] = 2 & 0xff;
        p[10] = 2048 >> 8; /* 2M buffer */
        p[11] = 2048 & 0xff;
        p[12] = (16 * 176) >> 8; /* 16x read speed current */
        p[13] = (16 * 176) & 0xff;
        p[16] = (16 * 176) >> 8; /* 16x write speed */
        p[17] = (16 * 176) & 0xff;
        p[18] = (16 * 176) >> 8; /* 16x write speed current */
        p[19] = (16 * 176) & 0xff;
        break;

    default:
        return -1;
    }

    assert(length < 256);
    (*p_outbuf)[0] = page;
    (*p_outbuf)[1] = length;
    *p_outbuf += length + 2;
    return length + 2;
}

static int scsi_disk_em_mode_sense(SCSIDiskEmReq *req,
                                   uint8_t *cdb, uint8_t *outbuf,
                                   int outbuflen)
{
    SCSIDiskEm *s = req->em;
    uint64_t nb_sectors;
    bool dbd;
    int page, buflen, ret, page_control;
    uint8_t *p;
    uint8_t dev_specific_param;

    dbd = (cdb[1] & 0x8) != 0;
    page = cdb[2] & 0x3f;
    page_control = (cdb[2] & 0xc0) >> 6;
    DPRINTF("Mode Sense(%d) (page %d, xfer %d, page_control %d)\n",
        (cdb[0] == MODE_SENSE) ? 6 : 10, page, outbuflen, page_control);
    memset(outbuf, 0, outbuflen);
    p = outbuf;

    if (s->scsi_type == TYPE_DISK) {
        dev_specific_param = s->features & (1 << SCSI_DISK_F_DPOFUA) ? 0x10 : 0;
        if (blk_is_read_only(s->conf->blk)) {
            dev_specific_param |= 0x80; /* Readonly.  */
        }
    } else {
        /* MMC prescribes that CD/DVD drives have no block descriptors,
         * and defines no device-specific parameter.  */
        dev_specific_param = 0x00;
        dbd = true;
    }

    if (cdb[0] == MODE_SENSE) {
        p[1] = 0; /* Default media type.  */
        p[2] = dev_specific_param;
        p[3] = 0; /* Block descriptor length.  */
        p += 4;
    } else { /* MODE_SENSE_10 */
        p[2] = 0; /* Default media type.  */
        p[3] = dev_specific_param;
        p[6] = p[7] = 0; /* Block descriptor length.  */
        p += 8;
    }

    blk_get_geometry(s->conf->blk, &nb_sectors);
    if (!dbd && nb_sectors) {
        if (cdb[0] == MODE_SENSE) {
            outbuf[3] = 8; /* Block descriptor length  */
        } else { /* MODE_SENSE_10 */
            outbuf[7] = 8; /* Block descriptor length  */
        }
        nb_sectors /= (s->blocksize / 512);
        if (nb_sectors > 0xffffff) {
            nb_sectors = 0;
        }
        p[0] = 0; /* media density code */
        p[1] = (nb_sectors >> 16) & 0xff;
        p[2] = (nb_sectors >> 8) & 0xff;
        p[3] = nb_sectors & 0xff;
        p[4] = 0; /* reserved */
        p[5] = 0; /* bytes 5-7 are the sector size in bytes */
        p[6] = s->blocksize >> 8;
        p[7] = 0;
        p += 8;
    }

    if (page_control == 3) {
        /* Saved Values */
        req->sense = &SENSE_CODE(SAVING_PARAMS_NOT_SUPPORTED);
        return -1;
    }

    if (page == 0x3f) {
        for (page = 0; page <= 0x3e; page++) {
            mode_sense_page(s, page, &p, page_control);
        }
    } else {
        ret = mode_sense_page(s, page, &p, page_control);
        if (ret == -1) {
            return -1;
        }
    }

    buflen = p - outbuf;
    /*
     * The mode data length field specifies the length in bytes of the
     * following data that is available to be transferred. The mode data
     * length does not include itself.
     */
    if (cdb[0] == MODE_SENSE) {
        outbuf[0] = buflen - 1;
    } else { /* MODE_SENSE_10 */
        outbuf[0] = ((buflen - 2) >> 8) & 0xff;
        outbuf[1] = (buflen - 2) & 0xff;
    }
    return buflen;
}

static int scsi_disk_em_read_toc(SCSIDiskEm *s, uint8_t *cdb, uint8_t *outbuf)
{
    int start_track, format, msf, toclen;
    uint64_t nb_sectors;

    msf = cdb[1] & 2;
    format = cdb[2] & 0xf;
    start_track = cdb[6];
    blk_get_geometry(s->conf->blk, &nb_sectors);
    DPRINTF("Read TOC (track %d format %d msf %d)\n",
            start_track, format, msf >> 1);
    nb_sectors /= s->blocksize / 512;
    switch (format) {
    case 0:
        toclen = cdrom_read_toc(nb_sectors, outbuf, msf, start_track);
        break;
    case 1:
        /* multi session : only a single session defined */
        toclen = 12;
        memset(outbuf, 0, 12);
        outbuf[1] = 0x0a;
        outbuf[2] = 0x01;
        outbuf[3] = 0x01;
        break;
    case 2:
        toclen = cdrom_read_toc_raw(nb_sectors, outbuf, msf, start_track);
        break;
    default:
        return -1;
    }
    return toclen;
}

static int scsi_disk_em_start_stop(SCSIDiskEmReq *req, uint8_t *cdb)
{
    SCSIDiskEm *s = req->em;
    bool start = cdb[4] & 1;
    bool loej = cdb[4] & 2; /* load on start, eject on !start */
    int pwrcnd = cdb[4] & 0xf0;

    if (pwrcnd) {
        /* eject/load only happens for power condition == 0 */
        return 0;
    }

    if ((s->features & (1 << SCSI_DISK_F_REMOVABLE)) && loej) {
        if (!start && !s->tray_open && s->tray_locked) {
            req->sense = blk_is_inserted(s->conf->blk) ?
                &SENSE_CODE(ILLEGAL_REQ_REMOVAL_PREVENTED) :
                &SENSE_CODE(NOT_READY_REMOVAL_PREVENTED);
            return -1;
        }

        if (s->tray_open != !start) {
            blk_eject(s->conf->blk, !start);
            s->tray_open = !start;
        }
    }
    return 0;
}

static int scsi_disk_em_mechanism_status(SCSIDiskEm *s, uint8_t *outbuf)
{
    if (s->scsi_type != TYPE_ROM) {
        return -1;
    }
    memset(outbuf, 0, 8);
    outbuf[5] = 1; /* CD-ROM */
    return 8;
}

static inline bool media_is_dvd(SCSIDiskEm *s)
{
    uint64_t nb_sectors;
    if (s->scsi_type != TYPE_ROM) {
        return false;
    }
    if (!blk_is_inserted(s->conf->blk)) {
        return false;
    }
    if (s->tray_open) {
        return false;
    }
    blk_get_geometry(s->conf->blk, &nb_sectors);
    return nb_sectors > CD_MAX_SECTORS;
}

static inline bool media_is_cd(SCSIDiskEm *s)
{
    uint64_t nb_sectors;
    if (s->scsi_type != TYPE_ROM) {
        return false;
    }
    if (!blk_is_inserted(s->conf->blk)) {
        return false;
    }
    if (s->tray_open) {
        return false;
    }
    blk_get_geometry(s->conf->blk, &nb_sectors);
    return nb_sectors <= CD_MAX_SECTORS;
}

static int scsi_disk_em_get_configuration(SCSIDiskEm *s, uint8_t *outbuf)
{
    int current;

    if (s->scsi_type != TYPE_ROM) {
        return -1;
    }

    if (media_is_dvd(s)) {
        current = MMC_PROFILE_DVD_ROM;
    } else if (media_is_cd(s)) {
        current = MMC_PROFILE_CD_ROM;
    } else {
        current = MMC_PROFILE_NONE;
    }

    memset(outbuf, 0, 40);
    stl_be_p(&outbuf[0], 36); /* Bytes after the data length field */
    stw_be_p(&outbuf[6], current);
    /* outbuf[8] - outbuf[19]: Feature 0 - Profile list */
    outbuf[10] = 0x03; /* persistent, current */
    outbuf[11] = 8; /* two profiles */
    stw_be_p(&outbuf[12], MMC_PROFILE_DVD_ROM);
    outbuf[14] = (current == MMC_PROFILE_DVD_ROM);
    stw_be_p(&outbuf[16], MMC_PROFILE_CD_ROM);
    outbuf[18] = (current == MMC_PROFILE_CD_ROM);
    /* outbuf[20] - outbuf[31]: Feature 1 - Core feature */
    stw_be_p(&outbuf[20], 1);
    outbuf[22] = 0x08 | 0x03; /* version 2, persistent, current */
    outbuf[23] = 8;
    stl_be_p(&outbuf[24], 1); /* SCSI */
    outbuf[28] = 1; /* DBE = 1, mandatory */
    /* outbuf[32] - outbuf[39]: Feature 3 - Removable media feature */
    stw_be_p(&outbuf[32], 3);
    outbuf[34] = 0x08 | 0x03; /* version 2, persistent, current */
    outbuf[35] = 4;
    outbuf[36] = 0x39; /* tray, load=1, eject=1, unlocked at powerup, lock=1 */
    /* TODO: Random readable, CD read, DVD read, drive serial number,
       power management */
    return 40;
}

static int scsi_event_status_media(SCSIDiskEm *s, uint8_t *outbuf)
{
    uint8_t event_code, media_status;

    media_status = 0;
    if (s->tray_open) {
        media_status = MS_TRAY_OPEN;
    } else if (blk_is_inserted(s->conf->blk)) {
        media_status = MS_MEDIA_PRESENT;
    }

    /* Event notification descriptor */
    event_code = MEC_NO_CHANGE;
    if (media_status != MS_TRAY_OPEN) {
        if (s->media_event) {
            event_code = MEC_NEW_MEDIA;
            s->media_event = false;
        } else if (s->eject_request) {
            event_code = MEC_EJECT_REQUESTED;
            s->eject_request = false;
        }
    }

    outbuf[0] = event_code;
    outbuf[1] = media_status;

    /* These fields are reserved, just clear them. */
    outbuf[2] = 0;
    outbuf[3] = 0;
    return 4;
}

static int scsi_disk_em_get_event_status_notification(SCSIDiskEm *s,
                                                      uint8_t *cdb,
                                                      uint8_t *outbuf)
{
    int size;
    uint8_t notification_class_request = cdb[4];
    if (s->scsi_type != TYPE_ROM) {
        return -1;
    }
    if ((cdb[1] & 1) == 0) {
        /* asynchronous */
        return -1;
    }

    size = 4;
    outbuf[0] = outbuf[1] = 0;
    outbuf[3] = 1 << GESN_MEDIA; /* supported events */
    if (notification_class_request & (1 << GESN_MEDIA)) {
        outbuf[2] = GESN_MEDIA;
        size += scsi_event_status_media(s, &outbuf[size]);
    } else {
        outbuf[2] = 0x80;
    }
    stw_be_p(outbuf, size - 4);
    return size;
}

static int scsi_disk_em_read_disc_information(SCSIDiskEmReq *req,
                                              uint8_t *cdb,
                                              uint8_t *outbuf)
{
    SCSIDiskEm *s = req->em;
    uint8_t type = cdb[1] & 7;

    if (s->scsi_type != TYPE_ROM) {
        return -1;
    }

    /* Types 1/2 are only defined for Blu-Ray.  */
    if (type != 0) {
        req->sense = &SENSE_CODE(INVALID_FIELD);
        return -1;
    }

    memset(outbuf, 0, 34);
    outbuf[1] = 32;
    outbuf[2] = 0xe; /* last session complete, disc finalized */
    outbuf[3] = 1;   /* first track on disc */
    outbuf[4] = 1;   /* # of sessions */
    outbuf[5] = 1;   /* first track of last session */
    outbuf[6] = 1;   /* last track of last session */
    outbuf[7] = 0x20; /* unrestricted use */
    outbuf[8] = 0x00; /* CD-ROM or DVD-ROM */
    /* 9-10-11: most significant byte corresponding bytes 4-5-6 */
    /* 12-23: not meaningful for CD-ROM or DVD-ROM */
    /* 24-31: disc bar code */
    /* 32: disc application code */
    /* 33: number of OPC tables */

    return 34;
}

static int scsi_disk_em_read_dvd_structure(SCSIDiskEmReq *req, uint8_t *cdb,
                                           uint8_t *outbuf)
{
    static const int rds_caps_size[5] = {
        [0] = 2048 + 4,
        [1] = 4 + 4,
        [3] = 188 + 4,
        [4] = 2048 + 4,
    };
    SCSIDiskEm *s = req->em;
    uint8_t media = cdb[1];
    uint8_t layer = cdb[6];
    uint8_t format = cdb[7];
    int size = -1;

    if (s->scsi_type != TYPE_ROM) {
        return -1;
    }
    if (media != 0) {
        req->sense = &SENSE_CODE(INVALID_FIELD);
        return -1;
    }

    if (format != 0xff) {
        if (s->tray_open || !blk_is_inserted(s->conf->blk)) {
            req->sense = &SENSE_CODE(NO_MEDIUM);
            return -1;
        }
        if (media_is_cd(s)) {
            req->sense = &SENSE_CODE(INCOMPATIBLE_FORMAT);
            return -1;
        }
        if (format >= ARRAY_SIZE(rds_caps_size)) {
            return -1;
        }
        size = rds_caps_size[format];
        memset(outbuf, 0, size);
    }

    switch (format) {
    case 0x00: {
        /* Physical format information */
        uint64_t nb_sectors;
        if (layer != 0) {
            goto fail;
        }
        blk_get_geometry(s->conf->blk, &nb_sectors);

        outbuf[4] = 1;   /* DVD-ROM, part version 1 */
        outbuf[5] = 0xf; /* 120mm disc, minimum rate unspecified */
        outbuf[6] = 1;   /* one layer, read-only (per MMC-2 spec) */
        outbuf[7] = 0;   /* default densities */

        stl_be_p(&outbuf[12], (nb_sectors >> 2) - 1); /* end sector */
        stl_be_p(&outbuf[16], (nb_sectors >> 2) - 1); /* l0 end sector */
        break;
    }

    case 0x01: /* DVD copyright information, all zeros */
        break;

    case 0x03: /* BCA information - invalid field for no BCA info */
        return -1;

    case 0x04: /* DVD disc manufacturing information, all zeros */
        break;

    case 0xff: { /* List capabilities */
        int i;
        size = 4;
        for (i = 0; i < ARRAY_SIZE(rds_caps_size); i++) {
            if (!rds_caps_size[i]) {
                continue;
            }
            outbuf[size] = i;
            outbuf[size + 1] = 0x40; /* Not writable, readable */
            stw_be_p(&outbuf[size + 2], rds_caps_size[i]);
            size += 4;
        }
        break;
     }

    default:
        return -1;
    }

    /* Size of buffer, not including 2 byte size field */
    stw_be_p(outbuf, size - 2);
    return size;

fail:
    return -1;
}

typedef struct {
    BlockCompletionFunc *cb;
    void *opaque;
} SCSIDiskAioCBData;

static void scsi_disk_em_aio_complete(void *opaque, int ret)
{
    SCSIDiskAioCBData *data = opaque;

    data->cb(data->opaque, ret);
    g_free(data);
}

int scsi_disk_em_start_req(SCSIDiskEmReq *req, SCSIDiskEm *s, uint8_t *cdb,
                           uint8_t *outbuf, int buflen, int cmd_xfer,
                           BlockCompletionFunc *cb, void *opaque)
{
    uint64_t nb_sectors;
    int ret;
    SCSIDiskAioCBData *data;

    switch (cdb[0]) {
    case INQUIRY:
    case MODE_SENSE:
    case MODE_SENSE_10:
    case RESERVE:
    case RESERVE_10:
    case RELEASE:
    case RELEASE_10:
    case START_STOP:
    case ALLOW_MEDIUM_REMOVAL:
    case GET_CONFIGURATION:
    case GET_EVENT_STATUS_NOTIFICATION:
    case MECHANISM_STATUS:
    case REQUEST_SENSE:
        break;

    default:
        if (s->tray_open || !blk_is_inserted(s->conf->blk)) {
            req->sense = &SENSE_CODE(NO_MEDIUM);
            return 0;
        }
        break;
    }

    switch (cdb[0]) {
    case TEST_UNIT_READY:
        assert(!s->tray_open && blk_is_inserted(s->conf->blk));
        break;
    case INQUIRY:
        ret = scsi_disk_em_inquiry(req, cdb, outbuf, cmd_xfer);
        if (ret < 0) {
            goto error;
        }
        break;
    case MODE_SENSE:
    case MODE_SENSE_10:
        ret = scsi_disk_em_mode_sense(req, cdb, outbuf, cmd_xfer);
        if (ret < 0) {
            goto error;
        }
        break;
    case READ_TOC:
        ret = scsi_disk_em_read_toc(s, cdb, outbuf);
        if (ret < 0) {
            goto error;
        }
        break;
    case RESERVE:
        if (cdb[1] & 1) {
            goto error;
        }
        break;
    case RESERVE_10:
        if (cdb[1] & 3) {
            goto error;
        }
        break;
    case RELEASE:
        if (cdb[1] & 1) {
            goto error;
        }
        break;
    case RELEASE_10:
        if (cdb[1] & 3) {
            goto error;
        }
        break;
    case START_STOP:
        if (scsi_disk_em_start_stop(req, cdb) < 0) {
            assert(req->sense);
            return 0;
        }
        break;
    case ALLOW_MEDIUM_REMOVAL:
        s->tray_locked = cdb[4] & 1;
        blk_lock_medium(s->conf->blk, cdb[4] & 1);
        break;
    case READ_CAPACITY_10:
        /* The normal LEN field for this command is zero.  */
        memset(outbuf, 0, 8);
        blk_get_geometry(s->conf->blk, &nb_sectors);
        if (!nb_sectors) {
            req->sense = &SENSE_CODE(LUN_NOT_READY);
            return 0;
        }
        if ((cdb[8] & 1) == 0 && scsi_cmd_lba(cdb)) {
            goto error;
        }
        nb_sectors /= s->blocksize / 512;
        /* Returned value is the address of the last sector.  */
        nb_sectors--;
        /* Remember the new size for read/write sanity checking. */
        *s->max_lba = nb_sectors;
        /* Clip to 2TB, instead of returning capacity modulo 2TB. */
        if (nb_sectors > UINT32_MAX) {
            nb_sectors = UINT32_MAX;
        }
        outbuf[0] = (nb_sectors >> 24) & 0xff;
        outbuf[1] = (nb_sectors >> 16) & 0xff;
        outbuf[2] = (nb_sectors >> 8) & 0xff;
        outbuf[3] = nb_sectors & 0xff;
        outbuf[4] = 0;
        outbuf[5] = 0;
        outbuf[6] = s->blocksize >> 8;
        outbuf[7] = 0;
        break;
    case REQUEST_SENSE:
        /* Just return "NO SENSE".  */
        ret = scsi_build_sense(NULL, 0, outbuf, buflen,
                                  (cdb[1] & 1) == 0);
        if (ret < 0) {
            goto error;
        }
        break;
    case MECHANISM_STATUS:
        ret = scsi_disk_em_mechanism_status(s, outbuf);
        if (ret < 0) {
            goto error;
        }
        break;
    case GET_CONFIGURATION:
        ret = scsi_disk_em_get_configuration(s, outbuf);
        if (ret < 0) {
            goto error;
        }
        break;
    case GET_EVENT_STATUS_NOTIFICATION:
        ret = scsi_disk_em_get_event_status_notification(s, cdb, outbuf);
        if (ret < 0) {
            goto error;
        }
        break;
    case READ_DISC_INFORMATION:
        ret = scsi_disk_em_read_disc_information(req, cdb, outbuf);
        if (ret < 0) {
            if (req->sense) {
                return 0;
            }
            goto error;
        }
        break;
    case READ_DVD_STRUCTURE:
        ret = scsi_disk_em_read_dvd_structure(req, cdb, outbuf);
        if (ret < 0) {
            goto error;
        }
        break;
    case SERVICE_ACTION_IN_16:
        /* Service Action In subcommands. */
        if ((cdb[1] & 31) == SAI_READ_CAPACITY_16) {
            DPRINTF("SAI READ CAPACITY(16)\n");
            memset(outbuf, 0, cmd_xfer);
            blk_get_geometry(s->conf->blk, &nb_sectors);
            if (!nb_sectors) {
                req->sense = &SENSE_CODE(LUN_NOT_READY);
                return 0;
            }
            if ((cdb[14] & 1) == 0 && scsi_cmd_lba(cdb)) {
                goto error;
            }
            nb_sectors /= s->blocksize / 512;
            /* Returned value is the address of the last sector.  */
            nb_sectors--;
            /* Remember the new size for read/write sanity checking. */
            *s->max_lba = nb_sectors;
            outbuf[0] = (nb_sectors >> 56) & 0xff;
            outbuf[1] = (nb_sectors >> 48) & 0xff;
            outbuf[2] = (nb_sectors >> 40) & 0xff;
            outbuf[3] = (nb_sectors >> 32) & 0xff;
            outbuf[4] = (nb_sectors >> 24) & 0xff;
            outbuf[5] = (nb_sectors >> 16) & 0xff;
            outbuf[6] = (nb_sectors >> 8) & 0xff;
            outbuf[7] = nb_sectors & 0xff;
            outbuf[8] = 0;
            outbuf[9] = 0;
            outbuf[10] = s->blocksize >> 8;
            outbuf[11] = 0;
            outbuf[12] = 0;
            outbuf[13] = get_physical_block_exp(s->conf);

            /* set TPE bit if the format supports discard */
            if (s->conf->discard_granularity) {
                outbuf[14] = 0x80;
            }

            /* Protection, exponent and lowest lba field left blank. */
            break;
        }
        DPRINTF("Unsupported Service Action In\n");
        goto error;
    case SYNCHRONIZE_CACHE:
        /* The request is used as the AIO opaque value, so add a ref.  */
        /* XXX: caller hold ref to r->req */
        block_acct_start(blk_get_stats(s->conf->blk), &req->acct, 0,
                         BLOCK_ACCT_FLUSH);
        data = g_new(SCSIDiskAioCBData, 1);
        data->cb = cb;
        data->opaque = opaque;
        blk_aio_flush(s->conf->blk, scsi_disk_em_aio_complete, data);
        return 0;
    case SEEK_10:
        DPRINTF("Seek(10) (sector %" PRId64 ")\n", scsi_cmd_lba(cdb));
        if (scsi_cmd_lba(cdb) > *s->max_lba) {
            goto illegal_lba;
        }
        break;
    case MODE_SELECT:
        DPRINTF("Mode Select(6) (len %lu)\n", (unsigned long)cmd_xfer);
        break;
    case MODE_SELECT_10:
        DPRINTF("Mode Select(10) (len %lu)\n", (unsigned long)cmd_xfer);
        break;
    case UNMAP:
        DPRINTF("Unmap (len %lu)\n", (unsigned long)cmd_xfer);
        break;
    case VERIFY_10:
    case VERIFY_12:
    case VERIFY_16:
        DPRINTF("Verify (bytchk %d)\n", (cdb[1] >> 1) & 3);
        if (cdb[1] & 6) {
            goto error;
        }
        break;
    case WRITE_SAME_10:
    case WRITE_SAME_16:
        DPRINTF("WRITE SAME %d (len %lu)\n",
                cdb[0] == WRITE_SAME_10 ? 10 : 16,
                (unsigned long)cmd_xfer);
        break;
    default:
        DPRINTF("Unknown SCSI command (%2.2x=%s)\n", cdb[0],
                scsi_command_name(cdb[0]));
        req->sense = &SENSE_CODE(INVALID_OPCODE);
        return 0;
    }
error:
    req->sense = req->sense ? : &SENSE_CODE(INVALID_FIELD);
    return 0;

illegal_lba:
    req->sense = &SENSE_CODE(LBA_OUT_OF_RANGE);
    return 0;
}
