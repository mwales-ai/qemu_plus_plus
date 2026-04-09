# QOM Replacement — Next Steps Plan

## Completed Work (Phases 1-3)

| Phase | Status | Scope |
|-------|--------|-------|
| 1. classInit conversion | COMPLETE | 196 devices |
| 2. Deep method conversion | COMPLETE | 183 devices, ~4000 method refs |
| 3. QOM cast replacement | COMPLETE | 3,043 reinterpret_cast (~95%) |

## Proposed Next Steps

### Step 4: Virtual Method Dispatch Wrappers (Low Risk, High Value)

**Problem:** QOM "virtual methods" are function pointers in `*DeviceClass`
structs, dispatched via the clunky pattern:

```c
MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);
mdc->portB_write(s);
```

**Solution:** Add inline dispatch methods to the device state struct:

```cpp
// In MOS6522State:
inline void callPortBWrite() {
    auto *mdc = MOS6522_GET_CLASS(this);
    mdc->portB_write(this);
}
// Called as: callPortBWrite();
```

This doesn't change the underlying mechanism — the function pointer dispatch
still happens — but it makes call sites cleaner and positions us for later
replacing the function pointers with C++ virtual methods.

**Candidates (by function pointer count):**
- VirtIODeviceClass: 23 function pointers
- SCSIDeviceClass: 20 function pointers
- DeviceClass: 11 function pointers
- MOS6522DeviceClass: 6 function pointers
- PCIDeviceClass: 1 (realize)

**Effort:** Small — just add inline wrapper methods to headers
**Risk:** Zero — purely additive, no existing code changes

### Step 5: Remaining 6 Hard-Blocked .c Files

Six files on the `cpp-port` branch remain as C due to C99-only features:

| File | Blocker |
|------|---------|
| hw/arm/xlnx-versal.c | Nested array designators `.chan[0]`, `or` keyword |
| hw/cxl/cxl-mailbox-utils.c | C99 designated range `[A ... B]` |
| hw/i386/kvm/xenstore_impl.c | `#include`d by C test file |
| hw/misc/imx6ul_ccm.c | Non-trivial designated array init |
| hw/misc/npcm_clk.c | Non-trivial designated array init |
| hw/misc/npcm_gcr.c | C99 designated range |

**Approach:** Each needs manual rewrite of the C99-only patterns:
- Designated ranges `[A ... B] = val` → `__attribute__((constructor))` init
- Nested array designators `.field[N]` → separate assignment after init
- `xenstore_impl.c` → modify test to include a wrapper instead

**Effort:** Medium — ~1 day for all 6
**Risk:** Low — isolated changes

### Step 6: True C++ Virtual Methods (Medium Risk, Transformative)

**Problem:** The `parent_obj` at offset 0 constraint prevents standard C++
virtual methods because the vtable pointer would be inserted before it.

**Possible approaches:**

**Option A: Vtable-after-parent layout**
Use `__attribute__((no_unique_address))` or layout tricks to place the vtable
pointer after `parent_obj`. Requires compiler-specific knowledge and testing.

**Option B: CRTP (Curiously Recurring Template Pattern)**
Use compile-time polymorphism instead of runtime vtable:
```cpp
template<typename Derived>
class DeviceBase {
    void doRealize(Error **errp) {
        static_cast<Derived*>(this)->realize(errp);
    }
};
```
No vtable pointer, but loses runtime polymorphism.

**Option C: Separate vtable field**
Add an explicit C++ interface pointer alongside QOM's mechanism:
```cpp
struct MOS6522State {
    SysBusDevice parent_obj;
    MOS6522Interface *cpp_vtable;  // explicit C++ dispatch table
    // ...
};
```
This maintains layout compatibility but adds a pointer per instance.

**Option D: Gradual migration via wrapper classes**
Create C++ wrapper classes that hold a pointer to the QOM object:
```cpp
class MOS6522 {
    MOS6522State *state;
public:
    virtual void portBWrite() { /* default */ }
};
```
Clean C++ but adds an indirection layer.

**Recommendation:** Start with Option A on a pilot device (MOS6522). If
compiler layout can be controlled, this is the cleanest path. If not, fall
back to Option C.

**Effort:** Large — requires careful design and testing
**Risk:** Medium — layout issues could cause subtle bugs

### Step 7: Property System Modernization

Replace `DEFINE_PROP_*` macros with typed C++ property declarations:

```cpp
// Before:
static const Property mos6522_properties[] = {
    DEFINE_PROP_UINT64("frequency", MOS6522State, frequency, 0),
};

// After:
QemuProperty<uint64_t> frequency{"frequency", 0};
```

**Effort:** Large — needs QemuProperty<T> template implementation
**Risk:** Medium — must maintain command-line `-device foo,prop=val` compat

### Step 8: Remove QOM Runtime Type Registry

The final goal — replace `object_new("type-name")` with C++ construction:

```cpp
// Before:
DeviceState *dev = qdev_new("serial");

// After:
auto *dev = new SerialState();
// or: auto *dev = DeviceFactory::create("serial");
```

This requires a factory registry mapping string names to C++ constructors,
since QEMU's command-line and QMP interface create devices by string name.

**Effort:** Very large
**Risk:** High — core infrastructure change

## Recommended Priority Order

1. **Step 4** (virtual dispatch wrappers) — easy win, zero risk
2. **Step 5** (remaining .c files) — gets to 100% C++
3. **Step 6** (true virtual methods) — the transformative change
4. **Step 7** (properties) — nice to have
5. **Step 8** (remove QOM registry) — long-term goal

## Current Metrics

- 89 commits on `qom-replacement`
- 2,211 .cpp files (6 .c remaining)
- 196 devices with ::classInit
- 183 deeply converted devices
- 3,043 reinterpret_cast (replacing QOM runtime casts)
- 108 _GET_CLASS macros (intentionally kept — runtime type lookup)
- 14/15 smoke tests passing
- All 5 target ISAs building clean
