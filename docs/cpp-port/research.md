# Research: Replacing QEMU's QOM with Native C++ Objects

## Executive Summary

QEMU's Object Model (QOM) is a runtime object-oriented system implemented in C
using macros, function pointers, and `void*` casts. Now that the codebase has
been ported to C++17, we can incrementally replace QOM machinery with native C++
classes, virtual methods, and RTTI — eliminating thousands of lines of
boilerplate while gaining compile-time type safety that QOM can never provide.

This document outlines the problems with QOM in a C++ codebase, proposes a
migration strategy, and describes the end-state architecture.

---

## 1. What QOM Does Today

QOM provides four things that C lacks natively:

1. **Single inheritance** — `parent_obj` as first struct member
2. **Virtual methods** — function pointers in `*Class` structs, set in `class_init()`
3. **Runtime type checking** — `OBJECT_CHECK()` / `object_dynamic_cast()`
4. **A property/introspection system** — `DEFINE_PROP_*` macros, command-line configuration

### 1.1 How a Device is Declared (Current QOM)

A typical device requires this boilerplate (using MOS6522 as example):

```c
/* --- Header --- */
#define TYPE_MOS6522 "mos6522"
OBJECT_DECLARE_TYPE(MOS6522State, MOS6522DeviceClass, MOS6522)

struct MOS6522State {
    SysBusDevice parent_obj;       /* "inheritance" via embedding */
    uint8_t a, b, dira, dirb;
    /* ... 20 more fields ... */
};

struct MOS6522DeviceClass {
    SysBusDeviceClass parent_class; /* "class inheritance" */
    void (*portB_write)(MOS6522State *dev);       /* virtual method */
    void (*portA_write)(MOS6522State *dev);       /* virtual method */
    uint64_t (*get_timer1_counter_value)(MOS6522State *dev, MOS6522Timer *ti);
    uint64_t (*get_timer2_counter_value)(MOS6522State *dev, MOS6522Timer *ti);
};

/* --- Implementation --- */
static void mos6522_init(Object *obj) { /* ... */ }
static void mos6522_finalize(Object *obj) { /* ... */ }
static void mos6522_reset_hold(Object *obj, ResetType type) { /* ... */ }

static const Property mos6522_properties[] = {
    DEFINE_PROP_UINT64("frequency", MOS6522State, frequency, 0),
};

static void mos6522_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    rc->phases.hold = mos6522_reset_hold;
    dc->vmsd = &vmstate_mos6522;
    device_class_set_props(dc, mos6522_properties);
    mdc->portB_write = mos6522_portB_write;
    mdc->portA_write = mos6522_portA_write;
    mdc->get_timer1_counter_value = mos6522_get_counter_value;
    mdc->get_timer2_counter_value = mos6522_get_counter_value;
}

static const TypeInfo mos6522_type_info = {
    .name = TYPE_MOS6522,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MOS6522State),
    .instance_init = mos6522_init,
    .instance_finalize = mos6522_finalize,
    .is_abstract = true,
    .class_size = sizeof(MOS6522DeviceClass),
    .class_init = mos6522_class_init,
};

static void mos6522_register_types(void)
{
    type_register_static(&mos6522_type_info);
}
type_init(mos6522_register_types)
```

This is **~60 lines of pure scaffolding** that contributes nothing to device
behavior. Every one of the 600+ device models in QEMU repeats this pattern.

### 1.2 How Virtual Method Dispatch Works

When code needs to call a virtual method, it goes through three macro
expansions and a runtime hash table lookup:

```c
/* User writes: */
uint64_t val = mdc->get_timer1_counter_value(s, ti);

/* But to GET mdc, they wrote: */
MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);

/* Which expands through: */
MOS6522_GET_CLASS(s)
  → OBJECT_GET_CLASS(MOS6522DeviceClass, s, TYPE_MOS6522)
    → OBJECT_CLASS_CHECK(MOS6522DeviceClass,
                         object_get_class(OBJECT(s)),
                         TYPE_MOS6522)
      → (MOS6522DeviceClass *)object_class_dynamic_cast_assert(
            object_get_class(OBJECT(s)),
            TYPE_MOS6522,
            __FILE__, __LINE__, __func__)
```

