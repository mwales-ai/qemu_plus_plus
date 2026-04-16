# QEMU++ Native C++ Port Status Report

**Date:** 2026-04-15
**Branch:** `cpp-native` (463 commits ahead of master)
**Base:** QEMU v10.2.1 (tag `v10.2.1`)
**Smoke Tests:** 13/15 passing (2 pre-existing ppc64 failures)

## What We've Done

### Phase 1: C++ File Port (Complete)
- Renamed ~2211 `.c` files to `.cpp` across the entire QEMU tree
- Fixed all C++ compilation issues: `static_cast`, keyword renames (`class`→`klass`,
  `new`→`new_val`), designated initializer reordering, compound literal extraction,
  `extern "C"` guards on ~520+ headers
- Tagged as `cpp-port-complete` on the `cpp-port` branch

### Phase 2: QOM Replacement via REGISTER_QEMU_DEVICE (In Progress)
- Built the `REGISTER_QEMU_DEVICE` macro infrastructure in `include/qom/cpp/object.h`
- Uses SFINAE to auto-detect `init()`, `finalize()`, `realize(Error**)`, `reset()`,
  and `static classInit(DeviceClass*)` methods on device state structs
- Zero runtime overhead: generates the same TypeInfo + type_init boilerplate at
  compile time, but lets devices define C++ methods instead of static trampolines

### Devices Ported to REGISTER_QEMU_DEVICE: 91 files, ~95 device types

| Subsystem     | Ported | Remaining | Total |
|---------------|--------|-----------|-------|
| hw/char       | 18     | 19        | 37    |
| hw/misc       | 52     | 66        | 118   |
| hw/timer      | 8      | 26        | 34    |
| hw/rtc        | 5      | 7         | 12    |
| hw/watchdog   | 5      | 6         | 11    |
| hw/gpio       | 3      | 11        | 14    |
| **Subtotal**  | **91** | **135**   | **226**|

### What Each Port Does
For every converted device:
1. Add `#ifdef __cplusplus` method declarations to the state struct in the header
2. Convert `xxx_init(Object*)` → `void T::init()` (uses `this` instead of cast macros)
3. Convert `xxx_realize(DeviceState*, Error**)` → `void T::realize(Error **errp)`
4. Convert `xxx_reset(DeviceState*)` → `void T::reset()`
5. Convert `xxx_class_init(ObjectClass*, const void*)` → `static void T::classInit(DeviceClass*)`
6. Replace `static const TypeInfo` + `type_register_static` + `type_init()` with
   one line: `REGISTER_QEMU_DEVICE(T, TYPE_NAME, PARENT_TYPE)`
7. Drop all static trampoline functions (xxxWrapper, etc.)

Average lines removed per device: ~15-20 lines of boilerplate.

## What's Left

### Immediate Work (~135 simple devices in started subsystems)
These follow the same pattern as already-ported devices. Most are straightforward
SysBusDevice subclasses with init/realize/reset/classInit:

- **hw/char** (19 remaining): serial, parallel, spapr_vty, renesas_sci, etc.
- **hw/misc** (66 remaining): Aspeed family, edu, unimp, pvpanic-isa, etc.
- **hw/timer** (26 remaining): hpet, a9gtimer, arm_mptimer, cadence_ttc, etc.
- **hw/rtc** (7 remaining): mc146818rtc, m48t59, ls7a_rtc, etc.
- **hw/watchdog** (6 remaining): wdt_diag288, wdt_aspeed, etc.
- **hw/gpio** (11 remaining): nrf51_gpio, pl061 variants, etc.

### Medium-Term Work (~465 devices in untouched subsystems)
These subsystems haven't been started yet but follow the same conversion pattern:

- **hw/net** (34): e1000, rtl8139, virtio-net, etc.
- **hw/display** (38): VGA, virtio-gpu, ramfb, etc.
- **hw/intc** (61): GIC, APIC, IOAPIC, PLIC, etc.
- **hw/virtio** (52): virtio-blk, virtio-net, virtio-scsi, etc.
- **hw/usb** (35): EHCI, xHCI, device models
- **hw/pci-host** (25): q35, virt, spapr PHBs
- **hw/pci-bridge** (12): standard PCI/PCIe bridges
- **hw/block** (9): floppy, virtio-blk, nvme
- **hw/scsi** (15): virtio-scsi, megasas, lsi53c895a
- **hw/ide** (13): AHCI, PIIX, CMD646
- **hw/i2c** (14): aspeed_i2c, smbus devices
- **hw/arm** (46): machine types (virt, raspi, etc.)
- **hw/ppc** (38): spapr, pnv machine infrastructure
- **hw/i386** (12): pc, q35 machine infrastructure
- **hw/riscv** (11): virt machine, SiFive boards
- Other: sensor(10), nvram(14), dma(13), acpi(7), isa(8), ipmi(8), mem(4), tpm(5)

### Complex Devices (Require Special Handling)
Some devices can't use the simple `REGISTER_QEMU_DEVICE` macro because they have:

- **Abstract base classes with subclass hierarchies** (e.g., Aspeed SCU/SDMC family,
  Allwinner SRAMC, serial-pci variants) — need `is_abstract = true` and `class_size`
- **Custom class types** (e.g., VirtIOSerialPortClass, PCIDeviceClass vtable methods)
  — the macro passes `DeviceClass*` but these need a subclass pointer
- **Multiple type registrations in one file** (e.g., virtio-console registers both
  `virtserialport` and `virtconsole`) — macro handles one type per invocation
- **Interface implementations** (e.g., devices implementing `TYPE_FW_PATH_PROVIDER`)

These will need either:
1. Extended macro variants (`REGISTER_QEMU_DEVICE_ABSTRACT`, etc.)
2. Manual TypeInfo registration with method-style init/realize/reset

### Estimated Effort
- **Simple devices** (~600 remaining): ~5 minutes each, mechanical conversion
- **Complex devices** (~100 remaining): ~15-30 minutes each, need judgment calls
- **Machine types** (hw/arm, hw/ppc, hw/i386, hw/riscv): more complex, often
  have deep class hierarchies and multiple interacting objects

### Not In Scope (Yet)
- `target/` directory (CPU models) — different object hierarchy
- `accel/` directory (KVM, TCG) — not device models
- Removing QOM entirely — current approach coexists with QOM, devices register
  through the same TypeInfo mechanism but define methods instead of static functions

## Build & Test

```bash
ninja -C build                          # Build all 5 targets
./scripts/cpp-port/run-smoke-tests.sh   # Run smoke tests (expect 13/15)
```

The 2 failing tests are pre-existing ppc64 issues unrelated to the C++ port:
- ppc64 machine list: `pseries` machine not found (known build config issue)
- ppc64 Debian netboot: segfault in ppc64 system emulation
