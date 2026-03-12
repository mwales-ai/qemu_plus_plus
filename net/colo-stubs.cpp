#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "qemu/notify.h"
#include "net/colo-compare.h"

void colo_compare_cleanup(void)
{
}

} /* extern "C" */
