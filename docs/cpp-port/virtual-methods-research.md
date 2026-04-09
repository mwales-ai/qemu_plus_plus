# Research: Replacing QOM Function Pointer Dispatch with C++ Virtual Methods

## The Problem

QEMU's QOM implements "virtual methods" as function pointers stored in
`*DeviceClass` structs. This requires three steps for every virtual call:

```c
// Step 1: Get the class struct (runtime type lookup + cast)
MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);

// Step 2: Check the function pointer isn't NULL (sometimes)
if (mdc->portB_write) {

// Step 3: Call through the function pointer
    mdc->portB_write(s);
}
```

In C++, this would be:

```cpp
portBWrite();  // single vtable lookup, compiler-verified
```

The QOM approach has several problems:
1. **No compile-time checking** — assigning a wrong function signature compiles silently
2. **No `override` keyword** — subclass typos in class_init go undetected
3. **No `= 0` (pure virtual)** — can't enforce that subclasses implement required methods
4. **Verbose dispatch** — every call needs `GET_CLASS` + dereference
5. **No `final`** — can't prevent unwanted overrides
6. **Manual vtable setup** — each class_init must manually assign every function pointer

## Real-World Examples

### Example 1: MOS6522 Timer Device (6 virtual methods)

**Current QOM dispatch (mos6522.cpp:131-138):**
```c
static uint64_t get_counter_value(MOS6522State *s, MOS6522Timer *ti)
{
    MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);

    if (ti->index == 0) {
        return mdc->get_timer1_counter_value(s, ti);
    } else {
        return mdc->get_timer2_counter_value(s, ti);
    }
}
```

**Current class_init setup (mos6522.cpp:708-718):**
```c
static void mos6522_class_init(ObjectClass *oc, const void *data)
{
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);
    mdc->portB_write = mos6522_portB_write;
    mdc->portA_write = mos6522_portA_write;
    mdc->get_timer1_counter_value = mos6522_get_counter_value;
    mdc->get_timer2_counter_value = mos6522_get_counter_value;
    mdc->get_timer1_load_time = mos6522_get_load_time;
    mdc->get_timer2_load_time = mos6522_get_load_time;
}
```

**Current subclass override (mac_via.cpp:1420-1425):**
```c
static void mos6522_q800_via2_class_init(ObjectClass *oc, const void *data)
{
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);
    mdc->portB_write = mos6522_q800_via2_portB_write;
    // Other methods inherit from parent because class_init copies parent first
}
```

### Example 2: VirtIO Device (23 virtual methods)

**Current dispatch (hw/virtio/virtio.cpp:3065-3112):**
```c
void virtio_save(VirtIODevice *vdev, QEMUFile *f)
{
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_GET_CLASS(vdev);
    // ... save common state ...
    if (vdc->save != NULL) {
        vdc->save(vdev, f);
    }
}
```

**Current class_init (virtio-blk.cpp):**
```c
void VirtIOBlock::classInit(ObjectClass *klass, const void *data)
{
    VirtioDeviceClass *vdc = reinterpret_cast<VirtioDeviceClass *>(klass);
    vdc->realize = virtio_blk_device_realize;
    vdc->unrealize = virtio_blk_device_unrealize;
    vdc->get_config = virtio_blk_update_config;
    vdc->set_config = virtio_blk_set_config;
    vdc->get_features = virtio_blk_get_features;
    vdc->set_status = virtio_blk_set_status;
    vdc->reset = virtio_blk_reset;
    vdc->save = virtio_blk_save_device;
    vdc->load = virtio_blk_load_device;
    // 9 of 23 virtual methods assigned — rest are NULL/inherited
}
```

## The Layout Constraint

The fundamental obstacle to using C++ virtual methods is QOM's memory layout
requirement. Every QOM object embeds its parent type as the **first field**:

```
MOS6522State at address 0x1000:
  +0x0000: SysBusDevice parent_obj    ← MUST be at offset 0
    +0x0000: DeviceState parent_obj
      +0x0000: Object parent_obj
        +0x0000: ObjectClass *klass   ← QOM type pointer
```

QOM relies on `(Object *)ptr == (MOS6522State *)ptr` — same address. This
means a C-style cast `(Object *)mos6522_ptr` works because `Object` is at
offset 0.

If we add C++ virtual methods, the compiler inserts a vtable pointer:

```
MOS6522State with virtual methods:
  +0x0000: vptr (C++ vtable pointer)  ← BREAKS QOM!
  +0x0008: SysBusDevice parent_obj    ← No longer at offset 0!
```

Now `(Object *)mos6522_ptr` points to the vtable pointer, not the Object.
Every QOM cast, VMState offset, and property access breaks.

## Design Options

### Option A: No Vtable — Dispatch Wrapper Methods

**Approach:** Keep QOM's function pointer mechanism but add inline dispatch
methods to the device state struct. No C++ vtable at all.

