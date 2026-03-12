#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "block/block_int.h"


void blockdev_close_all_bdrv_states(void)
{
}


} /* extern "C" */