This is a **runtime string-based type check** with a 4-entry LRU cache. Every
virtual method call pays this cost. Compare to C++:

```cpp
uint64_t val = getTimer1CounterValue(ti);  // direct vtable call
```

### 1.3 What OBJECT_CHECK Actually Does at Runtime

```c
Object *object_dynamic_cast_assert(Object *obj, const char *typename,
                                   const char *file, int line, const char *func)
{
    /* Check 4-entry LRU cache first */
    for (i = 0; i < OBJECT_CLASS_CAST_CACHE; i++) {
        if (qatomic_read(&obj->klass->object_cast_cache[i]) == typename) {
            goto out;  /* cache hit — still a branch + 4 compares */
        }
    }

    /* Cache miss: walk the entire type hierarchy */
    inst = object_dynamic_cast(obj, typename);  /* strcmp chain */

    if (!inst && obj) {
        fprintf(stderr, "%s:%d:%s: Object %p is not an instance of type %s\n",
                file, line, func, obj, typename);
        abort();
    }

    /* Update LRU cache */
    /* ... shift entries, atomic store ... */
out:
    return obj;
}
```

In C++, `static_cast` is **zero-cost** and `dynamic_cast` uses compiler-
generated RTTI tables — no string comparisons, no hash table lookups, no LRU
cache management.

---

## 2. Problems with QOM in a C++ Codebase

### 2.1 No Compile-Time Type Safety

Every QOM cast is a `void*` round-trip:

```c
MOS6522State *s = MOS6522(obj);          /* runtime check only */
DeviceClass *dc = DEVICE_CLASS(klass);   /* runtime check only */
```

If you pass the wrong object, you get an **abort at runtime** instead of a
**compiler error at build time**. In a 2M+ line codebase, this is a constant
source of subtle bugs that only appear when specific code paths execute.

With C++ inheritance:

```cpp
MOS6522State *s = static_cast<MOS6522State *>(obj);   // compile-time verified
MOS6522State *s = dynamic_cast<MOS6522State *>(obj);   // runtime-checked, returns nullptr
```

### 2.2 Massive Boilerplate

Every device model requires:
- A `TypeInfo` struct with 10+ fields
- A `class_init()` function to set up virtual method pointers
- Manual property arrays with `DEFINE_PROP_*` macros
- A registration function + `type_init()` macro
- Explicit `OBJECT_DECLARE_TYPE` / `OBJECT_DECLARE_SIMPLE_TYPE`

Across 600+ devices, this is **~40,000 lines of scaffolding** that adds no
functionality.

### 2.3 Virtual Methods Are Fragile

Virtual methods are function pointers manually assigned in `class_init()`:

```c
static void mos6522_class_init(ObjectClass *oc, const void *data)
{
    MOS6522DeviceClass *mdc = MOS6522_CLASS(oc);
    mdc->portB_write = mos6522_portB_write;
    mdc->portA_write = mos6522_portA_write;
}
```

Problems:
- **No compiler enforcement** that you assigned all required methods
- **No override checking** — misspelling a method name silently fails
- **No final/sealed** — can't prevent further override
- Adding a new virtual method requires updating every subclass's `class_init()`

C++ `virtual`/`override`/`= 0` catches all of these at compile time.

### 2.4 Two Separate Structs for One Object

QOM splits every type into `FooState` (instance data) and `FooClass` (vtable).
These are defined in different places, connected only by string names, and
require separate size calculations. C++ unifies these into a single class
definition.

### 2.5 Properties Are Stringly-Typed

```c
object_property_set_int(obj, "frequency", 4000000, &error_abort);
```

If you misspell `"frequency"`, it compiles fine and fails at runtime. C++
member access is checked at compile time.

---

## 3. Proposed C++ Architecture

### 3.1 The End State: What a Device Should Look Like

