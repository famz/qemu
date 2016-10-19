/*
 *  A TCMU userspace handler for QEMU block drivers.
 *
 *  Copyright (C) 2016 Red Hat. Inc.
 *
 *  Authors:
 *      Fam Zheng <famz@redhat.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "libtcmu.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "sysemu/block-backend.h"
#include "block/aio.h"
#include "block/scsi.h"
#include "scsi/tcmu.h"
#include "block/tcmu.h"
#include "qemu/main-loop.h"
#include "qmp-commands.h"

typedef struct TCMUExport TCMUExport;

struct TCMUExport {
    BlockBackend *blk;
    struct tcmu_device *tcmu_dev;
    bool writable;
    QLIST_ENTRY(TCMUExport) next;
};

typedef struct {
    struct tcmulib_context *tcmulib_ctx;
} TCMUHandlerState;

static QLIST_HEAD(, TCMUExport) tcmu_exports =
    QLIST_HEAD_INITIALIZER(tcmu_exports);

static TCMUHandlerState *handler_state;

static int qemu_tcmu_handle_cmd(TCMUExport *exp, uint8_t *cdb,
                                struct iovec *iovec, size_t iov_cnt,
                                uint8_t *sense)
{

    uint8_t cmd;

    cmd = cdb[0];
    printf("tcmu handle cmd: 0x%x\n", cdb[0]);
    switch (cmd) {
        case INQUIRY:
            return tcmu_emulate_inquiry(exp->tcmu_dev, cdb,
                                        iovec, iov_cnt, sense);
        case TEST_UNIT_READY:
            return tcmu_emulate_test_unit_ready(cdb, iovec, iov_cnt, sense);
        case SERVICE_ACTION_IN_16:
            if (cdb[1] == SAI_READ_CAPACITY_16) {
                return tcmu_emulate_read_capacity_16(1 << 20,
                                                     512,
                                                     cdb, iovec,
                                                     iov_cnt, sense);
            } else {
                return TCMU_NOT_HANDLED;
            }
        case MODE_SENSE:
        case MODE_SENSE_10:
            return tcmu_emulate_mode_sense(cdb, iovec, iov_cnt, sense);
        case MODE_SELECT:
        case MODE_SELECT_10:
            return tcmu_emulate_mode_select(cdb, iovec, iov_cnt, sense);
        case READ_6:
        case READ_10:
        case READ_12:
        case READ_16:
            return GOOD;

        case WRITE_6:
        case WRITE_10:
        case WRITE_12:
        case WRITE_16:
            return GOOD;

        default:
            printf("unknown command %x\n", cdb[0]);
            return TCMU_NOT_HANDLED;
    }
}


static void qemu_tcmu_dev_event_handler(void *opaque)
{
    TCMUExport *exp = opaque;
    struct tcmulib_cmd *cmd;
    struct tcmu_device *dev = exp->tcmu_dev;

    tcmulib_processing_start(dev);

    while ((cmd = tcmulib_get_next_command(dev)) != NULL) {
        int ret = qemu_tcmu_handle_cmd(exp, cmd->cdb, cmd->iovec, cmd->iov_cnt,
                                       cmd->sense_buf);
        tcmulib_command_complete(dev, cmd, ret);
    }

    tcmulib_processing_complete(dev);
}

static TCMUExport *qemu_tcmu_lookup(const BlockBackend *blk)
{
    TCMUExport *exp;

    QLIST_FOREACH(exp, &tcmu_exports, next) {
        if (exp->blk == blk) {
            return exp;
        }
    }
    return NULL;
}
static TCMUExport *qemu_tcmu_parse_cfgstr(const char *cfgstr,
                                          Error **errp);

static bool qemu_tcmu_check_config(const char *cfgstr, char **reason)
{
    Error *local_err = NULL;

    qemu_tcmu_parse_cfgstr(cfgstr, &local_err);
    if (local_err) {
        *reason = strdup(error_get_pretty(local_err));
        error_free(local_err);
        return false;
    }
    return true;
}

static int qemu_tcmu_added(struct tcmu_device *dev)
{
    TCMUExport *exp;
    const char *cfgstr = tcmu_get_dev_cfgstring(dev);
    Error *local_err = NULL;

    exp = qemu_tcmu_parse_cfgstr(cfgstr, &local_err);
    if (local_err) {
        return -1;
    }
    exp->tcmu_dev = dev;
    aio_set_fd_handler(blk_get_aio_context(exp->blk),
                       tcmu_get_dev_fd(dev),
                       true, qemu_tcmu_dev_event_handler, NULL, exp);
    return 0;
}

static void qemu_tcmu_removed(struct tcmu_device *dev)
{
    /* TODO. */
}

