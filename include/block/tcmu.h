#ifndef QEMU_BLOCK_TCMU_H
#define QEMU_BLOCK_TCMU_H

#include "qemu-common.h"
#include "scsi/tcmu.h"

typedef struct {
    void (*start)(const char *subtype, Error **errp);
    TCMUExport *(*add)(BlockBackend *blk, bool read, Error **errp);
} TCMUHandler;

void qemu_tcmu_handler_register(TCMUHandler *handler);

#endif
