/*
 * QEMU IDE Emulation: mmio support (for embedded).
 *
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2006 Openedhand Ltd.
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
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "system/dma.h"

#include "hw/ide/mmio.h"
#include "hw/qdev-properties.h"
#include "ide-internal.h"

/***********************************************************/
/* MMIO based ide port
 * This emulates IDE device connected directly to the CPU bus without
 * dedicated ide controller, which is often seen on embedded boards.
 */

struct MMIOIDEState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    IDEBus bus;

    uint32_t shift;
    qemu_irq irq;
    MemoryRegion iomem1, iomem2;

    void reset()
    {
        ide_bus_reset(&bus);
    }

    static uint64_t ioRead(void *opaque, hwaddr addr, unsigned size)
    {
        MMIOIDEState *s = static_cast<MMIOIDEState *>(opaque);
        addr >>= s->shift;
        if (addr & 7)
            return ide_ioport_read(&s->bus, addr);
        else
            return ide_data_readw(&s->bus, 0);
    }

    static void ioWrite(void *opaque, hwaddr addr,
                        uint64_t val, unsigned size)
    {
        MMIOIDEState *s = static_cast<MMIOIDEState *>(opaque);
        addr >>= s->shift;
        if (addr & 7)
            ide_ioport_write(&s->bus, addr, val);
        else
            ide_data_writew(&s->bus, 0, val);
    }

    static uint64_t statusRead(void *opaque, hwaddr addr, unsigned size)
    {
        MMIOIDEState *s = static_cast<MMIOIDEState *>(opaque);
        return ide_status_read(&s->bus, 0);
    }

    static void ctrlWrite(void *opaque, hwaddr addr,
                          uint64_t val, unsigned size)
    {
        MMIOIDEState *s = static_cast<MMIOIDEState *>(opaque);
        ide_ctrl_write(&s->bus, 0, val);
    }

    static const MemoryRegionOps ioOps;
    static const MemoryRegionOps csOps;

    void realize(Error **errp)
    {
        SysBusDevice *d = SYS_BUS_DEVICE(this);

        ide_bus_init_output_irq(&bus, irq);

        memory_region_init_io(&iomem1, OBJECT(this), &ioOps, this,
                              "ide-mmio.1", 16 << shift);
        memory_region_init_io(&iomem2, OBJECT(this), &csOps, this,
                              "ide-mmio.2", 2 << shift);
        sysbus_init_mmio(d, &iomem1);
        sysbus_init_mmio(d, &iomem2);
    }

    void init()
    {
        ide_bus_init(&bus, sizeof(bus), DEVICE(this), 0, 2);
        sysbus_init_irq(SYS_BUS_DEVICE(this), &irq);
    }

    static void classInit(DeviceClass *dc);

    static const VMStateDescription vmstate_ide_mmio;
};

const MemoryRegionOps MMIOIDEState::ioOps = {
    .read = MMIOIDEState::ioRead,
    .write = MMIOIDEState::ioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

const MemoryRegionOps MMIOIDEState::csOps = {
    .read = MMIOIDEState::statusRead,
    .write = MMIOIDEState::ctrlWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static const VMStateField vmstate_ide_mmio_fields[] = {
    VMSTATE_IDE_BUS(bus, MMIOIDEState),
    VMSTATE_IDE_DRIVES(bus.ifs, MMIOIDEState),
    VMSTATE_END_OF_LIST()
};

const VMStateDescription MMIOIDEState::vmstate_ide_mmio = {
    .name = "mmio-ide",
    .version_id = 3,
    .minimum_version_id = 0,
    .fields = vmstate_ide_mmio_fields,
};

static const Property mmio_ide_properties[] = {
    DEFINE_PROP_UINT32("shift", MMIOIDEState, shift, 0),
};

void MMIOIDEState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, mmio_ide_properties);
    dc->vmsd = &vmstate_ide_mmio;
}

void mmio_ide_init_drives(DeviceState *dev, DriveInfo *hd0, DriveInfo *hd1)
{
    MMIOIDEState *s = reinterpret_cast<MMIOIDEState *>(dev);

    if (hd0 != NULL) {
        ide_bus_create_drive(&s->bus, 0, hd0);
    }
    if (hd1 != NULL) {
        ide_bus_create_drive(&s->bus, 1, hd1);
    }
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(MMIOIDEState, TYPE_MMIO_IDE, TYPE_SYS_BUS_DEVICE)
