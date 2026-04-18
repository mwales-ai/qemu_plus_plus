/*
 * QEMU CPU cluster
 *
 * Copyright (c) 2018 GreenSocs SAS
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see
 * <http://www.gnu.org/licenses/gpl-2.0.html>
 */

#include "qemu/osdep.h"

#include "hw/core/cpu.h"
#include "hw/cpu/cluster.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"

static const Property cpu_cluster_properties[] = {
    DEFINE_PROP_UINT32("cluster-id", CPUClusterState, cluster_id, 0),
};

typedef struct CallbackData {
    CPUClusterState *cluster;
    int cpu_count;
} CallbackData;

static int add_cpu_to_cluster(Object *obj, void *opaque)
{
    CallbackData *cbdata = static_cast<CallbackData *>(opaque);
    CPUState *cpu = reinterpret_cast<CPUState *>(object_dynamic_cast(obj, TYPE_CPU));

    if (cpu) {
        cpu->cluster_index = cbdata->cluster->cluster_id;
        cbdata->cpu_count++;
    }
    return 0;
}

void CPUClusterState::realize(Error **errp)
{
    Object *cluster_obj = OBJECT(this);
    CallbackData cbdata = {
        .cluster = this,
        .cpu_count = 0,
    };

    if (cluster_id >= MAX_CLUSTERS) {
        error_setg(errp, "cluster-id must be less than %d", MAX_CLUSTERS);
        return;
    }

    object_child_foreach_recursive(cluster_obj, add_cpu_to_cluster, &cbdata);

    assert(cbdata.cpu_count > 0);
}

void CPUClusterState::classInit(DeviceClass *dc)
{
    device_class_set_props(dc, cpu_cluster_properties);
    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(CPUClusterState, TYPE_CPU_CLUSTER, TYPE_DEVICE)
