#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {


#include "system/runstate.h"

void qemu_system_vmstop_request_prepare(void)
{
    abort();
}

void qemu_system_vmstop_request(RunState state)
{
    abort();
}


} /* extern "C" */
