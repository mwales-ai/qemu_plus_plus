#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "hw/qdev-core.h"


BusState *sysbus_get_default(void)
{
    return NULL;
}


} /* extern "C" */
