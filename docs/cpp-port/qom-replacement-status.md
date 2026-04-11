# QOM Replacement Status

## Overview

QEMU++ is replacing QEMU's C-based Object Model (QOM) with native C++ classes,
virtual methods, and compile-time type checking. This document tracks progress.

**Branch:** `qom-replacement` (110+ commits)
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

### Phase 3: QOM Cast Replacement - 94% complete

| Metric | Value |
|--------|-------|
| `reinterpret_cast` uses | 2,751 |
| QOM macro casts remaining | 175 |
| **Conversion rate** | **94%** |

QOM's runtime type-checking macros (`SERIAL()`, `PL011()`, `VIRTIO_BLK()`,
etc.) are being replaced with compile-time `reinterpret_cast`. This eliminates
the runtime overhead of string-based type hierarchy walks and LRU cache checks.

The remaining 175 QOM macros are:
- `_GET_CLASS` macros (runtime type lookup — intentionally kept)
- VMSTATE and DEFINE_PROP macro expansions (infrastructure limitation)
- A small number of casts in complex polymorphic code paths

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

### Phase 5: Option D — C++ Virtual Methods on Class Structs

| Metric | Value |
|--------|-------|
| Class hierarchies fully converted | **10** |
| Class structs with C++ inheritance | **16** |
| Virtual methods replacing function pointers | **80** |
| Subclass files converted | **~90** |

**Option D** is the key architectural change: replacing QOM's function pointer
dispatch with actual C++ virtual methods on the class structs. This is done in
stages:

1. **Step 1:** Convert class struct from `parent_class` embedding to C++
   inheritance (`: ParentClass`), keeping function pointers
2. **Step 2:** Replace function pointers with `virtual` methods, convert
   subclass assignments to `override` methods with `qom_fixup_vtable<T>()`

The `qom_fixup_vtable<T>()` template (in `include/qom/cpp/object.h`) restores
the C++ vtable pointer after QOM's `type_initialize()` memcpy overwrites it.

**Fully converted to virtual methods (Option D Step 2):**

| Class Hierarchy | Virtual Methods | Subclasses | Files |
|----------------|----------------|------------|-------|
| VirtioDeviceClass | 25 | 27+ (balloon, blk, net, gpu, scsi, ...) | 42 files |
| USBDeviceClass | 14 | 15 (hub, hid, wacom, serial, ...) | 22 files |
| SDCardClass | 12 | 4 (sd, spi, emmc) | 3 files |
| VirtIOSerialPortClass | 7 | 2 (virtserialport, virtconsole) | 3 files |
| MOS6522DeviceClass | 6 | 4 (CUDA, VIA2, PMU) | 6 files |
| SCSIDeviceClass | 5 | 4 (hd, cd, block, generic) | 4 files |
| PITCommonClass | 4 | 2 (i8254, kvm-i8254) | 4 files |
| HDACodecDeviceClass | 4 | 4 (output, duplex, micro) | 3 files |
| AwRtcClass | 2 | 3 (sun4i, sun6i, sun7i) | 2 files |
| IDEDeviceClass | 1 | 3 (ide-hd, ide-cd, ide-cf) | 3 files |

**Intermediate class structs also converted to C++ inheritance:**
- VirtIOGPUBaseClass, VirtIOGPUClass (virtio-gpu hierarchy)
- VirtIOBlkClass (virtio-blk)
- VirtIOInputClass (virtio-input)
- VirtIOMEMClass, VirtIOPMEMClass (virtio memory devices)
- VHostUserBaseClass (vhost-user devices)
- SCSIDiskClass (scsi-disk sub-hierarchy)

**Step 1 only (C++ inheritance, function pointers remain):**

| Class Hierarchy | Function Pointers | Subclass Files |
|----------------|-------------------|----------------|
| PCIDeviceClass | 4 | 100+ |

**How virtual method dispatch works:**

