/*
 * nRF51 System-on-Chip general purpose input/output register definition
 *
 * Reference Manual: http://infocenter.nordicsemi.com/pdf/nRF51_RM_v3.0.pdf
 * Product Spec: http://infocenter.nordicsemi.com/pdf/nRF51822_PS_v3.1.pdf
 *
 * Copyright 2018 Steffen Görtz <contrib@steffen-goertz.de>
 *
 * This code is licensed under the GPL version 2 or later.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/gpio/nrf51_gpio.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "qom/cpp/object.h"

/*
 * Check if the output driver is connected to the direction switch
 * given the current configuration and logic level.
 * It is not differentiated between standard and "high"(-power) drive modes.
 */
static bool is_connected(uint32_t config, uint32_t level)
{
    bool state;
    uint32_t drive_config = extract32(config, 8, 3);

    switch (drive_config) {
    case 0 ... 3:
        state = true;
        break;
    case 4 ... 5:
        state = level != 0;
        break;
    case 6 ... 7:
        state = level == 0;
        break;
    default:
        g_assert_not_reached();
    }

    return state;
}

static int pull_value(uint32_t config)
{
    int pull = extract32(config, 2, 2);
    if (pull == NRF51_GPIO_PULLDOWN) {
        return 0;
    } else if (pull == NRF51_GPIO_PULLUP) {
        return 1;
    }
    return -1;
}

static void update_output_irq(NRF51GPIOState *s, size_t i,
                              bool connected, bool level)
{
    int64_t irq_level = connected ? level : -1;
    bool old_connected = extract32(s->old_out_connected, i, 1);
    bool old_level = extract32(s->old_out, i, 1);

    if ((old_connected != connected) || (old_level != level)) {
        qemu_set_irq(s->output[i], irq_level);
        trace_nrf51_gpio_update_output_irq(i, irq_level);
    }

    s->old_out = deposit32(s->old_out, i, 1, level);
    s->old_out_connected = deposit32(s->old_out_connected, i, 1, connected);
}

static void update_state(NRF51GPIOState *s)
{
    int pull;
    size_t i;
    bool connected_out, dir, connected_in, out, in, input;
    bool assert_detect = false;

    for (i = 0; i < NRF51_GPIO_PINS; i++) {
        pull = pull_value(s->cnf[i]);
        dir = extract32(s->cnf[i], 0, 1);
        connected_in = extract32(s->in_mask, i, 1);
        out = extract32(s->out, i, 1);
        in = extract32(s->in, i, 1);
        input = !extract32(s->cnf[i], 1, 1);
        connected_out = is_connected(s->cnf[i], out) && dir;

        if (!input) {
            if (pull >= 0) {
                s->in = deposit32(s->in, i, 1, pull);
            }
        } else {
            if (connected_out && connected_in && out != in) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "GPIO pin %zu short circuited\n", i);
            }
            if (connected_in) {
                uint32_t detect_config = extract32(s->cnf[i], 16, 2);
                if ((detect_config == 2) && (in == 1)) {
                    assert_detect = true;
                }
                if ((detect_config == 3) && (in == 0)) {
                    assert_detect = true;
                }
            } else {
                if (pull >= 0 && !connected_out) {
                    connected_out = true;
                    out = pull;
                }
                if (connected_out) {
                    s->in = deposit32(s->in, i, 1, out);
                }
            }
        }
        update_output_irq(s, i, connected_out, out);
    }

    qemu_set_irq(s->detect, assert_detect);
}

static void reflect_dir_bit_in_cnf(NRF51GPIOState *s)
{
    size_t i;

    uint32_t value = s->dir;

    for (i = 0; i < NRF51_GPIO_PINS; i++) {
        s->cnf[i] = (s->cnf[i] & ~(1UL)) | ((value >> i) & 0x01);
    }
}

