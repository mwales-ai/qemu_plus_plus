/*
 * QEMU SiFive E PRCI (Power, Reset, Clock, Interrupt)
 *
 * Copyright (c) 2017 SiFive, Inc.
 *
 * Simple model of the PRCI to emulate register reads made by the SDK BSP
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/sifive_e_prci.h"
#include "qom/cpp/object.h"

static uint64_t sifive_e_prci_read(void *opaque, hwaddr addr, unsigned int size)
{
    SiFiveEPRCIState *s = static_cast<SiFiveEPRCIState *>(opaque);
    switch (addr) {
    case SIFIVE_E_PRCI_HFROSCCFG:
        return s->hfrosccfg;
    case SIFIVE_E_PRCI_HFXOSCCFG:
        return s->hfxosccfg;
    case SIFIVE_E_PRCI_PLLCFG:
        return s->pllcfg;
    case SIFIVE_E_PRCI_PLLOUTDIV:
        return s->plloutdiv;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: read: addr=0x%x\n",
                  __func__, static_cast<int>(addr));
    return 0;
}

static void sifive_e_prci_write(void *opaque, hwaddr addr,
                                uint64_t val64, unsigned int size)
{
    SiFiveEPRCIState *s = static_cast<SiFiveEPRCIState *>(opaque);
    switch (addr) {
    case SIFIVE_E_PRCI_HFROSCCFG:
        s->hfrosccfg = static_cast<uint32_t>(val64);
        /* OSC stays ready */
        s->hfrosccfg |= SIFIVE_E_PRCI_HFROSCCFG_RDY;
        break;
    case SIFIVE_E_PRCI_HFXOSCCFG:
        s->hfxosccfg = static_cast<uint32_t>(val64);
        /* OSC stays ready */
        s->hfxosccfg |= SIFIVE_E_PRCI_HFXOSCCFG_RDY;
        break;
    case SIFIVE_E_PRCI_PLLCFG:
        s->pllcfg = static_cast<uint32_t>(val64);
        /* PLL stays locked */
        s->pllcfg |= SIFIVE_E_PRCI_PLLCFG_LOCK;
        break;
    case SIFIVE_E_PRCI_PLLOUTDIV:
        s->plloutdiv = static_cast<uint32_t>(val64);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write: addr=0x%x v=0x%x\n",
                      __func__, static_cast<int>(addr), static_cast<int>(val64));
    }
}

static const MemoryRegionOps sifive_e_prci_ops = {
    .read = sifive_e_prci_read,
    .write = sifive_e_prci_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4
    }
};

void SiFiveEPRCIState::init()
{
    memory_region_init_io(&mmio, reinterpret_cast<Object *>(this),
                          &sifive_e_prci_ops, this,
                          TYPE_SIFIVE_E_PRCI, SIFIVE_E_PRCI_REG_SIZE);
    sysbus_init_mmio(reinterpret_cast<SysBusDevice *>(this), &mmio);

    hfrosccfg = (SIFIVE_E_PRCI_HFROSCCFG_RDY | SIFIVE_E_PRCI_HFROSCCFG_EN);
    hfxosccfg = (SIFIVE_E_PRCI_HFXOSCCFG_RDY | SIFIVE_E_PRCI_HFXOSCCFG_EN);
    pllcfg = (SIFIVE_E_PRCI_PLLCFG_REFSEL | SIFIVE_E_PRCI_PLLCFG_BYPASS |
              SIFIVE_E_PRCI_PLLCFG_LOCK);
    plloutdiv = SIFIVE_E_PRCI_PLLOUTDIV_DIV1;
}

REGISTER_QEMU_DEVICE(SiFiveEPRCIState, TYPE_SIFIVE_E_PRCI,
                     TYPE_SYS_BUS_DEVICE)


/*
 * Create PRCI device.
 */
DeviceState *sifive_e_prci_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_SIFIVE_E_PRCI);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}
