# QOM Replacement Status

## Overview

QEMU++ is replacing QEMU's C-based Object Model (QOM) with native C++ classes,
virtual methods, and compile-time type checking. This document tracks progress.

**Branch:** `cpp-native`
**Build:** All 5 target ISAs building clean (x86_64, aarch64, arm, ppc64, riscv64)
**Tests:** 12/15 smoke tests passing (3 pre-existing failures)
**As of:** 2026-05-27

## TL;DR — Phase 1 (Mechanical TypeInfo Conversion) Is Complete

**1124** of **2212** total .cpp files use the `REGISTER_QEMU_*` macro family
(including the new `REGISTER_VIRTIO_PCI_TYPES` and `REGISTER_VIRTIO_PCI_TYPES_IF`
wrappers added 2026-05-27; 37 virtio-pci callers brought into the family).
The remaining ~1100 .cpp files simply have no QOM TypeInfo to register —
they are utility / glue / per-arch CPU emulation code, not device models.

**Of files that *do* contain QOM TypeInfo registration, only 3 remain
outside the macro family**, and every one is structurally blocked by design:

- `qom/object.cpp` — TYPE_OBJECT/TYPE_INTERFACE bootstrap (can't use
  a macro that depends on TYPE_OBJECT existing)
- `hw/block/m25p80.cpp` — parameter-driven loop over a
  `known_devices[]` table with per-variant `class_data`
- `hw/virtio/virtio-pci.cpp` — defines the `virtio_pci_types_register`
  helper that callers use via `REGISTER_VIRTIO_PCI_TYPES{,_IF}`

