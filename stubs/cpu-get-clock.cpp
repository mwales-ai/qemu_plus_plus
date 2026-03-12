#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "system/cpu-timers.h"
#include "qemu/main-loop.h"


int64_t cpu_get_clock(void)
{
    return get_clock_realtime();
}


} /* extern "C" */
