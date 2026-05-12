# QOM Replacement Status

## Overview

QEMU++ is replacing QEMU's C-based Object Model (QOM) with native C++ classes,
virtual methods, and compile-time type checking. This document tracks progress.

**Branch:** `cpp-native`
**Build:** All 5 target ISAs building clean (x86_64, aarch64, arm, ppc64, riscv64)
**Tests:** 12/15 smoke tests passing (3 pre-existing failures)
**As of:** 2026-05-10

**Conversion progress:** 948 .cpp files converted to REGISTER_QEMU_* macros
across hw/, backends/, chardev/, crypto/, net/, qom/, migration/, system/,
block/, audio/, accel/, io/, util/, gdbstub/, scsi/, authz/, ui/.

**New macro variants added in this session:**
- `REGISTER_QEMU_OBJECT_CLASS_ONLY` / `_SIZED` / `_FINI` / `_IFACES` /
  `_SIZED_IFACES` — derived types with only class_init (and optional
  finalize/interfaces) but no own state struct
- `REGISTER_QEMU_OBJECT_INIT_ONLY` — derived types with only instance_init
- `REGISTER_QEMU_OBJECT_INIT_CLASS` / `_SIZED` — sibling subtypes with
  free instance_init + free class_init
- `REGISTER_QEMU_OBJECT_ALIAS` — pure alias types (no init/class)
- `REGISTER_QEMU_DEVICE_FREE_INIT` — device with free instance_init,
  trampolined classInit
- `REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT` — abstract version of above
- `REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT_CI` — abstract with both
  instance_init and class_init as free functions
- `REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT_CI_NO_CS_IFACES` — adds
  interfaces, drops class_size
- `REGISTER_QEMU_DEVICE_ABSTRACT_FREE_FINI` — abstract with free
  instance_finalize
- `REGISTER_QEMU_DEVICE_ABSTRACT_FREE_FINI_CI` — abstract with free
  instance_finalize + free class_init
- `REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT_CI_IFACES` — abstract with
  class_size + interfaces; full kit for abstract bases
- `REGISTER_QEMU_OBJECT_INIT_CLASS_FULL` — concrete sibling subtype with
  own state struct + class struct + free instance_init + free class_init
- `REGISTER_QEMU_OBJECT_ABSTRACT_SIZED` — intermediate abstract type
  that just adds storage (no class_init/finalize)
- `REGISTER_QEMU_OBJECT_FREE_INIT_FINI_CS` — concrete object with free
  init + free finalize + class_size, no class_init
- `REGISTER_QEMU_OBJECT_ABSTRACT_FREE_INIT_CS` — abstract object with
  free init + class_size only
- `REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT_FINI_CI_CS` — full kit for
  abstract device with free init/finalize/class_init + class_size

Recent additions extend coverage into io/, audio, accel, migration, util,
gdbstub, scsi, authz, ui, and core QOM interfaces. Many derived subtypes
converted in this session: chardev (file, stdio, serial), aspeed (SLI 2700,
ADC variants, I2C variants), IOMMU memory regions (vtd, amdvi, riscv,
sun4m, sun4u), HDA codecs, USB devices (mouse/kbd/tablet/serial/braille/
storage), pl110 subtypes, lasips2 ports, pca9552, spapr DRC subtypes
(7 variants), TPM backend, xive interfaces, IPMI interface, isl_pmbus_vr
siblings, NeXT machine, integratorcp, strongarm.

**Caveat — REGISTER_QEMU_MACHINE and empty C++ structs:** `REGISTER_QEMU_*`
macros that take a `ClassName` set `instance_size = sizeof(ClassName)`. For
machine types whose C++ struct holds only static helper methods (no real
parent_obj field), `sizeof(ClassName) == 1` while `sizeof(MachineState)` is
much larger, triggering QOM's `parent->instance_size <= ti->instance_size`
assertion at runtime. Such machines must keep manual TypeInfo registration
(no instance_size). Bisected and fixed once during this session
(highbank/midway machines, reverted commit ea08c4fdf9).

