# QOM Replacement Status

## Overview

QEMU++ is replacing QEMU's C-based Object Model (QOM) with native C++ classes,
virtual methods, and compile-time type checking. This document tracks progress.

**Branch:** `cpp-native` (531 commits ahead of master)
**Build:** All 5 target ISAs building clean
**Tests:** 13/15 smoke tests passing (2 pre-existing ppc64 failures)

## What is QOM?

QEMU's Object Model is a runtime object-oriented system implemented in C using
macros, function pointers, and `void*` casts. It provides inheritance, virtual
methods, and type checking — but all at runtime, with string-based type names
and no compile-time safety.

Every QOM type check looks like this at runtime:

```c
SerialState *s = SERIAL(dev);
// Expands to: object_dynamic_cast_assert(dev, "serial", __FILE__, __LINE__)
// -> walks type hierarchy comparing strings, checks LRU cache, aborts on mismatch
```

In C++, the same operation is free:

```cpp
SerialState *s = static_cast<SerialState *>(dev);  // zero cost, verified at compile time
```

## Conversion Progress

### REGISTER_QEMU_DEVICE Macro — 321 devices

The `REGISTER_QEMU_DEVICE` macro auto-generates TypeInfo, trampolines, and
type registration via SFINAE detection of `init()`, `realize(Error**)`,
`reset()`, and `static classInit(DeviceClass*)` methods.

Each conversion:
- Replaces ~15-20 lines of boilerplate per device
- Converts free functions to C++ member functions (`this` instead of casts)
- Eliminates static trampoline functions
- One-line macro replaces TypeInfo + type_register_static + type_init

### Deep Method Conversion — 183 devices

ALL internal helper functions (not just lifecycle methods) converted from
C free functions to C++ struct methods:

**Before (QOM C style):**
```c
static void serial_update_irq(SerialState *s)
{
    if ((s->ier & UART_IER_RLSI) && (s->lsr & UART_LSR_INT_ANY)) {
        s->iir = UART_IIR_RLSI;
    }
    qemu_irq_raise(s->irq);
}
// Called as: serial_update_irq(s);
```

**After (C++ methods):**
```cpp
void SerialState::updateIrq()
{
    if ((ier & UART_IER_RLSI) && (lsr & UART_LSR_INT_ANY)) {
        iir = UART_IIR_RLSI;
    }
    qemu_irq_raise(irq);
}
// Called as: updateIrq();  (or s->updateIrq() from outside)
```

**Top devices by method count:**

| Device | File | Methods |
|--------|------|---------|
| MegaRAID SCSI | hw/scsi/megasas.cpp | 110 |
| MV64361 PCI host | hw/pci-host/mv64361.cpp | 79 |
| SunGEM NIC | hw/net/sungem.cpp | 76 |
| RTL8139 NIC | hw/net/rtl8139.cpp | 75 |
| SD card | hw/sd/sd.cpp | 64 |
| ivshmem | hw/misc/ivshmem-pci.cpp | 61 |
| VirtIO Net | hw/net/virtio-net.cpp | 58 |
| xHCI USB 3.0 | hw/usb/hcd-xhci.cpp | 53 |
| USB Smart Card | hw/usb/dev-smartcard-reader.cpp | 50 |
| Exynos MCT | hw/timer/exynos4210_mct.cpp | 50 |

### QOM Cast Replacement — ~95% complete

| Metric | Value |
|--------|-------|
| `reinterpret_cast` uses | ~2,750+ |
| QOM macro casts remaining | ~175 |
| **Conversion rate** | **~95%** |

QOM's runtime type-checking macros (`SERIAL()`, `PL011()`, `VIRTIO_BLK()`,
etc.) are being replaced with compile-time `reinterpret_cast`. The remaining
175 QOM macros are mostly in VMSTATE/DEFINE_PROP macro expansions and
`_GET_CLASS` macros (intentionally kept for runtime class lookup).

### Option D: Bus-Level Virtual Methods — 8 hierarchies complete

C++ class hierarchies added to QOM class structs, enabling `override` and
compile-time virtual method dispatch:

