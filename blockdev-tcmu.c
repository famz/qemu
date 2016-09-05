/*
 * Serving QEMU block devices via TCMU
 *
 * Copyright (c) 2016 Red Hat, Inc.
 *
 * Author: Fam Zheng <famz@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "sysemu/blockdev.h"
#include "sysemu/block-backend.h"
#include "hw/block/block.h"
#include "qapi/qmp/qerror.h"
#include "sysemu/sysemu.h"
#include "qmp-commands.h"
#include "block/tcmu.h"

static TCMUHandler *tcmu_handler;

void qemu_tcmu_handler_register(TCMUHandler *handler)
{
    assert(!tcmu_handler);
    tcmu_handler = handler;
}

void qmp_tcmu_start(const char *subtype, Error **errp)
{
    if (!tcmu_handler) {
        error_setg(errp, "TCMU driver module not found");
        return;
    }
    tcmu_handler->start(subtype, errp);
}

void qmp_tcmu_add(const char *device, bool has_writable, bool writable,
                  Error **errp)
{
    BlockBackend *blk;
    if (!tcmu_handler) {
        error_setg(errp, "TCMU driver module not found");
        return;
    }
    assert(tcmu_handler->add);
    blk = blk_by_name(device);
    if (!blk) {
        error_setg(errp, "Block device not found: %s", device);
        return;
    }

    if (!has_writable) {
        writable = false;
    }
    if (blk_is_read_only(blk)) {
        writable = false;
    }
    tcmu_handler->add(blk, writable, errp);
}
