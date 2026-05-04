/*
 * QEMU Thread Context
 *
 * Copyright Red Hat Inc., 2022
 *
 * Authors:
 *  David Hildenbrand <david@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/thread-context.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-visit.h"
#include "qapi/visitor.h"
#include "qemu/config-file.h"
#include "qom/object_interfaces.h"
#include "qemu/module.h"
#include "qemu/bitmap.h"
} /* extern "C" */

#ifdef CONFIG_NUMA
#include <numa.h>
#endif

enum {
    TC_CMD_NONE = 0,
    TC_CMD_STOP,
    TC_CMD_NEW,
};

typedef struct ThreadContextCmdNew {
    QemuThread *thread;
    const char *name;
    void *(*start_routine)(void *);
    void *arg;
    int mode;
} ThreadContextCmdNew;

static void *thread_context_run(void *opaque)
{
    ThreadContext *tc = static_cast<ThreadContext *>(opaque);

    tc->thread_id = qemu_get_thread_id();
    qemu_sem_post(&tc->sem);

    while (true) {
        /*
         * Threads inherit the CPU affinity of the creating thread. For this
         * reason, we create new (especially short-lived) threads from our
         * persistent context thread.
         *
         * Especially when QEMU is not allowed to set the affinity itself,
         * management tools can simply set the affinity of the context thread
         * after creating the context, to have new threads created via
         * the context inherit the CPU affinity automatically.
         */
        switch (tc->thread_cmd) {
        case TC_CMD_NONE:
            break;
        case TC_CMD_STOP:
            tc->thread_cmd = TC_CMD_NONE;
            qemu_sem_post(&tc->sem);
            return NULL;
        case TC_CMD_NEW: {
            ThreadContextCmdNew *cmd_new =
                static_cast<ThreadContextCmdNew *>(tc->thread_cmd_data);

            qemu_thread_create(cmd_new->thread, cmd_new->name,
                               cmd_new->start_routine, cmd_new->arg,
                               cmd_new->mode);
            tc->thread_cmd = TC_CMD_NONE;
            tc->thread_cmd_data = NULL;
            qemu_sem_post(&tc->sem);
            break;
        }
        default:
            g_assert_not_reached();
        }
        qemu_sem_wait(&tc->sem_thread);
    }
}

static void thread_context_set_cpu_affinity(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    ThreadContext *tc = THREAD_CONTEXT(obj);
    uint16List *l, *host_cpus = NULL;
    unsigned long *bitmap = NULL;
    int nbits = 0, ret;

    if (tc->init_cpu_bitmap) {
        error_setg(errp, "Mixing CPU and node affinity not supported");
        return;
    }

    if (!visit_type_uint16List(v, name, &host_cpus, errp)) {
        return;
    }

    if (!host_cpus) {
        error_setg(errp, "CPU list is empty");
        goto out;
    }

    for (l = host_cpus; l; l = l->next) {
        nbits = MAX(nbits, l->value + 1);
    }
    bitmap = bitmap_new(nbits);
    for (l = host_cpus; l; l = l->next) {
        set_bit(l->value, bitmap);
    }

    if (tc->thread_id != (unsigned)-1) {
        /*
         * Note: we won't be adjusting the affinity of any thread that is still
         * around, but only the affinity of the context thread.
         */
        ret = qemu_thread_set_affinity(&tc->thread, bitmap, nbits);
        if (ret) {
            error_setg(errp, "Setting CPU affinity failed: %s", strerror(ret));
        }
    } else {
        tc->init_cpu_bitmap = bitmap;
        bitmap = NULL;
        tc->init_cpu_nbits = nbits;
    }
out:
    g_free(bitmap);
    qapi_free_uint16List(host_cpus);
}

static void thread_context_get_cpu_affinity(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    unsigned long *bitmap, nbits, value;
    ThreadContext *tc = THREAD_CONTEXT(obj);
    uint16List *host_cpus = NULL;
    uint16List **tail = &host_cpus;
    int ret;

    if (tc->thread_id == (unsigned)-1) {
        error_setg(errp, "Object not initialized yet");
        return;
    }

    ret = qemu_thread_get_affinity(&tc->thread, &bitmap, &nbits);
    if (ret) {
        error_setg(errp, "Getting CPU affinity failed: %s", strerror(ret));
        return;
    }

    value = find_first_bit(bitmap, nbits);
    while (value < nbits) {
        /* Expand QAPI_LIST_APPEND manually for C++ void* cast safety */
        *tail = static_cast<uint16List *>(g_malloc0(sizeof(**tail)));
        (*tail)->value = value;
        tail = &(*tail)->next;

        value = find_next_bit(bitmap, nbits, value + 1);
    }
    g_free(bitmap);

    visit_type_uint16List(v, name, &host_cpus, errp);
    qapi_free_uint16List(host_cpus);
}

