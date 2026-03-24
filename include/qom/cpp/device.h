/*
 * QEMU++ C++ Device Wrappers (SysBus, PCI)
 *
 * Extends qom/cpp/object.h with wrappers for specific device types.
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

namespace qemu {

/**
 * CppSysBusDevice: C++ wrapper for SysBusDevice.
 *
 * Use this as the base class for memory-mapped devices that attach
 * to the system bus (most non-PCI hardware).
 *
 * Provides convenience methods for registering MMIO regions and IRQs.
 *
 * Example:
 *   class MyTimer : public qemu::CppSysBusDevice {
 *       QEMU_DEVICE_TYPE("my-timer")
 *   public:
 *       void realize(Error **errp) override;
 *       void reset() override;
 *
 *       static void classInit(DeviceClass *dc);
 *
 *   private:
 *       MemoryRegion theMmio;
 *       uint32_t theCount;
 *       uint32_t theReload;
 *       qemu_irq theIrq;
 *       QEMUTimer *theTimer;
 *   };
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
    ~CppSysBusDevice() override = default;
};

/**
 * CppPCIDevice: C++ wrapper for PCIDevice.
 *
 * Use this as the base class for PCI devices. Override realize()
 * to set up BARs, capabilities, and interrupts.
 *
 * Example:
 *   class MyNIC : public qemu::CppPCIDevice {
 *       QEMU_DEVICE_TYPE("my-nic")
 *   public:
 *       void realize(Error **errp) override;
 *       void reset() override;
 *
 *       static void classInit(DeviceClass *dc);
 *
 *   private:
 *       MemoryRegion theBar;
 *       NICState *theNic;
 *       NICConf theConf;
 *   };
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
    ~CppPCIDevice() override = default;
};

/*
 * QEMU_SYSBUS_DEVICE_REGISTER(ClassName)
 *
 * Convenience macro for SysBus devices — sets parent to TYPE_SYS_BUS_DEVICE.
 */
#define QEMU_SYSBUS_DEVICE_REGISTER(ClassName) \
    QEMU_DEVICE_REGISTER(ClassName, TYPE_SYS_BUS_DEVICE)

/*
 * QEMU_PCI_DEVICE_REGISTER(ClassName)
 *
 * Convenience macro for PCI devices — sets parent to TYPE_PCI_DEVICE.
 */
#define QEMU_PCI_DEVICE_REGISTER(ClassName) \
    QEMU_DEVICE_REGISTER(ClassName, TYPE_PCI_DEVICE)

} /* namespace qemu */

#endif /* QOM_CPP_DEVICE_H */
