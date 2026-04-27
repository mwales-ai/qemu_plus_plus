/*
 * QEMU PCI VGA Emulator.
 *
 * see docs/specs/standard-vga.rst for virtual hardware specs.
 *
 * Copyright (c) 2003 Fabrice Bellard
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
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "vga_int.h"
#include "ui/pixel_ops.h"
#include "ui/console.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/loader.h"
#include "hw/display/edid.h"
#include "qom/object.h"
#include "hw/acpi/acpi_aml_interface.h"

enum vga_pci_flags {
    PCI_VGA_FLAG_ENABLE_MMIO = 1,
    PCI_VGA_FLAG_ENABLE_QEXT = 2,
    PCI_VGA_FLAG_ENABLE_EDID = 3,
};

struct PCIVGAState {
    PCIDevice dev;
    VGACommonState vga;
    uint32_t flags;
    qemu_edid_info edid_info;
    MemoryRegion mmio;
    MemoryRegion mrs[4];
    uint8_t edid[384];

    /* methods */
    static uint64_t ioportRead(void *ptr, hwaddr addr, unsigned size);
    static void ioportWrite(void *ptr, hwaddr addr, uint64_t val,
                            unsigned size);
    static uint64_t bochsRead(void *ptr, hwaddr addr, unsigned size);
    static void bochsWrite(void *ptr, hwaddr addr, uint64_t val,
                           unsigned size);
    static uint64_t qextRead(void *ptr, hwaddr addr, unsigned size);
    static void qextWrite(void *ptr, hwaddr addr, uint64_t val,
                          unsigned size);
    static bool getBigEndianFb(Object *obj, Error **errp);
    static void setBigEndianFb(Object *obj, bool value, Error **errp);
    void stdRealize(Error **errp);
    void secondaryRealize(Error **errp);
    void secondaryExit();
    void secondaryInit();
    void secondaryReset();
    static void classInit(DeviceClass *dc);
    static void vgaClassInit(ObjectClass *klass, const void *data);
    static void secondaryClassInit(ObjectClass *klass, const void *data);
};

#define TYPE_PCI_VGA "pci-vga"
OBJECT_DECLARE_SIMPLE_TYPE(PCIVGAState, PCI_VGA)

static inline PCIVGAState *pci_vga_from_obj(void *obj)
{
    return reinterpret_cast<PCIVGAState *>(PCI_VGA(obj));
}

static const VMStateDescription vmstate_vga_pci = {
    .name = "vga",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, PCIVGAState),
        VMSTATE_STRUCT(vga, PCIVGAState, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_END_OF_LIST()
    }
};

uint64_t PCIVGAState::ioportRead(void *ptr, hwaddr addr,
                                  unsigned size)
{
    VGACommonState *s = static_cast<VGACommonState *>(ptr);
    uint64_t ret = 0;

    switch (size) {
    case 1:
        ret = vga_ioport_read(s, addr + 0x3c0);
        break;
    case 2:
        ret  = vga_ioport_read(s, addr + 0x3c0);
        ret |= vga_ioport_read(s, addr + 0x3c1) << 8;
        break;
    }
    return ret;
}

void PCIVGAState::ioportWrite(void *ptr, hwaddr addr,
                               uint64_t val, unsigned size)
{
    VGACommonState *s = static_cast<VGACommonState *>(ptr);

    switch (size) {
    case 1:
        vga_ioport_write(s, addr + 0x3c0, val);
        break;
    case 2:
        /*
         * Update bytes in little endian order.  Allows to update
         * indexed registers with a single word write because the
         * index byte is updated first.
         */
        vga_ioport_write(s, addr + 0x3c0, val & 0xff);
        vga_ioport_write(s, addr + 0x3c1, (val >> 8) & 0xff);
        break;
    }
}

static const MemoryRegionOps pci_vga_ioport_ops = {
    .read = PCIVGAState::ioportRead,
    .write = PCIVGAState::ioportWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, },
    .impl = { .min_access_size = 1, .max_access_size = 2, },
};

uint64_t PCIVGAState::bochsRead(void *ptr, hwaddr addr,
                                 unsigned size)
{
    VGACommonState *s = static_cast<VGACommonState *>(ptr);
    int index = addr >> 1;

    vbe_ioport_write_index(s, 0, index);
    return vbe_ioport_read_data(s, 0);
}

void PCIVGAState::bochsWrite(void *ptr, hwaddr addr,
                              uint64_t val, unsigned size)
{
    VGACommonState *s = static_cast<VGACommonState *>(ptr);
    int index = addr >> 1;

    vbe_ioport_write_index(s, 0, index);
    vbe_ioport_write_data(s, 0, val);
}

static const MemoryRegionOps pci_vga_bochs_ops = {
    .read = PCIVGAState::bochsRead,
    .write = PCIVGAState::bochsWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, },
    .impl = { .min_access_size = 2, .max_access_size = 2, },
};