static void thread_context_set_node_affinity(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
#ifdef CONFIG_NUMA
    const int nbits = numa_num_possible_cpus();
    ThreadContext *tc = THREAD_CONTEXT(obj);
    uint16List *l, *host_nodes = NULL;
    unsigned long *bitmap = NULL;
    struct bitmask *tmp_cpus;
    int ret, i;

    if (tc->init_cpu_bitmap) {
        error_setg(errp, "Mixing CPU and node affinity not supported");
        return;
    }

    if (!visit_type_uint16List(v, name, &host_nodes, errp)) {
        return;
    }

    if (!host_nodes) {
        error_setg(errp, "Node list is empty");
        goto out;
    }

    bitmap = bitmap_new(nbits);
    tmp_cpus = numa_allocate_cpumask();
    for (l = host_nodes; l; l = l->next) {
        numa_bitmask_clearall(tmp_cpus);
        ret = numa_node_to_cpus(l->value, tmp_cpus);
        if (ret) {
            /* We ignore any errors, such as impossible nodes. */
            continue;
        }
        for (i = 0; i < nbits; i++) {
            if (numa_bitmask_isbitset(tmp_cpus, i)) {
                set_bit(i, bitmap);
            }
        }
    }
    numa_free_cpumask(tmp_cpus);

    if (bitmap_empty(bitmap, nbits)) {
        error_setg(errp, "The nodes select no CPUs");
        goto out;
    }

    if (tc->thread_id != (unsigned)-1) {
        /*
         * Note: we won't be adjusting the affinity of any thread that is still
         * around for now, but only the affinity of the context thread.
         */
        ret = qemu_thread_set_affinity(&tc->thread, bitmap, nbits);
        if (ret) {
            error_setg(errp, "Setting CPU affinity failed: %s", strerror(ret));
        }
    } else {
        tc->init_cpu_bitmap = bitmap;
        bitmap = NULL;
        tc->init_cpu_nbits = nbits;
    }
out:
    g_free(bitmap);
    qapi_free_uint16List(host_nodes);
#else
    error_setg(errp, "NUMA node affinity is not supported by this QEMU");
#endif
}

static void thread_context_get_thread_id(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    ThreadContext *tc = THREAD_CONTEXT(obj);
    uint64_t value = tc->thread_id;

    visit_type_uint64(v, name, &value, errp);
}

static void thread_context_instance_complete(UserCreatable *uc, Error **errp)
{
    ThreadContext *tc = THREAD_CONTEXT(uc);
    char *thread_name;
    int ret;

    thread_name = g_strdup_printf("TC %s",
                               object_get_canonical_path_component(OBJECT(uc)));
    qemu_thread_create(&tc->thread, thread_name, thread_context_run, tc,
                       QEMU_THREAD_JOINABLE);
    g_free(thread_name);

    /* Wait until initialization of the thread is done. */
    while (tc->thread_id == (unsigned)-1) {
        qemu_sem_wait(&tc->sem);
    }

    if (tc->init_cpu_bitmap) {
        ret = qemu_thread_set_affinity(&tc->thread, tc->init_cpu_bitmap,
                                       tc->init_cpu_nbits);
        if (ret) {
            error_setg(errp, "Setting CPU affinity failed: %s", strerror(ret));
        }
        g_free(tc->init_cpu_bitmap);
        tc->init_cpu_bitmap = NULL;
    }
}

static void thread_context_class_init(ObjectClass *oc, const void *data)
{
    UserCreatableClass *ucc = USER_CREATABLE_CLASS(oc);

    ucc->complete = thread_context_instance_complete;
    object_class_property_add(oc, "thread-id", "int",
                              thread_context_get_thread_id, NULL, NULL,
                              NULL);
    object_class_property_add(oc, "cpu-affinity", "int",
                              thread_context_get_cpu_affinity,
                              thread_context_set_cpu_affinity, NULL, NULL);
    object_class_property_add(oc, "node-affinity", "int", NULL,
                              thread_context_set_node_affinity, NULL, NULL);
}

void ThreadContext::init()
{
    thread_id = (unsigned)-1;
    qemu_sem_init(&sem, 0);
    qemu_sem_init(&sem_thread, 0);
    qemu_mutex_init(&mutex);
}

void ThreadContext::finalize()
{
    if (thread_id != (unsigned)-1) {
        thread_cmd = TC_CMD_STOP;
        qemu_sem_post(&sem_thread);
        qemu_thread_join(&thread);
    }
    qemu_sem_destroy(&sem);
    qemu_sem_destroy(&sem_thread);
    qemu_mutex_destroy(&mutex);
}

static const InterfaceInfo thread_context_interfaces[] = {
    { TYPE_USER_CREATABLE },
    { }
};

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT_CI_CS_IFACES(ThreadContext, ObjectClass,
                                   TYPE_THREAD_CONTEXT, TYPE_OBJECT,
                                   thread_context_class_init,
                                   thread_context_interfaces)

extern "C"
void thread_context_create_thread(ThreadContext *tc, QemuThread *thread,
                                  const char *name,
                                  void *(*start_routine)(void *), void *arg,
                                  int mode)
{
    ThreadContextCmdNew data = {
        .thread = thread,
        .name = name,
        .start_routine = start_routine,
        .arg = arg,
        .mode = mode,
    };

    qemu_mutex_lock(&tc->mutex);
    tc->thread_cmd = TC_CMD_NEW;
    tc->thread_cmd_data = &data;
    qemu_sem_post(&tc->sem_thread);

    while (tc->thread_cmd != TC_CMD_NONE) {
        qemu_sem_wait(&tc->sem);
    }
    qemu_mutex_unlock(&tc->mutex);
}
