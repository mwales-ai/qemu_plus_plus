/*
 * QEMU Macintosh Nubus
 *
 * Copyright (c) 2013-2018 Laurent Vivier <laurent@vivier.eu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "exec/target_page.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/nubus/nubus.h"
#include "qapi/error.h"
#include "qemu/error-report.h"


void nubus_set_irq(NubusDevice *nd, int level)
{
    NubusBus *nubus = NUBUS_BUS(qdev_get_parent_bus(DEVICE(nd)));

    qemu_set_irq(nubus->irqs[nd->slot], level);
}

void NubusDevice::realize(Error **errp)
{
    NubusBus *nubus = NUBUS_BUS(qdev_get_parent_bus(DEVICE(this)));
    char *name, *path;
    hwaddr slot_offset;
    int64_t size, align_size;
    uint8_t *rom_ptr;
    int ret;

    if (slot < 0 || slot >= NUBUS_SLOT_NB) {
        error_setg(errp,
                   "'slot' value %d out of range (must be between 0 and %d)",
                   slot, NUBUS_SLOT_NB - 1);
        return;
    }

    /* Super */
    slot_offset = slot * NUBUS_SUPER_SLOT_SIZE;

    name = g_strdup_printf("nubus-super-slot-%x", slot);
    memory_region_init(&super_slot_mem, OBJECT(this), name,
                       NUBUS_SUPER_SLOT_SIZE);
    memory_region_add_subregion(&nubus->super_slot_io, slot_offset,
                                &super_slot_mem);
    g_free(name);

    /* Normal */
    slot_offset = slot * NUBUS_SLOT_SIZE;

    name = g_strdup_printf("nubus-slot-%x", slot);
    memory_region_init(&slot_mem, OBJECT(this), name, NUBUS_SLOT_SIZE);
    memory_region_add_subregion(&nubus->slot_io, slot_offset, &slot_mem);
    g_free(name);

    /* Declaration ROM */
    if (romfile != NULL) {
        path = qemu_find_file(QEMU_FILE_TYPE_BIOS, romfile);
        if (path == NULL) {
            path = g_strdup(romfile);
        }

        size = get_image_size(path, NULL);
        if (size < 0) {
            error_setg(errp, "failed to find romfile \"%s\"", romfile);
            g_free(path);
            return;
        } else if (size == 0) {
            error_setg(errp, "romfile \"%s\" is empty", romfile);
            g_free(path);
            return;
        } else if (size > NUBUS_DECL_ROM_MAX_SIZE) {
            error_setg(errp, "romfile \"%s\" too large (maximum size 128K)",
                       romfile);
            g_free(path);
            return;
        }

        name = g_strdup_printf("nubus-slot-%x-declaration-rom", slot);

        /*
         * Ensure ROM memory region is aligned to target page size regardless
         * of the size of the Declaration ROM image
         */
        align_size = ROUND_UP(size, qemu_target_page_size());
        memory_region_init_rom(&decl_rom, OBJECT(this), name, align_size,
                               &error_abort);
        rom_ptr = memory_region_get_ram_ptr(&decl_rom);
        ret = load_image_size(path, rom_ptr + (uintptr_t)(align_size - size),
                              size);
        g_free(path);
        g_free(name);
        if (ret < 0) {
            error_setg(errp, "could not load romfile \"%s\"", romfile);
            return;
        }
        memory_region_add_subregion(&slot_mem, NUBUS_SLOT_SIZE - align_size,
                                    &decl_rom);
    }
}

static const Property nubus_device_properties[] = {
    DEFINE_PROP_INT32("slot", NubusDevice, slot, -1),
    DEFINE_PROP_STRING("romfile", NubusDevice, romfile),
};

void NubusDevice::classInit(DeviceClass *dc)
{
    dc->bus_type = TYPE_NUBUS_BUS;
    device_class_set_props(dc, nubus_device_properties);
}

#include "qom/cpp/object.h"

REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS(NubusDevice,
                                     TYPE_NUBUS_DEVICE,
                                     TYPE_DEVICE)
