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
 *
 * Example (see hw/char/pl011.cpp for the real thing):
 *
 *   class PL011State : public CppDevice
 *   {
 *   public:
 *       SysBusDevice parent_obj;   // QOM parent; MUST be first
 *       MemoryRegion iomem;
 *       uint32_t theCr;
 *       // ...
 *
 *       void realize(Error **errp);
 *       void reset();
 *       void init();
 *   };
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
 * REGISTER_QEMU_DEVICE: register a C++ device class with QOM.
 *
 * Generates the trampolines, class_init, TypeInfo, and type_init
 * boilerplate from a device class that has the following members:
 *
 *   class FooState : public CppDevice {
 *       SysBusDevice parent_obj;   // or DeviceState / PCIDevice
 *       // ... data members ...
 *
 *       void init();             // called by instance_init (optional)
 *       void realize(Error **errp);
 *       void reset();            // optional
 *   };
 *
 * Plus a static `classInit(DeviceClass *dc)` method where the device
 * sets categories, vmsd, props, user_creatable, etc. The macro handles
 * the DEVICE_CLASS(oc) cast and calls classInit.
 *
 * Usage (at file scope in hw/foo/foo.cpp):
 *
 *   REGISTER_QEMU_DEVICE(FooState, "foo-device", TYPE_SYS_BUS_DEVICE)
 *
 * If the device has no `init()` method, pass nullptr via the
 * REGISTER_QEMU_DEVICE_NOINIT variant.
 */
#define REGISTER_QEMU_DEVICE(ClassName, type_name_str, parent_type_str)   \
static void ClassName##_cpp_realize(DeviceState *dev, Error **errp)       \
{                                                                         \
    ClassName *self = reinterpret_cast<ClassName *>(dev);                  \
    self->realize(errp);                                                  \
}                                                                         \
                                                                          \
static void ClassName##_cpp_reset(DeviceState *dev)                       \
{                                                                         \
    ClassName *self = reinterpret_cast<ClassName *>(dev);                  \
    self->reset();                                                        \
}                                                                         \
                                                                          \
static void ClassName##_cpp_init(Object *obj)                             \
{                                                                         \
    ClassName *self = reinterpret_cast<ClassName *>(obj);                  \
    self->init();                                                         \
}                                                                         \
                                                                          \
static void ClassName##_cpp_class_init(ObjectClass *oc, const void *data) \
{                                                                         \
    DeviceClass *dc = DEVICE_CLASS(oc);                                   \
    dc->realize = ClassName##_cpp_realize;                                \
    device_class_set_legacy_reset(dc, ClassName##_cpp_reset);             \
    ClassName::classInit(dc);                                             \
}                                                                         \
                                                                          \
static const TypeInfo ClassName##_type_info = {                           \
    .name          = type_name_str,                                       \
    .parent        = parent_type_str,                                     \
    .instance_size = sizeof(ClassName),                                   \
    .instance_init = ClassName##_cpp_init,                                \
    .class_init    = ClassName##_cpp_class_init,                          \
};                                                                        \
                                                                          \
static void ClassName##_cpp_register_types(void)                          \
{                                                                         \
    type_register_static(&ClassName##_type_info);                         \
}                                                                         \
                                                                          \
type_init(ClassName##_cpp_register_types)

/*
 * REGISTER_QEMU_DEVICE_NORESET: same as REGISTER_QEMU_DEVICE but skips
 * the legacy_reset callback. Use when the device uses ResettableClass
 * multi-phase reset instead (or doesn't need a reset handler).
 */
#define REGISTER_QEMU_DEVICE_NORESET(ClassName, type_name_str,            \
                                     parent_type_str)                     \
static void ClassName##_cpp_realize(DeviceState *dev, Error **errp)       \
{                                                                         \
    ClassName *self = reinterpret_cast<ClassName *>(dev);                  \
    self->realize(errp);                                                  \
}                                                                         \
                                                                          \
static void ClassName##_cpp_init(Object *obj)                             \
{                                                                         \
    ClassName *self = reinterpret_cast<ClassName *>(obj);                  \
    self->init();                                                         \
}                                                                         \
                                                                          \
static void ClassName##_cpp_class_init(ObjectClass *oc, const void *data) \
{                                                                         \
    DeviceClass *dc = DEVICE_CLASS(oc);                                   \
    dc->realize = ClassName##_cpp_realize;                                \
    ClassName::classInit(dc);                                             \
}                                                                         \
                                                                          \
static const TypeInfo ClassName##_type_info = {                           \
    .name          = type_name_str,                                       \
    .parent        = parent_type_str,                                     \
    .instance_size = sizeof(ClassName),                                   \
    .instance_init = ClassName##_cpp_init,                                \
    .class_init    = ClassName##_cpp_class_init,                          \
};                                                                        \
                                                                          \
static void ClassName##_cpp_register_types(void)                          \
{                                                                         \
    type_register_static(&ClassName##_type_info);                         \
}                                                                         \
                                                                          \
type_init(ClassName##_cpp_register_types)

#endif /* QOM_CPP_OBJECT_H */
