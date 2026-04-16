# Virtual Methods Plan — C++ Native QOM Replacement

## Motivation

The current `REGISTER_QEMU_DEVICE` approach adds non-virtual C++ member
functions to QOM state structs. This eliminates boilerplate but does not
give us real C++ polymorphism. QOM's function-pointer dispatch in class
structs (DeviceClass, PCIDeviceClass, I2CSlaveClass, etc.) is exactly
what C++ virtual methods do natively. Replacing it gives us compile-time
type safety, `override` checking, and eliminates the class/state struct
split over time.

The earlier `cpp-native-roadmap.md` declared "zero vtables in device
types" as a permanent design principle. This was overcautious — it
conflated two distinct problems:

1. **Class struct vtable corruption** (Option D failure): adding a C++
   vtable to `ObjectClass`-derived class structs overwrites
   `ObjectClass::type` at offset 0. This is a real, fatal problem that
   only affects class structs.

2. **State struct layout shift**: adding a vtable pointer to state structs
   shifts all field offsets by 8 bytes, breaking embedders. This is NOT
   fatal — since this is a full recompile of a fork, all `sizeof()` and
   `offsetof()` values recompute correctly at compile time.

The corrected understanding: we **can** add vtables to state structs
(Object, DeviceState, SysBusDevice, etc.) as long as we maintain C/C++
ABI compatibility using a padding field in the C view. We should NOT add
vtables to class structs — instead, we eliminate class structs entirely
as virtual methods move to the C++ vtable.

## Key Constraints

### Value-embedding is pervasive

200+ device state structs are embedded by value in SoC/board structs:

- `BCM2835PeripheralState` embeds 33 devices
- `XlnxZynqMPState` embeds 42 devices
- `AspeedSoCState` embeds 50+ devices
- `ARMSSE` embeds 30+ devices
- Similar patterns in NPCM7xx, SiFiveU, STM32F405, Exynos4210, etc.

Adding a vtable pointer increases `sizeof(T)` by 8 bytes, which shifts
fields in parent structs. Since all code is recompiled, `offsetof()` and
`sizeof()` auto-adjust. VMState migration descriptors use `offsetof()`,
so they also auto-adjust. Migration compatibility with upstream QEMU is
already broken by being a C++ fork — this is acceptable.

### QOM allocation path

`object_initialize_with_type()` in `qom/object.cpp` does:
1. `memset(obj, 0, instance_size)` — zeroes everything including vtable ptr
2. `obj->klass = type->klass` — sets QOM class pointer
3. `object_init_with_type()` — calls instance_init chain

The vtable pointer must be written AFTER memset but BEFORE user code
runs. We store the per-type vtable pointer in `TypeImpl` and write it
during step 2.5 of the initialization sequence.

### C/C++ ABI compatibility

`Object` is defined in a shared C/C++ header (`include/qom/object.h`).
The layout must be identical in both languages. Solution: use `#ifdef
__cplusplus` to provide a virtual destructor in C++ and a padding field
in C:

```c
struct Object {
#ifdef __cplusplus
    virtual ~Object() = default;
#else
    void *_cpp_vtable_reserved;
#endif
    ObjectClass *klass;
    ObjectFree *free;
    GHashTable *properties;
    uint32_t ref;
    Object *parent;
};
```

Both produce 8 bytes at offset 0, with `klass` at offset 8.

## QOM Virtual Method Surface to Replace

| Class Struct | Function Pointers | Purpose |
|---|---|---|
| DeviceClass | realize, unrealize, legacy_reset, sync_config | Base device lifecycle |
| ResettableClass | phases.enter, phases.hold, phases.exit | Multi-phase reset |
| SysBusDeviceClass | explicit_ofw_unit_address, connect_irq_notifier | System bus |
| PCIDeviceClass | realize, exit, config_read, config_write | PCI bus |
| I2CSlaveClass | send, recv, event, send_async, match_and_add | I2C bus |
| USBDeviceClass | 13 methods (handle_control, handle_data, etc.) | USB |
| VirtioDeviceClass | 24 methods (get_features, set_config, etc.) | VirtIO |
| MOS6522DeviceClass | 6 methods (portBWrite, portAWrite, timers) | Device-specific |

