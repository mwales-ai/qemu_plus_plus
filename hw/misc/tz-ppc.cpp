/*
 * ARM TrustZone peripheral protection controller emulation
 *
 * Copyright (c) 2018 Linaro Limited
 * Written by Peter Maydell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "trace.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/misc/tz-ppc.h"
#include "hw/qdev-properties.h"
#include "qom/cpp/object.h"

static void tz_ppc_update_irq(TZPPC *s)
{
    bool level = s->irq_status && s->irq_enable;

    trace_tz_ppc_update_irq(level);
    qemu_set_irq(s->irq, level);
}

static void tz_ppc_cfg_nonsec(void *opaque, int n, int level)
{
    TZPPC *s = TZ_PPC(opaque);

    assert(n < TZ_NUM_PORTS);
    trace_tz_ppc_cfg_nonsec(n, level);
    s->cfg_nonsec[n] = level;
}

static void tz_ppc_cfg_ap(void *opaque, int n, int level)
{
    TZPPC *s = TZ_PPC(opaque);

    assert(n < TZ_NUM_PORTS);
    trace_tz_ppc_cfg_ap(n, level);
    s->cfg_ap[n] = level;
}

static void tz_ppc_cfg_sec_resp(void *opaque, int n, int level)
{
    TZPPC *s = TZ_PPC(opaque);

    trace_tz_ppc_cfg_sec_resp(level);
    s->cfg_sec_resp = level;
}

static void tz_ppc_irq_enable(void *opaque, int n, int level)
{
    TZPPC *s = TZ_PPC(opaque);

    trace_tz_ppc_irq_enable(level);
    s->irq_enable = level;
    tz_ppc_update_irq(s);
}

static void tz_ppc_irq_clear(void *opaque, int n, int level)
{
    TZPPC *s = TZ_PPC(opaque);

    trace_tz_ppc_irq_clear(level);

    s->irq_clear = level;
    if (level) {
        s->irq_status = false;
        tz_ppc_update_irq(s);
    }
}

static bool tz_ppc_check(TZPPC *s, int n, MemTxAttrs attrs)
{
    /* Check whether to allow an access to port n; return true if
     * the check passes, and false if the transaction must be blocked.
     * If the latter, the caller must check cfg_sec_resp to determine
     * whether to abort or RAZ/WI the transaction.
     * The checks are:
     *  + nonsec_mask suppresses any check of the secure attribute
     *  + otherwise, block if cfg_nonsec is 1 and transaction is secure,
     *    or if cfg_nonsec is 0 and transaction is non-secure
     *  + block if transaction is usermode and cfg_ap is 0
     */
    if ((attrs.secure == s->cfg_nonsec[n] && !(s->nonsec_mask & (1 << n))) ||
        (attrs.user && !s->cfg_ap[n])) {
        /* Block the transaction. */
        if (!s->irq_clear) {
            /* Note that holding irq_clear high suppresses interrupts */
            s->irq_status = true;
            tz_ppc_update_irq(s);
        }
        return false;
    }
    return true;
}

static MemTxResult tz_ppc_read(void *opaque, hwaddr addr, uint64_t *pdata,
                               unsigned size, MemTxAttrs attrs)
{
    TZPPCPort *p = static_cast<TZPPCPort *>(opaque);
    TZPPC *s = p->ppc;
    int n = p - s->port;
    AddressSpace *as = &p->downstream_as;
    uint64_t data;
    MemTxResult res;

    if (!tz_ppc_check(s, n, attrs)) {
        trace_tz_ppc_read_blocked(n, addr, attrs.secure, attrs.user);
        if (s->cfg_sec_resp) {
            return MEMTX_ERROR;
        } else {
            *pdata = 0;
            return MEMTX_OK;
        }
    }

    switch (size) {
    case 1:
        data = address_space_ldub(as, addr, attrs, &res);
        break;
    case 2:
        data = address_space_lduw_le(as, addr, attrs, &res);
        break;
    case 4:
        data = address_space_ldl_le(as, addr, attrs, &res);
        break;
    case 8:
        data = address_space_ldq_le(as, addr, attrs, &res);
        break;
    default:
        g_assert_not_reached();
    }
    *pdata = data;
    return res;
}

static MemTxResult tz_ppc_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size, MemTxAttrs attrs)
{
    TZPPCPort *p = static_cast<TZPPCPort *>(opaque);
    TZPPC *s = p->ppc;
    AddressSpace *as = &p->downstream_as;
    int n = p - s->port;
    MemTxResult res;

    if (!tz_ppc_check(s, n, attrs)) {
        trace_tz_ppc_write_blocked(n, addr, attrs.secure, attrs.user);
        if (s->cfg_sec_resp) {
            return MEMTX_ERROR;
        } else {
            return MEMTX_OK;
        }
    }

    switch (size) {
    case 1:
        address_space_stb(as, addr, val, attrs, &res);
        break;
    case 2:
        address_space_stw_le(as, addr, val, attrs, &res);
        break;
    case 4:
        address_space_stl_le(as, addr, val, attrs, &res);
        break;
    case 8:
        address_space_stq_le(as, addr, val, attrs, &res);
        break;
    default:
        g_assert_not_reached();
    }
    return res;
}

