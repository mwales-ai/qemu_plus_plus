/*
 * Copyright (C) 2014 Freescale Semiconductor, Inc. All rights reserved.
 *
 * Author: Amit Tomar, <Amit.Tomar@freescale.com>
 *
 * Description:
 * This file is derived from IMX I2C controller,
 * by Jean-Christophe DUBOIS .
 *
 * Thanks to Scott Wood and Alexander Graf for their kind help on this.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 or later,
 * as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

/* #define DEBUG_I2C */

#ifdef DEBUG_I2C
#define DPRINTF(fmt, ...)              \
    do { fprintf(stderr, "mpc_i2c[%s]: " fmt, __func__, ## __VA_ARGS__); \
    } while (0)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define TYPE_MPC_I2C "mpc-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(MPCI2CState, MPC_I2C)

#define MPC_I2C_ADR   0x00
#define MPC_I2C_FDR   0x04
#define MPC_I2C_CR    0x08
#define MPC_I2C_SR    0x0c
#define MPC_I2C_DR    0x10
#define MPC_I2C_DFSRR 0x14

#define CCR_MEN  (1 << 7)
#define CCR_MIEN (1 << 6)
#define CCR_MSTA (1 << 5)
#define CCR_MTX  (1 << 4)
#define CCR_TXAK (1 << 3)
#define CCR_RSTA (1 << 2)
#define CCR_BCST (1 << 0)

#define CSR_MCF  (1 << 7)
#define CSR_MAAS (1 << 6)
#define CSR_MBB  (1 << 5)
#define CSR_MAL  (1 << 4)
#define CSR_SRW  (1 << 2)
#define CSR_MIF  (1 << 1)
#define CSR_RXAK (1 << 0)

#define CADR_MASK 0xFE
#define CFDR_MASK 0x3F
#define CCR_MASK  0xFC
#define CSR_MASK  0xED
#define CDR_MASK  0xFF

#define CYCLE_RESET 0xFF

struct MPCI2CState {
    SysBusDevice parent_obj;

    I2CBus *bus;
    qemu_irq irq;
    MemoryRegion iomem;

    uint8_t address;
    uint8_t adr;
    uint8_t fdr;
    uint8_t cr;
    uint8_t sr;
    uint8_t dr;
    uint8_t dfsrr;

    bool isEnabled();
    bool isMaster();
    bool directionIsTx();
    bool irqPending();
    bool irqIsEnabled();
    void reset();
    void updateIrq();
    void softReset();
    void addressSend();
    void dataSend();
    void dataReceive();
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size);
    void realize(Error **errp);
    static void resetWrapper(DeviceState *dev);
    static void realizeWrapper(DeviceState *dev, Error **errp);
    static void classInit(ObjectClass *klass, const void *data);
};

bool MPCI2CState::isEnabled()
{
    return cr & CCR_MEN;
}

bool MPCI2CState::isMaster()
{
    return cr & CCR_MSTA;
}

bool MPCI2CState::directionIsTx()
{
    return cr & CCR_MTX;
}

bool MPCI2CState::irqPending()
{
    return sr & CSR_MIF;
}

bool MPCI2CState::irqIsEnabled()
{
    return cr & CCR_MIEN;
}

void MPCI2CState::resetWrapper(DeviceState *dev)
{
    MPCI2CState *i2c = MPC_I2C(dev);
    i2c->reset();
}

void MPCI2CState::reset()
{
    address = 0xFF;
    adr = 0x00;
    fdr = 0x00;
    cr =  0x00;
    sr =  0x81;
    dr =  0x00;
}

void MPCI2CState::updateIrq()
{
    bool irq_active = false;

    if (isEnabled() && irqIsEnabled() && irqPending()) {
        irq_active = true;
    }

    if (irq_active) {
        qemu_irq_raise(irq);
    } else {
        qemu_irq_lower(irq);
    }
}

void MPCI2CState::softReset()
{
    /* This is a soft reset. ADR is preserved during soft resets */
    uint8_t saved_adr = adr;
    reset();
    adr = saved_adr;
}

void MPCI2CState::addressSend()
{
    /* if returns non zero slave address is not right */
    if (i2c_start_transfer(bus, dr >> 1, dr & (0x01))) {
        sr |= CSR_RXAK;
    } else {
        address = dr;
        sr &= ~CSR_RXAK;
        sr |=  CSR_MCF; /* Set after Byte Transfer is completed */
        sr |=  CSR_MIF; /* Set after Byte Transfer is completed */
        updateIrq();
    }
}

void MPCI2CState::dataSend()
{
    if (i2c_send(bus, dr)) {
        /* End of transfer */
        sr |= CSR_RXAK;
        i2c_end_transfer(bus);
    } else {
        sr &= ~CSR_RXAK;
        sr |=  CSR_MCF; /* Set after Byte Transfer is completed */
        sr |=  CSR_MIF; /* Set after Byte Transfer is completed */
        updateIrq();
    }
}

void MPCI2CState::dataReceive()
{
    int ret;
    /* get the next byte */
    ret = i2c_recv(bus);
    if (ret >= 0) {
        sr |= CSR_MCF; /* Set after Byte Transfer is completed */
        sr |= CSR_MIF; /* Set after Byte Transfer is completed */
        updateIrq();
    } else {
        DPRINTF("read failed for device");
        ret = 0xff;
    }
    dr = ret;
}

