#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/display/ramfb.h"
#include "ui/console.h"
#include "qom/object.h"

typedef struct RAMFBStandaloneState RAMFBStandaloneState;
DECLARE_INSTANCE_CHECKER(RAMFBStandaloneState, RAMFB,
                         TYPE_RAMFB_DEVICE)

struct RAMFBStandaloneState {
    SysBusDevice parent_obj;
    QemuConsole *con;
    RAMFBState *state;
    bool migrate;
    bool use_legacy_x86_rom;

    /* Instance methods */
    void realize(Error **errp);

    /* Static callbacks */
    static void displayUpdate(void *dev);
    static bool migrateNeeded(void *opaque);

    /* Class methods */
    static void classInit(ObjectClass *klass, const void *data);
};

void RAMFBStandaloneState::displayUpdate(void *dev)
{
    RAMFBStandaloneState *ramfb = reinterpret_cast<RAMFBStandaloneState *>(dev);

    if (0 /* native driver active */) {
        /* non-standalone device would run native display update here */;
    } else {
        ramfb_display_update(ramfb->con, ramfb->state);
    }
}

static const GraphicHwOps wrapper_ops = {
    .gfx_update = RAMFBStandaloneState::displayUpdate,
};

static void ramfb_realizefn(DeviceState *dev, Error **errp)
{
    RAMFBStandaloneState *ramfb = reinterpret_cast<RAMFBStandaloneState *>(dev);
    ramfb->realize(errp);
}

void RAMFBStandaloneState::realize(Error **errp)
{
    con = graphic_console_init(reinterpret_cast<DeviceState *>(this), 0,
                               &wrapper_ops, this);
    state = ramfb_setup(use_legacy_x86_rom, errp);
}

bool RAMFBStandaloneState::migrateNeeded(void *opaque)
{
    RAMFBStandaloneState *ramfb =
        reinterpret_cast<RAMFBStandaloneState *>(opaque);

    return ramfb->migrate;
}

static const VMStateField ramfb_dev_vmstate_fields[] = {
    VMSTATE_STRUCT_POINTER(state, RAMFBStandaloneState, ramfb_vmstate, RAMFBState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription ramfb_dev_vmstate = {
    .name = "ramfb-dev",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = RAMFBStandaloneState::migrateNeeded,
    .fields = ramfb_dev_vmstate_fields,
};

static const Property ramfb_properties[] = {
    DEFINE_PROP_BOOL("x-migrate", RAMFBStandaloneState, migrate,  true),
    DEFINE_PROP_BOOL("use-legacy-x86-rom", RAMFBStandaloneState,
                     use_legacy_x86_rom, false),
};

void RAMFBStandaloneState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    dc->vmsd = &ramfb_dev_vmstate;
    dc->realize = ramfb_realizefn;
    dc->desc = "ram framebuffer standalone device";
    device_class_set_props(dc, ramfb_properties);
}

static const TypeInfo ramfb_info = {
    .name          = TYPE_RAMFB_DEVICE,
    .parent        = TYPE_DYNAMIC_SYS_BUS_DEVICE,
    .instance_size = sizeof(RAMFBStandaloneState),
    .class_init    = RAMFBStandaloneState::classInit,
};

static void ramfb_register_types(void)
{
    type_register_static(&ramfb_info);
}

type_init(ramfb_register_types)
