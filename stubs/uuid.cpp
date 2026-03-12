#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/qapi-commands-machine.h"
#include "qemu/uuid.h"


UuidInfo *qmp_query_uuid(Error **errp)
{
    UuidInfo *info = static_cast<UuidInfo *>(g_malloc0(sizeof(*info)));

    info->UUID = g_strdup(UUID_NONE);
    return info;
}


} /* extern "C" */