```cpp
struct MOS6522State {
    SysBusDevice parent_obj;  // still at offset 0
    // ... fields ...

    // Dispatch wrappers — call through QOM function pointers
    inline void portBWrite() {
        MOS6522_GET_CLASS(this)->portB_write(this);
    }
    inline void portAWrite() {
        MOS6522_GET_CLASS(this)->portA_write(this);
    }
    inline uint64_t getTimer1CounterValue(MOS6522Timer *ti) {
        return MOS6522_GET_CLASS(this)->get_timer1_counter_value(this, ti);
    }
};

// Subclass still overrides in class_init:
void Mac_VIA2_class_init(ObjectClass *oc, const void *data) {
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);
    mdc->portB_write = via2_portB_write;  // same as before
}

// Call sites become clean:
void MOS6522State::writeReg(hwaddr addr, uint64_t val) {
    // Before: MOS6522_GET_CLASS(s)->portB_write(s);
    // After:
    portBWrite();
}
```

**Advantages:**
- Zero layout change — `parent_obj` stays at offset 0
- Zero runtime overhead — inline wrappers compile to same code
- Incremental — can convert one device at a time
- No subclass changes needed — class_init works identically
- No risk — purely additive

**Disadvantages:**
- Still function pointer dispatch underneath — no `override`/`final`
- Subclasses still use manual function pointer assignment
- Not "real" C++ virtual methods — it's a cosmetic improvement
- No pure virtual enforcement

**Verdict:** Best first step. Zero risk, immediate cleanup of call sites.

### Option B: Virtual Methods on a Separate C++ Class

**Approach:** Create a C++ class hierarchy alongside QOM, connected by a
pointer in the state struct.

```cpp
// C++ interface class (has vtable)
class IMOS6522 {
public:
    virtual ~IMOS6522() = default;
    virtual void portBWrite() = 0;
    virtual void portAWrite() = 0;
    virtual uint64_t getTimer1CounterValue(MOS6522Timer *ti) = 0;
    virtual uint64_t getTimer2CounterValue(MOS6522Timer *ti) = 0;
};

// QOM struct holds a pointer to the C++ interface
struct MOS6522State {
    SysBusDevice parent_obj;  // offset 0 preserved
    IMOS6522 *cppInterface;   // points to C++ object with vtable
    // ... fields ...
};

// Default implementation
class MOS6522Impl : public IMOS6522 {
    MOS6522State *state;
public:
    MOS6522Impl(MOS6522State *s) : state(s) {}
    void portBWrite() override { /* default */ }
    void portAWrite() override { /* default */ }
};

// Subclass override
class VIA2Impl : public MOS6522Impl {
public:
    void portBWrite() override { /* VIA2 specific */ }
};

// Usage:
void MOS6522State::writeReg(hwaddr addr, uint64_t val) {
    cppInterface->portBWrite();  // real C++ virtual dispatch!
}
```

**Advantages:**
- Real C++ virtual methods with `override`, `final`, `= 0`
- Compiler enforces correct signatures
- Clean subclass pattern — just override methods
- `parent_obj` stays at offset 0

**Disadvantages:**
- Extra pointer per device instance (8 bytes)
- Extra allocation for the C++ interface object
- Two objects to manage lifecycle for (QOM state + C++ interface)
- `this` in the interface doesn't have direct access to state fields
- Subclasses need to create the right interface impl in instance_init
- Awkward split: device state in QOM struct, behavior in C++ class

**Verdict:** Architecturally clean but practically awkward. The split between
state and behavior creates friction. Better suited for a full rewrite than
incremental migration.

### Option C: Virtual Methods with Controlled Layout

**Approach:** Use compiler attributes to control where the vtable pointer goes.

```cpp
struct MOS6522State {
    SysBusDevice parent_obj;  // offset 0 preserved

    // Virtual methods — vtable pointer goes HERE (after parent_obj)
    virtual void portBWrite();
    virtual void portAWrite();
    virtual uint64_t getTimer1CounterValue(MOS6522Timer *ti);
    virtual uint64_t getTimer2CounterValue(MOS6522Timer *ti);

    // ... fields ...
};
```

With standard C++ compilers, the vtable pointer is at offset 0 — before
`parent_obj`. This breaks QOM.

**Can we control it?** On GCC/Clang, there is **no standard way** to place
the vtable pointer at a specific offset. The vtable pointer always comes
first in the object layout when the class has virtual methods.

**Possible workaround:** Use an intermediate non-virtual base:

```cpp
struct MOS6522QOMBase {
    SysBusDevice parent_obj;  // offset 0
};

struct MOS6522State : MOS6522QOMBase {
    // vtable pointer inserted HERE by compiler (after parent_obj)
    virtual void portBWrite();
    // ... fields ...
};
```

**Problem:** `sizeof(MOS6522QOMBase)` is the size of `SysBusDevice`, but
`sizeof(MOS6522State)` adds 8 bytes for the vtable pointer. QOM allocates
`instance_size = sizeof(MOS6522State)` bytes, which is correct. But QOM
also does `(Object *)ptr` which goes to offset 0, which IS `parent_obj`
(in the base) — so this **might work**.

