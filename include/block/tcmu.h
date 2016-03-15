#ifndef QEMU_TCMU_H
#define QEMU_TCMU_H

#include "qemu-common.h"

typedef struct {
    void (*start)(const char *subtype, Error **errp);
    void (*add)(BlockBackend *blk, bool read, Error **errp);
} TCMUHandler;

void qemu_tcmu_handler_register(TCMUHandler *handler);

#endif
