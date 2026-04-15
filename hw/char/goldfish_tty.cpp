/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Goldfish TTY
 *
 * (c) 2020 Laurent Vivier <laurent@vivier.eu>
 *
 */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "qemu/log.h"
#include "trace.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "hw/char/goldfish_tty.h"
#include "qom/cpp/object.h"

#define GOLDFISH_TTY_VERSION 1

/* registers */

enum {
    REG_PUT_CHAR      = 0x00,
    REG_BYTES_READY   = 0x04,
    REG_CMD           = 0x08,
    REG_DATA_PTR      = 0x10,
    REG_DATA_LEN      = 0x14,
    REG_DATA_PTR_HIGH = 0x18,
    REG_VERSION       = 0x20,
};

/* commands */

enum {
    CMD_INT_DISABLE   = 0x00,
    CMD_INT_ENABLE    = 0x01,
    CMD_WRITE_BUFFER  = 0x02,
    CMD_READ_BUFFER   = 0x03,
};

static uint64_t goldfish_tty_read(void *opaque, hwaddr addr,
                                  unsigned size)
{
    GoldfishTTYState *s = static_cast<GoldfishTTYState *>(opaque);
    uint64_t value = 0;

    switch (addr) {
    case REG_BYTES_READY:
        value = fifo8_num_used(&s->rx_fifo);
        break;
    case REG_VERSION:
        value = GOLDFISH_TTY_VERSION;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented register read 0x%02"HWADDR_PRIx"\n",
                      __func__, addr);
        break;
    }

    trace_goldfish_tty_read(s, addr, size, value);

    return value;
}

static void goldfish_tty_cmd(GoldfishTTYState *s, uint32_t cmd)
{
    uint32_t to_copy;
    uint8_t data_out[GOLFISH_TTY_BUFFER_SIZE];
    int len;
    uint64_t ptr;

    switch (cmd) {
    case CMD_INT_DISABLE:
        if (s->int_enabled) {
            if (!fifo8_is_empty(&s->rx_fifo)) {
                qemu_set_irq(s->irq, 0);
            }
            s->int_enabled = false;
        }
        break;
    case CMD_INT_ENABLE:
        if (!s->int_enabled) {
            if (!fifo8_is_empty(&s->rx_fifo)) {
                qemu_set_irq(s->irq, 1);
            }
            s->int_enabled = true;
        }
        break;
    case CMD_WRITE_BUFFER:
        len = s->data_len;
        ptr = s->data_ptr;
        while (len) {
            to_copy = MIN(GOLFISH_TTY_BUFFER_SIZE, len);

            dma_memory_read_relaxed(&address_space_memory, ptr,
                                    data_out, to_copy);
            qemu_chr_fe_write_all(&s->chr, data_out, to_copy);

            len -= to_copy;
            ptr += to_copy;
        }
        break;
    case CMD_READ_BUFFER:
        len = s->data_len;
        ptr = s->data_ptr;
        while (len && !fifo8_is_empty(&s->rx_fifo)) {
            const uint8_t *buf = fifo8_pop_bufptr(&s->rx_fifo, len, &to_copy);

            dma_memory_write_relaxed(&address_space_memory, ptr, buf, to_copy);

            len -= to_copy;
            ptr += to_copy;
        }
        if (s->int_enabled && fifo8_is_empty(&s->rx_fifo)) {
            qemu_set_irq(s->irq, 0);
        }
        break;
    }
}

static void goldfish_tty_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    GoldfishTTYState *s = static_cast<GoldfishTTYState *>(opaque);
    unsigned char c;

    trace_goldfish_tty_write(s, addr, size, value);

    switch (addr) {
    case REG_PUT_CHAR:
        c = value;
        qemu_chr_fe_write_all(&s->chr, &c, sizeof(c));
        break;
    case REG_CMD:
        goldfish_tty_cmd(s, value);
        break;
    case REG_DATA_PTR:
        s->data_ptr = value;
        break;
    case REG_DATA_PTR_HIGH:
        s->data_ptr = deposit64(s->data_ptr, 32, 32, value);
        break;
    case REG_DATA_LEN:
        s->data_len = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented register write 0x%02"HWADDR_PRIx"\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps goldfish_tty_ops = {
    .read = goldfish_tty_read,
    .write = goldfish_tty_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static int goldfish_tty_can_receive(void *opaque)
{
    GoldfishTTYState *s = static_cast<GoldfishTTYState *>(opaque);
    int available = fifo8_num_free(&s->rx_fifo);

    trace_goldfish_tty_can_receive(s, available);

    return available;
}

static void goldfish_tty_receive(void *opaque, const uint8_t *buffer, int size)
{
    GoldfishTTYState *s = static_cast<GoldfishTTYState *>(opaque);

    trace_goldfish_tty_receive(s, size);

    g_assert(size <= fifo8_num_free(&s->rx_fifo));

    fifo8_push_all(&s->rx_fifo, buffer, size);

    if (s->int_enabled && !fifo8_is_empty(&s->rx_fifo)) {
        qemu_set_irq(s->irq, 1);
    }
}

void GoldfishTTYState::reset()
{
    trace_goldfish_tty_reset(this);

    fifo8_reset(&rx_fifo);
    int_enabled = false;
    data_ptr = 0;
    data_len = 0;
}

void GoldfishTTYState::realize(Error **errp)
{
    trace_goldfish_tty_realize(this);

    fifo8_create(&rx_fifo, GOLFISH_TTY_BUFFER_SIZE);
    memory_region_init_io(&iomem, reinterpret_cast<Object *>(this),
                          &goldfish_tty_ops, this, "goldfish_tty", 0x24);

    if (qemu_chr_fe_backend_connected(&chr)) {
        qemu_chr_fe_set_handlers(&chr, goldfish_tty_can_receive,
                                 goldfish_tty_receive, NULL, NULL,
                                 this, NULL, true);
    }
}

static void goldfish_tty_unrealize(DeviceState *dev)
{
    GoldfishTTYState *s = GOLDFISH_TTY(dev);

    trace_goldfish_tty_unrealize(s);

    fifo8_destroy(&s->rx_fifo);
}

static const VMStateField vmstate_goldfish_tty_fields[] = {
    VMSTATE_UINT32(data_len, GoldfishTTYState),
    VMSTATE_UINT64(data_ptr, GoldfishTTYState),
    VMSTATE_BOOL(int_enabled, GoldfishTTYState),
    VMSTATE_FIFO8(rx_fifo, GoldfishTTYState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_goldfish_tty = {
    .name = "goldfish_tty",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_goldfish_tty_fields
};

static const Property goldfish_tty_properties[] = {
    DEFINE_PROP_CHR("chardev", GoldfishTTYState, chr),
};

void GoldfishTTYState::init()
{
    SysBusDevice *dev = reinterpret_cast<SysBusDevice *>(this);

    trace_goldfish_tty_instance_init(this);

    sysbus_init_mmio(dev, &iomem);
    sysbus_init_irq(dev, &irq);
}

void GoldfishTTYState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, goldfish_tty_properties);
    dc->unrealize = goldfish_tty_unrealize;
    dc->vmsd = &vmstate_goldfish_tty;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

REGISTER_QEMU_DEVICE(GoldfishTTYState, TYPE_GOLDFISH_TTY, TYPE_SYS_BUS_DEVICE)