```cpp
// mos6522.h
class MOS6522Device : public SysBusDevice
{
    QEMU_DEVICE(MOS6522Device, "mos6522")

public:
    MOS6522Device();
    ~MOS6522Device() override;

    void realize(Error **errp) override;
    void reset() override;

    // Virtual methods for subclass customization
    virtual void portBWrite();
    virtual void portAWrite();
    virtual uint64_t getTimer1CounterValue(MOS6522Timer *ti);
    virtual uint64_t getTimer2CounterValue(MOS6522Timer *ti);

    // Properties — type-checked at compile time
    uint64_t frequency() const { return theFrequency; }
    void setFrequency(uint64_t freq) { theFrequency = freq; }

    // Migration support
    static const VMStateDescription vmstate;

protected:
    uint8_t theA, theB, theDirA, theDirB;
    uint8_t theSR, theACR, thePCR, theIFR, theIER;
    MOS6522Timer theTimers[2];
    uint64_t theFrequency;
    qemu_irq theIRQ;

private:
    MemoryRegion theMem;
};

// mos6522.cpp
MOS6522Device::MOS6522Device()
    : theFrequency(0)
{
    memory_region_init_io(&theMem, OBJECT(this), &mos6522_ops, this,
                          "mos6522", MOS6522_NUM_REGS);
    sysbus_init_mmio(SYS_BUS_DEVICE(this), &theMem);
    sysbus_init_irq(SYS_BUS_DEVICE(this), &theIRQ);

    for (int i = 0; i < 2; i++) {
        theTimers[i].index = i;
    }
    theTimers[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mos6522_timer1, this);
    theTimers[1].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mos6522_timer2, this);
}

MOS6522Device::~MOS6522Device()
{
    timer_free(theTimers[0].timer);
    timer_free(theTimers[1].timer);
}

void MOS6522Device::reset()
{
    theB = 0; theA = 0;
    theDirB = 0xff; theDirA = 0;
    // ...
}

void MOS6522Device::portBWrite() { /* default implementation */ }
void MOS6522Device::portAWrite() { /* default implementation */ }
```

### 3.2 What Changes — and What Doesn't

| Feature | QOM (Current) | C++ (Proposed) |
|---------|--------------|----------------|
| Inheritance | `parent_obj` embedding | `: public Base` |
| Virtual methods | Function pointers in `*Class` | `virtual` methods |
| Type checking | Runtime string compare | Compile-time `static_cast` / `dynamic_cast` |
| Construction | `instance_init()` callback | Constructor |
| Destruction | `instance_finalize()` callback | Destructor |
| Properties | `DEFINE_PROP_*` macros | Getter/setter methods |
| VMState | **Unchanged** — still uses VMState macros | Same |
| MMIO dispatch | **Unchanged** — still uses `MemoryRegionOps` | Same |
| Type registration | **Simplified** — auto-registration macro | `QEMU_DEVICE()` macro |
| Interfaces | `InterfaceInfo` arrays | Multiple inheritance or `interface` base classes |

### 3.3 The QEMU_DEVICE Macro

A thin compatibility macro handles registration with QEMU's existing
infrastructure:

```cpp
#define QEMU_DEVICE(ClassName, TypeName)                          \
public:                                                           \
    static const char *staticTypeName() { return TypeName; }      \
    static TypeInfo typeInfo();                                    \
private:                                                          \
    static void _classInit(ObjectClass *oc, const void *data);    \
    static void _instanceInit(Object *obj);                       \
    static void _registerType() __attribute__((constructor));
```

This generates the QOM registration glue automatically while the class uses
normal C++ patterns internally.

### 3.4 Handling the QOM↔C++ Boundary

During migration, both systems must coexist. The key insight is that **QOM
objects and C++ objects can share the same memory layout** — a C++ object with
`Object` as its first base class IS a valid QOM object:

```cpp
class Device : public Object     // Object is first member — QOM compatible
{
    // C++ virtual methods here
    // QOM still sees this as an Object with the right TypeInfo
};
```

