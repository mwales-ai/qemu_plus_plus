#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "block/block.h"


BlockDriverState *bdrv_next_monitor_owned(BlockDriverState *bs)
{
    return NULL;
}


} /* extern "C" */
