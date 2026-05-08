/*
 * QEMU++ C++ Object Model Wrappers
 *
 * Provides C++ helper classes and registration macros for QOM devices.
 * Object (the QOM base) now has a C++ vtable pointer at offset 0,
 * enabling virtual methods on device state structs. The vtable pointer
 * is set during QOM initialization via object_cpp_set_vtable().
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
#include <new>
#include <type_traits>
#include <utility>

/*
 * LEGACY: qom_fixup_vtable<T> — writes a C++ vtable pointer into a QOM
 * CLASS struct at offset 0. This corrupts ObjectClass::type and will be
 * removed when MOS6522 class-struct virtual methods are migrated to
 * instance-struct virtual methods (Phase D). Do NOT use in new code.
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
 * These wrappers sit on top of QOM. A CppDevice IS-A DeviceState in
 * memory, registered through normal TypeInfo, and fully compatible with
 * existing C code that uses DEVICE(), OBJECT(), etc.
 *
 * Object now has a virtual destructor (C++) / padding field (C), giving
 * all QOM objects a vtable pointer at offset 0. This enables C++ virtual
 * methods on device state structs (realize, reset, etc. with override).
 *
 * The vtable pointer is set during QOM init (object_cpp_set_vtable)
 * using the per-type pointer extracted by REGISTER_QEMU_DEVICE.
 *
 * What changes for the device author:
 *   - Device state struct becomes a C++ class with member functions
 *   - Methods can be virtual with override for polymorphic dispatch
 *   - No s->field syntax; methods access members directly
 *   - Type casting uses static_cast instead of OBJECT_CHECK
 *   - Registration boilerplate is collapsed via REGISTER_QEMU_DEVICE
 *
 * What stays the same:
 *   - The state struct embeds its QOM parent (DeviceState, SysBusDevice,
 *     PCIDevice) as its first data member — required for QOM layout
 *   - VMState migration descriptors (offsetof works on C++ classes)
 *   - MemoryRegionOps dispatch tables
 *   - QOM type registration (TypeInfo, type_init)
 *   - Property system (DEFINE_PROP_* macros, for now)
 *   - Two-phase init (instance_init + realize)
 */

/*
 * CppObject: utility view over a QOM Object.
 *
 * Provides reinterpret_cast helpers for accessing the underlying QOM
 * pointers. Concrete device classes embed their QOM parent as their
 * first data member, so reinterpret_cast<Object *>(this) is valid.
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
    ~CppObject() = default;

    /* Non-copyable, non-movable (QOM manages object lifecycle) */
    CppObject(const CppObject &) = delete;
    CppObject &operator=(const CppObject &) = delete;
};

