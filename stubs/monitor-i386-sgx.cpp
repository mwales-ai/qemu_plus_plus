/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/error.h"
#include "qapi/qapi-commands-misc-i386.h"


SgxInfo *qmp_query_sgx(Error **errp)
{
    error_setg(errp, "SGX support is not compiled in");
    return NULL;
}

SgxInfo *qmp_query_sgx_capabilities(Error **errp)
{
    error_setg(errp, "SGX support is not compiled in");
    return NULL;
}


} /* extern "C" */