This means:
- Old C code can still use `DEVICE(obj)` to cast
- New C++ code can use `static_cast<Device*>(obj)`
- Both see the same memory layout
- Migration is incremental — one device at a time

---

## 4. Migration Strategy

### Phase 1: Infrastructure (Estimated: 2-4 weeks)

**Goal:** Create base C++ classes that wrap QOM without breaking existing code.

1. Create `QemuObject` C++ base class that wraps `Object`:
   ```cpp
   class QemuObject {
   protected:
       Object parent_obj;  // QOM compatibility
   public:
       QemuObject();
       virtual ~QemuObject();
   };
   ```

2. Create `QemuDevice` base class wrapping `DeviceState`:
   ```cpp
   class QemuDevice : public QemuObject {
   protected:
       DeviceState parent_obj;
   public:
       virtual void realize(Error **errp);
       virtual void reset();
       const char *id() const;
   };
   ```

3. Create `QemuSysBusDevice`, `QemuPCIDevice` wrappers similarly.

4. Create the `QEMU_DEVICE()` registration macro that generates TypeInfo
   from C++ class metadata.

5. Create a `QemuProperty<T>` template for type-safe property declarations:
   ```cpp
   template<typename T>
   class QemuProperty {
       T theValue;
       T theDefault;
       const char *theName;
   public:
       QemuProperty(const char *name, T defaultVal);
       T value() const { return theValue; }
       void setValue(T val) { theValue = val; }
       // Auto-generates QEMU property info for registration
   };
   ```

### Phase 2: Pilot Devices (Estimated: 2-4 weeks)

**Goal:** Convert 5-10 simple devices to validate the approach.

Good candidates for first conversions:
- `hw/misc/mos6522.cpp` — simple SysBus device with virtual methods
- `hw/timer/hpet.cpp` — timer with MMIO and IRQs
- `hw/char/serial.cpp` — character device with VMState
- `hw/net/e1000e.cpp` — PCI device with complex state
- `hw/block/virtio-blk.cpp` — VirtIO device

For each device:
1. Create C++ class inheriting from `QemuSysBusDevice`/`QemuPCIDevice`
2. Move state fields into the class as members
3. Convert `class_init` virtual method assignments to C++ `virtual` declarations
4. Convert `instance_init`/`instance_finalize` to constructor/destructor
5. Keep VMState unchanged (it works with offsetof on C++ classes)
6. Verify: builds, boots, migrates, passes existing tests

### Phase 3: Mass Conversion (Estimated: 2-3 months)

**Goal:** Convert all ~600 device models.

This can be largely automated:
1. Script to identify QOM patterns in each file
2. Generate C++ class skeleton from `TypeInfo` + `*State` + `*Class` structs
3. Move function bodies into class methods
4. Human review for correctness
5. Build + test after each batch

### Phase 4: Remove QOM Runtime (Estimated: 1-2 months)

**Goal:** Once all devices are C++ classes, simplify the type system.

1. Replace `object_dynamic_cast_assert()` with C++ `dynamic_cast`
2. Remove the LRU type-checking cache (unnecessary with RTTI)
3. Remove `TypeInfo` registration infrastructure
4. Replace property hash tables with compile-time reflection
5. Simplify `object_new()` to use C++ `new`

---

## 5. Detailed Design: Key Components

### 5.1 Virtual Method Migration

**Before (QOM):**
```c
// In header: function pointer typedef
struct MOS6522DeviceClass {
    SysBusDeviceClass parent_class;
    void (*portB_write)(MOS6522State *dev);
};

// In class_init: manual assignment
mdc->portB_write = mos6522_portB_write;

// Dispatch: 3 macro expansions + runtime type check
MOS6522DeviceClass *mdc = MOS6522_GET_CLASS(s);
mdc->portB_write(s);
```