/*
 * CppDevice: utility view over a QOM DeviceState.
 *
 * Concrete device classes embed `DeviceState parent_obj` (or
 * SysBusDevice, or PCIDevice) as their first data member. They
 * define realize/reset as member functions and register them via
 * REGISTER_QEMU_DEVICE.
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
    ~CppDevice() = default;
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
    /*
     * Only call DEVICE_CLASS(oc) when the type has realize/reset that
     * actually need a DeviceClass*. Machine types (TYPE_MACHINE -> TYPE_OBJECT)
     * are not devices and would fail the DEVICE_CLASS runtime check.
     * For classInit, pass oc reinterpret_cast'd to DeviceClass* — the user's
     * classInit body casts it back to whatever class struct it actually needs.
     */
    if constexpr (has_realize<T>::value || has_reset<T>::value) {
        DeviceClass *dc = DEVICE_CLASS(oc);
        if constexpr (has_realize<T>::value) {
            dc->realize = trampoline_realize<T>;
        }
        if constexpr (has_reset<T>::value) {
            device_class_set_legacy_reset(dc, trampoline_reset<T>);
        }
    }
    if constexpr (has_class_init<T>::value) {
        T::classInit(reinterpret_cast<DeviceClass *>(oc));
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

/*
 * Extract the C++ vtable pointer for a given type. Uses a static local
 * so the temporary is constructed only once per type.
 *
 * Falls back to nullptr for types that are not default-constructible
 * (e.g., structs containing unions of types with non-trivial constructors,
 * such as SDHCIState's PCIDevice/SysBusDevice union). For such types the
 * vtable feature is unavailable, but the macro still works — virtual
 * methods just won't be dispatched through C++ vtables.
 */
template<typename T>
const void *extract_vtable_impl(std::true_type)
{
    static const void *vtable = []() {
        alignas(T) unsigned char buf[sizeof(T)]{};
        T *tmp = new (buf) T;
        const void *vptr;
        std::memcpy(&vptr, buf, sizeof(void *));
        tmp->~T();
        return vptr;
    }();
    return vtable;
}

template<typename T>
const void *extract_vtable_impl(std::false_type)
{
    return nullptr;
}

template<typename T>
const void *extract_vtable()
{
    return extract_vtable_impl<T>(std::is_default_constructible<T>{});
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
 * TypeInfo + type_init pair. The C++ vtable pointer is extracted at
 * registration time and stored in TypeInfo::cpp_vtable so QOM can
 * set it during object initialization.
 */
#define REGISTER_QEMU_DEVICE(ClassName, type_name_str, parent_type_str)   \
static void ClassName##_cpp_register_types(void)                          \
{                                                                         \
    static TypeInfo info = {                                              \
        .name              = type_name_str,                               \
        .parent            = parent_type_str,                             \
        .instance_size     = sizeof(ClassName),                           \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
    };                                                                    \
    info.cpp_vtable = qemu_device_detail::extract_vtable<ClassName>();    \
    type_register_static(&info);                                          \
}                                                                         \
                                                                          \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_IFACES: like REGISTER_QEMU_DEVICE but with
 * an InterfaceInfo array for QOM interfaces.
 *
 * Usage:
 *   static const InterfaceInfo foo_ifaces[] = {
 *       { TYPE_CONVENTIONAL_PCI_DEVICE },
 *       { }
 *   };
 *   REGISTER_QEMU_DEVICE_IFACES(FooState, TYPE_FOO, TYPE_PCI_DEVICE,
 *                                foo_ifaces)
 */
#define REGISTER_QEMU_DEVICE_IFACES(ClassName, type_name_str,                \
                                     parent_type_str, ifaces_array)          \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static TypeInfo info = {                                                 \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    info.cpp_vtable = qemu_device_detail::extract_vtable<ClassName>();       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_CLASS_SIZE: like REGISTER_QEMU_DEVICE but for
 * device hierarchies with a custom class struct (subclass of DeviceClass).
 * Sets TypeInfo::class_size so QOM allocates the correct class layout.
 *
 * Usage:
 *   REGISTER_QEMU_DEVICE_CLASS_SIZE(FooState, FooClass, TYPE_FOO, TYPE_PARENT)
 *
 * The classInit member receives a DeviceClass* but can downcast to FooClass*
 * via XYZ_CLASS(klass) macros once obtaining ObjectClass via reinterpret_cast.
 */
#define REGISTER_QEMU_DEVICE_CLASS_SIZE(ClassName, ClassStruct,              \
                                         type_name_str, parent_type_str)     \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static TypeInfo info = {                                                 \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
    };                                                                       \
    info.cpp_vtable = qemu_device_detail::extract_vtable<ClassName>();       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_CUSTOM_CI: register a device with a user-supplied
 * class_init free function (instead of the SFINAE-generated trampoline).
 * Useful for devices that need parent_realize chaining via
 * device_class_set_parent_realize, where the macro-generated trampoline's
 * automatic dc->realize wiring would interfere.
 *
 * The user-supplied class_init_fn has the canonical
 *   void (ObjectClass *, const void *data)
 * signature. instance_init/instance_finalize are still SFINAE-detected.
 */
#define REGISTER_QEMU_DEVICE_CUSTOM_CI(ClassName, ClassStruct,               \
                                        type_name_str, parent_type_str,      \
                                        class_init_fn)                       \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static TypeInfo info = {                                                 \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    info.cpp_vtable = qemu_device_detail::extract_vtable<ClassName>();       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS: abstract base class WITHOUT a custom
 * class struct. The QOM class layout uses the parent's class_size; only
 * the state struct is extended. Useful for abstract bases that customize
 * instance state (e.g., PCIQXLDevice extends PCIDevice) without needing
 * extra class-level fields.
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS(ClassName, type_name_str,        \
                                             parent_type_str)                \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS_IFACES: as above but with interfaces.
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS_IFACES(ClassName,                \
                                                    type_name_str,           \
                                                    parent_type_str,         \
                                                    ifaces_array)            \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI: abstract device base class with
 * a user-supplied class_init function. For abstract bases that use
 * device_class_set_parent_realize chaining or otherwise need control over
 * the class_init body (e.g., touch parent class struct fields directly).
 * SFINAE-wires init()/finalize() on the state struct.
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI(ClassName, ClassStruct,      \
                                                 type_name_str,              \
                                                 parent_type_str,            \
                                                 class_init_fn)              \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS: abstract base with
 * caller-supplied class_init, no custom class struct (uses parent's
 * class_size).
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS(ClassName,             \
                                                       type_name_str,        \
                                                       parent_type_str,      \
                                                       class_init_fn)        \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_CUSTOM_CI_NO_CS: concrete with caller-supplied
 * class_init, no custom class struct.
 */
#define REGISTER_QEMU_DEVICE_CUSTOM_CI_NO_CS(ClassName, type_name_str,       \
                                              parent_type_str, class_init_fn)\
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_CUSTOM_CI_NO_CS_IFACES: concrete with caller-supplied
 * class_init and InterfaceInfo array, no custom class struct.
 */