static void qemu_tcmu_master_read(void *opaque)
{
    TCMUHandlerState *s = opaque;
    printf("tcmu master read\n");
    tcmulib_master_fd_ready(s->tcmulib_ctx);
}

static struct tcmulib_handler qemu_tcmu_handler = {
    .name = "Handler for QEMU block devices",
    .subtype = NULL, /* Dynamically generated when starting. */
    .cfg_desc = "Format: device=<name>",
    .added = qemu_tcmu_added,
    .removed = qemu_tcmu_removed,
    .check_config = qemu_tcmu_check_config,
};

static TCMUExport *qemu_tcmu_parse_cfgstr(const char *cfgstr,
                                          Error **errp)
{
    BlockBackend *blk;
    const char *dev_str, *device;
    const char *subtype = qemu_tcmu_handler.subtype;
    size_t subtype_len;
    TCMUExport *exp;

    if (!subtype) {
        error_setg(errp, "TCMU Handler not started");
    }
    subtype_len = strlen(subtype);
    if (strncmp(cfgstr, subtype, subtype_len) ||
        cfgstr[subtype_len] != '/') {
        error_report("TCMU: Invalid subtype in device cfgstring: %s", cfgstr);
        return NULL;
    }
    dev_str = &cfgstr[subtype_len + 1];
    if (dev_str[0] != '@') {
        error_report("TCMU: Invalid cfgstring format. Must be @<device_name>");
        return NULL;
    }
    device = &dev_str[1];

    blk = blk_by_name(device);
    if (!blk) {
        error_setg(errp, "TCMU: Device not found: %s", device);
        return NULL;
    }
    exp = qemu_tcmu_lookup(blk);
    if (!exp) {
        error_setg(errp, "TCMU: Device not found: %s", device);
        return NULL;
    }
    return exp;
}

static void qemu_tcmu_errp(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    error_vprintf(fmt, ap);
    va_end(ap);
}

void qemu_tcmu_start(const char *subtype, Error **errp)
{
    int fd;

    printf("tcmu start\n");
    if (handler_state) {
        error_setg(errp, "TCMU handler already started");
        return;
    }
    assert(!qemu_tcmu_handler.subtype);
    qemu_tcmu_handler.subtype = g_strdup(subtype);
    handler_state = g_new0(TCMUHandlerState, 1);
    handler_state->tcmulib_ctx = tcmulib_initialize(&qemu_tcmu_handler, 1,
                                                    qemu_tcmu_errp);
    if (!handler_state->tcmulib_ctx) {
        error_setg(errp, "Failed to initialize tcmulib");
        goto fail;
    }
    fd = tcmulib_get_master_fd(handler_state->tcmulib_ctx);
    qemu_set_fd_handler(fd, qemu_tcmu_master_read, NULL, handler_state);
    printf("register\n");
    tcmulib_register(handler_state->tcmulib_ctx);
    return;
fail:
    g_free(handler_state);
    handler_state = NULL;
}

TCMUExport *qemu_tcmu_export(BlockBackend *blk, bool writable, Error **errp)
{
    TCMUExport *exp;

    exp = qemu_tcmu_lookup(blk);
    if (exp) {
        error_setg(errp, "Block device already added");
        return NULL;
    }
    exp = g_new0(TCMUExport, 1);
    exp->blk = blk;
    blk_ref(blk);
    exp->writable = writable;
    QLIST_INSERT_HEAD(&tcmu_exports, exp, next);
    return exp;
}

__attribute__((constructor))
static void qemu_tcmu_init(void)
{
    static TCMUHandler handler = (TCMUHandler) {
        .start = qemu_tcmu_start,
        .add   = qemu_tcmu_export,
    };
    qemu_tcmu_handler_register(&handler);
}