static const MemoryRegionOps tz_ppc_ops = {
    .read_with_attrs = tz_ppc_read,
    .write_with_attrs = tz_ppc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static bool tz_ppc_dummy_accepts(void *opaque, hwaddr addr,
                                 unsigned size, bool is_write,
                                 MemTxAttrs attrs)
{
    /*
     * Board code should never map the upstream end of an unused port,
     * so we should never try to make a memory access to it.
     */
    g_assert_not_reached();
}

static uint64_t tz_ppc_dummy_read(void *opaque, hwaddr addr, unsigned size)
{
    g_assert_not_reached();
}

static void tz_ppc_dummy_write(void *opaque, hwaddr addr,
                                        uint64_t data, unsigned size)
{
    g_assert_not_reached();
}

static MemoryRegionOps tz_ppc_dummy_ops;

static void __attribute__((constructor)) init_tz_ppc_dummy_ops(void)
{
    memset(&tz_ppc_dummy_ops, 0, sizeof(tz_ppc_dummy_ops));
    tz_ppc_dummy_ops.read = tz_ppc_dummy_read;
    tz_ppc_dummy_ops.write = tz_ppc_dummy_write;
    tz_ppc_dummy_ops.valid.accepts = tz_ppc_dummy_accepts;
}

void TZPPC::reset()
{
    trace_tz_ppc_reset();
    cfg_sec_resp = false;
    memset(cfg_nonsec, 0, sizeof(cfg_nonsec));
    memset(cfg_ap, 0, sizeof(cfg_ap));
}

void TZPPC::init()
{
    DeviceState *dev = DEVICE(this);

    qdev_init_gpio_in_named(dev, tz_ppc_cfg_nonsec, "cfg_nonsec", TZ_NUM_PORTS);
    qdev_init_gpio_in_named(dev, tz_ppc_cfg_ap, "cfg_ap", TZ_NUM_PORTS);
    qdev_init_gpio_in_named(dev, tz_ppc_cfg_sec_resp, "cfg_sec_resp", 1);
    qdev_init_gpio_in_named(dev, tz_ppc_irq_enable, "irq_enable", 1);
    qdev_init_gpio_in_named(dev, tz_ppc_irq_clear, "irq_clear", 1);
    qdev_init_gpio_out_named(dev, &irq, "irq", 1);
}

void TZPPC::realize(Error **errp)
{
    Object *obj = OBJECT(this);
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);
    int i;
    int max_port = 0;

    for (i = 0; i < TZ_NUM_PORTS; i++) {
        if (port[i].downstream) {
            max_port = i;
        }
    }

    for (i = 0; i <= max_port; i++) {
        TZPPCPort *p = &port[i];
        char *name;
        uint64_t size;

        if (!p->downstream) {
            name = g_strdup_printf("tz-ppc-dummy-port[%d]", i);
            memory_region_init_io(&p->upstream, obj, &tz_ppc_dummy_ops,
                                  p, name, 0x10000);
            sysbus_init_mmio(sbd, &p->upstream);
            g_free(name);
            continue;
        }

        name = g_strdup_printf("tz-ppc-port[%d]", i);

        p->ppc = this;
        address_space_init(&p->downstream_as, p->downstream, name);

        size = memory_region_size(p->downstream);
        memory_region_init_io(&p->upstream, obj, &tz_ppc_ops,
                              p, name, size);
        sysbus_init_mmio(sbd, &p->upstream);
        g_free(name);
    }
}

static const VMStateField vmstate_tz_ppc_vmstate_fields[] = {
        VMSTATE_BOOL_ARRAY(cfg_nonsec, TZPPC, 16),
        VMSTATE_BOOL_ARRAY(cfg_ap, TZPPC, 16),
        VMSTATE_BOOL(cfg_sec_resp, TZPPC),
        VMSTATE_BOOL(irq_enable, TZPPC),
        VMSTATE_BOOL(irq_clear, TZPPC),
        VMSTATE_BOOL(irq_status, TZPPC),
        VMSTATE_END_OF_LIST()
    };

static const VMStateDescription tz_ppc_vmstate = {
    .name = "tz-ppc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_tz_ppc_vmstate_fields,
};

#define DEFINE_PORT(N)                                          \
    DEFINE_PROP_LINK("port[" #N "]", TZPPC, port[N].downstream, \
                     TYPE_MEMORY_REGION, MemoryRegion *)

static const Property tz_ppc_properties[] = {
    DEFINE_PROP_UINT32("NONSEC_MASK", TZPPC, nonsec_mask, 0),
    DEFINE_PORT(0),
    DEFINE_PORT(1),
    DEFINE_PORT(2),
    DEFINE_PORT(3),
    DEFINE_PORT(4),
    DEFINE_PORT(5),
    DEFINE_PORT(6),
    DEFINE_PORT(7),
    DEFINE_PORT(8),
    DEFINE_PORT(9),
    DEFINE_PORT(10),
    DEFINE_PORT(11),
    DEFINE_PORT(12),
    DEFINE_PORT(13),
    DEFINE_PORT(14),
    DEFINE_PORT(15),
};

void TZPPC::classInit(DeviceClass *dc)
{
    dc->vmsd = &tz_ppc_vmstate;
    device_class_set_props(dc, tz_ppc_properties);
}

REGISTER_QEMU_DEVICE(TZPPC, TYPE_TZ_PPC, TYPE_SYS_BUS_DEVICE)
