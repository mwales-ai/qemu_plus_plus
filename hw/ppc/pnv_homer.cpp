/*
 * QEMU PowerPC PowerNV Emulation of a few HOMER related registers
 *
 * Copyright (c) 2019, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "exec/hwaddr.h"
#include "system/memory.h"
#include "system/cpus.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/ppc/pnv.h"
#include "hw/ppc/pnv_chip.h"
#include "hw/ppc/pnv_homer.h"
#include "hw/ppc/pnv_xscom.h"
#include "qom/cpp/object.h"

/* P8 PBA BARs */
#define PBA_BAR0                     0x00
#define PBA_BAR1                     0x01
#define PBA_BAR2                     0x02
#define PBA_BAR3                     0x03
#define PBA_BARMASK0                 0x04
#define PBA_BARMASK1                 0x05
#define PBA_BARMASK2                 0x06
#define PBA_BARMASK3                 0x07

static uint64_t pnv_homer_power8_pba_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    PnvHomer *homer = PNV_HOMER(opaque);
    PnvHomerClass *hmrc = PNV_HOMER_GET_CLASS(homer);
    uint32_t reg = addr >> 3;
    uint64_t val = 0;

    switch (reg) {
    case PBA_BAR0:
        val = homer->base;
        break;
    case PBA_BARMASK0: /* P8 homer region mask */
        val = (hmrc->size - 1) & 0x300000;
        break;
    case PBA_BAR3: /* P8 occ common area */
        val = PNV_OCC_COMMON_AREA_BASE;
        break;
    case PBA_BARMASK3: /* P8 occ common area mask */
        val = (PNV_OCC_COMMON_AREA_SIZE - 1) & 0x700000;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "PBA: read to unimplemented register: Ox%"
                      HWADDR_PRIx "\n", addr >> 3);
    }
    return val;
}

static void pnv_homer_power8_pba_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "PBA: write to unimplemented register: Ox%"
                  HWADDR_PRIx "\n", addr >> 3);
}

static const MemoryRegionOps pnv_homer_power8_pba_ops = {
    .read = pnv_homer_power8_pba_read,
    .write = pnv_homer_power8_pba_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8, },
    .impl = { .min_access_size = 8, .max_access_size = 8, },
};

static hwaddr pnv_homer_power8_get_base(PnvChip *chip)
{
    return PNV_HOMER_BASE(chip);
}

static void pnv_homer_power8_class_init(ObjectClass *klass, const void *data)
{
    PnvHomerClass *homer = PNV_HOMER_CLASS(klass);

    homer->get_base = pnv_homer_power8_get_base;
    homer->size = PNV_HOMER_SIZE;
    homer->pba_size = PNV_XSCOM_PBA_SIZE;
    homer->pba_ops = &pnv_homer_power8_pba_ops;
}


static uint64_t pnv_homer_power9_pba_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    PnvHomer *homer = PNV_HOMER(opaque);
    PnvHomerClass *hmrc = PNV_HOMER_GET_CLASS(homer);
    uint32_t reg = addr >> 3;
    uint64_t val = 0;

    switch (reg) {
    case PBA_BAR0:
        val = homer->base;
        break;
    case PBA_BARMASK0: /* P9 homer region mask */
        val = (hmrc->size - 1) & 0x300000;
        break;
    case PBA_BAR2: /* P9 occ common area */
        val = PNV9_OCC_COMMON_AREA_BASE;
        break;
    case PBA_BARMASK2: /* P9 occ common area size */
        val = (PNV9_OCC_COMMON_AREA_SIZE - 1) & 0x700000;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "PBA: read to unimplemented register: Ox%"
                      HWADDR_PRIx "\n", addr >> 3);
    }
    return val;
}

static void pnv_homer_power9_pba_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "PBA: write to unimplemented register: Ox%"
                  HWADDR_PRIx "\n", addr >> 3);
}

static const MemoryRegionOps pnv_homer_power9_pba_ops = {
    .read = pnv_homer_power9_pba_read,
    .write = pnv_homer_power9_pba_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8, },
    .impl = { .min_access_size = 8, .max_access_size = 8, },
};

static hwaddr pnv_homer_power9_get_base(PnvChip *chip)
{
    return PNV9_HOMER_BASE(chip);
}

