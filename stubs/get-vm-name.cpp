#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "system/system.h"


const char *qemu_get_vm_name(void)
{
    return NULL;
}


} /* extern "C" */