| Hierarchy | Virtual Methods | Status |
|-----------|----------------|--------|
| MOS6522DeviceClass | 6 | Complete (full replacement) |
| PCIDeviceClass | 5 | Complete (hybrid — C++ + function pointers) |
| I2CSlaveClass | 5 | Complete |
| SMBusDeviceClass | 3 | Complete |
| PMBusDeviceClass | 7 | Complete |
| IPackDeviceClass | 11 | Complete |
| SSIPeripheralClass | 3 | Complete |
| ADBDeviceClass | 3 | Complete |
| VirtIODeviceClass | 24 | Dispatch wrappers only |
| SCSIDeviceClass | 20 | Dispatch wrappers only |
| USBDeviceClass | 13 | Dispatch wrappers only |

### Header Method Declarations — 20+ device types

C++ method declarations added to shared header-defined structs across all
target platforms:

**x86 platform:** SerialState, KBDState, PS2KbdState/PS2MouseState,
Q35PCIHost, MCHPCIState, HPETState

**ARM platform:** PL011State, GICv3State, GICv3ITSClass

**RISC-V platform:** SiFivePLICState, RISCVAclintMTimerState/SwiState,
RISCVAPLICState

**Cross-platform:** VirtIOBlock, VirtIONet, VirtIOGPU, VirtIOSCSI,
GPEXHost, XHCIState, SpaprMachineState

## Key Bug Fixes

### struct {} Layout Mismatch
Empty struct markers (`struct {} end_reset_fields;`) used in CPU state structs
have size 0 in C but size 1 in C++, causing field offset mismatches. Fixed with
`QEMU_STRUCT_MARKER()` macro using `char name[0]` in C++. Applied to all 11
target CPU headers. Fixed 3 ARM/aarch64 boot failures.

### SDHCIState Deleted Destructor
`SDHCIState` contains a union of `PCIDevice` and `SysBusDevice`, which makes
the destructor deleted in C++. This blocks SoC files that embed SDHCIState
(fsl-imx25, fsl-imx6, fsl-imx6ul, fsl-imx7) from using REGISTER_QEMU_DEVICE.

## Design Decisions

1. **No C++ vtable in device structs (yet)** — QOM requires `parent_obj` at
   offset 0. Using C++ virtual methods would insert a vtable pointer before
   `parent_obj`. The virtual-methods-plan.md describes how to add this via
   C/C++ ABI-compatible padding.

2. **VMState works unchanged** — `offsetof()` works on C++ structs with
   non-virtual methods. All VMState migration descriptors continue to work.

3. **Incremental migration** — Every commit keeps all 5 targets building and
   booting. Old QOM code and new C++ methods coexist in the same binary.

4. **reinterpret_cast for QOM casts** — QOM structs embed their parent as the
   first field (C-style composition, not C++ inheritance), so `static_cast`
   doesn't work. `reinterpret_cast` correctly reflects the layout guarantee.

## Target Machine Coverage

| Target | Key Devices Converted |
|--------|----------------------|
| **x86_64 (q35)** | Serial, KBD/PS2, Q35/MCH, HPET, E1000E, RTL8139, xHCI, LSI SCSI, Intel HDA, AC97, SB16, Bochs display, VirtIO block/net/GPU/SCSI |
| **aarch64 (virt)** | PL011, GICv3/ITS, PL061 GPIO, PL110 display, GPEX PCIe, DWC2 USB, VirtIO block/net/GPU/SCSI |
| **riscv64 (virt)** | PLIC, ACLINT, APLIC, GPEX PCIe, VirtIO block/net/GPU/SCSI |
| **ppc64 (pseries)** | spapr machine, OpenPIC, VirtIO block/net/SCSI |

## What's Next

1. **Extend REGISTER_QEMU_DEVICE** for ResettableClass, interfaces, abstract types
2. **Complete Option D** bus-level virtual methods (VirtIO, SCSI, USB)
3. **Implement virtual-methods-plan.md Phase A** — vtable pointer in Object
4. **Phase B/C** — virtual realize()/reset() with `override` on pilot devices
5. **Phase E** — mass `override` addition to existing 321 devices
