/*
 * *AT24C* series I2C EEPROM
 *
 * Copyright (c) 2015 Michael Davidsaver
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the LICENSE file in the top-level directory.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/nvram/eeprom_at24c.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/block-backend.h"
#include "qom/object.h"

/* #define DEBUG_AT24C */

#ifdef DEBUG_AT24C
#define DPRINTK(FMT, ...) printf(TYPE_AT24C_EE " : " FMT, ## __VA_ARGS__)
#else
#define DPRINTK(FMT, ...) do {} while (0)
#endif

#define TYPE_AT24C_EE "at24c-eeprom"
OBJECT_DECLARE_SIMPLE_TYPE(EEPROMState, AT24C_EE)

struct EEPROMState {
    I2CSlave parent_obj;

    /* address counter */
    uint16_t cur;
    /* total size in bytes */
    uint32_t rsize;
    /*
     * address byte number
     *  for  24c01, 24c02 size <= 256 byte, use only 1 byte
     *  otherwise size > 256, use 2 byte
     */
    uint8_t asize;

    bool writable;
    /* cells changed since last START? */
    bool changed;
    /* during WRITE, # of address bytes transferred */
    uint8_t haveaddr;

    uint8_t *mem;

    BlockBackend *blk;

    const uint8_t *init_rom;
    uint32_t init_rom_size;

    static int event(I2CSlave *s, enum i2c_event event)
    {
        EEPROMState *ee = reinterpret_cast<EEPROMState *>(s);

        switch (event) {
        case I2C_START_SEND:
        case I2C_FINISH:
            ee->haveaddr = 0;
            /* fallthrough */
        case I2C_START_RECV:
            DPRINTK("clear\n");
            if (ee->blk && ee->changed) {
                int ret = blk_pwrite(ee->blk, 0, ee->rsize, ee->mem, static_cast<BdrvRequestFlags>(0));
                if (ret < 0) {
                    error_report("%s: failed to write backing file", __func__);
                }
                DPRINTK("Wrote to backing file\n");
            }
            ee->changed = false;
            break;
        case I2C_NACK:
            break;
        default:
            return -1;
        }
        return 0;
    }

    static uint8_t recv(I2CSlave *s)
    {
        EEPROMState *ee = reinterpret_cast<EEPROMState *>(s);
        uint8_t ret;

        /*
         * If got the byte address but not completely with address size
         * will return the invalid value
         */
        if (ee->haveaddr > 0 && ee->haveaddr < ee->asize) {
            return 0xff;
        }

        ret = ee->mem[ee->cur];

        ee->cur = (ee->cur + 1u) % ee->rsize;
        DPRINTK("Recv %02x %c\n", ret, ret);

        return ret;
    }

    static int send(I2CSlave *s, uint8_t data)
    {
        EEPROMState *ee = reinterpret_cast<EEPROMState *>(s);

        if (ee->haveaddr < ee->asize) {
            ee->cur <<= 8;
            ee->cur |= data;
            ee->haveaddr++;
            if (ee->haveaddr == ee->asize) {
                ee->cur %= ee->rsize;
                DPRINTK("Set pointer %04x\n", ee->cur);
            }

        } else {
            if (ee->writable) {
                DPRINTK("Send %02x\n", data);
                ee->mem[ee->cur] = data;
                ee->changed = true;
            } else {
                DPRINTK("Send error %02x read-only\n", data);
            }
            ee->cur = (ee->cur + 1u) % ee->rsize;

        }

        return 0;
    }

    void realize(Error **errp)
    {
        if (init_rom_size > rsize) {
            error_setg(errp, "%s: init rom is larger than rom: %u > %u",
                       TYPE_AT24C_EE, init_rom_size, rsize);
            return;
        }

        if (blk) {
            int64_t len = blk_getlength(blk);

            if (len != rsize) {
                error_setg(errp, "%s: Backing file size %" PRId64 " != %u",
                           TYPE_AT24C_EE, len, rsize);
                return;
            }

            if (blk_set_perm(blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                             BLK_PERM_ALL, &error_fatal) < 0)
            {
                error_setg(errp, "%s: Backing file incorrect permission",
                           TYPE_AT24C_EE);
                return;
            }
        }

        mem = static_cast<uint8_t *>(g_malloc0(rsize));

        if (blk) {
            int ret = blk_pread(blk, 0, rsize, mem, static_cast<BdrvRequestFlags>(0));

            if (ret < 0) {
                error_setg(errp, "%s: Failed initial sync with backing file",
                           TYPE_AT24C_EE);
                return;
            }
            DPRINTK("Reset read backing file\n");
        } else if (init_rom) {
            memcpy(mem, init_rom, MIN(init_rom_size, rsize));
        }

        /*
         * If address size didn't define with property set
         *   value is 0 as default, setting it by Rom size detecting.
         */
        if (asize == 0) {
            if (rsize <= 256) {
                asize = 1;
            } else {
                asize = 2;
            }
        }
    }

    void reset()
    {
        changed = false;
        cur = 0;
        haveaddr = 0;
    }

    static void classInit(DeviceClass *dc);
};

extern "C"
I2CSlave *at24c_eeprom_init(I2CBus *bus, uint8_t address, uint32_t rom_size)
{
    return at24c_eeprom_init_rom(bus, address, rom_size, NULL, 0);
}

extern "C"
I2CSlave *at24c_eeprom_init_rom(I2CBus *bus, uint8_t address, uint32_t rom_size,
                                const uint8_t *init_rom, uint32_t init_rom_size)
{
    EEPROMState *s;

    s = reinterpret_cast<EEPROMState *>(i2c_slave_new(TYPE_AT24C_EE, address));

    qdev_prop_set_uint32(reinterpret_cast<DeviceState *>(s), "rom-size", rom_size);

    /* TODO: Model init_rom with QOM properties. */
    s->init_rom = init_rom;
    s->init_rom_size = init_rom_size;

    i2c_slave_realize_and_unref(reinterpret_cast<I2CSlave *>(s), bus, &error_abort);

    return reinterpret_cast<I2CSlave *>(s);
}

static const Property at24c_eeprom_props[] = {
    DEFINE_PROP_UINT32("rom-size", EEPROMState, rsize, 0),
    DEFINE_PROP_UINT8("address-size", EEPROMState, asize, 0),
    DEFINE_PROP_BOOL("writable", EEPROMState, writable, true),
    DEFINE_PROP_DRIVE("drive", EEPROMState, blk),
};

void EEPROMState::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    I2CSlaveClass *k = reinterpret_cast<I2CSlaveClass *>(klass);

    k->event = EEPROMState::event;
    k->recv = EEPROMState::recv;
    k->send = EEPROMState::send;

    device_class_set_props(dc, at24c_eeprom_props);
}

#include "qom/cpp/object.h"

REGISTER_QEMU_DEVICE_CLASS_SIZE(EEPROMState, I2CSlaveClass,
                                TYPE_AT24C_EE, TYPE_I2C_SLAVE)