## Phase Plan

### Phase A: Infrastructure — vtable support in QOM init path

**Goal:** All QOM objects get a vtable pointer at offset 0. The pointer
is correctly set during initialization. Everything compiles and boots.

**Files to modify:**
- `include/qom/object.h` — virtual destructor (C++) / padding (C)
- `qom/object.cpp` — TypeImpl gets `cpp_vtable` field; write it during init
- `include/qom/cpp/object.h` — REGISTER_QEMU_DEVICE extracts and stores vtable

**Mechanism:** At type registration time, REGISTER_QEMU_DEVICE constructs
a temporary on the stack, extracts the vtable pointer (first 8 bytes),
and stores it in TypeInfo. `type_register_internal` copies it to TypeImpl.
During `object_initialize_with_type()`, after memset and klass assignment,
the vtable pointer is written to offset 0.

**Verification:** All 5 targets build. `./scripts/cpp-port/run-smoke-tests.sh`
passes 13/15 (same baseline). `-machine help` and `-device help` work.

### Phase B: Base class virtual methods

**Goal:** DeviceState (and its parents) gain virtual `realize()` and
`reset()` methods. Devices can use `override`.

**Approach:** Add virtual methods to DeviceState under `#ifdef __cplusplus`.
For this to work with `override` in derived types, derived types need C++
inheritance (not just composition). Use `#ifdef __cplusplus` to switch
between inheritance and composition:

```c
struct DeviceState {
#ifdef __cplusplus
    /* Inherits from Object — same binary layout as composition */
#else
    Object parent_obj;
#endif
    // ... fields ...
#ifdef __cplusplus
    virtual void realize(Error **errp) {}
    virtual void reset() {}
#endif
};
```

**Note:** On the Itanium ABI (x86-64 Linux), inheritance and first-member
composition have identical binary layout for single inheritance. This is
relied upon by QEMU's existing `OBJECT_CHECK` macros (which cast between
Object* and derived struct*).

### Phase C: Pilot device conversions (3-5 devices)

Convert a few devices to use `override`:
- `hw/char/renesas_sci.cpp` — simple SysBus
- `hw/sensor/tmp105.cpp` — I2CSlave
- `hw/char/pl011.cpp` — canonical SysBus device
- `hw/misc/mos6522.cpp` — device-specific virtuals

Device code change is minimal — add `override` to method declarations.

### Phase D: Bus-level virtual methods

Add virtual methods to bus-specific base structs:
- I2CSlave: virtual send(), recv(), event()
- PCIDevice: virtual realize(), config_read(), config_write()
- SysBusDevice: virtual explicit_ofw_unit_address()

### Phase E: Mass conversion

Convert remaining ~600 devices. The ~91 already ported with
REGISTER_QEMU_DEVICE just need `override` added — no structural changes.

### Phase F: Eliminate class structs

Once all virtual methods are dispatched through C++ vtables, QOM class
structs (DeviceClass, PCIDeviceClass, etc.) become vestigial. Gradually
remove function pointer fields that are now handled by vtable dispatch.

## Impact on Already-Ported Devices (~91)

The ~91 devices already converted with `REGISTER_QEMU_DEVICE` are NOT
wasted work. The member functions (init, realize, reset, classInit) stay
exactly as-is. Changes needed:
1. Add `override` keyword to realize() and reset() declarations
2. The REGISTER_QEMU_DEVICE macro gains vtable-setting logic (transparent)

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| C/C++ ABI mismatch | Padding field in C matches vtable pointer in C++ |
| memset zeroes vtable ptr | Write vtable after memset in init path |
| Raw pointer arithmetic on Object | Grep for hardcoded offset assumptions |
| Non-Itanium ABI platforms | Only targeting x86-64 Linux (Itanium ABI) |
| Migration compat with upstream | Already broken by C++ fork — acceptable |
