/*
 * Xilinx Display Port Control Data
 *
 *  Copyright (C) 2015 : GreenSocs Ltd
 *      http://www.greensocs.com/ , email: info@greensocs.com
 *
 *  Developed by :
 *  Frederic Konrad   <fred.konrad@greensocs.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option)any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * This is a simple AUX slave which emulates a connected screen.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/auxbus.h"
#include "migration/vmstate.h"
#include "hw/display/dpcd.h"
#include "trace.h"

#define DPCD_READABLE_AREA                      0x600

struct DPCDState {
    /*< private >*/
    AUXSlave parent_obj;

    /*< public >*/
    /*
     * The DCPD is 0x7FFFF length but read as 0 after offset 0x5FF.
     */
    uint8_t dpcd_info[DPCD_READABLE_AREA];

    MemoryRegion iomem;

    /* Instance methods */
    void init();
    void reset();

    /* Static MMIO callbacks */
    static uint64_t mmioRead(void *opaque, hwaddr offset, unsigned size);
    static void mmioWrite(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size);

    /* Class methods */
    static void classInit(DeviceClass *dc);
};

uint64_t DPCDState::mmioRead(void *opaque, hwaddr offset, unsigned size)
{
    uint8_t ret;
    DPCDState *e = reinterpret_cast<DPCDState *>(opaque);

    if (offset < DPCD_READABLE_AREA) {
        ret = e->dpcd_info[offset];
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "dpcd: Bad offset 0x%" HWADDR_PRIX "\n",
                                       offset);
        ret = 0;
    }
    trace_dpcd_read(offset, ret);

    return ret;
}

void DPCDState::mmioWrite(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    DPCDState *e = reinterpret_cast<DPCDState *>(opaque);

    trace_dpcd_write(offset, value);
    if (offset < DPCD_READABLE_AREA) {
        e->dpcd_info[offset] = value;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "dpcd: Bad offset 0x%" HWADDR_PRIX "\n",
                                       offset);
    }
}

static const MemoryRegionOps aux_ops = {
    .read = DPCDState::mmioRead,
    .write = DPCDState::mmioWrite,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

void DPCDState::reset()
{
    memset(&dpcd_info, 0, sizeof(dpcd_info));

    dpcd_info[DPCD_REVISION] = DPCD_REV_1_0;
    dpcd_info[DPCD_MAX_LINK_RATE] = DPCD_5_4GBPS;
    dpcd_info[DPCD_MAX_LANE_COUNT] = DPCD_FOUR_LANES;
    dpcd_info[DPCD_RECEIVE_PORT0_CAP_0] = DPCD_EDID_PRESENT;
    /* buffer size */
    dpcd_info[DPCD_RECEIVE_PORT0_CAP_1] = 0xFF;

    dpcd_info[DPCD_LANE0_1_STATUS] = DPCD_LANE0_CR_DONE
                                   | DPCD_LANE0_CHANNEL_EQ_DONE
                                   | DPCD_LANE0_SYMBOL_LOCKED
                                   | DPCD_LANE1_CR_DONE
                                   | DPCD_LANE1_CHANNEL_EQ_DONE
                                   | DPCD_LANE1_SYMBOL_LOCKED;
    dpcd_info[DPCD_LANE2_3_STATUS] = DPCD_LANE2_CR_DONE
                                   | DPCD_LANE2_CHANNEL_EQ_DONE
                                   | DPCD_LANE2_SYMBOL_LOCKED
                                   | DPCD_LANE3_CR_DONE
                                   | DPCD_LANE3_CHANNEL_EQ_DONE
                                   | DPCD_LANE3_SYMBOL_LOCKED;

    dpcd_info[DPCD_LANE_ALIGN_STATUS_UPDATED] = DPCD_INTERLANE_ALIGN_DONE;
    dpcd_info[DPCD_SINK_STATUS] = DPCD_RECEIVE_PORT_0_STATUS;
}

void DPCDState::init()
{
    memory_region_init_io(&iomem, OBJECT(this), &aux_ops,
                          this, TYPE_DPCD, 0x80000);
    aux_init_mmio(reinterpret_cast<AUXSlave *>(this), &iomem);
}

static const VMStateDescription vmstate_dpcd = {
    .name = TYPE_DPCD,
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY_V(dpcd_info, DPCDState, DPCD_READABLE_AREA, 0),
        VMSTATE_END_OF_LIST()
    }
};

void DPCDState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_dpcd;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(DPCDState, TYPE_DPCD, TYPE_AUX_SLAVE)
