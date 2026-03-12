/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/error.h"
#include "qapi/qapi-commands-machine.h"


CpuModelCompareInfo *
qmp_query_cpu_model_comparison(CpuModelInfo *infoa,
                               CpuModelInfo *infob,
                               Error **errp)
{
    error_setg(errp, "CPU model comparison is not supported on this target");
    return NULL;
}

CpuModelBaselineInfo *
qmp_query_cpu_model_baseline(CpuModelInfo *infoa,
                             CpuModelInfo *infob,
                             Error **errp)
{
    error_setg(errp, "CPU model baseline is not supported on this target");
    return NULL;
}


} /* extern "C" */