static uint64_t nrf51_gpio_read(void *opaque, hwaddr offset, unsigned int size)
{
    NRF51GPIOState *s = static_cast<NRF51GPIOState *>(opaque);
    uint64_t r = 0;
    size_t idx;

    switch (offset) {
    case NRF51_GPIO_REG_OUT ... NRF51_GPIO_REG_OUTCLR:
        r = s->out;
        break;

    case NRF51_GPIO_REG_IN:
        r = s->in;
        break;

    case NRF51_GPIO_REG_DIR ... NRF51_GPIO_REG_DIRCLR:
        r = s->dir;
        break;

    case NRF51_GPIO_REG_CNF_START ... NRF51_GPIO_REG_CNF_END:
        idx = (offset - NRF51_GPIO_REG_CNF_START) / 4;
        r = s->cnf[idx];
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }

    trace_nrf51_gpio_read(offset, r);

    return r;
}

static void nrf51_gpio_write(void *opaque, hwaddr offset,
                       uint64_t value, unsigned int size)
{
    NRF51GPIOState *s = static_cast<NRF51GPIOState *>(opaque);
    size_t idx;

    trace_nrf51_gpio_write(offset, value);

    switch (offset) {
    case NRF51_GPIO_REG_OUT:
        s->out = value;
        break;

    case NRF51_GPIO_REG_OUTSET:
        s->out |= value;
        break;

    case NRF51_GPIO_REG_OUTCLR:
        s->out &= ~value;
        break;

    case NRF51_GPIO_REG_DIR:
        s->dir = value;
        reflect_dir_bit_in_cnf(s);
        break;

    case NRF51_GPIO_REG_DIRSET:
        s->dir |= value;
        reflect_dir_bit_in_cnf(s);
        break;

    case NRF51_GPIO_REG_DIRCLR:
        s->dir &= ~value;
        reflect_dir_bit_in_cnf(s);
        break;

    case NRF51_GPIO_REG_CNF_START ... NRF51_GPIO_REG_CNF_END:
        idx = (offset - NRF51_GPIO_REG_CNF_START) / 4;
        s->cnf[idx] = value;
        s->dir = (s->dir & ~(1UL << idx)) | ((value & 0x01) << idx);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }

    update_state(s);
}

static const MemoryRegionOps gpio_ops = {
    .read =  nrf51_gpio_read,
    .write = nrf51_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void nrf51_gpio_set(void *opaque, int line, int value)
{
    NRF51GPIOState *s = static_cast<NRF51GPIOState *>(opaque);

    trace_nrf51_gpio_set(line, value);

    assert(line >= 0 && line < NRF51_GPIO_PINS);

    s->in_mask = deposit32(s->in_mask, line, 1, value >= 0);
    if (value >= 0) {
        s->in = deposit32(s->in, line, 1, value != 0);
    }

    update_state(s);
}

void NRF51GPIOState::reset()
{
    size_t i;

    out = 0;
    old_out = 0;
    old_out_connected = 0;
    in = 0;
    in_mask = 0;
    dir = 0;

    for (i = 0; i < NRF51_GPIO_PINS; i++) {
        cnf[i] = 0x00000002;
    }
}

static const VMStateField vmstate_nrf51_gpio_fields[] = {
    VMSTATE_UINT32(out, NRF51GPIOState),
    VMSTATE_UINT32(in, NRF51GPIOState),
    VMSTATE_UINT32(in_mask, NRF51GPIOState),
    VMSTATE_UINT32(dir, NRF51GPIOState),
    VMSTATE_UINT32_ARRAY(cnf, NRF51GPIOState, NRF51_GPIO_PINS),
    VMSTATE_UINT32(old_out, NRF51GPIOState),
    VMSTATE_UINT32(old_out_connected, NRF51GPIOState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_nrf51_gpio = {
    .name = TYPE_NRF51_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_nrf51_gpio_fields,
};

void NRF51GPIOState::init()
{
    memory_region_init_io(&mmio, OBJECT(this), &gpio_ops, this,
            TYPE_NRF51_GPIO, NRF51_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &mmio);

    qdev_init_gpio_in(DEVICE(this), nrf51_gpio_set, NRF51_GPIO_PINS);
    qdev_init_gpio_out(DEVICE(this), output, NRF51_GPIO_PINS);
    qdev_init_gpio_out_named(DEVICE(this), &detect, "detect", 1);
}

void NRF51GPIOState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_nrf51_gpio;
    dc->desc = "nRF51 GPIO";
}

REGISTER_QEMU_DEVICE(NRF51GPIOState, TYPE_NRF51_GPIO, TYPE_SYS_BUS_DEVICE)
