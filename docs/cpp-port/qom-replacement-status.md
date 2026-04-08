# QOM Replacement Status

## Overview

QEMU++ is replacing QEMU's C-based Object Model (QOM) with native C++ classes,
virtual methods, and compile-time type checking. This document tracks progress.

**Branch:** `qom-replacement` (84 commits)
**Build:** All 5 target ISAs building clean
**Tests:** 14/15 smoke tests passing

## What is QOM?

QEMU's Object Model is a runtime object-oriented system implemented in C using
macros, function pointers, and `void*` casts. It provides inheritance, virtual
methods, and type checking — but all at runtime, with string-based type names
and no compile-time safety.

Every QOM type check looks like this at runtime:

```c
SerialState *s = SERIAL(dev);
// Expands to: object_dynamic_cast_assert(dev, "serial", __FILE__, __LINE__)
// → walks type hierarchy comparing strings, checks LRU cache, aborts on mismatch
```

In C++, the same operation is free:

```cpp
SerialState *s = static_cast<SerialState *>(dev);  // zero cost, verified at compile time
```

## Conversion Progress

### Phase 1: classInit Conversion - COMPLETE

| Metric | Value |
|--------|-------|
| Devices with `::classInit` static methods | 196 |
| Free `class_init` functions remaining | 0* |

\* Only macro-generated subtype variants in spapr.cpp remain.

Every device's `class_init` function — which sets up the QOM type metadata,
virtual method pointers, and properties — is now a C++ static method on the
device struct.

### Phase 2: Deep Method Conversion - 183 devices

| Metric | Value |
|--------|-------|
| Deeply converted devices | 183 |
| Total C++ method references | ~4,000 |

"Deep conversion" means ALL internal helper functions (not just lifecycle
methods) are converted from C free functions to C++ struct methods:

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

No `s->` noise everywhere. The method belongs to the struct. The compiler
enforces member access. Code reads more naturally.

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
| TCX display | hw/display/tcx.cpp | 48 |
| LSI SCSI | hw/scsi/lsi53c895a.cpp | 46 |
| Slavio misc | hw/misc/slavio_misc.cpp | 46 |
| Cirrus VGA | hw/display/cirrus_vga.cpp | 44 |
| PS/2 protocol | hw/input/ps2.cpp | 44 |
| ARTIST display | hw/display/artist.cpp | 42 |

### Phase 3: QOM Cast Replacement - 76% complete

| Metric | Value |
|--------|-------|
| `reinterpret_cast` uses | 2,113 |
| QOM macro casts remaining | 663 |
| **Conversion rate** | **76%** |

QOM's runtime type-checking macros (`SERIAL()`, `PL011()`, `VIRTIO_BLK()`,
etc.) are being replaced with compile-time `reinterpret_cast`. This eliminates
the runtime overhead of string-based type hierarchy walks and LRU cache checks.

The remaining 663 QOM macros are in:
- `hw/ppc/spapr.cpp` (~87 — large machine file with header-defined struct)
- `_GET_CLASS` macros (runtime type lookup — intentionally kept)
- VMSTATE and DEFINE_PROP macro expansions (infrastructure limitation)
- A long tail of 3-5 casts across ~100 files

### Phase 4: Header Method Declarations - 20 device types

C++ method declarations added to shared header-defined structs, proving that
all `include/hw/` headers can be extended (they're only included from .cpp
files now):

**x86 platform:**
- SerialState (16550A UART) — boots x86 with serial console
- KBDState + PS2KbdState/PS2MouseState — keyboard/mouse
- Q35PCIHost + MCHPCIState — Q35 chipset
- HPETState — High Precision Event Timer

**ARM platform:**
- PL011State — PL011 UART, boots ARM virt
- GICv3State — interrupt controller
- GICv3ITSClass — interrupt translation

**RISC-V platform:**
- SiFivePLICState — PLIC interrupt controller
- RISCVAclintMTimerState/SwiState — timer/software interrupts
- RISCVAPLICState — Advanced PLIC

**Cross-platform:**
- VirtIOBlock, VirtIONet, VirtIOGPU, VirtIOSCSI — all VirtIO devices
- GPEXHost — PCIe host bridge
- XHCIState — USB 3.0 host controller
- SpaprMachineState — PPC pseries machine

## Key Bug Fix: struct {} Layout Mismatch

During the conversion, we discovered a critical C/C++ struct layout mismatch.
Empty struct markers (`struct {} end_reset_fields;`) used in CPU state structs
have size 0 in C but size 1 in C++, causing field offset mismatches between
C-compiled and C++-compiled code sharing the same struct.

This was causing ARM boot failures — the `features` field in `CPUARMState` was
at offset 78680 in C but 78688 in C++, so C code writing feature bits and C++
code reading them were accessing different memory.

**Fix:** Created `QEMU_STRUCT_MARKER()` macro using `char name[0]` in C++
(zero-length array) and `struct {} name` in C. Applied to all 11 target CPU
headers. This fixed 3 ARM/aarch64 boot failures.

## Design Decisions

1. **No C++ vtable in device structs** — QOM requires `parent_obj` at offset 0.
   Using C++ virtual methods would insert a vtable pointer before `parent_obj`,
   breaking QOM binary compatibility. Methods are regular (non-virtual) C++
   member functions with thin static wrappers for QOM callback dispatch.

2. **VMState works unchanged** — `offsetof()` works on C++ structs with
   non-virtual methods. All VMState migration descriptors and DEFINE_PROP
   property macros continue to work without modification.

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
| **ppc64 (pseries)** | spapr machine, VirtIO block/net/SCSI |

## What's Next

1. Complete QOM cast replacement in remaining files
2. Replace QOM virtual method function pointers with C++ virtual methods
3. Modernize the property system with typed C++ declarations
4. Eventually remove the QOM runtime type registry entirely
