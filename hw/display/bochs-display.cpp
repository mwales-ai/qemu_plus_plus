/*
 * QEMU PCI bochs display adapter.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/display/bochs-vbe.h"
#include "hw/display/edid.h"

#include "qapi/error.h"

#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"

typedef struct BochsDisplayMode {
    pixman_format_code_t format;
    uint32_t             bytepp;
    uint32_t             width;
    uint32_t             height;
    uint32_t             stride;
    uint64_t             offset;
    uint64_t             size;
} BochsDisplayMode;

struct BochsDisplayState {
    /* parent */
    PCIDevice        pci;

    /* device elements */
    QemuConsole      *con;
    MemoryRegion     vram;
    MemoryRegion     mmio;
    MemoryRegion     vbe;
    MemoryRegion     qext;
    MemoryRegion     edid;

    /* device config */
    uint64_t         vgamem;
    bool             enable_edid;
    qemu_edid_info   edid_info;
    uint8_t          edid_blob[256];

    /* device registers */
    uint16_t         vbe_regs[VBE_DISPI_INDEX_NB];
    bool             big_endian_fb;

    /* device state */
    BochsDisplayMode mode;

    /* Static MMIO callbacks */
    static uint64_t vbeRead(void *ptr, hwaddr addr, unsigned size);
    static void vbeWrite(void *ptr, hwaddr addr, uint64_t val, unsigned size);
    static uint64_t qextRead(void *ptr, hwaddr addr, unsigned size);
    static void qextWrite(void *ptr, hwaddr addr, uint64_t val, unsigned size);

    /* Static display callback */
    static void updateDisplay(void *opaque);

    /* Instance methods */
    int getMode(BochsDisplayMode *mode);
    void realize(Error **errp);
    void exit();
    void initfn();

    /* Static QOM property callbacks */
    static bool getBigEndianFb(Object *obj, Error **errp);
    static void setBigEndianFb(Object *obj, bool value, Error **errp);

    /* Class init */
    static void classInit(ObjectClass *klass, const void *data);
};

#define TYPE_BOCHS_DISPLAY "bochs-display"
OBJECT_DECLARE_SIMPLE_TYPE(BochsDisplayState, BOCHS_DISPLAY)

