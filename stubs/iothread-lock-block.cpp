#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qemu/main-loop.h"


bool qemu_in_main_thread(void)
{
    return qemu_get_current_aio_context() == qemu_get_aio_context();
}


} /* extern "C" */
