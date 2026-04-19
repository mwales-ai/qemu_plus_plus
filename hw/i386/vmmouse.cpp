/*
 * QEMU VMMouse emulation
 *
 * Copyright (C) 2007 Anthony Liguori <anthony@codemonkey.ws>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

extern "C" {
#include "qapi/error.h"
#include "ui/console.h"
#include "hw/i386/vmport.h"
#include "hw/input/i8042.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "cpu.h"
#include "qom/object.h"
}

#include "trace.h"

/* debug only vmmouse */
//#define DEBUG_VMMOUSE

#define VMMOUSE_READ_ID			0x45414552
#define VMMOUSE_DISABLE			0x000000f5
#define VMMOUSE_REQUEST_RELATIVE	0x4c455252
#define VMMOUSE_REQUEST_ABSOLUTE	0x53424152

#define VMMOUSE_QUEUE_SIZE	1024

#define VMMOUSE_VERSION		0x3442554a

#define VMMOUSE_RELATIVE_PACKET    0x00010000

#define VMMOUSE_LEFT_BUTTON        0x20
#define VMMOUSE_RIGHT_BUTTON       0x10
#define VMMOUSE_MIDDLE_BUTTON      0x08

#define VMMOUSE_MIN_X 0
#define VMMOUSE_MIN_Y 0
#define VMMOUSE_MAX_X 0xFFFF
#define VMMOUSE_MAX_Y 0xFFFF

#define TYPE_VMMOUSE "vmmouse"
OBJECT_DECLARE_SIMPLE_TYPE(VMMouseState, VMMOUSE)

struct VMMouseState {
    ISADevice parent_obj;

    uint32_t queue[VMMOUSE_QUEUE_SIZE];
    int32_t queue_size;
    uint16_t nb_queue;
    uint16_t status;
    uint8_t absolute;
    QEMUPutMouseEntry *entry;
    ISAKBDState *i8042;

    /* methods */
    uint32_t getStatus();
    void readId();
    void requestRelative();
    void requestAbsolute();
    void disable();
    void getData(uint32_t *data, uint32_t size);
    void removeHandler();
    void updateHandler(int absolute);

    static void mouseEvent(void *opaque, int x, int y, int dz, int buttons_state);
    static uint32_t ioportRead(void *opaque, uint32_t addr);
    static int postLoad(void *opaque, int version_id);

    void reset();
    void realize(Error **errp);
    static void classInit(DeviceClass *dc);
};

static void vmmouse_get_data(uint32_t *data)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    CPUX86State *env = &cpu->env;

    data[0] = env->regs[R_EAX]; data[1] = env->regs[R_EBX];
    data[2] = env->regs[R_ECX]; data[3] = env->regs[R_EDX];
    data[4] = env->regs[R_ESI]; data[5] = env->regs[R_EDI];
}

static void vmmouse_set_data(const uint32_t *data)
{
    X86CPU *cpu = X86_CPU(current_cpu);
    CPUX86State *env = &cpu->env;

    env->regs[R_EAX] = data[0]; env->regs[R_EBX] = data[1];
    env->regs[R_ECX] = data[2]; env->regs[R_EDX] = data[3];
    env->regs[R_ESI] = data[4]; env->regs[R_EDI] = data[5];
}

uint32_t VMMouseState::getStatus()
{
    trace_vmmouse_get_status();

    return (status << 16) | nb_queue;
}

void VMMouseState::mouseEvent(void *opaque, int x, int y, int dz, int buttons_state)
{
    VMMouseState *s = static_cast<VMMouseState *>(opaque);
    int buttons = 0;

    if (s->nb_queue > (VMMOUSE_QUEUE_SIZE - 4))
        return;

    trace_vmmouse_mouse_event(x, y, dz, buttons_state);

    if ((buttons_state & MOUSE_EVENT_LBUTTON))
        buttons |= VMMOUSE_LEFT_BUTTON;
    if ((buttons_state & MOUSE_EVENT_RBUTTON))
        buttons |= VMMOUSE_RIGHT_BUTTON;
    if ((buttons_state & MOUSE_EVENT_MBUTTON))
        buttons |= VMMOUSE_MIDDLE_BUTTON;

    if (s->absolute) {
        x = qemu_input_scale_axis(x,
                                  INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX,
                                  VMMOUSE_MIN_X, VMMOUSE_MAX_X);
        y = qemu_input_scale_axis(y,
                                  INPUT_EVENT_ABS_MIN, INPUT_EVENT_ABS_MAX,
                                  VMMOUSE_MIN_Y, VMMOUSE_MAX_Y);
    } else{
        /* add for guest vmmouse driver to judge this is a relative packet. */
        buttons |= VMMOUSE_RELATIVE_PACKET;
    }

    s->queue[s->nb_queue++] = buttons;
    s->queue[s->nb_queue++] = x;
    s->queue[s->nb_queue++] = y;
    s->queue[s->nb_queue++] = dz;

    /* need to still generate PS2 events to notify driver to
       read from queue */
    i8042_isa_mouse_fake_event(s->i8042);
}

