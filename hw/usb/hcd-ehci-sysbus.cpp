/*
 * QEMU USB EHCI Emulation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb/hcd-ehci.h"
#include "migration/vmstate.h"

static const VMStateField vmstate_ehci_sysbus_fields[] = {
    VMSTATE_STRUCT(ehci, EHCISysBusState, 2, vmstate_ehci, EHCIState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_ehci_sysbus = {
    .name        = "ehci-sysbus",
    .version_id  = 2,
    .minimum_version_id  = 1,
    .fields = vmstate_ehci_sysbus_fields,
};

static const Property ehci_sysbus_properties[] = {
    DEFINE_PROP_UINT32("maxframes", EHCISysBusState, ehci.maxframes, 128),
    DEFINE_PROP_BOOL("companion-enable", EHCISysBusState, ehci.companion_enable,
                     false),
};

static void usb_ehci_sysbus_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *d = SYS_BUS_DEVICE(dev);
    EHCISysBusState *i = SYS_BUS_EHCI(dev);
    EHCIState *s = &i->ehci;

    usb_ehci_realize(s, dev, errp);
    sysbus_init_irq(d, &s->irq);
}

static void usb_ehci_sysbus_reset(DeviceState *dev)
{
    SysBusDevice *d = SYS_BUS_DEVICE(dev);
    EHCISysBusState *i = SYS_BUS_EHCI(d);
    EHCIState *s = &i->ehci;

    ehci_reset(s);
}

static void ehci_sysbus_init(Object *obj)
{
    SysBusDevice *d = SYS_BUS_DEVICE(obj);
    EHCISysBusState *i = SYS_BUS_EHCI(obj);
    SysBusEHCIClass *sec = SYS_BUS_EHCI_GET_CLASS(obj);
    EHCIState *s = &i->ehci;

    s->capsbase = sec->capsbase;
    s->opregbase = sec->opregbase;
    s->portscbase = sec->portscbase;
    s->portnr = sec->portnr;
    s->as = &address_space_memory;

    usb_ehci_init(s, DEVICE(obj));
    sysbus_init_mmio(d, &s->mem);
}

static void ehci_sysbus_finalize(Object *obj)
{
    EHCISysBusState *i = SYS_BUS_EHCI(obj);
    EHCIState *s = &i->ehci;

    usb_ehci_finalize(s);
}

static void ehci_sysbus_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(klass);

    sec->portscbase = 0x44;
    sec->portnr = EHCI_PORTS;

    dc->realize = usb_ehci_sysbus_realize;
    dc->vmsd = &vmstate_ehci_sysbus;
    device_class_set_props(dc, ehci_sysbus_properties);
    device_class_set_legacy_reset(dc, usb_ehci_sysbus_reset);
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void ehci_platform_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x0;
    sec->opregbase = 0x20;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void ehci_exynos4210_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x0;
    sec->opregbase = 0x10;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void ehci_aw_h3_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x0;
    sec->opregbase = 0x10;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void ehci_npcm7xx_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x0;
    sec->opregbase = 0x10;
    sec->portscbase = 0x44;
    sec->portnr = 1;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void ehci_tegra2_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x100;
    sec->opregbase = 0x140;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static void ehci_ppc4xx_init(Object *o)
{
    EHCISysBusState *s = SYS_BUS_EHCI(o);

    s->ehci.companion_enable = true;
}

static void ehci_ppc4xx_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x0;
    sec->opregbase = 0x10;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

/*
 * Faraday FUSBH200 USB 2.0 EHCI
 */

/**
 * FUSBH200EHCIRegs:
 * @FUSBH200_REG_EOF_ASTR: EOF/Async. Sleep Timer Register
 * @FUSBH200_REG_BMCSR: Bus Monitor Control/Status Register
 */
enum FUSBH200EHCIRegs {
    FUSBH200_REG_EOF_ASTR = 0x34,
    FUSBH200_REG_BMCSR    = 0x40,
};

static uint64_t fusbh200_ehci_read(void *opaque, hwaddr addr, unsigned size)
{
    EHCIState *s = static_cast<EHCIState *>(opaque);
    hwaddr off = s->opregbase + s->portscbase + 4 * s->portnr + addr;

    switch (off) {
    case FUSBH200_REG_EOF_ASTR:
        return 0x00000041;
    case FUSBH200_REG_BMCSR:
        /* High-Speed, VBUS valid, interrupt level-high active */
        return (2 << 9) | (1 << 8) | (1 << 3);
    }

    return 0;
}

static void fusbh200_ehci_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

static const MemoryRegionOps fusbh200_ehci_mmio_ops = {
    .read = fusbh200_ehci_read,
    .write = fusbh200_ehci_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4, },
};

static void fusbh200_ehci_init(Object *obj)
{
    EHCISysBusState *i = SYS_BUS_EHCI(obj);
    FUSBH200EHCIState *f = FUSBH200_EHCI(obj);
    EHCIState *s = &i->ehci;

    memory_region_init_io(&f->mem_vendor, OBJECT(f), &fusbh200_ehci_mmio_ops, s,
                          "fusbh200", 0x4c);
    memory_region_add_subregion(&s->mem,
                                s->opregbase + s->portscbase + 4 * s->portnr,
                                &f->mem_vendor);
}

static void fusbh200_ehci_class_init(ObjectClass *oc, const void *data)
{
    SysBusEHCIClass *sec = SYS_BUS_EHCI_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sec->capsbase = 0x0;
    sec->opregbase = 0x10;
    sec->portscbase = 0x20;
    sec->portnr = 1;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

#include "qom/cpp/object.h"

REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT_FINI_CI_CS(EHCISysBusState,
                                                    SysBusEHCIClass,
                                                    TYPE_SYS_BUS_EHCI,
                                                    TYPE_SYS_BUS_DEVICE,
                                                    ehci_sysbus_init,
                                                    ehci_sysbus_finalize,
                                                    ehci_sysbus_class_init)

REGISTER_QEMU_OBJECT_CLASS_ONLY(platform_ehci, TYPE_PLATFORM_EHCI,
                                 TYPE_SYS_BUS_EHCI, ehci_platform_class_init)

REGISTER_QEMU_OBJECT_CLASS_ONLY(exynos4210_ehci, TYPE_EXYNOS4210_EHCI,
                                 TYPE_SYS_BUS_EHCI, ehci_exynos4210_class_init)

REGISTER_QEMU_OBJECT_CLASS_ONLY(aw_h3_ehci, TYPE_AW_H3_EHCI,
                                 TYPE_SYS_BUS_EHCI, ehci_aw_h3_class_init)

REGISTER_QEMU_OBJECT_CLASS_ONLY(npcm7xx_ehci, TYPE_NPCM7XX_EHCI,
                                 TYPE_SYS_BUS_EHCI, ehci_npcm7xx_class_init)

REGISTER_QEMU_OBJECT_CLASS_ONLY(tegra2_ehci, TYPE_TEGRA2_EHCI,
                                 TYPE_SYS_BUS_EHCI, ehci_tegra2_class_init)

REGISTER_QEMU_OBJECT_INIT_CLASS(ppc4xx_ehci, TYPE_PPC4xx_EHCI,
                                 TYPE_SYS_BUS_EHCI, ehci_ppc4xx_init,
                                 ehci_ppc4xx_class_init)

REGISTER_QEMU_OBJECT_INIT_CLASS_SIZED_NOCS(fusbh200_ehci, FUSBH200EHCIState,
                                            TYPE_FUSBH200_EHCI,
                                            TYPE_SYS_BUS_EHCI,
                                            fusbh200_ehci_init,
                                            fusbh200_ehci_class_init)