uint64_t PCIVGAState::qextRead(void *ptr, hwaddr addr, unsigned size)
{
    VGACommonState *s = static_cast<VGACommonState *>(ptr);

    switch (addr) {
    case PCI_VGA_QEXT_REG_SIZE:
        return PCI_VGA_QEXT_SIZE;
    case PCI_VGA_QEXT_REG_BYTEORDER:
        return s->big_endian_fb ?
            PCI_VGA_QEXT_BIG_ENDIAN : PCI_VGA_QEXT_LITTLE_ENDIAN;
    default:
        return 0;
    }
}

void PCIVGAState::qextWrite(void *ptr, hwaddr addr,
                             uint64_t val, unsigned size)
{
    VGACommonState *s = static_cast<VGACommonState *>(ptr);

    switch (addr) {
    case PCI_VGA_QEXT_REG_BYTEORDER:
        if (val == PCI_VGA_QEXT_BIG_ENDIAN) {
            s->big_endian_fb = true;
        }
        if (val == PCI_VGA_QEXT_LITTLE_ENDIAN) {
            s->big_endian_fb = false;
        }
        break;
    }
}

bool PCIVGAState::getBigEndianFb(Object *obj, Error **errp)
{
    PCIVGAState *d = pci_vga_from_obj(PCI_DEVICE(obj));

    return d->vga.big_endian_fb;
}

void PCIVGAState::setBigEndianFb(Object *obj, bool value, Error **errp)
{
    PCIVGAState *d = pci_vga_from_obj(PCI_DEVICE(obj));

    d->vga.big_endian_fb = value;
}

static const MemoryRegionOps pci_vga_qext_ops = {
    .read = PCIVGAState::qextRead,
    .write = PCIVGAState::qextWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

void pci_std_vga_mmio_region_init(VGACommonState *s,
                                  Object *owner,
                                  MemoryRegion *parent,
                                  MemoryRegion *subs,
                                  bool qext, bool edid)
{
    PCIVGAState *d = container_of(s, PCIVGAState, vga);

    memory_region_init_io(&subs[0], owner, &pci_vga_ioport_ops, s,
                          "vga ioports remapped", PCI_VGA_IOPORT_SIZE);
    memory_region_add_subregion(parent, PCI_VGA_IOPORT_OFFSET,
                                &subs[0]);

    memory_region_init_io(&subs[1], owner, &pci_vga_bochs_ops, s,
                          "bochs dispi interface", PCI_VGA_BOCHS_SIZE);
    memory_region_add_subregion(parent, PCI_VGA_BOCHS_OFFSET,
                                &subs[1]);

    if (qext) {
        memory_region_init_io(&subs[2], owner, &pci_vga_qext_ops, s,
                              "qemu extended regs", PCI_VGA_QEXT_SIZE);
        memory_region_add_subregion(parent, PCI_VGA_QEXT_OFFSET,
                                    &subs[2]);
    }

    if (edid) {
        qemu_edid_generate(d->edid, sizeof(d->edid), &d->edid_info);
        qemu_edid_region_io(&subs[3], owner, d->edid, sizeof(d->edid));
        memory_region_add_subregion(parent, 0, &subs[3]);
    }
}

static void pci_std_vga_realize(PCIDevice *dev, Error **errp)
{
    PCIVGAState *d = pci_vga_from_obj(dev);
    d->stdRealize(errp);
}

void PCIVGAState::stdRealize(Error **errp)
{
    VGACommonState *s = &this->vga;
    bool qext = false;
    bool edid = false;

    /* vga + console init */
    if (!vga_common_init(s, OBJECT(this), errp)) {
        return;
    }
    vga_init(s, OBJECT(this), pci_address_space(PCI_DEVICE(DEVICE(this))),
             pci_address_space_io(PCI_DEVICE(DEVICE(this))),
             true);

    s->con = graphic_console_init(DEVICE(this), 0, s->hw_ops, s);

    /* XXX: VGA_RAM_SIZE must be a power of two */
    pci_register_bar(&this->dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);

    /* mmio bar for vga register access */
    if (this->flags & (1 << PCI_VGA_FLAG_ENABLE_MMIO)) {
        memory_region_init_io(&this->mmio, OBJECT(this), &unassigned_io_ops, NULL,
                              "vga.mmio", PCI_VGA_MMIO_SIZE);

        if (this->flags & (1 << PCI_VGA_FLAG_ENABLE_QEXT)) {
            qext = true;
            pci_set_byte(&this->dev.config[PCI_REVISION_ID], 2);
        }
        if (this->flags & (1 << PCI_VGA_FLAG_ENABLE_EDID)) {
            edid = true;
        }
        pci_std_vga_mmio_region_init(s, OBJECT(this), &this->mmio, this->mrs,
                                     qext, edid);

        pci_register_bar(&this->dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &this->mmio);
    }
}

static void pci_secondary_vga_realize(PCIDevice *dev, Error **errp)
{
    PCIVGAState *d = pci_vga_from_obj(dev);
    d->secondaryRealize(errp);
}

void PCIVGAState::secondaryRealize(Error **errp)
{
    VGACommonState *s = &this->vga;
    bool qext = false;
    bool edid = false;

    /* vga + console init */
    if (!vga_common_init(s, OBJECT(this), errp)) {
        return;
    }
    s->con = graphic_console_init(DEVICE(this), 0, s->hw_ops, s);

    /* mmio bar */
    memory_region_init_io(&this->mmio, OBJECT(this), &unassigned_io_ops, NULL,
                          "vga.mmio", PCI_VGA_MMIO_SIZE);

    if (this->flags & (1 << PCI_VGA_FLAG_ENABLE_QEXT)) {
        qext = true;
        pci_set_byte(&this->dev.config[PCI_REVISION_ID], 2);
    }
    if (this->flags & (1 << PCI_VGA_FLAG_ENABLE_EDID)) {
        edid = true;
    }
    pci_std_vga_mmio_region_init(s, OBJECT(this), &this->mmio, this->mrs, qext, edid);

    pci_register_bar(&this->dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vram);
    pci_register_bar(&this->dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &this->mmio);
}

static void pci_secondary_vga_exit(PCIDevice *dev)
{
    PCIVGAState *d = pci_vga_from_obj(dev);
    d->secondaryExit();
}

void PCIVGAState::secondaryExit()
{
    VGACommonState *s = &this->vga;

    graphic_console_close(s->con);
    memory_region_del_subregion(&this->mmio, &this->mrs[0]);
    memory_region_del_subregion(&this->mmio, &this->mrs[1]);
    if (this->flags & (1 << PCI_VGA_FLAG_ENABLE_QEXT)) {
        memory_region_del_subregion(&this->mmio, &this->mrs[2]);
    }
    if (this->flags & (1 << PCI_VGA_FLAG_ENABLE_EDID)) {
        memory_region_del_subregion(&this->mmio, &this->mrs[3]);
    }
}

static void pci_secondary_vga_init(Object *obj)
{
    PCIVGAState *d = pci_vga_from_obj(PCI_DEVICE(obj));
    d->secondaryInit();
}

void PCIVGAState::secondaryInit()
{
    /* Expose framebuffer byteorder via QOM */
    object_property_add_bool(OBJECT(this), "big-endian-framebuffer",
                             PCIVGAState::getBigEndianFb,
                             PCIVGAState::setBigEndianFb);
}

static void pci_secondary_vga_reset(DeviceState *dev)
{
    PCIVGAState *d = pci_vga_from_obj(PCI_DEVICE(dev));
    d->secondaryReset();
}

void PCIVGAState::secondaryReset()
{
    vga_common_reset(&this->vga);
}

static const Property vga_pci_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", PCIVGAState, vga.vram_size_mb, 16),
    DEFINE_PROP_BIT("mmio", PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_MMIO, true),
    DEFINE_PROP_BIT("qemu-extended-regs",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_QEXT, true),
    DEFINE_PROP_BIT("edid",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_EDID, true),
    DEFINE_EDID_PROPERTIES(PCIVGAState, edid_info),
    DEFINE_PROP_BOOL("global-vmstate", PCIVGAState, vga.global_vmstate, false),
};

