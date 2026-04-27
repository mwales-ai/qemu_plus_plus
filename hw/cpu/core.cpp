/*
 * CPU core abstract device
 *
 * Copyright (C) 2016 Bharata B Rao <bharata@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include "hw/boards.h"
#include "hw/cpu/core.h"
#include "qapi/error.h"
#include "qapi/visitor.h"

static void core_prop_get_core_id(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    CPUCore *core = CPU_CORE(obj);
    int64_t value = core->core_id;

    visit_type_int(v, name, &value, errp);
}

static void core_prop_set_core_id(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    CPUCore *core = CPU_CORE(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }

    if (value < 0) {
        error_setg(errp, "Invalid core id %" PRId64, value);
        return;
    }

    core->core_id = value;
}

static void core_prop_get_nr_threads(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    CPUCore *core = CPU_CORE(obj);
    int64_t value = core->nr_threads;

    visit_type_int(v, name, &value, errp);
}

static void core_prop_set_nr_threads(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    CPUCore *core = CPU_CORE(obj);
    int64_t value;

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }

    core->nr_threads = value;
}

void CPUCore::init()
{
    /*
     * Only '-device something-cpu-core,help' can get us there before
     * the machine has been created. We don't care to set nr_threads
     * in this case since it isn't used afterwards.
     */
    if (current_machine) {
        nr_threads = current_machine->smp.threads;
    }
}

void CPUCore::classInit(DeviceClass *dc)
{
    ObjectClass *oc = reinterpret_cast<ObjectClass *>(dc);

    set_bit(DEVICE_CATEGORY_CPU, dc->categories);
    object_class_property_add(oc, "core-id", "int", core_prop_get_core_id,
                              core_prop_set_core_id, NULL, NULL);
    object_class_property_add(oc, "nr-threads", "int", core_prop_get_nr_threads,
                              core_prop_set_nr_threads, NULL, NULL);
}

#include "qom/cpp/object.h"

REGISTER_QEMU_DEVICE_ABSTRACT_NO_CS(CPUCore, TYPE_CPU_CORE, TYPE_DEVICE)
