/*
 * QEMU++ C++ Device Helpers (SysBus, PCI)
 *
 * Extends qom/cpp/object.h with zero-vtable helpers for specific device
 * types. Same design rule as object.h: NO virtual methods.
 *
 * Copyright (c) 2026 QEMU++ Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QOM_CPP_DEVICE_H
#define QOM_CPP_DEVICE_H

#include "qom/cpp/object.h"

/* These headers have deep deps that pull in C++ std headers via liburing,
 * so they MUST NOT be inside extern "C". They're already C++-safe via
 * osdep.h's extern "C" infrastructure. */
#include "hw/sysbus.h"
#include "hw/pci/pci_device.h"

/*
 * CppSysBusDevice: zero-vtable view over a QOM SysBusDevice.
 *
 * Use as a CRTP-free mixin: the device class inherits CppSysBusDevice,
 * embeds `SysBusDevice parent_obj` as its first data member, and gets
 * convenience helpers for MMIO/IRQ registration and the sysBusDevice()
 * accessor.
 *
 * Example:
 *   class PL011State : public CppSysBusDevice {
 *   public:
 *       SysBusDevice parent_obj;   // MUST be first — QOM embedding
 *       MemoryRegion iomem;
 *       qemu_irq irq[6];
 *       uint32_t theCr;
 *       // ...
 *
 *       void init();
 *       void realize(Error **errp);
 *       void reset();
 *       static void classInit(DeviceClass *dc);
 *   };
 *
 * Note: CppSysBusDevice contributes 0 bytes to PL011State via empty-base
 * optimization, so sizeof(PL011State) is unchanged from the equivalent
 * C struct. Binary layout is preserved and embedders that do
 * `PL011State uart0;` keep working unchanged.
 */
class CppSysBusDevice : public CppDevice
{
public:
    SysBusDevice *sysBusDevice()
    {
        return reinterpret_cast<SysBusDevice *>(this);
    }

    const SysBusDevice *sysBusDevice() const
    {
        return reinterpret_cast<const SysBusDevice *>(this);
    }

    /* Register an MMIO region on this device */
    void initMmio(MemoryRegion *mr)
    {
        sysbus_init_mmio(sysBusDevice(), mr);
    }

    /* Initialize an output IRQ line */
    void initIrq(qemu_irq *irq)
    {
        sysbus_init_irq(sysBusDevice(), irq);
    }

protected:
    CppSysBusDevice() = default;
    ~CppSysBusDevice() = default;  /* NON-virtual */
};

/*
 * CppPCIDevice: zero-vtable view over a QOM PCIDevice.
 *
 * Same idiom as CppSysBusDevice. The concrete device class embeds
 * `PCIDevice parent_obj` as its first data member.
 */
class CppPCIDevice : public CppDevice
{
public:
    PCIDevice *pciDevice()
    {
        return reinterpret_cast<PCIDevice *>(this);
    }

    const PCIDevice *pciDevice() const
    {
        return reinterpret_cast<const PCIDevice *>(this);
    }

    /* Access PCI config space */
    uint8_t *config() { return pciDevice()->config; }
    const uint8_t *config() const { return pciDevice()->config; }

    /* Register a PCI BAR */
    void registerBar(int region, uint8_t type, MemoryRegion *mr)
    {
        pci_register_bar(pciDevice(), region, type, mr);
    }

protected:
    CppPCIDevice() = default;
    ~CppPCIDevice() = default;  /* NON-virtual */
};

/*
 * REGISTER_QEMU_SYSBUS_DEVICE: register a SysBus-based C++ device.
 * Convenience wrapper around REGISTER_QEMU_DEVICE.
 */
#define REGISTER_QEMU_SYSBUS_DEVICE(ClassName, type_name_str)             \
    REGISTER_QEMU_DEVICE(ClassName, type_name_str, TYPE_SYS_BUS_DEVICE)

/*
 * REGISTER_QEMU_PCI_DEVICE: register a PCI-based C++ device.
 */
#define REGISTER_QEMU_PCI_DEVICE(ClassName, type_name_str)                \
    REGISTER_QEMU_DEVICE(ClassName, type_name_str, TYPE_PCI_DEVICE)

/*
 * REGISTER_QEMU_DEVICE_OF: register a C++ device type whose parent is
 * another already-registered device. Useful for subtypes that refine a
 * base class's behavior (e.g. the luminary variant of pl011).
 */
#define REGISTER_QEMU_DEVICE_OF(ClassName, type_name_str, parent_type_str) \
    REGISTER_QEMU_DEVICE(ClassName, type_name_str, parent_type_str)

#endif /* QOM_CPP_DEVICE_H */
