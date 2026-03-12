/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/error.h"
#include "qapi/qapi-commands-misc-i386.h"


void qmp_rtc_reset_reinjection(Error **errp)
{
    error_setg(errp,
               "RTC interrupt reinjection backlog reset is not available for"
               "this machine");
}


} /* extern "C" */