**After (C++):**
```cpp
// In header: virtual method declaration
class MOS6522Device : public SysBusDevice {
    virtual void portBWrite();
};

// In subclass: override with compiler checking
class ADBMOS6522 : public MOS6522Device {
    void portBWrite() override;  // compiler verifies signature match
};

// Dispatch: single vtable lookup (same cost as C function pointer)
portBWrite();
```

**Benefits:**
- `override` keyword catches method signature mismatches at compile time
- `= 0` makes abstract methods that MUST be implemented
- `final` prevents unwanted overrides
- No manual function pointer assignment — compiler handles vtable

### 5.2 Type Checking Migration

**Before (QOM):**
```c
MOS6522State *s = MOS6522(obj);
// Expands to: object_dynamic_cast_assert(obj, "mos6522", __FILE__, __LINE__)
// Runtime cost: cache check (4 strcmp), possible hash walk, abort on failure
```

**After (C++):**
```cpp
// Zero-cost downcast (when you know the type):
MOS6522Device *s = static_cast<MOS6522Device *>(obj);

// Safe downcast (when type is uncertain):
MOS6522Device *s = dynamic_cast<MOS6522Device *>(obj);
if (!s) { /* handle wrong type */ }
```

**Benefits:**
- `static_cast` is zero instructions — pure compile-time
- `dynamic_cast` uses RTTI tables, no string comparison
- Wrong casts caught by compiler or clean nullptr return

### 5.3 Interface Migration

**Before (QOM interfaces):**
```c
static const InterfaceInfo rtl8139_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

// Check at runtime:
if (object_dynamic_cast(obj, TYPE_HOTPLUG_HANDLER)) { ... }
```

**After (C++ multiple inheritance):**
```cpp
class RTL8139 : public PCIDevice, public ConventionalPCIDevice {
    // ConventionalPCIDevice is an empty interface base
};

// Check at compile time:
static_assert(std::is_base_of<ConventionalPCIDevice, RTL8139>::value);

// Or at runtime:
if (auto *hp = dynamic_cast<HotplugHandler *>(obj)) { ... }
```

### 5.4 Property Migration

**Before (QOM properties):**
```c
static const Property mos6522_properties[] = {
    DEFINE_PROP_UINT64("frequency", MOS6522State, frequency, 0),
};
// Access: object_property_set_int(obj, "frequency", val, errp);
// No compile-time checking of property name or type
```

**After (C++ typed properties):**
```cpp
class MOS6522Device : public SysBusDevice {
    QemuProperty<uint64_t> theFrequency{"frequency", 0};
public:
    uint64_t frequency() const { return theFrequency.value(); }
    void setFrequency(uint64_t f) { theFrequency.setValue(f); }
};
// Access: dev->setFrequency(val);  // compile-time type checked
```

For command-line compatibility, `QemuProperty<T>` registers itself with the
existing QEMU property system at construction time, so `-device mos6522,
frequency=4000000` still works.

### 5.5 VMState: Keep As-Is

VMState is already compatible with C++ classes because it uses `offsetof()` to
locate fields. The macros work with both C structs and C++ classes as long as
the class is standard-layout (which device state classes are).

No changes needed to VMState. This is a major advantage — migration
compatibility is preserved automatically.

---

## 6. Risks and Mitigations

### 6.1 Memory Layout Compatibility

**Risk:** C++ may add vtable pointers that change struct layout, breaking
VMState `offsetof()` calculations and QOM `parent_obj` embedding.

**Mitigation:** C++ classes with virtual methods have the vtable pointer as the
first hidden member. By making `Object` the first base class and accounting for
the vtable pointer offset, layout compatibility is preserved. Use
`static_assert(offsetof(...) == expected)` to verify at compile time.

### 6.2 Incremental Migration

**Risk:** Big-bang conversion is infeasible for a 2M-line codebase.

**Mitigation:** The proposed approach is fully incremental. A C++ device class
and a QOM device class can coexist in the same binary because the C++ class
generates a valid `TypeInfo` for QOM registration. Convert one device at a time,
test after each conversion.

### 6.3 Upstream Divergence

**Risk:** QEMU upstream is C-only; this fork diverges further.

