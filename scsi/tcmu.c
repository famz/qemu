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
#include "block/tcmu.h"
#include "qemu/main-loop.h"
#include "qmp-commands.h"
#include "qemu/module.h"

typedef struct {
    BlockBackend *blk;
    struct tcmu_device *tcmu_dev;
} TCMUExport;

static QTAILQ_HEAD(, TCMUExport) exports = QTAILQ_HEAD_INITIALIZER(exports);

typedef struct {
    struct tcmulib_context *tcmulib_ctx;
} TCMUHandlerState;

static TCMUHandlerState *handler_state;

struct foo_state {
    int fd;
    uint64_t num_lbas;
    uint32_t block_size;
};

static int qemu_tcmu_handle_cmd(TCMUExport *exp, uint8_t *cdb,
                                struct iovec *iovec, size_t iov_cnt,
                                uint8_t *sense)
{
    return -EIO;
}


static void qemu_tcmu_dev_event_handler(void *opaque)
{
    TCMUExport *exp = opaque;
    struct tcmulib_cmd *cmd;
    struct tcmu_device *dev = exp->tcmu_dev;

    printf("event handler\n");
    tcmulib_processing_start(dev);

    while ((cmd = tcmulib_get_next_command(dev)) != NULL) {
        int ret = qemu_tcmu_handle_cmd(exp, cmd->cdb, cmd->iovec, cmd->iov_cnt,
                                       cmd->sense_buf);
        tcmulib_command_complete(dev, cmd, ret);
    }

    tcmulib_processing_complete(dev);
}

static int qemu_tcmu_added(struct tcmu_device *dev)
{
    BlockBackend *blk;
    TCMUExport *exp;
    const char *cfgstr = tcmu_get_dev_cfgstring(dev);
    struct tcmulib_handler *handler = tcmu_get_dev_handler(dev);
    const char *subtype = handler->subtype;
    size_t subtype_len = strlen(subtype);
    const char *dev_name;

    if (strncmp(cfgstr, handler->subtype, subtype_len) ||
        cfgstr[subtype_len] != '/') {
        error_report("TCMU: Invalid subtype in device cfgstring: %s", cfgstr);
        return -1;
    }
    dev_name = &cfgstr[subtype_len + 1];
    blk = blk_by_name(dev_name);
    if (!blk) {
        error_report("TCMU: device not found: %s", dev_name);
        return -1;
    }

    exp = g_new0(TCMUExport, 1);
    blk_ref(blk);
    exp->blk = blk;
    exp->tcmu_dev = dev;
    aio_set_fd_handler(blk_get_aio_context(blk),
                       tcmu_get_dev_fd(dev),
                       true, qemu_tcmu_dev_event_handler, NULL, exp);
    /* TODO: QMP message? */
    return 0;
}

static void qemu_tcmu_removed(struct tcmu_device *dev)
{
    /* not supported in this example */
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
    .cfg_desc = "The export name (device name)",
    .added = qemu_tcmu_added,
    .removed = qemu_tcmu_removed,
};

static void qemu_tcmu_errp(const char *fmt, ...)
{
    printf("fmt\n");
}

static void qemu_tcmu_start(const char *subtype, Error **errp)
{
    TCMUHandlerState *s;
    int fd;

    if (handler_state) {
        error_setg(errp, "TCMU handler already started");
        return;
    }
    if (!qemu_tcmu_handler.subtype) {
        qemu_tcmu_handler.subtype = g_strdup(subtype);
    }
    s = g_new0(TCMUHandlerState, 1);
    s->tcmulib_ctx = tcmulib_initialize(&qemu_tcmu_handler, 1,
                                        qemu_tcmu_errp);
    if (!s->tcmulib_ctx) {
        error_setg(errp, "Failed to initialize tcmulib");
        goto fail;
    }
    fd = tcmulib_get_master_fd(s->tcmulib_ctx);
    qemu_set_fd_handler(fd, qemu_tcmu_master_read, NULL, s);
    tcmulib_register(s->tcmulib_ctx);
    handler_state = s;
    return;
fail:
    g_free(s);
}

static void qemu_tcmu_add(BlockBackend *blk, bool writable, Error **errp)
{
}

static void qemu_tcmu_init(void)
{
    static TCMUHandler handler = (TCMUHandler) {
        .start = qemu_tcmu_start,
        .add   = qemu_tcmu_add,
    };
    qemu_tcmu_handler_register(&handler);
}

block_init(qemu_tcmu_init);
