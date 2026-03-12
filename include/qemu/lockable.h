/*
 * Polymorphic locking functions (aka poor man templates)
 *
 * Copyright Red Hat, Inc. 2017, 2018
 *
 * Author: Paolo Bonzini <pbonzini@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#ifndef QEMU_LOCKABLE_H
#define QEMU_LOCKABLE_H

#include "qemu/coroutine-core.h"
#include "qemu/thread.h"

typedef void QemuLockUnlockFunc(void *);

typedef struct QemuLockable {
    void *object;
    QemuLockUnlockFunc *lock;
    QemuLockUnlockFunc *unlock;
} QemuLockable;

static inline __attribute__((__always_inline__)) QemuLockable *
qemu_make_lockable(void *x, QemuLockable *lockable)
{
    /*
     * We cannot test this in a macro, otherwise we get compiler
     * warnings like "the address of 'm' will always evaluate as 'true'".
     */
    return x ? lockable : NULL;
}

static inline __attribute__((__always_inline__)) QemuLockable *
qemu_null_lockable(void *x)
{
    if (x != NULL) {
        qemu_build_not_reached();
    }
    return NULL;
}

/*
 * In C++, void* does not implicitly convert to typed pointers, so we
 * need explicit casts in the lock/unlock wrappers.
 */
#ifdef __cplusplus
#define QML_FUNC_(name, type)                                     \
    static inline void qemu_lockable_ ## name ## _lock(void *x)   \
    {                                                             \
        qemu_ ## name ## _lock(static_cast<type *>(x));           \
    }                                                             \
    static inline void qemu_lockable_ ## name ## _unlock(void *x) \
    {                                                             \
        qemu_ ## name ## _unlock(static_cast<type *>(x));         \
    }

QML_FUNC_(mutex, QemuMutex)
QML_FUNC_(rec_mutex, QemuRecMutex)
QML_FUNC_(co_mutex, CoMutex)
QML_FUNC_(spin, QemuSpin)
#else
#define QML_FUNC_(name, type)                                     \
    static inline void qemu_lockable_ ## name ## _lock(void *x)   \
    {                                                             \
        qemu_ ## name ## _lock(x);                                \
    }                                                             \
    static inline void qemu_lockable_ ## name ## _unlock(void *x) \
    {                                                             \
        qemu_ ## name ## _unlock(x);                              \
    }

QML_FUNC_(mutex, QemuMutex)
QML_FUNC_(rec_mutex, QemuRecMutex)
QML_FUNC_(co_mutex, CoMutex)
QML_FUNC_(spin, QemuSpin)
#endif

/*
 * In C, compound literals have the lifetime of an automatic variable.
 * In C++ it would be different, but then C++ wouldn't need QemuLockable
 * either...
 */
#define QML_OBJ_(x, name) (&(QemuLockable) {        \
        .object = (x),                              \
        .lock = qemu_lockable_ ## name ## _lock,    \
        .unlock = qemu_lockable_ ## name ## _unlock \
    })

/**
 * QEMU_MAKE_LOCKABLE - Make a polymorphic QemuLockable
 *
 * @x: a lock object (currently one of QemuMutex, QemuRecMutex,
 *     CoMutex, QemuSpin).
 *
 * Returns a QemuLockable object that can be passed around
 * to a function that can operate with locks of any kind, or
 * NULL if @x is %NULL.
 *
 * Note the special case for void *, so that we may pass "NULL".
 */
#ifdef __cplusplus
/*
 * C++ uses function overloading instead of _Generic, and GCC statement
 * expressions to create QemuLockable values with expression-scoped lifetime.
 */
static inline QemuLockable qemu_lockable_init_(QemuMutex *x) {
    QemuLockable l = { x, qemu_lockable_mutex_lock, qemu_lockable_mutex_unlock };
    return l;
}
static inline QemuLockable qemu_lockable_init_(QemuRecMutex *x) {
    QemuLockable l = { x, qemu_lockable_rec_mutex_lock,
                        qemu_lockable_rec_mutex_unlock };
    return l;
}
static inline QemuLockable qemu_lockable_init_(CoMutex *x) {
    QemuLockable l = { x, qemu_lockable_co_mutex_lock,
                        qemu_lockable_co_mutex_unlock };
    return l;
}
static inline QemuLockable qemu_lockable_init_(QemuSpin *x) {
    QemuLockable l = { x, qemu_lockable_spin_lock, qemu_lockable_spin_unlock };
    return l;
}
static inline QemuLockable *qemu_lockable_init_(QemuLockable *x) {
    return x;
}

#define QEMU_MAKE_LOCKABLE(x) __extension__ ({                          \
        QemuLockable qml_lockable_ = qemu_lockable_init_(x);           \
        qemu_make_lockable(static_cast<void *>(x), &qml_lockable_);    \
    })

#define QEMU_MAKE_LOCKABLE_NONNULL(x) __extension__ ({                 \
        QemuLockable qml_lockable_ = qemu_lockable_init_(x);           \
        &qml_lockable_;                                                 \
    })
#else
#define QEMU_MAKE_LOCKABLE(x)                                           \
    _Generic((x), QemuLockable *: (x),                                  \
             void *: qemu_null_lockable(x),                             \
             QemuMutex *: qemu_make_lockable(x, QML_OBJ_(x, mutex)),    \
             QemuRecMutex *: qemu_make_lockable(x, QML_OBJ_(x, rec_mutex)), \
             CoMutex *: qemu_make_lockable(x, QML_OBJ_(x, co_mutex)),   \
             QemuSpin *: qemu_make_lockable(x, QML_OBJ_(x, spin)))