uint64_t MPCI2CState::mmioRead(void *opaque, hwaddr addr, unsigned size)
{
    MPCI2CState *s = static_cast<MPCI2CState *>(opaque);
    uint8_t value;

    switch (addr) {
    case MPC_I2C_ADR:
        value = s->adr;
        break;
    case MPC_I2C_FDR:
        value = s->fdr;
        break;
    case MPC_I2C_CR:
        value = s->cr;
        break;
    case MPC_I2C_SR:
        value = s->sr;
        break;
    case MPC_I2C_DR:
        value = s->dr;
        if (s->isMaster()) { /* master mode */
            if (s->directionIsTx()) {
                DPRINTF("MTX is set not in recv mode\n");
            } else {
                s->dataReceive();
            }
        }
        break;
    default:
        value = 0;
        DPRINTF("ERROR: Bad read addr 0x%x\n", (unsigned int)addr);
        break;
    }

    trace_mpc_i2c_read(addr, value);

    return (uint64_t)value;
}

void MPCI2CState::mmioWrite(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    MPCI2CState *s = static_cast<MPCI2CState *>(opaque);

    trace_mpc_i2c_write(addr, value);

    switch (addr) {
    case MPC_I2C_ADR:
        s->adr = value & CADR_MASK;
        break;
    case MPC_I2C_FDR:
        s->fdr = value & CFDR_MASK;
        break;
    case MPC_I2C_CR:
        if (s->isEnabled() && ((value & CCR_MEN) == 0)) {
            s->softReset();
            break;
        }
        /* normal write */
        s->cr = value & CCR_MASK;
        if (s->isMaster()) { /* master mode */
            /* set the bus to busy after master is set as per RM */
            s->sr |= CSR_MBB;
        } else {
            /* bus is not busy anymore */
            s->sr &= ~CSR_MBB;
            /* Reset the address for fresh write/read cycle */
        if (s->address != CYCLE_RESET) {
            i2c_end_transfer(s->bus);
            s->address = CYCLE_RESET;
            }
        }
        /* For restart end the onging transfer */
        if (s->cr & CCR_RSTA) {
            if (s->address != CYCLE_RESET) {
                s->address = CYCLE_RESET;
                i2c_end_transfer(s->bus);
                s->cr &= ~CCR_RSTA;
            }
        }
        break;
    case MPC_I2C_SR:
        s->sr = value & CSR_MASK;
        /* Lower the interrupt */
        if (!(s->sr & CSR_MIF) || !(s->sr & CSR_MAL)) {
            s->updateIrq();
        }
        break;
    case MPC_I2C_DR:
        /* if the device is not enabled, nothing to do */
        if (!s->isEnabled()) {
            break;
        }
        s->dr = value & CDR_MASK;
        if (s->isMaster()) { /* master mode */
            if (s->address == CYCLE_RESET) {
                s->addressSend();
            } else {
                s->dataSend();
            }
        }
        break;
    case MPC_I2C_DFSRR:
        s->dfsrr = value;
        break;
    default:
        DPRINTF("ERROR: Bad write addr 0x%x\n", (unsigned int)addr);
        break;
    }
}

static const MemoryRegionOps i2c_ops = {
    .read =  MPCI2CState::mmioRead,
    .write =  MPCI2CState::mmioWrite,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .max_access_size = 1, },
};

static const VMStateDescription mpc_i2c_vmstate = {
    .name = TYPE_MPC_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(address, MPCI2CState),
        VMSTATE_UINT8(adr, MPCI2CState),
        VMSTATE_UINT8(fdr, MPCI2CState),
        VMSTATE_UINT8(cr, MPCI2CState),
        VMSTATE_UINT8(sr, MPCI2CState),
        VMSTATE_UINT8(dr, MPCI2CState),
        VMSTATE_UINT8(dfsrr, MPCI2CState),
        VMSTATE_END_OF_LIST()
    }
};

void MPCI2CState::realize(Error **errp)
{
    DeviceState *dev = reinterpret_cast<DeviceState *>(this);
    SysBusDevice *sbd = reinterpret_cast<SysBusDevice *>(this);

    sysbus_init_irq(sbd, &irq);
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this), &i2c_ops, this,
                          "mpc-i2c", 0x15);
    sysbus_init_mmio(sbd, &iomem);
    bus = i2c_init_bus(dev, "i2c");
}

void MPCI2CState::realizeWrapper(DeviceState *dev, Error **errp)
{
    MPCI2CState *s = MPC_I2C(dev);
    s->realize(errp);
}

void MPCI2CState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

    dc->vmsd  = &mpc_i2c_vmstate ;
    device_class_set_legacy_reset(dc, resetWrapper);
    dc->realize = realizeWrapper;
    dc->desc = "MPC I2C Controller";
}

static const TypeInfo mpc_i2c_types[] = {
    {
        .name          = TYPE_MPC_I2C,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MPCI2CState),
        .class_init    = MPCI2CState::classInit,
    },
};

DEFINE_TYPES(mpc_i2c_types)
