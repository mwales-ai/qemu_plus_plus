#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "system/block-backend.h"


int blk_commit_all(void)
{
    return 0;
}


} /* extern "C" */