**Phase 2 (replacing QOM's runtime dispatch with C++ virtual methods) is
the next strategic step.** See `docs/cpp-port/virtual-methods-plan.md`.

## Phase 1 Detail

**Conversion progress:** 1124 .cpp files converted to REGISTER_QEMU_* macros
across hw/, backends/, chardev/, crypto/, net/, qom/, migration/, system/,
block/, audio/, accel/, io/, util/, gdbstub/, scsi/, authz/, ui/. All
DEFINE_TYPES patterns converted. Foundational types (TYPE_DEVICE/TYPE_MACHINE/
TYPE_PCI_DEVICE/TYPE_SYS_BUS_DEVICE/TYPE_SYSTEM_BUS) now use macros that
expose class_base_init. All conditional-registration and DEFINE_*_MACHINE
generator blockers are converted (arm/virt, m68k/virt, ppc/spapr, s390x).
The new `REGISTER_VIRTIO_PCI_TYPES` + `_IF` family-naming wrappers (2026-05-27)
brought all 37 VirtioPCIDeviceTypeInfo callers into the macro family.

**Conversion by directory (top hw/ subsystems):**

| Directory | Files converted |
|---|---|
| hw/misc | 124 |
| hw/arm | 91 |
| hw/intc | 68 |
| hw/ppc | 41 |
| hw/char | 39 |
| hw/net | 38 |
| hw/usb | 37 |
| hw/s390x | 37 |
| hw/display | 36 |
| hw/timer | 34 |
| hw/pci-host | 30 |
| hw/virtio | 26 |
| hw/core | 19 |

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
- `REGISTER_QEMU_DEVICE_ABSTRACT_POST_INIT_CI_IFACES` — abstract with
  instance_post_init + class_init + class_size + interfaces
- `REGISTER_QEMU_OBJECT_CLASS_DATA` / `_CS` / `_IFACES` — derived types
  with class_init + class_data pointer (and optional class_size /
  interfaces)
- `REGISTER_QEMU_OBJECT_INIT_CLASS_SIZED_IFACES` — sibling concrete
  subtype with own state struct + free init + free class_init + interfaces

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

Only 40 files remain unconverted; all blocked by structural patterns
that no new macro variant can fix (see "What's Left" section below).

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

## What's Left — 3 files, all structurally blocked

The `REGISTER_QEMU_*` macro family covers every common QOM type pattern.
Only 3 files now bypass it; every one is structurally blocked by design.

| File | Why it can't use a macro |
|---|---|
| `qom/object.cpp` | Bootstraps TYPE_OBJECT and TYPE_INTERFACE — must run before any `REGISTER_QEMU_*` macro can work, and registers types that have no parent |
| `hw/block/m25p80.cpp` | Walks a `known_devices[]` array at runtime, registering one TypeInfo per entry with a per-variant `class_data` pointer |
| `hw/virtio/virtio-pci.cpp` | Defines the `virtio_pci_types_register` helper that callers use via `REGISTER_VIRTIO_PCI_TYPES{,_IF}` |

### Converted on 2026-05-27 (37 files)

The new `REGISTER_VIRTIO_PCI_TYPES(tag, descriptor)` wrapper macro in
`include/qom/cpp/object.h` replaces the
`static void X_register(void) { virtio_pci_types_register(&desc); }` +
`type_init(X_register)` boilerplate with a single macro line. The
wrapper preserves the underlying runtime helper (which uses
`g_strdup_printf` to generate derived type names that don't exist as
compile-time string literals), so this is family-naming consolidation
rather than a structural change. Converted:

```
hw/audio/virtio-snd-pci.cpp                  (register fn split:
                                              types via macro,
                                              audio_register_model
                                              kept in its own type_init)
hw/display/{vhost-user-gpu-pci, vhost-user-vga,
            virtio-gpu-pci, virtio-gpu-pci-gl, virtio-vga}.cpp
hw/virtio/{virtio-9p-pci, virtio-balloon-pci, virtio-blk-pci,
           virtio-crypto-pci, virtio-input-host-pci,
           virtio-iommu-pci, virtio-mem-pci, virtio-net-pci,
           virtio-nsm-pci, virtio-pmem-pci, virtio-rng-pci,
           virtio-scsi-pci, virtio-serial-pci, vhost-scsi-pci,
           vhost-user-blk-pci, vhost-user-fs-pci,
           vhost-user-gpio-pci, vhost-user-i2c-pci,
           vhost-user-input-pci, vhost-user-rng-pci,
           vhost-user-scmi-pci, vhost-user-scsi-pci,
           vhost-user-snd-pci, vhost-user-test-device-pci,
           vhost-user-vsock-pci, vhost-vsock-pci,
           vdpa-dev-pci}.cpp                 (28 standard pattern)
hw/virtio/virtio-input-pci.cpp               (4 descriptors → 4 macro
                                              invocations)
hw/vmapple/virtio-blk.cpp
hw/display/virtio-vga-gl.cpp                 (REGISTER_VIRTIO_PCI_TYPES_IF
                                              with `have_vga` gate)
hw/display/virtio-vga-rutabaga.cpp           (same)
```

### Resolved categories (kept here for history)

- **Category A — variant-configuration via class_data (6 files):**
  e1000, eepro100, megasas, m48t59 sysbus/ISA, hcd-uhci. All use
  `REGISTER_QEMU_OBJECT_CLASS_DATA*` variants.
- **Category C — multi-binary struct collisions:** tod-tcg/tod-kvm
  resolved via `REGISTER_QEMU_OBJECT_INIT_CLASS_FULL` with distinct
  unique_tags.
- **Category D — `instance_post_init` / `class_base_init`:**
  `pcie_root_port` → `REGISTER_QEMU_DEVICE_ABSTRACT_POST_INIT_CI_IFACES`;
  `qdev` → `REGISTER_QEMU_OBJECT_DEVICE_BASE_FULL`;
  `machine` → `REGISTER_QEMU_OBJECT_ABSTRACT_BASE_CBI`.
- **Category E — DEFINE_*_MACHINE generators:** arm/virt, m68k/virt,
  ppc/spapr, s390x/s390-virtio-ccw all expand through
  `REGISTER_QEMU_OBJECT_CLASS_ONLY_IF` / `_IFACES_IF` variants. The
  new `_IF` macros place `TypeInfo` at file scope (so the class_init
  reference survives `-Werror=unused-function` with compile-time-constant
  `cond_expr`) and use indirect token pasting (`_QEMU_CPP_PASTE`) so
  the `unique_tag` argument can be a complex `MACHINE_VER_SYM(...)`
  expression. `cond_expr` is `!MACHINE_VER_SHOULD_DELETE(__VA_ARGS__)`,
  preserving the original runtime version-deletion behavior.
- **Category B — `VirtioPCIDeviceTypeInfo` helper users (37/37):**
  35 use `REGISTER_VIRTIO_PCI_TYPES(tag, descriptor)` and 2 (the
  `have_vga`-gated `virtio-vga-{gl,rutabaga}`) use the `_IF` variant.
  Both wrappers are family-naming consolidation over the unchanged
  runtime helper (it still emits base + transitional + non_transitional
  + generic types per descriptor with `g_strdup_printf` name generation).

## Future Infrastructure Work — Phase 2

Phase 1 (mechanical TypeInfo conversion) is essentially done. Phase 2
replaces QOM's runtime dispatch with native C++ virtual methods.
See `docs/cpp-port/virtual-methods-plan.md` for the active plan.

1. **Complete Option D** bus-level virtual methods (VirtIO, SCSI, USB).
   8 hierarchies done as full replacements; 3 more (VirtIODeviceClass,
   SCSIDeviceClass, USBDeviceClass) are "dispatch wrappers only" — the
   function pointers still drive dispatch, virtual methods just shadow
   them. Promote to full replacements.

2. **virtual-methods-plan.md Phase A** — add a vtable pointer to
   `Object` via a C/C++ ABI-compatible padding field, enabling
   compile-time virtual dispatch on device state structs.

3. **Phase B/C** — convert pilot devices (mos6522, serial, hpet,
   e1000e, virtio-blk) to use `virtual void realize() override` instead
   of the trampoline mechanism. These ~91 devices already use
   `REGISTER_QEMU_DEVICE` so the diff is small per device.

4. **Phase E** — mass `override` addition to existing converted devices
   once the vtable infrastructure is in place.

## Blockers

- **VirtioPCIDeviceTypeInfo refactor (medium effort)** — single-file
  rewrite of `hw/virtio/virtio-pci.cpp`'s helper unblocks 38 files.
  Not urgent — the helper works correctly and the manual registration
  is isolated.
- **None for Phase 2 prerequisites** — Phase 1 has cleared every
  blocker that would prevent virtual-methods experimentation. ABI
  compatibility, build cleanliness across all 5 targets, and 12/15
  smoke-test baseline are all in place.