Plus a latent class_size bug that had silently regressed smoke tests
from 12/15 to 5/15: several `REGISTER_QEMU_DEVICE_CUSTOM_CI(..., DeviceClass,
..., TYPE_SYS_BUS_DEVICE, ...)` calls used DeviceClass as ClassStruct
under a SysBusDevice parent, making the child class smaller than its
parent and triggering QOM's runtime `parent->class_size <= ti->class_size`
assertion. Fixed by switching those callers to `_CUSTOM_CI_NO_CS` and by
guarding `trampoline_class_init` against `DEVICE_CLASS(oc)` failures for
non-device types (e.g. machines). Smoke tests restored to 12/15.

About 66 hw/ + 39 non-hw files remain unconverted; the remainder have
structural blockers (VirtioPCIDeviceTypeInfo helper, multi-machine
generators, class_data variants, multi-type-per-file with shared structs,
runtime type loops, bus/interface registrations, conditional registration
based on host capabilities, instance_post_init / class_base_init).

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

### REGISTER_QEMU_DEVICE/INTERFACE/BUS/OBJECT/MACHINE Macro Family — 846+ files

The `REGISTER_QEMU_DEVICE` macro family auto-generates TypeInfo, trampolines,
and type registration via SFINAE detection of `init()`, `finalize()`,
`realize(Error**)`, `reset()`, and `static classInit(DeviceClass*)` methods.

**Macro variants (all auto-wire lifecycle methods via SFINAE):**

| Macro | Use case |
|---|---|
| `REGISTER_QEMU_DEVICE` | Plain concrete device, no class struct, no interfaces |
| `REGISTER_QEMU_DEVICE_IFACES` | Concrete device that implements QOM interfaces |
| `REGISTER_QEMU_DEVICE_CLASS_SIZE` | Concrete device with custom class struct |
| `REGISTER_QEMU_DEVICE_CLASS_SIZE_IFACES` | Concrete device with class struct + interfaces |
| `REGISTER_QEMU_DEVICE_ABSTRACT` | Abstract base class with custom class struct |
| `REGISTER_QEMU_DEVICE_ABSTRACT_IFACES` | Abstract base with class struct + interfaces |
| `REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS` | Abstract base, parent's class struct (no extension) |
| `REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS_IFACES` | Abstract NO_CS + interfaces |
| `REGISTER_QEMU_DEVICE_CUSTOM_CI` | Pass user's class_init function (e.g., for parent_realize chains) |
| `REGISTER_QEMU_DEVICE_CUSTOM_CI_IFACES` | _CUSTOM_CI variant with interfaces |
| `REGISTER_QEMU_INTERFACE` | QOM interface type (parent TYPE_INTERFACE) |
| `REGISTER_QEMU_INTERFACE_CI` | Interface type with class_init function |
| `REGISTER_QEMU_BUS` | QOM bus type (parent TYPE_BUS) |
| `REGISTER_QEMU_BUS_CI` | Bus type with class_init function |
| `REGISTER_QEMU_BUS_CI_IFACES` | Bus + class_init + interfaces |
| `REGISTER_QEMU_BUS_INSTANCE_CI` | Bus + instance_init + class_init |
| `REGISTER_QEMU_BUS_CLASS_SIZE` | Bus + custom class_size only |
| `REGISTER_QEMU_BUS_FULL` | Bus + instance_init + class_size + class_init |
| `REGISTER_QEMU_BUS_ABSTRACT` | Abstract bus base + class_size |
| `REGISTER_QEMU_OBJECT` | TYPE_OBJECT-rooted type, no class_init |
| `REGISTER_QEMU_OBJECT_CI` | TYPE_OBJECT type + class_init function |
| `REGISTER_QEMU_OBJECT_CI_CS` | TYPE_OBJECT type + class_init + class_size |
| `REGISTER_QEMU_OBJECT_CI_CS_IFACES` | _OBJECT_CI_CS + interfaces |
| `REGISTER_QEMU_MACHINE` | Concrete machine type |
| `REGISTER_QEMU_MACHINE_IFACES` | Concrete machine + interfaces |
| `REGISTER_QEMU_MACHINE_ABSTRACT` | Abstract MachineClass base + class_size |
| `REGISTER_QEMU_MACHINE_ABSTRACT_IFACES` | Abstract machine + class_size + interfaces |