#define REGISTER_QEMU_DEVICE_CUSTOM_CI_NO_CS_IFACES(ClassName,               \
                                                     type_name_str,          \
                                                     parent_type_str,        \
                                                     class_init_fn,          \
                                                     ifaces_array)           \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = class_init_fn,                                  \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS_IFACES: abstract base with
 * caller-supplied class_init and InterfaceInfo array, but no custom class
 * struct (uses parent's class_size).
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_NO_CS_IFACES(ClassName,      \
                                                              type_name_str, \
                                                              parent_type_str,\
                                                              class_init_fn, \
                                                              ifaces_array)  \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_init        = class_init_fn,                                  \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_IFACES: like _ABSTRACT_CUSTOM_CI
 * with InterfaceInfo array.
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_CUSTOM_CI_IFACES(ClassName, ClassStruct, \
                                                        type_name_str,         \
                                                        parent_type_str,       \
                                                        class_init_fn,         \
                                                        ifaces_array)          \
static void ClassName##_cpp_register_types(void)                               \
{                                                                              \
    static const TypeInfo info = {                                             \
        .name              = type_name_str,                                    \
        .parent            = parent_type_str,                                  \
        .instance_size     = sizeof(ClassName),                                \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                             \
        .class_size        = sizeof(ClassStruct),                              \
        .class_init        = class_init_fn,                                    \
        .interfaces        = ifaces_array,                                     \
    };                                                                         \
    type_register_static(&info);                                               \
}                                                                              \
                                                                               \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_CUSTOM_CI_IFACES: like REGISTER_QEMU_DEVICE_CUSTOM_CI
 * but with an InterfaceInfo array. Use for devices that need both a
 * user-supplied class_init (e.g. parent_realize chaining) and QOM interfaces.
 */
#define REGISTER_QEMU_DEVICE_CUSTOM_CI_IFACES(ClassName, ClassStruct,         \
                                               type_name_str,                 \
                                               parent_type_str,               \
                                               class_init_fn,                 \
                                               ifaces_array)                  \
static void ClassName##_cpp_register_types(void)                              \
{                                                                             \
    static TypeInfo info = {                                                  \
        .name              = type_name_str,                                   \
        .parent            = parent_type_str,                                 \
        .instance_size     = sizeof(ClassName),                               \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_size        = sizeof(ClassStruct),                             \
        .class_init        = class_init_fn,                                   \
        .interfaces        = ifaces_array,                                    \
    };                                                                        \
    info.cpp_vtable = qemu_device_detail::extract_vtable<ClassName>();        \
    type_register_static(&info);                                              \
}                                                                             \
                                                                              \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_CLASS_SIZE_IFACES: like REGISTER_QEMU_DEVICE_CLASS_SIZE
 * but with an InterfaceInfo array.
 */
