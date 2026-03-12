#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "system/cpu-timers.h"
#include "qemu/main-loop.h"


void qemu_timer_notify_cb(void *opaque, QEMUClockType type)
{
    qemu_notify_event();
}


} /* extern "C" */
