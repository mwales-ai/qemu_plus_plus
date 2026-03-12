#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qapi/error.h"
#include "monitor/monitor.h"
#include "../monitor/monitor-internal.h"


int monitor_fdset_dup_fd_add(int64_t fdset_id, int flags, Error **errp)
{
    errno = ENOSYS;
    return -1;
}

void monitor_fdset_dup_fd_remove(int dupfd)
{
}

void monitor_fdsets_cleanup(void)
{
}


} /* extern "C" */
