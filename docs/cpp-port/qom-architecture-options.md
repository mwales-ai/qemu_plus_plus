# The QOM Problem and What to Do About It

## The situation in one paragraph

QOM is a runtime object system written in C using macros, `void*` casts,
function-pointer dispatch tables, and offset-0 struct embedding. The QEMU++
project is trying to port it into C++17 while keeping QOM itself intact.
What we're calling "Option D" (C++ virtual methods on QOM class structs) is
an attempt to have both — and it's hitting the wall where C++ inheritance
and QOM's C layout are mutually incompatible at the ABI level. Every new
conversion adds a `qom_fixup_vtable<T>()` hack, `#ifdef __cplusplus`
dual-view structs, and `reinterpret_cast`s to paper over the mismatch. The
warts are compounding. Smoke tests regressed from 13/15 to 5/15 at commit
`0b9e54f967` and nobody noticed for 13 subsequent commits because they all
build clean.

## Why this is actually hard

Four genuine problems, not just implementation bugs:

### 1. Two type systems can't share one memory layout

QOM expects `ObjectClass` at offset 0 of every class struct. C++ expects
the vtable pointer at offset 0 of every polymorphic class. These can both
be true only if `ObjectClass` is itself polymorphic — but `ObjectClass` is
C code that's accessed from C-layout assumptions everywhere in QOM's
runtime (`klass->type`, `DEVICE_CLASS(klass)`, the cast-cache, the property
hash). Making it polymorphic is a domino that knocks over everything
downstream.

### 2. QOM macros are reinterpret_casts in disguise

```c
#define OBJECT_CLASS(klass) ((ObjectClass *)(klass))
```

That C-style cast, when `klass` is a C++ polymorphic type, does
`reinterpret_cast`, not `static_cast`. It doesn't adjust the pointer for
the vtable offset. Every single `DEVICE_CLASS()`, `PCI_DEVICE_CLASS()`,
`OBJECT_CHECK()` macro has this problem silently. We've been getting away
with it because most of the time the code accesses the memory through the
original `ObjectClass *` pointer QOM handed us, not through a
C++-constructed object.

### 3. `qom_fixup_vtable<T>()` is structurally wrong, not just buggy

It takes a stack-constructed `T tmp;` whose vtable-pointer lives at offset
0 of `tmp`, and memcpy's 8 bytes into the live class struct at offset 0 —
which is where QOM put `ObjectClass::type`. It destroys `type` on every
call. MOS6522 survived because its types aren't enumerated during
`-machine help`; virtio-serial isn't so lucky. Any "fix" that keeps the
concept (write the vtable pointer somewhere on the class struct) requires
the rest of the class struct to be in C++ layout, which cascades back to
problem (1).

### 4. The warts are load-bearing

You can't simplify any individual wart without removing QOM.
`#ifdef __cplusplus` dual views exist because of (1). `reinterpret_cast`
exists because of (2). `qom_fixup_vtable` exists because we want C++
virtuals on C memory. Each wart supports the others. It's a Jenga tower.

## The options

### Option A — Patch and continue

Fix `qom_fixup_vtable` by some layout trick (the cleanest being: add a
single `virtual ~ObjectClass()` so every QOM class is polymorphic and the
vtable slot is part of the canonical layout at offset 0, with `type` moved
to offset 8). Update every `offsetof()` user, every hand-written VMState
descriptor, and every `container_of()` call that assumes the old offsets.
Keep driving Option D conversions once the layout stabilizes.

**What it looks like:** Same code as today plus a coordinated
all-the-offsets-shift-by-8 patch. Every device file changes. VMState
on-wire format changes by 8 bytes per class pointer. The
`#ifdef __cplusplus` dual views, `qom_fixup_vtable` calls, and
`reinterpret_cast`s all stay.

**Cost:** One week of careful work, plus re-stabilizing smoke tests.
Doesn't remove a single wart, just fixes the crash.

**Verdict:** Preserves progress. Also preserves the thing we're trying to
escape.

### Option B — "Methods pointer" sidecar

Stop putting virtual methods on QOM class structs entirely. Add one
pointer field to each class struct that points to a separate polymorphic
C++ object holding the virtual methods. Class struct stays pure C-layout.
Virtual dispatch goes `class->methods->foo()`.

**What it looks like:**

```cpp
/* In include/hw/virtio/virtio-serial.h (pure C layout, QOM unchanged): */
struct VirtIOSerialPortClass {
    DeviceClass parent_class;
    const VirtIOSerialPortMethods *methods;
    uint32_t is_console;
};

/* In a new header, the C++ method table: */
class VirtIOSerialPortMethods {
public:
    virtual void init(VirtIOSerialPort *p, Error **errp) {}
    virtual void exit(VirtIOSerialPort *p) {}
    /* ... etc */
};

/* Concrete device provides its own: */
class VirtConsolePortMethods : public VirtIOSerialPortMethods {
    void init(VirtIOSerialPort *p, Error **errp) override;
};
static const VirtConsolePortMethods virt_console_methods;

/* class_init: */
static void virt_console_class_init(ObjectClass *oc, const void *data) {
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_CLASS(oc);
    k->methods = &virt_console_methods;
}
```