void VMMouseState::removeHandler()
{
    if (entry) {
        qemu_remove_mouse_event_handler(entry);
        entry = NULL;
    }
}

void VMMouseState::updateHandler(int abs)
{
    if (status != 0) {
        return;
    }
    if (absolute != abs) {
        absolute = abs;
        removeHandler();
    }
    if (entry == NULL) {
        entry = qemu_add_mouse_event_handler(VMMouseState::mouseEvent,
                                                this, absolute,
                                                "vmmouse");
        qemu_activate_mouse_event_handler(entry);
    }
}

void VMMouseState::readId()
{
    trace_vmmouse_read_id();

    if (nb_queue == VMMOUSE_QUEUE_SIZE)
        return;

    queue[nb_queue++] = VMMOUSE_VERSION;
    status = 0;
    updateHandler(absolute);
}

void VMMouseState::requestRelative()
{
    trace_vmmouse_request_relative();

    updateHandler(0);
}

void VMMouseState::requestAbsolute()
{
    trace_vmmouse_request_absolute();

    updateHandler(1);
}

void VMMouseState::disable()
{
    trace_vmmouse_disable();

    status = 0xffff;
    removeHandler();
}

void VMMouseState::getData(uint32_t *data, uint32_t size)
{
    int i;

    trace_vmmouse_data(size);

    if (size == 0 || size > 6 || size > nb_queue) {
        printf("vmmouse: driver requested too much data %d\n", size);
        status = 0xffff;
        removeHandler();
        return;
    }

    for (i = 0; i < size; i++)
        data[i] = queue[i];

    nb_queue -= size;
    if (nb_queue)
        memmove(queue, &queue[size], sizeof(queue[0]) * nb_queue);
}

uint32_t VMMouseState::ioportRead(void *opaque, uint32_t addr)
{
    VMMouseState *s = static_cast<VMMouseState *>(opaque);
    uint32_t data[6];
    uint16_t command;

    vmmouse_get_data(data);

    command = data[2] & 0xFFFF;

    switch (command) {
    case VMPORT_CMD_VMMOUSE_STATUS:
        data[0] = s->getStatus();
        break;
    case VMPORT_CMD_VMMOUSE_COMMAND:
        switch (data[1]) {
        case VMMOUSE_DISABLE:
            s->disable();
            break;
        case VMMOUSE_READ_ID:
            s->readId();
            break;
        case VMMOUSE_REQUEST_RELATIVE:
            s->requestRelative();
            break;
        case VMMOUSE_REQUEST_ABSOLUTE:
            s->requestAbsolute();
            break;
        default:
            printf("vmmouse: unknown command %x\n", data[1]);
            break;
        }
        break;
    case VMPORT_CMD_VMMOUSE_DATA:
        s->getData(data, data[1]);
        break;
    default:
        printf("vmmouse: unknown command %x\n", command);
        break;
    }

    vmmouse_set_data(data);
    return data[0];
}

int VMMouseState::postLoad(void *opaque, int version_id)
{
    VMMouseState *s = static_cast<VMMouseState *>(opaque);

    s->removeHandler();
    s->updateHandler(s->absolute);
    return 0;
}

static const VMStateField vmstate_vmmouse_fields[] = {
    VMSTATE_INT32_EQUAL(queue_size, VMMouseState, NULL),
    VMSTATE_UINT32_ARRAY(queue, VMMouseState, VMMOUSE_QUEUE_SIZE),
    VMSTATE_UINT16(nb_queue, VMMouseState),
    VMSTATE_UINT16(status, VMMouseState),
    VMSTATE_UINT8(absolute, VMMouseState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_vmmouse = {
    .name = "vmmouse",
    .version_id = 0,
    .minimum_version_id = 0,
    .post_load = VMMouseState::postLoad,
    .fields = vmstate_vmmouse_fields,
};

void VMMouseState::reset()
{
    queue_size = VMMOUSE_QUEUE_SIZE;
    nb_queue = 0;

    disable();
}

void VMMouseState::realize(Error **errp)
{
    trace_vmmouse_init();

    if (!i8042) {
        error_setg(errp, "'i8042' link is not set");
        return;
    }
    if (!object_resolve_path_type("", TYPE_VMPORT, NULL)) {
        error_setg(errp, "vmmouse needs a machine with vmport");
        return;
    }

    vmport_register(VMPORT_CMD_VMMOUSE_STATUS, VMMouseState::ioportRead, this);
    vmport_register(VMPORT_CMD_VMMOUSE_COMMAND, VMMouseState::ioportRead, this);
    vmport_register(VMPORT_CMD_VMMOUSE_DATA, VMMouseState::ioportRead, this);
}

static const Property vmmouse_properties[] = {
    DEFINE_PROP_LINK("i8042", VMMouseState, i8042, TYPE_I8042, ISAKBDState *),
};

void VMMouseState::classInit(DeviceClass *dc)
{
    dc->vmsd = &vmstate_vmmouse;
    device_class_set_props(dc, vmmouse_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(VMMouseState, TYPE_VMMOUSE, TYPE_ISA_DEVICE)
