/*
 * QEMU++ C++ Object Model Wrappers
 *
 * Provides zero-vtable C++ helper classes that are binary-compatible
 * with QOM structs. A CppObject/CppDevice is a "view" over QOM memory —
 * it adds C++ member-function syntax and compile-time type checking
 * without adding any data members or virtual methods.
 *
 * Design rule: NO VIRTUAL METHODS anywhere in this file or its callers.
 * Virtual methods introduce a vtable pointer at offset 0, which collides
 * with QOM's ObjectClass::type at offset 0. Use direct member functions
 * dispatched through static function pointers in TypeInfo instead.
 *
 * Copyright (c) 2026 QEMU++ Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QOM_CPP_OBJECT_H
#define QOM_CPP_OBJECT_H

#include "qemu/osdep.h"

/* These headers are already C++-safe via osdep.h infrastructure.
 * Do NOT wrap in extern "C" — they pull in C++ std headers transitively. */
#include "qom/object.h"
#include "hw/qdev-core.h"

#include <cstring>
#include <type_traits>
#include <utility>

/*
 * DEPRECATED: qom_fixup_vtable<T> — legacy helper from the abandoned
 * Option D Step 2 virtual-method approach. It memcpys a C++ vtable
 * pointer from a stack-constructed T into the class struct at offset 0.
 *
 * This is broken: it overwrites ObjectClass::type and causes SIGSEGV
 * during type enumeration. Only the MOS6522 hierarchy on this branch
 * still calls it, and only because MOS6522 types are not enumerated
 * during -machine help so the corruption goes unobserved.
 *
 * Do NOT use this in new code. It will be deleted along with the
 * MOS6522 Step 2 conversion in a later phase.
 */
template<typename T>
inline void qom_fixup_vtable(void *obj) {
    T tmp;
    std::memcpy(obj, &tmp, sizeof(void *));
}

/*
 * Design Philosophy
 * =================
 *
 * These wrappers do NOT replace QOM — they sit on top of it. A CppDevice
 * IS-A DeviceState in memory, registered through normal TypeInfo, and
 * fully compatible with existing C code that uses DEVICE(), OBJECT(), etc.
 *
 * What changes for the device author:
 *   - Device state struct becomes a C++ class with member functions
 *   - No s->field syntax; methods access members directly
 *   - Type casting uses static_cast instead of OBJECT_CHECK
 *   - Registration boilerplate is collapsed via REGISTER_QEMU_DEVICE
 *
 * What stays the same:
 *   - The state struct embeds its QOM parent (DeviceState, SysBusDevice,
 *     PCIDevice) as its first data member — required for QOM layout
 *   - VMState migration descriptors (offsetof works on C++ classes
 *     with standard-layout fields)
 *   - MemoryRegionOps dispatch tables
 *   - QOM type registration (TypeInfo, type_init)
 *   - Property system (DEFINE_PROP_* macros, for now)
 *   - Two-phase init (instance_init + realize)
 *
 * What MUST NOT happen:
 *   - No virtual methods. Ever. They add a vtable pointer that corrupts
 *     ObjectClass::type at offset 0. Use static function pointers on
 *     TypeInfo/DeviceClass for dispatch.
 */

/*
 * CppObject: zero-vtable view over a QOM Object.
 *
 * This class has no data members and no virtual methods. Its sizeof is
 * 1 (empty class) and it contributes 0 bytes to derived classes via
 * empty-base optimization. The "this" pointer of a CppObject is the
 * same as the QOM Object pointer it views.
 *
 * Subclasses provide their own data — which must start with the QOM
 * parent struct (Object, DeviceState, SysBusDevice, PCIDevice) as the
 * first data member, so that reinterpret_cast<Object *>(this) is valid.
 */
class CppObject
{
public:
    Object *qomObject()
    {
        return reinterpret_cast<Object *>(this);
    }

    const Object *qomObject() const
    {
        return reinterpret_cast<const Object *>(this);
    }

    const char *typeName() const
    {
        return object_get_typename(const_cast<Object *>(qomObject()));
    }

    void ref()   { object_ref(qomObject()); }
    void unref() { object_unref(qomObject()); }

protected:
    /* No public construction — always created through QOM's instance_init */
    CppObject() = default;
    ~CppObject() = default;  /* NON-virtual: adding virtual breaks layout */

    /* Non-copyable, non-movable (QOM manages object lifecycle) */
    CppObject(const CppObject &) = delete;
    CppObject &operator=(const CppObject &) = delete;
};

/*
 * CppDevice: zero-vtable view over a QOM DeviceState.
 *
 * Like CppObject, this contributes 0 bytes to derived classes. Concrete
 * device classes embed `DeviceState parent_obj` (or SysBusDevice, or
 * PCIDevice) as their first data member. They do NOT get `realize` and
 * `reset` from inheritance — instead they define their own static or
 * member functions and register them via REGISTER_QEMU_DEVICE.
 */
class CppDevice : public CppObject
{
public:
    DeviceState *deviceState()
    {
        return reinterpret_cast<DeviceState *>(this);
    }

    const DeviceState *deviceState() const
    {
        return reinterpret_cast<const DeviceState *>(this);
    }