#define REGISTER_QEMU_DEVICE_CLASS_SIZE_IFACES(ClassName, ClassStruct,       \
                                                 type_name_str,              \
                                                 parent_type_str,            \
                                                 ifaces_array)               \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static TypeInfo info = {                                                 \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    info.cpp_vtable = qemu_device_detail::extract_vtable<ClassName>();       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_IFACES: like _ABSTRACT but with InterfaceInfo.
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_IFACES(ClassName, ClassStruct,         \
                                              type_name_str,                 \
                                              parent_type_str,               \
                                              ifaces_array)                  \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT: register an abstract device base class
 * with a custom class struct. No instance_init/instance_finalize/realize/
 * reset wiring is generated (abstract types cannot be instantiated), but
 * classInit is wired so subclass-class-struct fields can be initialized.
 *
 * Usage:
 *   REGISTER_QEMU_DEVICE_ABSTRACT(FooBase, FooBaseClass, TYPE_FOO_BASE,
 *                                  TYPE_DEVICE)
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT(ClassName, ClassStruct,                \
                                       type_name_str, parent_type_str)       \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_MACHINE: register a concrete MachineClass-rooted type.
 * For machines that derive from TYPE_MACHINE directly (or any abstract
 * machine base) without adding their own class struct extension.
 *
 * Wires init()/finalize() via SFINAE; class_init is a user-supplied
 * free function with the canonical (ObjectClass *, const void *)
 * signature since machines extend MachineClass not DeviceClass.
 */
#define REGISTER_QEMU_MACHINE(ClassName, type_name_str, parent_type_str,     \
                               class_init_fn)                                \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_MACHINE_IFACES: concrete machine + interfaces.
 */
#define REGISTER_QEMU_MACHINE_IFACES(ClassName, type_name_str,               \
                                      parent_type_str, class_init_fn,        \
                                      ifaces_array)                          \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = class_init_fn,                                  \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_MACHINE_ABSTRACT: register an abstract MachineClass-rooted type.
 * Many SoC/board machine families share a base class (e.g.
 * SpaprMachineClass) with subclasses for individual machine versions.
 *
 * Wires init()/finalize() via SFINAE; class_init is a user-supplied free
 * function with `(ObjectClass *, const void *)` signature since machines
 * extend MachineClass not DeviceClass.
 */
#define REGISTER_QEMU_MACHINE_ABSTRACT(ClassName, ClassStruct,               \
                                        type_name_str, parent_type_str,      \
                                        class_init_fn)                       \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_MACHINE_ABSTRACT_IFACES: like the abstract machine variant
 * but with InterfaceInfo array.
 */
#define REGISTER_QEMU_MACHINE_ABSTRACT_IFACES(ClassName, ClassStruct,        \
                                               type_name_str,                \
                                               parent_type_str,              \
                                               class_init_fn,                \
                                               ifaces_array)                 \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT: register a QOM type rooted at TYPE_OBJECT (or any
 * non-Device, non-Bus, non-Interface base). For things like Clock, IRQState,
 * RemoteObject — types that aren't devices but live in the QOM hierarchy.
 *
 * SFINAE-detects init() and finalize() members on the state struct. The
 * class_init function (if needed) is a user-supplied free function with
 * the canonical (ObjectClass *, const void *) signature.
 *
 * Usage:
 *   REGISTER_QEMU_OBJECT(Clock, TYPE_CLOCK, TYPE_OBJECT)                    // no class_init
 *   REGISTER_QEMU_OBJECT_CI(Clock, TYPE_CLOCK, TYPE_OBJECT, clock_class_init)
 *   REGISTER_QEMU_OBJECT_CI_CS(RemoteObject, RemoteObjectClass, ...)        // with class_size
 */