static const VMStateField vmstate_bochs_display_fields[] = {
    VMSTATE_PCI_DEVICE(pci, BochsDisplayState),
    VMSTATE_UINT16_ARRAY(vbe_regs, BochsDisplayState, VBE_DISPI_INDEX_NB),
    VMSTATE_BOOL(big_endian_fb, BochsDisplayState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_bochs_display = {
    .name = "bochs-display",
    .fields = vmstate_bochs_display_fields,
};

uint64_t BochsDisplayState::vbeRead(void *ptr, hwaddr addr, unsigned size)
{
    BochsDisplayState *s = static_cast<BochsDisplayState *>(ptr);
    unsigned int index = addr >> 1;

    switch (index) {
    case VBE_DISPI_INDEX_ID:
        return VBE_DISPI_ID5;
    case VBE_DISPI_INDEX_VIDEO_MEMORY_64K:
        return s->vgamem / (64 * KiB);
    }

    if (index >= ARRAY_SIZE(s->vbe_regs)) {
        return -1;
    }
    return s->vbe_regs[index];
}

void BochsDisplayState::vbeWrite(void *ptr, hwaddr addr,
                                  uint64_t val, unsigned size)
{
    BochsDisplayState *s = static_cast<BochsDisplayState *>(ptr);
    unsigned int index = addr >> 1;

    if (index >= ARRAY_SIZE(s->vbe_regs)) {
        return;
    }
    s->vbe_regs[index] = val;
}

static const MemoryRegionOps bochs_display_vbe_ops = {
    .read = BochsDisplayState::vbeRead,
    .write = BochsDisplayState::vbeWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

uint64_t BochsDisplayState::qextRead(void *ptr, hwaddr addr, unsigned size)
{
    BochsDisplayState *s = static_cast<BochsDisplayState *>(ptr);

    switch (addr) {
    case PCI_VGA_QEXT_REG_SIZE:
        return PCI_VGA_QEXT_SIZE;
    case PCI_VGA_QEXT_REG_BYTEORDER:
        return s->big_endian_fb ?
            PCI_VGA_QEXT_BIG_ENDIAN : PCI_VGA_QEXT_LITTLE_ENDIAN;
    default:
        return 0;
    }
}

void BochsDisplayState::qextWrite(void *ptr, hwaddr addr,
                                   uint64_t val, unsigned size)
{
    BochsDisplayState *s = static_cast<BochsDisplayState *>(ptr);

    switch (addr) {
    case PCI_VGA_QEXT_REG_BYTEORDER:
        if (val == PCI_VGA_QEXT_BIG_ENDIAN) {
            s->big_endian_fb = true;
        }
        if (val == PCI_VGA_QEXT_LITTLE_ENDIAN) {
            s->big_endian_fb = false;
        }
        break;
    }
}

static const MemoryRegionOps bochs_display_qext_ops = {
    .read = BochsDisplayState::qextRead,
    .write = BochsDisplayState::qextWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

int BochsDisplayState::getMode(BochsDisplayMode *mode)
{
    uint16_t *vbe = vbe_regs;
    uint32_t virt_width;

    if (!(vbe[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED)) {
        return -1;
    }

    memset(mode, 0, sizeof(*mode));
    switch (vbe[VBE_DISPI_INDEX_BPP]) {
    case 16:
        /* best effort: support native endianness only */
        mode->format = PIXMAN_r5g6b5;
        mode->bytepp = 2;
        break;
    case 32:
        mode->format = big_endian_fb
            ? PIXMAN_BE_x8r8g8b8
            : PIXMAN_LE_x8r8g8b8;
        mode->bytepp = 4;
        break;
    default:
        return -1;
    }

    mode->width  = vbe[VBE_DISPI_INDEX_XRES];
    mode->height = vbe[VBE_DISPI_INDEX_YRES];
    virt_width  = vbe[VBE_DISPI_INDEX_VIRT_WIDTH];
    if (virt_width < mode->width) {
        virt_width = mode->width;
    }
    mode->stride = virt_width * mode->bytepp;
    mode->size   = static_cast<uint64_t>(mode->stride) * mode->height;
    mode->offset = (static_cast<uint64_t>(vbe[VBE_DISPI_INDEX_X_OFFSET]) *
                    mode->bytepp +
                    static_cast<uint64_t>(vbe[VBE_DISPI_INDEX_Y_OFFSET]) *
                    mode->stride);

    if (mode->width < 64 || mode->height < 64) {
        return -1;
    }
    if (mode->offset + mode->size > vgamem) {
        return -1;
    }
    return 0;
}

void BochsDisplayState::updateDisplay(void *opaque)
{
    BochsDisplayState *s = static_cast<BochsDisplayState *>(opaque);
    DirtyBitmapSnapshot *snap = NULL;
    bool full_update = false;
    BochsDisplayMode mode;
    DisplaySurface *ds;
    uint8_t *ptr;
    bool dirty;
    int y, ys, ret;

    ret = s->getMode(&mode);
    if (ret < 0) {
        /* no (valid) video mode */
        return;
    }

    if (memcmp(&s->mode, &mode, sizeof(mode)) != 0) {
        /* video mode switch */
        s->mode = mode;
        ptr = static_cast<uint8_t *>(memory_region_get_ram_ptr(&s->vram));
        ds = qemu_create_displaysurface_from(mode.width,
                                             mode.height,
                                             mode.format,
                                             mode.stride,
                                             ptr + mode.offset);
        dpy_gfx_replace_surface(s->con, ds);
        full_update = true;
    }

    if (full_update) {
        dpy_gfx_update_full(s->con);
    } else {
        snap = memory_region_snapshot_and_clear_dirty(&s->vram,
                                                      mode.offset, mode.size,
                                                      DIRTY_MEMORY_VGA);
        ys = -1;
        for (y = 0; y < static_cast<int>(mode.height); y++) {
            dirty = memory_region_snapshot_get_dirty(&s->vram, snap,
                                                     mode.offset + mode.stride * y,
                                                     mode.stride);
            if (dirty && ys < 0) {
                ys = y;
            }
            if (!dirty && ys >= 0) {
                dpy_gfx_update(s->con, 0, ys,
                               mode.width, y - ys);
                ys = -1;
            }
        }
        if (ys >= 0) {
            dpy_gfx_update(s->con, 0, ys,
                           mode.width, y - ys);
        }

        g_free(snap);
    }
}

static const GraphicHwOps bochs_display_gfx_ops = {
    .gfx_update = BochsDisplayState::updateDisplay,
};

void BochsDisplayState::realize(Error **errp)
{
    PCIDevice *dev = reinterpret_cast<PCIDevice *>(this);
    Object *obj = reinterpret_cast<Object *>(this);
    int ret;

    if (vgamem < 4 * MiB) {
        error_setg(errp, "bochs-display: video memory too small");
        return;
    }
    if (vgamem > 256 * MiB) {
        error_setg(errp, "bochs-display: video memory too big");
        return;
    }
    vgamem = pow2ceil(vgamem);

    con = graphic_console_init(reinterpret_cast<DeviceState *>(dev), 0, &bochs_display_gfx_ops, this);

    memory_region_init_ram(&vram, obj, "bochs-display-vram", vgamem,
                           &error_fatal);
    memory_region_init_io(&vbe, obj, &bochs_display_vbe_ops, this,
                          "bochs dispi interface", PCI_VGA_BOCHS_SIZE);
    memory_region_init_io(&qext, obj, &bochs_display_qext_ops, this,
                          "qemu extended regs", PCI_VGA_QEXT_SIZE);

    memory_region_init_io(&mmio, obj, &unassigned_io_ops, NULL,
                          "bochs-display-mmio", PCI_VGA_MMIO_SIZE);
    memory_region_add_subregion(&mmio, PCI_VGA_BOCHS_OFFSET, &vbe);
    memory_region_add_subregion(&mmio, PCI_VGA_QEXT_OFFSET, &qext);

    pci_set_byte(&pci.config[PCI_REVISION_ID], 2);
    pci_register_bar(&pci, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &vram);
    pci_register_bar(&pci, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &mmio);

    if (enable_edid) {
        qemu_edid_generate(edid_blob, sizeof(edid_blob), &edid_info);
        qemu_edid_region_io(&edid, obj, edid_blob, sizeof(edid_blob));
        memory_region_add_subregion(&mmio, 0, &edid);
    }

    if (pci_bus_is_express(pci_get_bus(dev))) {
        ret = pcie_endpoint_cap_init(dev, 0x80);
        assert(ret > 0);
    } else {
        dev->cap_present &= ~QEMU_PCI_CAP_EXPRESS;
    }

    memory_region_set_log(&vram, true, DIRTY_MEMORY_VGA);
}

static void bochs_display_realize(PCIDevice *dev, Error **errp)
{
    BochsDisplayState *s = reinterpret_cast<BochsDisplayState *>(dev);
    s->realize(errp);
}

bool BochsDisplayState::getBigEndianFb(Object *obj, Error **errp)
{
    BochsDisplayState *s = reinterpret_cast<BochsDisplayState *>(obj);

    return s->big_endian_fb;
}

void BochsDisplayState::setBigEndianFb(Object *obj, bool value, Error **errp)
{
    BochsDisplayState *s = reinterpret_cast<BochsDisplayState *>(obj);

    s->big_endian_fb = value;
}

void BochsDisplayState::initfn()
{
    PCIDevice *dev = reinterpret_cast<PCIDevice *>(this);

    /* Expose framebuffer byteorder via QOM */
    object_property_add_bool(reinterpret_cast<Object *>(this), "big-endian-framebuffer",
                             BochsDisplayState::getBigEndianFb,
                             BochsDisplayState::setBigEndianFb);

    dev->cap_present |= QEMU_PCI_CAP_EXPRESS;
}

static void bochs_display_init(Object *obj)
{
    BochsDisplayState *s = reinterpret_cast<BochsDisplayState *>(obj);
    s->initfn();
}

void BochsDisplayState::exit()
{
    graphic_console_close(con);
}

static void bochs_display_exit(PCIDevice *dev)
{
    BochsDisplayState *s = reinterpret_cast<BochsDisplayState *>(dev);
    s->exit();
}

static const Property bochs_display_properties[] = {
    DEFINE_PROP_SIZE("vgamem", BochsDisplayState, vgamem, 16 * MiB),
    DEFINE_PROP_BOOL("edid", BochsDisplayState, enable_edid, true),
    DEFINE_EDID_PROPERTIES(BochsDisplayState, edid_info),
};

void BochsDisplayState::classInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = reinterpret_cast<DeviceClass *>(klass);
    PCIDeviceClass *k = reinterpret_cast<PCIDeviceClass *>(klass);

    k->class_id  = PCI_CLASS_DISPLAY_OTHER;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = PCI_DEVICE_ID_QEMU_VGA;

    k->realize   = bochs_display_realize;
    k->romfile   = "vgabios-bochs-display.bin";
    k->exit      = bochs_display_exit;
    dc->vmsd     = &vmstate_bochs_display;
    device_class_set_props(dc, bochs_display_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const InterfaceInfo bochs_display_interfaces[] = {
    { INTERFACE_PCIE_DEVICE },
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo bochs_display_type_info = {
    .name           = TYPE_BOCHS_DISPLAY,
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(BochsDisplayState),
    .instance_init  = bochs_display_init,
    .class_init     = BochsDisplayState::classInit,
    .interfaces     = bochs_display_interfaces,
};

static void bochs_display_register_types(void)
{
    type_register_static(&bochs_display_type_info);
}

type_init(bochs_display_register_types)