**Mitigation:** This is already a deliberate fork (QEMU++). The goal is not
upstream compatibility but demonstrating that C++ produces better, safer code.
Track upstream for bug fixes via cherry-picks of the logic, not the scaffolding.

### 6.4 Constructor/Destructor vs instance_init/realize

**Risk:** QOM's two-phase init (instance_init + realize) allows introspection
before resource allocation. C++ constructors are single-phase.

**Mitigation:** Keep the two-phase pattern with a `realize()` virtual method:

```cpp
class QemuDevice {
public:
    QemuDevice();                           // lightweight init (like instance_init)
    virtual void realize(Error **errp);     // heavy init (like DeviceClass.realize)
    virtual ~QemuDevice();                  // cleanup (like instance_finalize)
};
```

The constructor handles field initialization. `realize()` handles resource
allocation, error reporting, and anything that might fail.

---

## 7. Expected Benefits

### 7.1 Quantified Improvements

| Metric | QOM (Current) | C++ (Proposed) | Improvement |
|--------|--------------|----------------|-------------|
| Lines per device (boilerplate) | ~60-110 | ~10-20 | 5-6x reduction |
| Total boilerplate across 600 devices | ~40,000 lines | ~8,000 lines | 32,000 lines eliminated |
| Type checking | Runtime (string-based) | Compile-time | Bugs caught earlier |
| Virtual dispatch overhead | Cache lookup + branch | Single indirect call | Faster |
| Wrong-type cast detection | Runtime abort | Compile error | Safer |
| New virtual method addition | Update all class_init() | Add to header | Faster development |

### 7.2 Qualitative Improvements

- **Readability**: Device implementations read as straightforward classes
  instead of macro-heavy scaffolding interspersed with callback registration
- **IDE support**: C++ classes work with autocomplete, go-to-definition,
  refactoring tools. QOM macros confuse every IDE.
- **Onboarding**: New developers understand C++ classes immediately; QOM
  requires studying QEMU-specific macro systems
- **Refactoring safety**: Renaming a virtual method with C++ catches all call
  sites at compile time. With QOM, you grep and hope.

---

## 8. Non-Goals

This proposal does NOT aim to:

- Replace VMState with a new serialization system (VMState works fine)
- Replace MemoryRegionOps dispatch (it's already efficient)
- Remove all C code (utility functions, TCG, coroutines stay as C)
- Match upstream QEMU's API (this is a fork)
- Use advanced C++ features (no templates beyond QemuProperty, no exceptions,
  no RTTI beyond dynamic_cast, no smart pointers for QOM-managed objects)

---

## 9. Open Questions

1. **Should we use C++ RTTI or keep QOM's type registry?**
   C++ RTTI provides `dynamic_cast` and `typeid` but doesn't support QEMU's
   string-based type lookup for command-line device creation. We may need a
   thin registration layer that maps type names to factory functions.

2. **How do we handle QOM's lazy class initialization?**
   QOM initializes classes on first use. C++ static initialization runs at
   program startup. This changes timing but shouldn't affect correctness.

3. **What about hot-pluggable devices?**
   Device creation via string name (`object_new("rtl8139")`) needs a factory
   registry. A simple `std::unordered_map<std::string, FactoryFn>` suffices.

4. **How does this interact with QAPI-generated code?**
   QAPI generates C code for QMP commands. The QAPI layer sits above the device
   model and would continue to work through a C-compatible wrapper layer.

---

## 10. Recommended Next Steps

1. **Prototype the base classes** — `QemuObject`, `QemuDevice`,
   `QemuSysBusDevice`, `QemuPCIDevice` wrappers in `include/qom/cpp/`
2. **Convert MOS6522** as the first pilot device
3. **Validate VMState** — verify migration works with C++ class layout
4. **Validate QMP** — verify device introspection still works
5. **Convert 5 more devices** across different categories (PCI, VirtIO, timer)
6. **Write a conversion guide** for bulk migration of remaining devices
7. **Automate** the mechanical parts of conversion with a Python script