#define REGISTER_QEMU_OBJECT(ClassName, type_name_str, parent_type_str)      \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

#define REGISTER_QEMU_OBJECT_CI(ClassName, type_name_str, parent_type_str,   \
                                 class_init_fn)                              \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

#define REGISTER_QEMU_OBJECT_CI_CS(ClassName, ClassStruct, type_name_str,    \
                                    parent_type_str, class_init_fn)          \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_CI_IFACES: like REGISTER_QEMU_OBJECT_CI but with
 * an InterfaceInfo array and no custom class struct.
 */
#define REGISTER_QEMU_OBJECT_CI_IFACES(ClassName, type_name_str,             \
                                        parent_type_str, class_init_fn,      \
                                        ifaces_array)                        \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_init        = class_init_fn,                                  \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

#define REGISTER_QEMU_OBJECT_CI_CS_IFACES(ClassName, ClassStruct,            \
                                           type_name_str, parent_type_str,   \
                                           class_init_fn, ifaces_array)      \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_CS: concrete QOM object with custom class struct
 * but no class_init.
 */
#define REGISTER_QEMU_OBJECT_CS(ClassName, ClassStruct,                      \
                                 type_name_str, parent_type_str)             \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .class_size        = sizeof(ClassStruct),                            \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_ABSTRACT_CS: abstract QOM object base with custom
 * class struct but no class_init (the class struct just declares vtable
 * entries that subclasses fill in).
 */
#define REGISTER_QEMU_OBJECT_ABSTRACT_CS(ClassName, ClassStruct,             \
                                          type_name_str, parent_type_str)    \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_ABSTRACT_CI: abstract QOM object base with
 * caller-supplied class_init. No custom class struct (uses parent's
 * class_size).
 */
#define REGISTER_QEMU_OBJECT_ABSTRACT_CI(ClassName, type_name_str,           \
                                          parent_type_str, class_init_fn)    \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_ABSTRACT_CI_CS: abstract QOM object base with
 * caller-supplied class_init and custom class struct (no interfaces).
 */
#define REGISTER_QEMU_OBJECT_ABSTRACT_CI_CS(ClassName, ClassStruct,          \
                                             type_name_str, parent_type_str, \
                                             class_init_fn)                  \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_ABSTRACT_CI_CS_IFACES: abstract QOM object base with
 * caller-supplied class_init, custom class struct, and InterfaceInfo array.
 */
#define REGISTER_QEMU_OBJECT_ABSTRACT_CI_CS_IFACES(ClassName, ClassStruct,   \
                                                    type_name_str,           \
                                                    parent_type_str,         \
                                                    class_init_fn,           \
                                                    ifaces_array)            \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = qemu_device_detail::get_instance_finalize<ClassName>(), \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = class_init_fn,                                  \
        .interfaces        = ifaces_array,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_BUS: register a QOM bus type (parent TYPE_BUS).
 * Buses extend BusClass, not DeviceClass. Use the _CI variant if a
 * class_init is needed.
 */