**Critical test needed:** Does `(Object *)mos6522_ptr` correctly access the
`Object` at offset 0 when `MOS6522State` inherits from `MOS6522QOMBase`?

With standard C++ layout rules:
```
MOS6522State layout:
  +0x0000: MOS6522QOMBase
    +0x0000: SysBusDevice parent_obj
      +0x0000: DeviceState parent_obj
        +0x0000: Object parent_obj   ← Still at offset 0!
  +sizeof(SysBusDevice): vptr        ← Vtable pointer after parent
  +sizeof(SysBusDevice)+8: fields    ← Device-specific fields
```

Wait — this **doesn't work** either. When a class has virtual methods, the
compiler puts the vtable pointer at offset 0 of THAT class, pushing the
base class members after it. The layout would actually be:

```
MOS6522State layout (compiler-dependent):
  +0x0000: vptr                      ← BREAKS: vtable before parent
  +0x0008: MOS6522QOMBase
    +0x0008: SysBusDevice parent_obj ← Not at offset 0!
```

**Advantages (if layout could be controlled):**
- Real C++ virtual methods
- Clean syntax
- Compiler-enforced overrides

**Disadvantages:**
- **Compiler-dependent layout** — no standard way to control vtable position
- GCC and Clang both put vtable at offset 0 of the most-derived class
- Breaks the fundamental `(Object *)ptr` QOM assumption
- Would need every QOM cast to account for the vtable offset

**Verdict:** Not feasible with standard C++ compilers. The vtable pointer
always goes at offset 0, which is where QOM needs `parent_obj`.

### Option D: Replace QOM's Class Struct with C++ Vtable Entirely

**Approach:** Instead of having both a QOM `*DeviceClass` and C++ vtable,
replace the QOM class struct's function pointers with actual C++ virtual
methods. The QOM `ObjectClass *klass` pointer becomes a pointer to a C++
class with virtual methods.

This requires deep changes to the QOM core:
1. `ObjectClass` becomes a C++ class with virtual methods
2. `type_initialize()` no longer copies function pointers — C++ vtable handles it
3. `class_init()` no longer assigns function pointers — constructors do it
4. `OBJECT_GET_CLASS()` returns a C++ class pointer

```cpp
class MOS6522Class : public SysBusDeviceClass {
public:
    virtual void portBWrite(MOS6522State *dev);
    virtual void portAWrite(MOS6522State *dev);
    virtual uint64_t getTimer1CounterValue(MOS6522State *dev, MOS6522Timer *ti);
    // ...
};

class VIA2Class : public MOS6522Class {
public:
    void portBWrite(MOS6522State *dev) override;
    // inherits other methods from MOS6522Class
};
```

The device state struct stays the same — no vtable in the instance. The
vtable is in the **class** object, which is shared among all instances of
that type (same as QOM's current design).

**Advantages:**
- True C++ virtual methods with `override`/`final`/`= 0`
- No change to instance layout — `parent_obj` stays at offset 0
- Class objects already exist in QOM (one per type, shared by instances)
- Conceptually identical to QOM's current design
- `klass` pointer already points to the class object at runtime

**Disadvantages:**
- Requires changing `ObjectClass` and all `*DeviceClass` to C++ classes
- Every `class_init()` must become a constructor
- QOM's `type_initialize()` memcpy of parent class must be replaced with
  C++ base class constructor
- `OBJECT_GET_CLASS()` return type changes
- Massive infrastructure change touching every device
- Interface inheritance becomes C++ multiple inheritance on the class object

**Verdict:** This is the correct long-term architecture. It matches QOM's
existing design (one class object per type, shared by instances, virtual
dispatch through the class pointer). But it requires rewriting the QOM core,
which is a very large undertaking.

## Recommendation

**Phase 1 (now):** Implement **Option A** — dispatch wrapper methods.
Zero risk, immediate benefit, positions all call sites for future migration.

**Phase 2 (medium-term):** Implement **Option D** incrementally.
Start by making `ObjectClass` a C++ class, then convert one `*DeviceClass`
at a time. This is the correct architecture because:
- The class object IS QOM's vtable — making it a C++ class is natural
- Instance layout doesn't change — no offset 0 problems
- It's the same design QOM already has, just with compiler enforcement

**Not recommended:** Options B and C. Option B creates an awkward split
between state and behavior. Option C is infeasible because compilers don't
let you control vtable pointer placement.

## Metrics

If we convert the top device class hierarchies:
- VirtIODeviceClass: 23 virtual methods × ~30 subclasses = ~690 assignments eliminated
- SCSIDeviceClass: 20 virtual methods × ~15 subclasses = ~300 assignments eliminated
- DeviceClass: 11 virtual methods × ~600 devices = ~6,600 assignments eliminated
- MOS6522DeviceClass: 6 virtual methods × 3 subclasses = ~18 assignments eliminated

Total: ~7,600 manual function pointer assignments that become compiler-verified
virtual method overrides.
