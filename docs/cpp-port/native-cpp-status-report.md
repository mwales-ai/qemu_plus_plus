# QEMU++ Native C++ Port Status Report

**Date:** 2026-04-20
**Branch:** `cpp-native` (531 commits ahead of master)
**Base:** QEMU v10.2.1 (tag `v10.2.1`)
**Smoke Tests:** 13/15 passing (2 pre-existing ppc64 failures)

## What We've Done

### Phase 1: C++ File Port (Complete)
- Renamed ~2211 `.c` files to `.cpp` across the entire QEMU tree
- Fixed all C++ compilation issues: `static_cast`, keyword renames (`class`→`klass`,
  `new`→`new_val`), designated initializer reordering, compound literal extraction,
  `extern "C"` guards on ~520+ headers
- Tagged as `cpp-port-complete` on the `cpp-port` branch

### Phase 2: QOM Replacement (In Progress)

Three parallel tracks have been running:

#### Track A: REGISTER_QEMU_DEVICE Macro — 321 devices

The `REGISTER_QEMU_DEVICE` macro in `include/qom/cpp/object.h` auto-generates
TypeInfo, trampolines, and type registration. Uses SFINAE to auto-detect
`init()`, `realize(Error**)`, `reset()`, and `static classInit(DeviceClass*)`
methods on device state structs.

| Subsystem       | Ported | Total .cpp | Coverage |
|-----------------|--------|------------|----------|
| hw/misc         | 80     | 126        | 63%      |
| hw/intc         | 27     | 80         | 34%      |
| hw/timer        | 26     | 36         | 72%      |
| hw/char         | 24     | 42         | 57%      |
| hw/net          | 18     | 48         | 38%      |
| hw/display      | 13     | 59         | 22%      |
| hw/arm          | 12     | 110        | 11%      |
| hw/gpio         | 11     | 17         | 65%      |
| hw/i386         | 10     | 34         | 29%      |
| hw/ssi          | 9      | 16         | 56%      |
| hw/i2c          | 9      | 20         | 45%      |
| hw/ppc          | 8      | 63         | 13%      |
| hw/dma          | 7      | 15         | 47%      |
| hw/rtc          | 7      | 14         | 50%      |
| hw/nvram        | 6      | 19         | 32%      |
| hw/watchdog     | 5      | 10         | 50%      |
| hw/usb          | 5      | 48         | 10%      |
| hw/core         | 5      | 42         | 12%      |
| hw/audio        | 4      | 22         | 18%      |
| hw/ide          | 4      | 18         | 22%      |
| Other (16 dirs) | 11     | 197        | 6%       |
| **Total**       | **321**| **1416**   | **23%**  |

#### Track B: Deep Method Conversion — 183 devices

Internal helper functions (not just lifecycle methods) converted from C free
functions to C++ struct methods. Eliminates `s->` prefix noise and makes code
read more naturally.

Top devices by method count: MegaRAID SCSI (110), MV64361 PCI host (79),
SunGEM NIC (76), RTL8139 NIC (75), SD card (64), ivshmem (61).

#### Track C: Option D — Bus-Level Virtual Methods

C++ class hierarchies added to QOM class structs to enable `override` and
compile-time dispatch for bus-specific virtual methods:

| Hierarchy | Virtual Methods | Status |
|-----------|----------------|--------|
| MOS6522DeviceClass | 6 (portBWrite, portAWrite, timers, etc.) | Complete |
| PCIDeviceClass | 5 (realize, exit, config_read, config_write, etc.) | Complete (hybrid) |
| I2CSlaveClass | 5 (send, recv, event, send_async, match_and_add) | Complete |
| SMBusDeviceClass | 3 (write_data, receive_byte, quick_cmd) | Complete |
| PMBusDeviceClass | 7 (check_lcrit, check_lo, etc.) | Complete |
| IPackDeviceClass | 11 (io_read, io_write, mem_read, etc.) | Complete |
| SSIPeripheralClass | 3 (realize, transfer, cs_polarity) | Complete |
| ADBDeviceClass | 3 (devrequest, devhasdata, etc.) | Complete |
| VirtIODeviceClass | dispatch wrappers only | Partial |
| SCSIDeviceClass | dispatch wrappers only | Partial |
| USBDeviceClass | dispatch wrappers only | Partial |