#define REGISTER_QEMU_BUS(StateStruct, type_name_str)                        \
static void StateStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = TYPE_BUS,                                           \
        .instance_size = sizeof(StateStruct),                                \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(StateStruct##_cpp_register_types)

#define REGISTER_QEMU_BUS_CI(StateStruct, type_name_str, class_init_fn)      \
static void StateStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = TYPE_BUS,                                           \
        .instance_size = sizeof(StateStruct),                                \
        .class_init    = class_init_fn,                                      \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(StateStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_BUS_CI_IFACES: bus type with class_init AND interfaces.
 */
#define REGISTER_QEMU_BUS_CI_IFACES(StateStruct, type_name_str,              \
                                     class_init_fn, ifaces_array)            \
static void StateStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = TYPE_BUS,                                           \
        .instance_size = sizeof(StateStruct),                                \
        .class_init    = class_init_fn,                                      \
        .interfaces    = ifaces_array,                                       \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(StateStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_BUS_CI_CS_IFACES: bus type with class_size + class_init +
 * interfaces.
 */
#define REGISTER_QEMU_BUS_CI_CS_IFACES(StateStruct, ClassStruct,             \
                                        type_name_str, class_init_fn,        \
                                        ifaces_array)                        \
static void StateStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = TYPE_BUS,                                           \
        .instance_size = sizeof(StateStruct),                                \
        .class_size    = sizeof(ClassStruct),                                \
        .class_init    = class_init_fn,                                      \
        .interfaces    = ifaces_array,                                       \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(StateStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_BUS_FULL: bus type with everything: instance_init, class_init,
 * class_size, optional parent (defaults TYPE_BUS-rooted).
 *
 * For unusual cases like nubus-bus that need instance_init.
 */
#define REGISTER_QEMU_BUS_FULL(StateStruct, ClassStruct, type_name_str,      \
                                parent_type_str, instance_init_fn,           \
                                class_init_fn)                               \
static void StateStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_size = sizeof(StateStruct),                                \
        .instance_init = instance_init_fn,                                   \
        .class_size    = sizeof(ClassStruct),                                \
        .class_init    = class_init_fn,                                      \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(StateStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_BUS_INSTANCE_CI: bus type with instance_init + class_init
 * but no custom class struct (uses parent class_size). For buses like
 * NubusBus that need instance_init without a dedicated ClassStruct.
 */
#define REGISTER_QEMU_BUS_INSTANCE_CI(StateStruct, type_name_str,            \
                                       parent_type_str, instance_init_fn,    \
                                       class_init_fn)                        \
static void StateStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_size = sizeof(StateStruct),                                \
        .instance_init = instance_init_fn,                                   \
        .class_init    = class_init_fn,                                      \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(StateStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_BUS_CLASS_SIZE: bus type with a custom class struct but
 * no class_init and no instance_init. The class struct is allocated by
 * QOM at the correct size; class fields default to zero. Use for buses
 * like SDBus whose class methods are filled in by child types.
 */
#define REGISTER_QEMU_BUS_CLASS_SIZE(StateStruct, ClassStruct, type_name_str) \
static void StateStruct##_cpp_register_types(void)                            \
{                                                                             \
    static const TypeInfo info = {                                            \
        .name          = type_name_str,                                       \
        .parent        = TYPE_BUS,                                            \
        .instance_size = sizeof(StateStruct),                                 \
        .class_size    = sizeof(ClassStruct),                                 \
    };                                                                        \
    type_register_static(&info);                                              \
}                                                                             \
                                                                              \
type_init(StateStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_BUS_ABSTRACT: abstract bus type (parent TYPE_BUS, has class_size).
 */
#define REGISTER_QEMU_BUS_ABSTRACT(StateStruct, ClassStruct, type_name_str,  \
                                    class_init_fn)                           \
static void StateStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = TYPE_BUS,                                           \
        .instance_size = sizeof(StateStruct),                                \
        .is_abstract   = true,                                               \
        .class_size    = sizeof(ClassStruct),                                \
        .class_init    = class_init_fn,                                      \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(StateStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_INTERFACE: register a QOM interface type. Interfaces have
 * no instance_size (no per-object data), only a class struct that derived
 * types extend. Simpler than the device variants — no SFINAE, no
 * trampolines.
 *
 * Usage:
 *   REGISTER_QEMU_INTERFACE(HotplugHandlerClass, TYPE_HOTPLUG_HANDLER)
 */
#define REGISTER_QEMU_INTERFACE(ClassStruct, type_name_str)                  \
static void ClassStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name       = type_name_str,                                         \
        .parent     = TYPE_INTERFACE,                                        \
        .class_size = sizeof(ClassStruct),                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_INTERFACE_CI: as REGISTER_QEMU_INTERFACE but with a
 * custom class_init function. Used by interfaces that need to wire up
 * default method implementations during class initialization.
 */
#define REGISTER_QEMU_INTERFACE_CI(ClassStruct, type_name_str,               \
                                    class_init_fn)                           \
static void ClassStruct##_cpp_register_types(void)                           \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name       = type_name_str,                                         \
        .parent     = TYPE_INTERFACE,                                        \
        .class_size = sizeof(ClassStruct),                                   \
        .class_init = class_init_fn,                                         \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassStruct##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_CLASS_ONLY: register a derived type that only adds
 * a class_init function (no instance struct, no instance_init/finalize).
 * The instance_size is inherited from the parent type. Useful for chardev
 * subtypes that override class methods (parse, open, etc.) without adding
 * any instance state. The unique_tag parameter provides a unique C symbol
 * for the registration function (e.g., "char_serial").
 *
 * Usage:
 *   REGISTER_QEMU_OBJECT_CLASS_ONLY(char_serial, TYPE_CHARDEV_SERIAL,
 *                                    TYPE_CHARDEV_FD,
 *                                    char_serial_class_init)
 */
#define REGISTER_QEMU_OBJECT_CLASS_ONLY(unique_tag, type_name_str,           \
                                         parent_type_str, class_init_fn)     \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name       = type_name_str,                                         \
        .parent     = parent_type_str,                                       \
        .class_init = class_init_fn,                                         \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED_IFACES: like
 * REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED but with an additional interfaces
 * array. Used for sibling concrete subtypes that share a state struct
 * (so unique_tag distinguishes them) and advertise interfaces.
 */
#define REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED_IFACES(unique_tag, ClassName,  \
                                                      type_name_str,         \
                                                      parent_type_str,       \
                                                      class_init_fn,         \
                                                      ifaces_array)          \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_size = sizeof(ClassName),                                  \
        .class_init    = class_init_fn,                                      \
        .interfaces    = ifaces_array,                                       \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED: like REGISTER_QEMU_OBJECT_CLASS_ONLY
 * but additionally sets instance_size = sizeof(ClassName). Used when the
 * derived type has its own state struct that's larger than the parent's
 * (no init/finalize methods needed, just the larger storage).
 */
