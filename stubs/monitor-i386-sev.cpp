/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/error.h"
#include "qapi/qapi-commands-misc-i386.h"


SevInfo *qmp_query_sev(Error **errp)
{
    error_setg(errp, "SEV is not available in this QEMU");
    return NULL;
}

SevLaunchMeasureInfo *qmp_query_sev_launch_measure(Error **errp)
{
    error_setg(errp, "SEV is not available in this QEMU");
    return NULL;
}

SevCapability *qmp_query_sev_capabilities(Error **errp)
{
    error_setg(errp, "SEV is not available in this QEMU");
    return NULL;
}

void qmp_sev_inject_launch_secret(const char *packet_header, const char *secret,
                                  bool has_gpa, uint64_t gpa, Error **errp)
{
    error_setg(errp, "SEV is not available in this QEMU");
}

SevAttestationReport *qmp_query_sev_attestation_report(const char *mnonce,
                                                       Error **errp)
{
    error_setg(errp, "SEV is not available in this QEMU");
    return NULL;
}


} /* extern "C" */
