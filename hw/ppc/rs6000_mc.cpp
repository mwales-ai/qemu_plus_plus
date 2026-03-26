/*
 * QEMU RS/6000 memory controller
 *
 * Copyright (c) 2017 Hervé Poussineau
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/units.h"
#include "hw/isa/isa.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qapi/error.h"
#include "trace.h"
#include "qom/object.h"

#define TYPE_RS6000MC "rs6000-mc"
OBJECT_DECLARE_SIMPLE_TYPE(RS6000MCState, RS6000MC)

enum {
    PORT0841_NO_ERROR_DETECTED = 0x01,
};

struct RS6000MCState {
    ISADevice parent_obj;
    /* see US patent 5,684,979 for details (expired 2001-11-04) */
    uint32_t ram_size;
    bool autoconfigure;
    MemoryRegion simm[6];
    unsigned int simm_size[6];
    uint32_t end_address[8];
    uint8_t port0820_index;
    PortioList portio;

    static uint32_t port0803Read(void *opaque, uint32_t addr)
    {
        RS6000MCState *s = static_cast<RS6000MCState *>(opaque);
        uint32_t val = 0;
        int socket;

        /* (1 << socket) indicates 32 MB SIMM at given socket */
        for (socket = 0; socket < 6; socket++) {
            if (s->simm_size[socket] == 32) {
                val |= (1 << socket);
            }
        }

        trace_rs6000mc_id_read(addr, val);
        return val;
    }

    static uint32_t port0804Read(void *opaque, uint32_t addr)
    {
        RS6000MCState *s = static_cast<RS6000MCState *>(opaque);
        uint32_t val = 0xff;
        int socket;

        /* (1 << socket) indicates SIMM absence at given socket */
        for (socket = 0; socket < 6; socket++) {
            if (s->simm_size[socket]) {
                val &= ~(1 << socket);
            }
        }
        s->port0820_index = 0;

        trace_rs6000mc_presence_read(addr, val);
        return val;
    }

    static uint32_t port0820Read(void *opaque, uint32_t addr)
    {
        RS6000MCState *s = static_cast<RS6000MCState *>(opaque);
        uint32_t val = s->end_address[s->port0820_index] & 0x1f;
        s->port0820_index = (s->port0820_index + 1) & 7;
        trace_rs6000mc_size_read(addr, val);
        return val;
    }

    static void port0820Write(void *opaque, uint32_t addr, uint32_t val)
    {
        RS6000MCState *s = static_cast<RS6000MCState *>(opaque);
        uint8_t socket = val >> 5;
        uint32_t end_address = val & 0x1f;

        trace_rs6000mc_size_write(addr, val);
        s->end_address[socket] = end_address;
        if (socket > 0 && socket < 7) {
            if (s->simm_size[socket - 1]) {
                uint32_t size;
                uint32_t start_address = 0;
                if (socket > 1) {
                    start_address = s->end_address[socket - 1];
                }

                size = end_address - start_address;
                memory_region_set_enabled(&s->simm[socket - 1], size != 0);
                memory_region_set_address(&s->simm[socket - 1],
                                          start_address * 8 * MiB);
            }
        }
    }

    static uint32_t port0841Read(void *opaque, uint32_t addr)
    {
        uint32_t val = PORT0841_NO_ERROR_DETECTED;
        trace_rs6000mc_parity_read(addr, val);
        return val;
    }

    void realize(Error **errp)
    {
        int socket = 0;
        unsigned int ram_sz = ram_size / MiB;

        while (socket < 6) {
            if (ram_sz >= 64) {
                simm_size[socket] = 32;
                simm_size[socket + 1] = 32;
                ram_sz -= 64;
            } else if (ram_sz >= 16) {
                simm_size[socket] = 8;
                simm_size[socket + 1] = 8;
                ram_sz -= 16;
            } else {
                /* Not enough memory */
                break;
            }
            socket += 2;
        }

        for (socket = 0; socket < 6; socket++) {
            if (simm_size[socket]) {
                char name[] = "simm.?";
                name[5] = socket + '0';
                if (!memory_region_init_ram(&simm[socket], OBJECT(this), name,
                                            simm_size[socket] * MiB, errp)) {
                    return;
                }
                memory_region_add_subregion_overlap(get_system_memory(), 0,
                                                    &simm[socket], socket);
            }
        }
        if (ram_sz) {
            /* unable to push all requested RAM in SIMMs */
            error_setg(errp, "RAM size incompatible with this board. "
                       "Try again with something else, like %" PRId64 " MB",
                       ram_size / MiB - ram_sz);
            return;
        }

        if (autoconfigure) {
            uint32_t start_address = 0;
            for (socket = 0; socket < 6; socket++) {
                if (simm_size[socket]) {
                    memory_region_set_enabled(&simm[socket], true);
                    memory_region_set_address(&simm[socket], start_address);
                    start_address += memory_region_size(&simm[socket]);
                }
            }
        }

        isa_register_portio_list(ISA_DEVICE(this), &portio, 0x0,
                                 rs6000mc_port_list, this, "rs6000mc");
    }

    static void realizeWrapper(DeviceState *dev, Error **errp)
    {
        RS6000MCState *s = RS6000MC(dev);
        s->realize(errp);
    }

    static const MemoryRegionPortio rs6000mc_port_list[];
};

const MemoryRegionPortio RS6000MCState::rs6000mc_port_list[] = {
    { 0x803, 1, 1, .read = RS6000MCState::port0803Read },
    { 0x804, 1, 1, .read = RS6000MCState::port0804Read },
    { 0x820, 1, 1, .read = RS6000MCState::port0820Read,
                   .write = RS6000MCState::port0820Write, },
    { 0x841, 1, 1, .read = RS6000MCState::port0841Read },
    PORTIO_END_OF_LIST()
};

static const VMStateDescription vmstate_rs6000mc = {
    .name = "rs6000-mc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(port0820_index, RS6000MCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rs6000mc_properties[] = {
    DEFINE_PROP_UINT32("ram-size", RS6000MCState, ram_size, 0),
    DEFINE_PROP_BOOL("auto-configure", RS6000MCState, autoconfigure, true),
};

static void rs6000mc_class_initfn(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = RS6000MCState::realizeWrapper;
    dc->vmsd = &vmstate_rs6000mc;
    device_class_set_props(dc, rs6000mc_properties);
}

static const TypeInfo rs6000mc_info = {
    .name          = TYPE_RS6000MC,
    .parent        = TYPE_ISA_DEVICE,
    .instance_size = sizeof(RS6000MCState),
    .class_init    = rs6000mc_class_initfn,
};

static void rs6000mc_types(void)
{
    type_register_static(&rs6000mc_info);
}

type_init(rs6000mc_types)