Each conversion:
- Replaces ~15-20 lines of TypeInfo / register_types / type_init boilerplate
- Converts free functions to C++ member functions (`this` instead of casts)
- Eliminates static trampoline functions
- Auto-wires lifecycle methods that exist; quietly skips ones that don't

**Recent macro improvements:**
- `extract_vtable<T>()` falls back to `nullptr` for non-default-constructible
  types (unblocks SDHCIState-embedding SoCs: fsl-imx*, xlnx-zynqmp, raspi4b,
  bcm2838, etc.)
- All four `_ABSTRACT*` variants now SFINAE-wire instance_init/finalize, since
  QOM invokes those callbacks through inheritance chains when concrete
  subclasses are instantiated (unblocked GICv3 family, etc.)
- `_CLASS_SIZE` and `_ABSTRACT` variants unblock dozens of devices with
  custom class structs (GIC family, Aspeed multi-variant timers/SCU/SDMC,
  PIT/PIC, ICP, mos6522, scsi-bus, scsi-disk, virtio-blk, virtio-mem, etc.)
- `_CUSTOM_CI` variants accept caller-supplied class_init free functions
  for devices needing parent_realize chaining or with classInit defined
  elsewhere on the same struct (i8259/i8259_common, arm_gic_kvm, etc.)
- `_OBJECT` variants cover TYPE_OBJECT-rooted types (Clock, IRQState,
  RemoteIommu, RegisterInfoArray)
- `_MACHINE_ABSTRACT` variants cover MachineClass-rooted abstract bases
  like SpaprMachineState

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

### SDHCIState Deleted Destructor — RESOLVED
`SDHCIState` contains a union of `PCIDevice` and `SysBusDevice`, neither
default-constructible, which previously broke `extract_vtable<T>()`'s
default-construct-into-buffer trick — blocking REGISTER_QEMU_DEVICE for
SoCs that embed SDHCIState. Fixed by SFINAE-dispatching extract_vtable on
`std::is_default_constructible_v<T>` and returning `nullptr` for the
non-constructible case. Such types lose the C++ vtable hookup but
init/finalize/realize/reset/classInit trampolines still work.

Files now converted: fsl-imx25/6/6ul/7, xlnx-zynqmp, xlnx-zcu102, raspi4b.

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

## What's Left — 66 files, categorized by blocker

The macro family covers every fundamental QOM type pattern. Each
remaining file falls into one of these structural-blocker categories
that need targeted refactoring rather than a new macro variant:

### A. `data` parameter used for variant configuration (~6 files)
The `class_init(ObjectClass *, const void *data)` reads `data` and uses
it to configure per-variant device IDs/sizes/etc. via a registration
loop. Each variant shares the same state struct.

| File | Variants |
|---|---|
| `hw/net/e1000.cpp` | i82540em, i82544gc, i82545em |
| `hw/net/eepro100.cpp` | i82550, i82551, i82557a–c, i82562, ... (13 total) |
| `hw/scsi/megasas.cpp` | megasas, megasas-gen2 |
| `hw/rtc/m48t59.cpp` + `m48t59-isa.cpp` | m48t02, m48t08, m48t59 sysbus + ISA |
| `hw/usb/hcd-uhci.cpp` | piix3-uhci, piix4-uhci, vt82c686b-uhci, ich9-uhci-{1..6} |

**Refactor path:** Either give each variant its own state struct (large
churn) or add a `_DATA` macro variant that propagates `data` to a member
init function. Skipped for now.

### B. VirtioPCIDeviceTypeInfo helper / virtio_pci_types_register (~10 files)
Helper macros that generate **families** of derived TypeInfos from a
single descriptor — base, transitional, non-transitional, generic.

