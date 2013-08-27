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

#include "hw/usb/hcd-ohci.h"

#define TYPE_PCI_OHCI "pci-ohci"
#define PCI_OHCI(obj) OBJECT_CHECK(OHCIPCIState, (obj), TYPE_PCI_OHCI)

typedef struct {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    OHCIState state;
    char *masterbus;
    uint32_t num_ports;
    uint32_t firstport;
} OHCIPCIState;

static Property ohci_pci_properties[] = {
    DEFINE_PROP_STRING("masterbus", OHCIPCIState, masterbus),
    DEFINE_PROP_UINT32("num-ports", OHCIPCIState, num_ports, 3),
    DEFINE_PROP_UINT32("firstport", OHCIPCIState, firstport, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static int usb_ohci_initfn_pci(PCIDevice *dev)
{
    OHCIPCIState *ohci = PCI_OHCI(dev);

    dev->config[PCI_CLASS_PROG] = 0x10; /* OHCI */
    dev->config[PCI_INTERRUPT_PIN] = 0x01; /* interrupt pin A */

    if (usb_ohci_init(&ohci->state, DEVICE(dev), ohci->num_ports, 0,
                      ohci->masterbus, ohci->firstport,
                      pci_get_address_space(dev)) != 0) {
        return -1;
    }
    ohci->state.irq = dev->irq[0];

    pci_register_bar(dev, 0, 0, &ohci->state.mem);
    return 0;
}

static void ohci_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = usb_ohci_initfn_pci;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = PCI_DEVICE_ID_APPLE_IPID_USB;
    k->class_id = PCI_CLASS_SERIAL_USB;
    k->no_hotplug = 1;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
    dc->desc = "Apple USB Controller";
    dc->props = ohci_pci_properties;
}

static const TypeInfo ohci_pci_info = {
    .name          = TYPE_PCI_OHCI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(OHCIPCIState),
    .class_init    = ohci_pci_class_init,
};

static void ohci_pci_register_type(void)
{
    type_register_static(&ohci_pci_info);
}

type_init(ohci_pci_register_type)