#define REGISTER_QEMU_OBJECT_CLASS_ONLY_SIZED(unique_tag, ClassName,         \
                                               type_name_str,                \
                                               parent_type_str,              \
                                               class_init_fn)                \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_size = sizeof(ClassName),                                  \
        .class_init    = class_init_fn,                                      \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_CLASS_FINI: like REGISTER_QEMU_OBJECT_CLASS_ONLY
 * but additionally takes a free instance_finalize function (not a member).
 * Useful for chardev subtypes with custom finalize logic but no own state.
 */
#define REGISTER_QEMU_OBJECT_CLASS_FINI(unique_tag, type_name_str,           \
                                         parent_type_str, class_init_fn,     \
                                         instance_finalize_fn)               \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_finalize = instance_finalize_fn,                           \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_INIT_CLASS: register a derived type that has both
 * an instance_init (free function) and a class_init (free function), but
 * uses the parent's instance struct (instance_size = 0 → inherit from
 * parent). Useful for sibling subtypes that share a state struct but
 * have different class_init logic and different per-instance setup.
 */
#define REGISTER_QEMU_OBJECT_INIT_CLASS(unique_tag, type_name_str,           \
                                         parent_type_str,                    \
                                         instance_init_fn,                   \
                                         class_init_fn)                      \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_init     = instance_init_fn,                               \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_FREE_FINI: abstract device with a
 * caller-supplied free instance_finalize function and trampolined
 * classInit. instance_init goes through SFINAE on ClassName::init().
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_FREE_FINI(ClassName, ClassStruct,      \
                                                 type_name_str,              \
                                                 parent_type_str,            \
                                                 instance_finalize_fn)       \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = qemu_device_detail::get_instance_init<ClassName>(), \
        .instance_finalize = instance_finalize_fn,                           \
        .is_abstract       = true,                                           \
        .class_size        = sizeof(ClassStruct),                            \
        .class_init        = qemu_device_detail::trampoline_class_init<ClassName>, \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT_CI: like
 * REGISTER_QEMU_DEVICE_ABSTRACT but uses a caller-supplied free
 * instance_init function AND a caller-supplied free class_init function
 * (instead of trampolining through ClassName::classInit()). Used for
 * abstract base types where both setup paths are intricate enough to
 * stay as free functions.
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT_CI(ClassName, ClassStruct,   \
                                                    type_name_str,           \
                                                    parent_type_str,         \
                                                    instance_init_fn,        \
                                                    class_init_fn)           \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_size = sizeof(ClassName),                                  \
        .instance_init = instance_init_fn,                                   \
        .is_abstract   = true,                                               \
        .class_size    = sizeof(ClassStruct),                                \
        .class_init    = class_init_fn,                                      \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT: like REGISTER_QEMU_DEVICE_ABSTRACT
 * but uses a caller-supplied free instance_init function (instead of SFINAE
 * on ClassName::init()). The class_init still goes through the C++
 * trampoline so ClassName::classInit() runs.
 */