| File | Notes |
|---|---|
| `hw/display/virtio-vga.cpp` | virtio-vga + GL + rutabaga variants |
| `hw/virtio/virtio-input-pci.cpp` | input PCI base + concretes |
| `hw/virtio/virtio-pci.cpp` | base virtio-pci infrastructure |
| `hw/usb/hcd-{ehci-pci,xhci-pci}.cpp` | EHCI/XHCI PCI families |
| `hw/vmapple/virtio-blk.cpp` | VMApple virtio-blk + PCI variant |

**Refactor path:** Would need to refactor the `virtio_pci_types_register`
helper itself to emit individual `REGISTER_QEMU_DEVICE` invocations.

### C. Multi-binary struct collisions (~3 files)
Both KVM and TCG implementations define methods on the same struct.
When both are compiled into the same binary, the methods would clash
as duplicate symbols.

| File | Conflict |
|---|---|
| `hw/s390x/tod-tcg.cpp` ↔ `tod-kvm.cpp` | both define on `S390TODState` |

**Refactor path:** Introduce per-impl wrapper structs. Skipped.

### D. `instance_post_init` / `class_base_init` (~3 files)
TypeInfo fields not handled by any current macro variant.

| File | Field |
|---|---|
| `hw/pci-bridge/pcie_root_port.cpp` | `instance_post_init` |
| `hw/core/qdev.cpp` | `instance_post_init` |
| `hw/core/machine.cpp` | `class_base_init` |

**Refactor path:** Add macro variants. Each is a low-impact 1-file fix.

### E. Multi-machine generators (~5 files)
Use `DEFINE_VIRT_MACHINE` / `DEFINE_CCW_MACHINE` macros that expand to
multiple `type_init` calls per file.

| File | Pattern |
|---|---|
| `hw/arm/virt.cpp` | DEFINE_VIRT_MACHINE for each version |
| `hw/m68k/virt.cpp` | similar |
| `hw/s390x/s390-virtio-ccw.cpp` | DEFINE_CCW_MACHINE |
| `hw/xtensa/xtfpga.cpp` | 8 boards, no interfaces |
| `hw/arm/xlnx-versal-virt.cpp` | non-trivial init using class accessors |

**Refactor path:** Refactor the DEFINE_*_MACHINE expansions to use
REGISTER_QEMU_MACHINE_IFACES underneath.

### F. Remaining structural variety (~39 files)
Each has its own combination of: complex class_init bodies that touch
3+ class types (PCIE, ACPI, HotplugHandler), free-function instance_init
where converting to a member would force a member-method add to a
header struct, parent_realize chains combined with class_data, etc.

Examples:
- `hw/core/{cpu-common,bus,resettable,qdev,machine}.cpp` — foundational
  QOM infrastructure with intricate class_init wiring
- `hw/arm/{musicpal,armsse,armv7m,bcm2838,bcm2838_peripherals}.cpp`
- `hw/pci-host/{i440fx,aspeed_pcie,pnv_phb4_pec}.cpp`
- `hw/xen/{xen_pt,xen_pt_graphics,xen-bus}.cpp`
- `hw/virtio/virtio.cpp` — base virtio device
- `hw/scsi/virtio-scsi.cpp`
- `hw/ssi/aspeed_smc.cpp`
- `hw/usb/{hcd-dwc2,dev-storage-bot,dev-storage-classic}.cpp`
- `hw/nvram/fw_cfg.cpp`
- and more — each needs case-by-case attention

## Future Infrastructure Work

The mechanical TypeInfo conversion is mostly done. The next strategic
phases are about *replacing* QOM's runtime dispatch with C++ constructs,
not just wrapping it:

1. **Complete Option D** bus-level virtual methods (VirtIO, SCSI, USB).
   Currently 8 hierarchies done as full replacements; 3 more are
   "dispatch wrappers only" (the function pointers still drive the
   dispatch, virtual methods just shadow them).

2. **Implement virtual-methods-plan.md Phase A** — add vtable pointer
   to Object via a C/C++ ABI-compatible padding field, enabling
   compile-time virtual dispatch on device state structs.

3. **Phase B/C** — pilot devices use `virtual void realize() override`
   instead of the trampoline mechanism.

4. **Phase E** — mass `override` addition to existing converted devices.
