/*
 * QEMU++ C++ Object Model Wrappers
 *
 * Provides C++ base classes that are binary-compatible with QOM structs.
 * These wrappers allow new device code to use C++ inheritance, virtual
 * methods, and compile-time type checking while remaining compatible
 * with the existing QOM type registration system.
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

#include <type_traits>

/*
 * Design Philosophy
 * =================
 *
 * These wrappers do NOT replace QOM — they sit on top of it. A CppDevice
 * IS-A DeviceState in memory, registered through normal TypeInfo, and
 * fully compatible with existing C code that uses DEVICE(), OBJECT(), etc.
 *
 * What changes for the device author:
 *   - Virtual methods are C++ virtual (compiler-checked override)
 *   - Construction/destruction use C++ constructors/destructors
 *   - Type casting uses static_cast/dynamic_cast instead of OBJECT_CHECK
 *   - No more manual class_init() function pointer assignments
 *
 * What stays the same:
 *   - VMState migration descriptors (offsetof works on C++ classes)
 *   - MemoryRegionOps dispatch tables
 *   - QOM type registration (TypeInfo, type_init)
 *   - Property system (DEFINE_PROP_* macros)
 *   - Two-phase init (constructor + realize)
 */

/*
 * qom_fixup_vtable<T>(ptr): Restore C++ vtable after QOM's memcpy.
 *
 * QOM's type_initialize() copies parent class data via memcpy, which
 * overwrites the C++ vtable pointer. Call this in class_init() for
 * any class struct that uses C++ virtual methods.
 *
 * This copies just the vtable pointer (first sizeof(void*) bytes)
 * from a properly-constructed temporary, preserving all other fields
 * that were set by QOM's memcpy and class_base_init.
 */
template<typename T>
inline void qom_fixup_vtable(void *obj) {
    T tmp;
    memcpy(obj, &tmp, sizeof(void *));
}

namespace qemu {

/**
 * CppObject: C++ wrapper around QOM Object.
 *
 * This is not meant to be instantiated directly. It provides helper
 * methods for accessing the underlying QOM Object that all QEMU objects
 * embed as their first member.
 *
 * Any class inheriting from CppObject (through CppDevice etc.) can
 * safely cast to Object* because Object is always at offset 0 in
 * the DeviceState/SysBusDevice/PCIDevice hierarchy.
 */
class CppObject
{
public:
    /* Access the underlying QOM Object pointer */
    Object *qomObject()
    {
        return reinterpret_cast<Object *>(this);
    }

    const Object *qomObject() const
    {
        return reinterpret_cast<const Object *>(this);
    }

    /* Get the QOM type name string for this instance */
    const char *typeName() const
    {
        return object_get_typename(const_cast<Object *>(qomObject()));
    }

    /* QOM-compatible reference counting */
    void ref()   { object_ref(qomObject()); }
    void unref() { object_unref(qomObject()); }

    /*
     * Safe downcast using QOM's runtime type system.
     * Returns nullptr if the cast is invalid.
     */
    template<typename T>
    T *dynamicCast()
    {
        static_assert(std::is_base_of<CppObject, T>::value,
                      "dynamicCast target must derive from CppObject");
        Object *result = object_dynamic_cast(qomObject(),
                                             T::staticTypeName());
        return reinterpret_cast<T *>(result);
    }

    template<typename T>
    const T *dynamicCast() const
    {
        return const_cast<CppObject *>(this)->dynamicCast<T>();
    }

protected:
    /* No public construction — always created through QOM */
    CppObject() = default;
    ~CppObject() = default;

    /* Non-copyable, non-movable (QOM manages object lifecycle) */
    CppObject(const CppObject &) = delete;
    CppObject &operator=(const CppObject &) = delete;
};

/**
 * CppDevice: C++ wrapper for DeviceState.
 *
 * Subclass this to write QEMU devices using C++ patterns.
 * The memory layout is identical to DeviceState — the class adds
 * no new data members, only virtual methods and helper functions.
 *
 * Example:
 *   class MyTimer : public CppSysBusDevice {
 *       QEMU_DEVICE_TYPE("my-timer")
 *   public:
 *       void realize(Error **errp) override;
 *       void reset() override;
 *   private:
 *       MemoryRegion theMmio;
 *       uint32_t theCounter;
 *       qemu_irq theIrq;
 *   };
 */
class CppDevice : public CppObject
{
public:
    /**
     * realize: called when the device is fully configured and ready
     * to allocate resources. May fail with errp.
     *
     * Override this instead of setting DeviceClass.realize in class_init().
     */
    virtual void realize(Error **errp) {}