### What Each REGISTER_QEMU_DEVICE Port Does

For every converted device:
1. Add `#ifdef __cplusplus` method declarations to the state struct header
2. Convert `xxx_init(Object*)` → `void T::init()` (uses `this` instead of casts)
3. Convert `xxx_realize(DeviceState*, Error**)` → `void T::realize(Error **errp)`
4. Convert `xxx_reset(DeviceState*)` → `void T::reset()`
5. Convert `xxx_class_init(ObjectClass*, const void*)` → `static void T::classInit(DeviceClass*)`
6. Replace `static const TypeInfo` + `type_register_static` + `type_init()` with
   one line: `REGISTER_QEMU_DEVICE(T, TYPE_NAME, PARENT_TYPE)`
7. Drop all static trampoline functions

Average lines removed per device: ~15-20 lines of boilerplate.

## What's Left

### Remaining REGISTER_QEMU_DEVICE Candidates

Most remaining devices are blocked by features the macro doesn't handle:

| Blocker | ~Files Affected | Example |
|---------|----------------|---------|
| Custom class methods (VirtioDeviceClass, PCIDeviceClass, etc.) | ~200 | hw/virtio/*.cpp, hw/net/e1000.cpp |
| ResettableClass multi-phase reset (rc->phases) | ~80 | Xilinx, NPCM, MAX78000 devices |
| `.interfaces` in TypeInfo | ~60 | Most PCI and ISA devices |
| `.is_abstract` / `.class_size` | ~50 | Base classes (pci_host, cpu_core) |
| `device_class_set_parent_realize` | ~30 | Subclass hierarchies |
| `instance_finalize` | ~15 | Exynos timers/rtc |
| Multiple TypeInfo per file | ~40 | slavio_misc, pl110, armv7m |
| Extra code in register_types | ~10 | spapr_tpm_proxy, spapr_nvram |

To convert significantly more devices, the macro needs extensions:
1. **ResettableClass support** — detect multi-phase reset methods
2. **Interface support** — pass InterfaceInfo array
3. **Abstract type support** — `REGISTER_QEMU_DEVICE_ABSTRACT` variant
4. **Custom class init** — allow passing ObjectClass* for custom class casts

### Old-Style ::classInit Devices (123 files)

These files have C++ member functions (from the deep method conversion) but
still use manual TypeInfo registration instead of REGISTER_QEMU_DEVICE.
Most are blocked by custom class methods in their classInit.

### Virtual Methods Plan (Future)

See `virtual-methods-plan.md` for the full plan to add C++ vtable pointers
to QOM state structs, enabling real `override`-based polymorphism:

- **Phase A:** Add vtable pointer to Object (C++ virtual destructor / C padding)
- **Phase B:** Virtual realize() and reset() on DeviceState
- **Phase C:** Pilot device conversions with `override`
- **Phase D:** Bus-level virtual methods (partially done via Option D)
- **Phase E:** Mass conversion — existing 321 devices just add `override`
- **Phase F:** Eliminate QOM class structs

### Not In Scope (Yet)
- `target/` directory (CPU models) — different object hierarchy
- `accel/` directory (KVM, TCG) — not device models
- Remaining 6 hard-blocked `.c` files (C99-only features)
- Removing QOM entirely — current approach coexists with QOM

## Build & Test

```bash
ninja -C build                          # Build all 5 targets
./scripts/cpp-port/run-smoke-tests.sh   # Run smoke tests (expect 13/15)
```

The 2 failing tests are pre-existing ppc64 issues unrelated to the C++ port:
- ppc64 machine list: `pseries` machine not found (known build config issue)
- ppc64 Debian netboot: segfault in ppc64 system emulation

## Key Metrics Summary

| Metric | Value |
|--------|-------|
| Branch commits ahead of master | 531 |
| Total .cpp files | ~2211 (6 .c remaining) |
| Devices using REGISTER_QEMU_DEVICE | 321 |
| Devices with ::classInit (old-style) | 123 |
| Deep method conversions | 183 devices |
| Option D bus hierarchies | 8 complete, 3 partial |
| Smoke tests | 13/15 passing |
| Target ISAs | x86_64, aarch64, arm, ppc64, riscv64 |