/**
 * QEMU_MAKE_LOCKABLE_NONNULL - Make a polymorphic QemuLockable
 *
 * @x: a lock object (currently one of QemuMutex, QemuRecMutex,
 *     CoMutex, QemuSpin).
 *
 * Returns a QemuLockable object that can be passed around
 * to a function that can operate with locks of any kind.
 */
#define QEMU_MAKE_LOCKABLE_NONNULL(x)                           \
    _Generic((x), QemuLockable *: (x),                          \
                  QemuMutex *: QML_OBJ_(x, mutex),              \
                  QemuRecMutex *: QML_OBJ_(x, rec_mutex),       \
                  CoMutex *: QML_OBJ_(x, co_mutex),             \
                  QemuSpin *: QML_OBJ_(x, spin))
#endif

static inline void qemu_lockable_lock(QemuLockable *x)
{
    x->lock(x->object);
}

static inline void qemu_lockable_unlock(QemuLockable *x)
{
    x->unlock(x->object);
}

static inline QemuLockable *qemu_lockable_auto_lock(QemuLockable *x)
{
    qemu_lockable_lock(x);
    return x;
}

static inline void qemu_lockable_auto_unlock(QemuLockable *x)
{
    if (x) {
        qemu_lockable_unlock(x);
    }
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(QemuLockable, qemu_lockable_auto_unlock)

/*
 * In C++, the statement-expression-based QEMU_MAKE_LOCKABLE creates a
 * QemuLockable with expression-scoped lifetime, which is fine for passing
 * to functions but not for the LOCK_GUARD macros where the lockable must
 * outlive the guard variable. So provide C++ specific guard macros that
 * create named QemuLockable locals with proper block scope.
 */
#ifdef __cplusplus

#define WITH_QEMU_LOCK_GUARD_(x, id)                                    \
    QemuLockable glue(qemu_lockable_obj_, id) =                         \
        qemu_lockable_init_(x);                                         \
    for (g_autoptr(QemuLockable) glue(qemu_lockable_auto_, id) =        \
                qemu_lockable_auto_lock(                                \
                    &glue(qemu_lockable_obj_, id));                      \
         glue(qemu_lockable_auto_, id);                                  \
         qemu_lockable_auto_unlock(glue(qemu_lockable_auto_, id)),       \
            glue(qemu_lockable_auto_, id) = NULL)

#define WITH_QEMU_LOCK_GUARD(x) \
    WITH_QEMU_LOCK_GUARD_((x), __COUNTER__)

#define QEMU_LOCK_GUARD_(x, id)                                          \
    QemuLockable glue(qemu_lockable_obj_, id) =                          \
        qemu_lockable_init_(x);                                          \
    g_autoptr(QemuLockable)                                              \
    glue(qemu_lockable_auto_, id) G_GNUC_UNUSED =                        \
            qemu_lockable_auto_lock(&glue(qemu_lockable_obj_, id))

#define QEMU_LOCK_GUARD(x) QEMU_LOCK_GUARD_((x), __COUNTER__)

#else /* !__cplusplus */

#define WITH_QEMU_LOCK_GUARD_(x, var) \
    for (g_autoptr(QemuLockable) var = \
                qemu_lockable_auto_lock(QEMU_MAKE_LOCKABLE_NONNULL((x))); \
         var; \
         qemu_lockable_auto_unlock(var), var = NULL)

/**
 * WITH_QEMU_LOCK_GUARD - Lock a lock object for scope
 *
 * @x: a lock object (currently one of QemuMutex, CoMutex, QemuSpin).
 *
 * This macro defines a lock scope such that entering the scope takes the lock
 * and leaving the scope releases the lock.  Return statements are allowed
 * within the scope and release the lock.  Break and continue statements leave
 * the scope early and release the lock.
 *
 *   WITH_QEMU_LOCK_GUARD(&mutex) {
 *       ...
 *       if (error) {
 *           return; <-- mutex is automatically unlocked
 *       }
 *
 *       if (early_exit) {
 *           break;  <-- leave this scope early
 *       }
 *       ...
 *   }
 */
#define WITH_QEMU_LOCK_GUARD(x) \
    WITH_QEMU_LOCK_GUARD_((x), glue(qemu_lockable_auto, __COUNTER__))

/**
 * QEMU_LOCK_GUARD - Lock an object until the end of the scope
 *
 * @x: a lock object (currently one of QemuMutex, CoMutex, QemuSpin).
 *
 * This macro takes a lock until the end of the scope.  Return statements
 * release the lock.
 *
 *   ... <-- mutex not locked
 *   QEMU_LOCK_GUARD(&mutex); <-- mutex locked from here onwards
 *   ...
 *   if (error) {
 *       return; <-- mutex is automatically unlocked
 *   }
 */
#define QEMU_LOCK_GUARD(x)                                       \
    g_autoptr(QemuLockable)                                      \
    glue(qemu_lockable_auto, __COUNTER__) G_GNUC_UNUSED =        \
            qemu_lockable_auto_lock(QEMU_MAKE_LOCKABLE((x)))

#endif /* __cplusplus */

#endif
