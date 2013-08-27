/*
 * QEMU USB OHCI Emulation
 * Copyright (c) 2004 Gianni Tedesco
 * Copyright (c) 2006 CodeSourcery
 * Copyright (c) 2006 Openedhand Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * TODO:
 *  o Isochronous transfers
 *  o Allocate bandwidth in frames properly
 *  o Disable timers when nothing needs to be done, or remove timer usage
 *    all together.
 *  o BIOS work to boot from USB storage
*/

#include "hw/sysbus.h"
#include "hw/usb/hcd-ohci.h"

#define TYPE_SYSBUS_OHCI "sysbus-ohci"
#define SYSBUS_OHCI(obj) OBJECT_CHECK(OHCISysBusState, (obj), TYPE_SYSBUS_OHCI)

typedef struct {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    OHCIState ohci;
    uint32_t num_ports;
    dma_addr_t dma_offset;
} OHCISysBusState;

static Property ohci_sysbus_properties[] = {
    DEFINE_PROP_UINT32("num-ports", OHCISysBusState, num_ports, 3),
    DEFINE_PROP_DMAADDR("dma-offset", OHCISysBusState, dma_offset, 3),
    DEFINE_PROP_END_OF_LIST(),
};

static void ohci_realize_pxa(DeviceState *dev, Error **errp)
{
    OHCISysBusState *s = SYSBUS_OHCI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /* Cannot fail as we pass NULL for masterbus */
    usb_ohci_init(&s->ohci, dev, s->num_ports, s->dma_offset, NULL, 0,
                  &address_space_memory);
    sysbus_init_irq(sbd, &s->ohci.irq);
    sysbus_init_mmio(sbd, &s->ohci.mem);
}

static void ohci_sysbus_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ohci_realize_pxa;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    dc->desc = "OHCI USB Controller";
    dc->props = ohci_sysbus_properties;
}

static const TypeInfo ohci_sysbus_info = {
    .name          = TYPE_SYSBUS_OHCI,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OHCISysBusState),
    .class_init    = ohci_sysbus_class_init,
};

static void ohci_sysbus_register_type(void)
{
    type_register_static(&ohci_sysbus_info);
}

type_init(ohci_sysbus_register_type)