    /* Convenience accessors */
    const char *id() const { return deviceState()->id; }
    bool isRealized() const { return deviceState()->realized; }
    BusState *parentBus() const { return deviceState()->parent_bus; }

protected:
    CppDevice() = default;
    ~CppDevice() = default;  /* NON-virtual */
};

/*
 * Device registration plumbing: detects which lifecycle methods a device
 * class defines and auto-wires trampolines + TypeInfo fields for them.
 *
 * Supported methods (all optional):
 *   void T::init()
 *   void T::finalize()
 *   void T::realize(Error **errp)
 *   void T::reset()
 *   static void T::classInit(DeviceClass *dc)
 *
 * The REGISTER_QEMU_DEVICE macro only needs the class name and type
 * strings — it figures out which methods the class has and wires up
 * instance_init / instance_finalize / dc->realize / dc->legacy_reset /
 * T::classInit accordingly. No boilerplate even when methods are absent.
 */
namespace qemu_device_detail {

/* --- method detection via SFINAE --- */

template<typename, typename = void>
struct has_init : std::false_type {};
template<typename T>
struct has_init<T, std::void_t<decltype(std::declval<T &>().init())>>
    : std::true_type {};

template<typename, typename = void>
struct has_finalize : std::false_type {};
template<typename T>
struct has_finalize<T, std::void_t<decltype(std::declval<T &>().finalize())>>
    : std::true_type {};

template<typename, typename = void>
struct has_realize : std::false_type {};
template<typename T>
struct has_realize<T, std::void_t<
    decltype(std::declval<T &>().realize(std::declval<Error **>()))>>
    : std::true_type {};

template<typename, typename = void>
struct has_reset : std::false_type {};
template<typename T>
struct has_reset<T, std::void_t<decltype(std::declval<T &>().reset())>>
    : std::true_type {};

template<typename, typename = void>
struct has_class_init : std::false_type {};
template<typename T>
struct has_class_init<T, std::void_t<
    decltype(T::classInit(std::declval<DeviceClass *>()))>>
    : std::true_type {};

/* --- trampolines (instantiated only when needed) --- */

template<typename T>
void trampoline_init(Object *obj)
{
    reinterpret_cast<T *>(obj)->init();
}

template<typename T>
void trampoline_finalize(Object *obj)
{
    reinterpret_cast<T *>(obj)->finalize();
}

template<typename T>
void trampoline_realize(DeviceState *dev, Error **errp)
{
    reinterpret_cast<T *>(dev)->realize(errp);
}

template<typename T>
void trampoline_reset(DeviceState *dev)
{
    reinterpret_cast<T *>(dev)->reset();
}

/*
 * Generic class_init wrapper. Called by QOM with an ObjectClass pointer;
 * casts it to DeviceClass, auto-wires realize/reset based on method
 * detection, then calls the user's classInit(DeviceClass *) if present.
 */
template<typename T>
void trampoline_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    if constexpr (has_realize<T>::value) {
        dc->realize = trampoline_realize<T>;
    }
    if constexpr (has_reset<T>::value) {
        device_class_set_legacy_reset(dc, trampoline_reset<T>);
    }
    if constexpr (has_class_init<T>::value) {
        T::classInit(dc);
    }
}

/* --- conditional function-pointer accessors for TypeInfo --- */

template<typename T>
constexpr auto get_instance_init() -> void (*)(Object *)
{
    if constexpr (has_init<T>::value) {
        return trampoline_init<T>;
    } else {
        return nullptr;
    }
}

template<typename T>
constexpr auto get_instance_finalize() -> void (*)(Object *)
{
    if constexpr (has_finalize<T>::value) {
        return trampoline_finalize<T>;
    } else {
        return nullptr;
    }
}

}  /* namespace qemu_device_detail */

/*
 * REGISTER_QEMU_DEVICE: register a C++ device class with QOM.
 *
 * Usage (at file scope in hw/foo/foo.cpp):
 *   REGISTER_QEMU_DEVICE(FooState, TYPE_FOO, TYPE_SYS_BUS_DEVICE)
 *
 * The class `FooState` may define any subset of:
 *   void init();                              // called by instance_init
 *   void finalize();                          // called by instance_finalize
 *   void realize(Error **errp);               // wired to dc->realize
 *   void reset();                             // wired to dc->legacy_reset
 *   static void classInit(DeviceClass *dc);   // called after wiring above
 *
 * Methods not defined are simply not wired up. The macro expands to a
 * single TypeInfo + type_init pair, no trampoline functions pollute
 * the translation unit's namespace.
 *
 * For devices that register additional QOM types (subtypes) from the
 * same file, use REGISTER_QEMU_DEVICE plus manual type_register_static
 * calls in a separate type_init — see hw/char/pl011.cpp for an example
 * with the pl011_luminary subtype.
 */
#define REGISTER_QEMU_DEVICE(ClassName, type_name_str, parent_type_str)   \
static const TypeInfo ClassName##_type_info = {                           \
    .name              = type_name_str,                                   \
    .parent            = parent_type_str,                                 \
    .instance_size     = sizeof(ClassName),                               \
    .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
    .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
    .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
};                                                                        \
                                                                          \
static void ClassName##_cpp_register_types(void)                          \
{                                                                         \
    type_register_static(&ClassName##_type_info);                         \
}                                                                         \
                                                                          \
type_init(ClassName##_cpp_register_types)

#endif /* QOM_CPP_OBJECT_H */