```cpp
/* Before (QOM function pointer): */
VirtioDeviceClass *vdc = VIRTIO_DEVICE_GET_CLASS(vdev);
if (vdc->get_config) {
    vdc->get_config(vdev, config);   /* null check + indirect call */
}

/* After (C++ virtual method): */
VirtioDeviceClass *vdc = VIRTIO_DEVICE_GET_CLASS(vdev);
vdc->get_config(vdev, config);       /* direct virtual dispatch, no null check */
```

The call sites are simplified — null checks are eliminated because virtual
methods always exist (default implementations do nothing). The dispatch
mechanism moves from manually-assigned function pointers to compiler-managed
vtables.

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

## Key Infrastructure: qom_fixup_vtable

When a QOM class struct gains C++ virtual methods, QOM's `type_initialize()`
breaks the vtable. QOM copies parent class data into the child via `memcpy()`,
which overwrites the C++ vtable pointer with the parent's vtable.

The `qom_fixup_vtable<T>()` template (in `include/qom/cpp/object.h`) fixes
this by copying just the vtable pointer from a properly-constructed temporary:

```cpp
template<typename T>
inline void qom_fixup_vtable(void *obj) {
    T tmp;
    memcpy(obj, &tmp, sizeof(void *));  // restore vtable pointer only
}
```

This must be called at the START of every `class_init` for a class struct
that uses C++ virtual methods.

## Design Decisions

1. **C++ vtable on class structs, not instance structs** — QOM requires
   `parent_obj` at offset 0 in instance structs. Virtual methods on class
   structs don't affect instance layout. The vtable lives in the class object
   (one per type), not in every device instance.

2. **`#ifdef __cplusplus` dual view** — Class struct headers provide both a
   C++ view (with inheritance and virtual methods) and a C view (with embedded
   parent and function pointers). This allows incremental migration — C files
   continue to work unchanged.

3. **VMState works unchanged** — `offsetof()` works on C++ structs with
   non-virtual methods. All VMState migration descriptors and DEFINE_PROP
   property macros continue to work without modification.

4. **Incremental migration** — Every commit keeps all 5 targets building and
   booting. Old QOM code and new C++ methods coexist in the same binary.

5. **reinterpret_cast for QOM casts** — QOM structs embed their parent as the
   first field (C-style composition, not C++ inheritance), so `static_cast`
   doesn't work. `reinterpret_cast` correctly reflects the layout guarantee.

## Target Machine Coverage

| Target | Key Devices Converted |
|--------|----------------------|
| **x86_64 (q35)** | Serial, KBD/PS2, Q35/MCH, HPET, E1000E, RTL8139, xHCI, LSI SCSI, Intel HDA, AC97, SB16, Bochs display, VirtIO block/net/GPU/SCSI, i8254 PIT |
| **aarch64 (virt)** | PL011, GICv3/ITS, PL061 GPIO, PL110 display, GPEX PCIe, DWC2 USB, VirtIO block/net/GPU/SCSI, Allwinner RTC |
| **riscv64 (virt)** | PLIC, ACLINT, APLIC, GPEX PCIe, VirtIO block/net/GPU/SCSI |
| **ppc64 (pseries)** | spapr machine, VirtIO block/net/SCSI, MOS6522/CUDA/VIA |

## What's Next

1. Convert PCIDeviceClass (4 function pointers, 100+ subclass files — mass
   conversion phase, requires scripted approach)
2. Convert remaining small hierarchies (XenDevice, SSI, I2C, PCDIMMDevice,
   SysBusDevice, etc.)
3. Modernize the property system with typed C++ declarations
4. Slim down QOM infrastructure — the runtime type *registry* must stay
   (it powers `-device`, `device_add`, QMP introspection, and hotplug),
   but the runtime type *checking* (OBJECT_CHECK string comparisons,
   dynamic_cast_assert) can be replaced with compile-time C++ casts
   wherever the type is statically known
