#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/qapi-commands-control.h"
#include "qapi/qmp-registry.h"


void qmp_quit(Error **errp)
{
    g_assert_not_reached();
}


} /* extern "C" */
