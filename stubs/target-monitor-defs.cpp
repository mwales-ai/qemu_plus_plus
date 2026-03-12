#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "monitor/hmp-target.h"


const MonitorDef *target_monitor_defs(void)
{
    return NULL;
}


} /* extern "C" */