static const Property secondary_pci_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", PCIVGAState, vga.vram_size_mb, 16),
    DEFINE_PROP_BIT("qemu-extended-regs",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_QEXT, true),
    DEFINE_PROP_BIT("edid",
                    PCIVGAState, flags, PCI_VGA_FLAG_ENABLE_EDID, true),
    DEFINE_EDID_PROPERTIES(PCIVGAState, edid_info),
};

void PCIVGAState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    AcpiDevAmlIfClass *adevc = ACPI_DEV_AML_IF_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = PCI_DEVICE_ID_QEMU_VGA;
    dc->vmsd = &vmstate_vga_pci;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    adevc->build_dev_aml = build_vga_aml;
}

static const InterfaceInfo vga_pci_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { TYPE_ACPI_DEV_AML_IF },
    { },
};

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS_IFACES(PCIVGAState, TYPE_PCI_VGA,
                                            TYPE_PCI_DEVICE, vga_pci_interfaces)

void PCIVGAState::vgaClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pci_std_vga_realize;
    k->romfile = "vgabios-stdvga.bin";
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    device_class_set_props(dc, vga_pci_properties);
    dc->hotpluggable = false;

    /* Expose framebuffer byteorder via QOM */
    object_class_property_add_bool(klass, "big-endian-framebuffer",
                                   PCIVGAState::getBigEndianFb,
                                   PCIVGAState::setBigEndianFb);
}

void PCIVGAState::secondaryClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = pci_secondary_vga_realize;
    k->exit = pci_secondary_vga_exit;
    k->class_id = PCI_CLASS_DISPLAY_OTHER;
    device_class_set_props(dc, secondary_pci_properties);
    device_class_set_legacy_reset(dc, pci_secondary_vga_reset);
}

static const TypeInfo vga_info = {
    .name          = "VGA",
    .parent        = TYPE_PCI_VGA,
    .class_init    = PCIVGAState::vgaClassInit,
};

static const TypeInfo secondary_info = {
    .name          = "secondary-vga",
    .parent        = TYPE_PCI_VGA,
    .instance_init = pci_secondary_vga_init,
    .class_init    = PCIVGAState::secondaryClassInit,
};

static void __attribute__((constructor)) register_vga_concretes(void)
{
    type_register_static(&vga_info);
    type_register_static(&secondary_info);
}
