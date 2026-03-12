#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/qmp-registry.h"


bool qmp_command_available(const QmpCommand *cmd, Error **errp)
{
    return true;
}


} /* extern "C" */