    /**
     * reset: called when the device or machine resets.
     *
     * Override this instead of setting a legacy_reset or resettable callback.
     */
    virtual void reset() {}

    /* Access the underlying DeviceState */
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
    virtual ~CppDevice() = default;
};

/*
 * Compile-time verification that our wrapper classes don't add data members.
 * CppDevice must be the same size as DeviceState, or the first subclass
 * that adds members must account for the vtable pointer.
 *
 * Note: CppDevice has virtual methods, so sizeof(CppDevice) includes a
 * vtable pointer. This is fine — QOM allocates instance_size bytes and
 * we declare instance_size = sizeof(MyCppDevice), which includes the vtable.
 * The vtable pointer occupies space that QOM would have allocated anyway
 * (instance_size covers the full object).
 */

} /* namespace qemu */

/*
 * QEMU_DEVICE_TYPE(type_name)
 *
 * Declares the QOM type name string for a C++ device class.
 * Place this in the public section of your class declaration.
 *
 * Usage:
 *   class MyDevice : public qemu::CppSysBusDevice {
 *       QEMU_DEVICE_TYPE("my-device")
 *   public:
 *       void realize(Error **errp) override;
 *   };
 */
#define QEMU_DEVICE_TYPE(name)                                        \
public:                                                               \
    static const char *staticTypeName() { return name; }              \
private:

/*
 * QEMU_DEVICE_REGISTER(ClassName)
 *
 * Generates the QOM TypeInfo and registration boilerplate for a C++ device.
 * Place this in the .cpp implementation file, at file scope.
 *
 * This macro generates:
 *   - A static realize callback that delegates to ClassName::realize()
 *   - A static reset callback that delegates to ClassName::reset()
 *   - A TypeInfo struct with correct sizes
 *   - A type_init() registration
 *
 * The parent type must be provided as the second argument.
 *
 * Usage:
 *   QEMU_DEVICE_REGISTER(MyDevice, TYPE_SYS_BUS_DEVICE)
 */
#define QEMU_DEVICE_REGISTER(ClassName, ParentType)                       \
                                                                          \
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
static void ClassName##_class_init(ObjectClass *oc, const void *data)     \
{                                                                         \
    DeviceClass *dc = DEVICE_CLASS(oc);                                   \
    dc->realize = ClassName##_cpp_realize;                                \
    device_class_set_legacy_reset(dc, ClassName##_cpp_reset);             \
    ClassName::classInit(dc);                                             \
}                                                                         \
                                                                          \
static void ClassName##_instance_init(Object *obj)                        \
{                                                                         \
    ClassName *self = reinterpret_cast<ClassName *>(obj);                  \
    new (self) ClassName();                                               \
}                                                                         \
                                                                          \
static void ClassName##_instance_finalize(Object *obj)                    \
{                                                                         \
    ClassName *self = reinterpret_cast<ClassName *>(obj);                  \
    self->~ClassName();                                                   \
}                                                                         \
                                                                          \
static const TypeInfo ClassName##_type_info = {                           \
    .name          = ClassName::staticTypeName(),                         \
    .parent        = ParentType,                                          \
    .instance_size = sizeof(ClassName),                                   \
    .instance_init = ClassName##_instance_init,                           \
    .instance_finalize = ClassName##_instance_finalize,                   \
    .class_init    = ClassName##_class_init,                              \
};                                                                        \
                                                                          \
static void ClassName##_register_types(void)                              \
{                                                                         \
    type_register_static(&ClassName##_type_info);                         \
}                                                                         \
                                                                          \
type_init(ClassName##_register_types)

#endif /* QOM_CPP_OBJECT_H */
