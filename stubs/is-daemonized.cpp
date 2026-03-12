#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {



/* Win32 has its own inline stub */
#ifndef _WIN32
bool is_daemonized(void)
{
    return false;
}
#endif


} /* extern "C" */