#define REGISTER_QEMU_DEVICE_ABSTRACT_FREE_INIT(ClassName, ClassStruct,      \
                                                 type_name_str,              \
                                                 parent_type_str,            \
                                                 instance_init_fn)           \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_size = sizeof(ClassName),                                  \
        .instance_init = instance_init_fn,                                   \
        .is_abstract   = true,                                               \
        .class_size    = sizeof(ClassStruct),                                \
        .class_init    = qemu_device_detail::trampoline_class_init<ClassName>, \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_FREE_INIT: like REGISTER_QEMU_DEVICE but uses a
 * caller-supplied free instance_init function (no SFINAE), while keeping
 * the C++ trampoline_class_init wired up for ClassName::classInit().
 * Useful for devices where the per-instance setup is intricate enough
 * that it stays as a free function but the class side is a member.
 */
#define REGISTER_QEMU_DEVICE_FREE_INIT(ClassName, type_name_str,             \
                                        parent_type_str, instance_init_fn)   \
static void ClassName##_cpp_register_types(void)                             \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_size = sizeof(ClassName),                                  \
        .instance_init = instance_init_fn,                                   \
        .class_init    = qemu_device_detail::trampoline_class_init<ClassName>, \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_INIT_ONLY: register a derived type that needs
 * only an instance_init (free function) hook. instance_size and class_init
 * are inherited from the parent. Useful for subtypes that just need to
 * tweak per-instance state during construction.
 */
#define REGISTER_QEMU_OBJECT_INIT_ONLY(unique_tag, type_name_str,            \
                                        parent_type_str, instance_init_fn)   \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name          = type_name_str,                                      \
        .parent        = parent_type_str,                                    \
        .instance_init = instance_init_fn,                                   \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_INIT_CLASS_SIZED: like REGISTER_QEMU_OBJECT_INIT_CLASS
 * but additionally explicitly sets instance_size = sizeof(ClassName). Used
 * for sibling subtypes that need their own state struct (often larger than
 * parent's) without participating in the parent's C++ trampoline machinery.
 */
#define REGISTER_QEMU_OBJECT_INIT_CLASS_SIZED(unique_tag, ClassName,         \
                                               type_name_str,                \
                                               parent_type_str,              \
                                               instance_init_fn,             \
                                               class_init_fn)                \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name              = type_name_str,                                  \
        .parent            = parent_type_str,                                \
        .instance_size     = sizeof(ClassName),                              \
        .instance_init     = instance_init_fn,                               \
        .class_init        = class_init_fn,                                  \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

/*
 * REGISTER_QEMU_OBJECT_ALIAS: register a type that is just an alias of
 * another type (no class_init, no instance state). Used for compatibility
 * aliases like TYPE_CHARDEV_MEMORY = TYPE_CHARDEV_RINGBUF.
 */
#define REGISTER_QEMU_OBJECT_ALIAS(unique_tag, type_name_str,                \
                                    parent_type_str)                         \
static void unique_tag##_cpp_register_types(void)                            \
{                                                                            \
    static const TypeInfo info = {                                           \
        .name   = type_name_str,                                             \
        .parent = parent_type_str,                                           \
    };                                                                       \
    type_register_static(&info);                                             \
}                                                                            \
                                                                             \
type_init(unique_tag##_cpp_register_types)

#endif /* QOM_CPP_OBJECT_H */