**Cost:** Moderate. Each converted hierarchy gets ~30 lines of new
scaffolding (the methods class plus its concrete subclass instances).
Doable by script. No layout mismatch. No `qom_fixup_vtable`.

**Verdict:** The least-bad incremental fix. Still has two type systems —
QOM for the class struct, C++ for the method table — but the seam is
clean. Doesn't remove `OBJECT_DECLARE_TYPE`, `TypeInfo`, `DEFINE_PROP_*`,
`OBJECT_CLASS_CHECK` string comparisons, etc. It's an honest sidecar that
doesn't pretend to be native inheritance.

### Option C — Make ObjectClass polymorphic; port QOM internals to match

Bite the bullet from Option A but go all the way: treat C++ inheritance as
the primary type system, rewrite QOM's internal cast/check/registry
functions to use `static_cast` / `typeid` / `type_info::name()` instead of
manual string hash tables and offset math. Keep `type_register_static`,
`type_init`, `TypeInfo` as a thin facade over the C++ RTTI.

**What it looks like:** `qom/object.cpp` shrinks by 60%. Half of
`include/qom/object.h` disappears (all the cast-cache machinery,
`object_class_dynamic_cast_assert`, etc.). Class structs become normal C++
classes with `: ParentClass` inheritance. No more `#ifdef __cplusplus` —
there is only the C++ view. No more `qom_fixup_vtable`. Every QOM macro
(`OBJECT_CHECK`, `DEVICE_CLASS`, etc.) becomes a one-line `static_cast`.

Example of what a single device looks like after:

```cpp
class VirtConsolePort : public VirtIOSerialPort {
public:
    static constexpr const char *TYPE = "virtconsole";
    void init(Error **errp) override;
    void exit() override;
};
```

No `TypeInfo`. No `class_init`. No `DEVICE_CLASS(oc)`. No
`reinterpret_cast`. The compiler knows the type hierarchy; ask it.

**Cost:** Two to three weeks of focused work across QOM internals, every
class struct header, and every device's `class_init`. Can be scripted.
Smoke tests must be green first.

**Verdict:** This is "remove the warts, keep QEMU's device scope intact."
The architecture becomes consistent with itself. You still have ~600
device files, but they're dramatically simpler.

### Option D — Clean-slate device model for the 5 target ISAs

Stop trying to port QOM. Replace it in-place with an idiomatic C++ device
model. This is the "what would QEMU look like if a C++ team had written it
in 2026" option.

**What it looks like:**

- A new base class tree: `CppObject` → `CppDevice` → `CppSysBusDevice` /
  `CppPCIDevice` / `CppISADevice`. Already stubbed in
  `include/qom/cpp/object.h`.
- Device state is just class members with `the` prefix per the coding
  standard — no more `DeviceState parent_obj; uint32_t cr; uint32_t lcr;`
  gymnastics, just `uint32_t theCr; uint32_t theLcr;` in a `PL011` class.
- Construction is a C++ constructor. Realize is a virtual method. Reset
  is a virtual method. Property binding is
  `bindProperty("baudbase", &PL011::theBaudBase)` — a template function
  that walks `offsetof` at compile time via member pointers.
- Migration state replaces `VMStateDescription` with a
  `serialize(QEMUFile *)` method or a `migrationFields(MigrationBuilder &)`
  visitor — either works, both are 5× shorter.
- QMP introspection keeps working via a single global registry:
  `std::unordered_map<std::string, DeviceFactory>` populated by
  `REGISTER_DEVICE("pl011")` macros.
- `-device pl011,...` dispatches through that registry → factory →
  `new PL011()`.

A real PL011 example — before/after side by side:

**Before (current tree, `hw/char/pl011.cpp`):** ~870 lines, mostly
scaffolding.
- 40 lines of `VMStateDescription` compound literals
- 25 lines of `Property pl011_properties[]`
- 15 lines of `pl011_class_init` + `TypeInfo` + `type_init`
- 60 lines of QOM `OBJECT_DECLARE_TYPE` + `DECLARE_INSTANCE_CHECKER` +
  struct definitions in header
- ~730 lines of actual PL011 emulation logic

**After (Option D):** ~550 lines, all signal.
- Class declaration: `class PL011 : public CppSysBusDevice { ... };` in
  the header
- Fields are regular C++ data members
- `void realize() override`, `void reset() override` — pure virtuals from
  the base
- `REGISTER_DEVICE("pl011")` one-liner for registration
- `void migrate(MigrationStream &m) override { m(theCr, theLcr, theFr, ...); }`
  — one line replaces the VMState descriptor
- The emulation logic is identical, modulo `s->cr` → `theCr`