static void pnv_homer_power9_class_init(ObjectClass *klass, const void *data)
{
    PnvHomerClass *homer = PNV_HOMER_CLASS(klass);

    homer->get_base = pnv_homer_power9_get_base;
    homer->size = PNV_HOMER_SIZE;
    homer->pba_size = PNV9_XSCOM_PBA_SIZE;
    homer->pba_ops = &pnv_homer_power9_pba_ops;
}


static uint64_t pnv_homer_power10_pba_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    PnvHomer *homer = PNV_HOMER(opaque);
    PnvHomerClass *hmrc = PNV_HOMER_GET_CLASS(homer);
    uint32_t reg = addr >> 3;
    uint64_t val = 0;

    switch (reg) {
    case PBA_BAR0:
        val = homer->base;
        break;
    case PBA_BARMASK0: /* P10 homer region mask */
        val = (hmrc->size - 1) & 0x300000;
        break;
    case PBA_BAR2: /* P10 occ common area */
        val = PNV10_OCC_COMMON_AREA_BASE;
        break;
    case PBA_BARMASK2: /* P10 occ common area size */
        val = (PNV10_OCC_COMMON_AREA_SIZE - 1) & 0x700000;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "PBA: read to unimplemented register: Ox%"
                      HWADDR_PRIx "\n", addr >> 3);
    }
    return val;
}

static void pnv_homer_power10_pba_write(void *opaque, hwaddr addr,
                                         uint64_t val, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "PBA: write to unimplemented register: Ox%"
                  HWADDR_PRIx "\n", addr >> 3);
}

static const MemoryRegionOps pnv_homer_power10_pba_ops = {
    .read = pnv_homer_power10_pba_read,
    .write = pnv_homer_power10_pba_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8, },
    .impl = { .min_access_size = 8, .max_access_size = 8, },
};

static hwaddr pnv_homer_power10_get_base(PnvChip *chip)
{
    return PNV10_HOMER_BASE(chip);
}

static void pnv_homer_power10_class_init(ObjectClass *klass, const void *data)
{
    PnvHomerClass *homer = PNV_HOMER_CLASS(klass);

    homer->get_base = pnv_homer_power10_get_base;
    homer->size = PNV_HOMER_SIZE;
    homer->pba_size = PNV10_XSCOM_PBA_SIZE;
    homer->pba_ops = &pnv_homer_power10_pba_ops;
}


static void pnv_homer_realize(DeviceState *dev, Error **errp)
{
    PnvHomer *homer = PNV_HOMER(dev);
    PnvHomerClass *hmrc = PNV_HOMER_GET_CLASS(homer);
    char homer_str[32];

    assert(homer->chip);

    pnv_xscom_region_init(&homer->pba_regs, OBJECT(dev), hmrc->pba_ops,
                          homer, "xscom-pba", hmrc->pba_size);

    /* Homer RAM region */
    homer->base = hmrc->get_base(homer->chip);

    snprintf(homer_str, sizeof(homer_str), "homer-chip%d-memory",
             homer->chip->chip_id);
    if (!memory_region_init_ram(&homer->mem, OBJECT(homer),
                                homer_str, hmrc->size, errp)) {
        return;
    }
}

static const Property pnv_homer_properties[] = {
    DEFINE_PROP_LINK("chip", PnvHomer, chip, TYPE_PNV_CHIP, PnvChip *),
};

void PnvHomer::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    (void)klass;

    dc->realize = pnv_homer_realize;
    dc->desc = "PowerNV HOMER Memory";
    device_class_set_props(dc, pnv_homer_properties);
    dc->user_creatable = false;
}

REGISTER_QEMU_DEVICE_ABSTRACT(PnvHomer, PnvHomerClass,
                               TYPE_PNV_HOMER, TYPE_DEVICE)

REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED(pnv_homer_power8, PnvHomer,
                                       TYPE_PNV8_HOMER, TYPE_PNV_HOMER,
                                       pnv_homer_power8_class_init)

REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED(pnv_homer_power9, PnvHomer,
                                       TYPE_PNV9_HOMER, TYPE_PNV_HOMER,
                                       pnv_homer_power9_class_init)

REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED(pnv_homer_power10, PnvHomer,
                                       TYPE_PNV10_HOMER, TYPE_PNV_HOMER,
                                       pnv_homer_power10_class_init)
