/*
 * QEMU SCSI helpers
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

uint64_t scsi_cmd_lba(uint8_t *cdb)
{
    switch (cdb[0] >> 5) {
    case 0:
        return ldl_be_p(&cdb[0]) & 0x1fffff;
    case 1:
    case 2:
    case 5:
        return ldl_be_p(&cdb[2]) & 0xffffffffULL;
    case 4:
        return ldq_be_p(&cdb[2]);
    default:
        return -1;

    }
}

int scsi_cdb_length(uint8_t *buf) {
    int cdb_len;

    switch (buf[0] >> 5) {
    case 0:
        cdb_len = 6;
        break;
    case 1:
    case 2:
        cdb_len = 10;
        break;
    case 4:
        cdb_len = 16;
        break;
    case 5:
        cdb_len = 12;
        break;
    default:
        cdb_len = -1;
    }
    return cdb_len;
}

uint32_t scsi_data_cdb_xfer(uint8_t *buf)
{
    if ((buf[0] >> 5) == 0 && buf[4] == 0) {
        return 256;
    } else {
        return scsi_cdb_xfer(buf);
    }
}

uint32_t scsi_cdb_xfer(uint8_t *buf)
{
    switch (buf[0] >> 5) {
    case 0:
        return buf[4];
        break;
    case 1:
    case 2:
        return lduw_be_p(&buf[7]);
        break;
    case 4:
        return ldl_be_p(&buf[10]) & 0xffffffffULL;
        break;
    case 5:
        return ldl_be_p(&buf[6]) & 0xffffffffULL;
        break;
    default:
        return -1;
    }
}

int scsi_get_performance_length(int num_desc, int type, int data_type)
{
    /* MMC-6, paragraph 6.7.  */
    switch (type) {
    case 0:
        if ((data_type & 3) == 0) {
            /* Each descriptor is as in Table 295 - Nominal performance.  */
            return 16 * num_desc + 8;
        } else {
            /* Each descriptor is as in Table 296 - Exceptions.  */
            return 6 * num_desc + 8;
        }
    case 1:
    case 4:
    case 5:
        return 8 * num_desc + 8;
    case 2:
        return 2048 * num_desc + 8;
    case 3:
        return 16 * num_desc + 8;
    default:
        return 8;
    }
}

bool scsi_is_cmd_fua(uint8_t *cdb)
{
    switch (cdb[0]) {
    case READ_10:
    case READ_12:
    case READ_16:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
        return (cdb[1] & 8) != 0;

    case VERIFY_10:
    case VERIFY_12:
    case VERIFY_16:
    case WRITE_VERIFY_10:
    case WRITE_VERIFY_12:
    case WRITE_VERIFY_16:
        return true;

    case READ_6:
    case WRITE_6:
    default:
        return false;
    }
}