Every device follows the same shape. You can template-generate the
boilerplate. An AI can do the conversion on a device in minutes, with the
test suite as the oracle. **600 devices × 30 minutes ≈ 2-3 weeks of
driving AI** — which is the entire cost of Option C anyway, but you end up
with the proper architecture instead of a fixed-up old one.

**Cost:** Real. Not a weekend. Needs a working safety net (smoke tests
green, ideally some functional tests) because device emulation bugs are
subtle — one wrong bit in an interrupt controller and a guest panics at
boot. Without tests you're flying blind.

**Verdict:** This is the answer that matches the project's actual
ambition. "QEMU++" implies it's going to look like modern C++. If the goal
was "QEMU with `.cpp` extensions" we'd already be done. The phrase
"properly architected without giant stupid warts" can only mean Option D.

## The drastic thing worth considering

Here's the piece that may not have been said out loud: **QOM exists to
solve problems QEMU++ doesn't have.**

QOM's runtime type registry exists so that `-device pl011` works — you
type a string at the command line and get a device. Fair. But we don't
need a 2,000-line runtime to do that; a
`std::unordered_map<std::string, std::function<Object*()>>` does it in 20
lines.

QOM's runtime type checking exists so that `PL011(obj)` fails gracefully
when `obj` is actually a PL031. Fair. But C++ `dynamic_cast<PL011*>(obj)`
does the same thing for free if the types are polymorphic, with zero
string comparison.

QOM's property system exists so that machine definitions can wire up
devices via `object_property_set_str(dev, "chardev", "serial0", ...)`.
Fair. But that's a reflection problem, and C++17 has enough tools
(template argument deduction, member pointers, a small amount of macro) to
expose class members as named properties without `DEFINE_PROP_*` macro
forests.

QOM's VMState exists for migration. Fair. But VMState is already just a
description of field offsets and sizes — it's reflection, again, done the
1990s way. A modern replacement is a visitor that walks member pointers.
The on-wire format is the same.

**Every single thing QOM provides is something C++17 can provide more
cleanly.** The only reason it exists is that QEMU is 20+ years old and C
didn't have those tools. QEMU++ is specifically the project that says "we
have C++17 now, let's use it." And yet we're still dragging QOM along.

The drastic call: **delete QOM.** Not "replace it with Option D Step 2
virtual methods." Not "paper over it with a methods pointer." Delete it.
Replace it with 500 lines of modern C++ infrastructure and a
device-by-device port of the logic. The coding standard already says what
the replacement looks like — "C with classes, explicit types, PascalCase
types, `the` prefix, Allman braces." That's not what QEMU has today. It's
what QEMU should become.

## Recommendation

**Short-term (this week):** Do Option A's layout fix minimally — just
enough to get smoke tests back to 13/15. Don't add more Option D
conversions. Document the current state. Branch-freeze Option D Step 2
work.

**Medium-term (next 2-3 weeks):** Commit to Option D (the rewrite, not
the current "Option D Step 2"). Start with the infrastructure —
`include/qom/cpp/object.h` already has the skeleton. Finish the base
classes: `CppDevice`, `CppSysBusDevice`, `CppPCIDevice`, `CppISADevice`.
Write one reference device (PL011 is ideal — simple, single machine,
well-understood, has existing tests) end-to-end. Validate it on
`aarch64-softmmu virt` machine. This is the proof of concept.

**Long-term (month 2):** Mechanical conversion of devices in batches,
AI-driven. Start with the smallest/most similar first (UARTs, RTCs,
timers, GPIO). Use the existing device as a correctness oracle — boot a
test image before and after. Delete QOM infrastructure as the last
devices leave it.

**What you get at the end:** A QEMU++ that's about 30% smaller, compiles
2-3× faster, has proper IDE support, is understandable by any C++
developer, and has *zero* `#ifdef __cplusplus` dual views, *zero*
`qom_fixup_vtable`, *zero* `reinterpret_cast` for QOM casts, *zero*
`OBJECT_DECLARE_TYPE` macros. The "warts" will be memories.

The power-of-AI argument is real here, and it's not hype. The reason
Option D was off the table historically was that 600 devices × a day each
= two person-years of work. With Claude as the porter and smoke tests as
the oracle, you can do a simple device in 10 minutes and a complex one in
30. That collapses the cost to weeks, not years. The bottleneck becomes
review attention, not typing speed.

## Decisions needed to move forward

Three, in order:

1. **Accept the short-term loss.** Are you OK with freezing Option D Step
   2 where it is and fixing the smoke tests by some minimal layout patch,
   even if it leaves warts?

2. **Pick the medium-term direction.** Option B (methods pointer), Option
   C (C++-inheritance QOM rewrite), or Option D (clean-slate device
   model). If the call is "delete QOM," take that at face value.

3. **Decide what to do with the uncommitted Xen changes.** They're built
   on the broken foundation. Discard them, or keep them in a branch for
   reference when the infrastructure is ready.
