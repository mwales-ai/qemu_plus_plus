#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "block/export.h"


/* Only used in programs that support block exports (libblockdev.a) */
void blk_exp_close_all(void)
{
}


} /* extern "C" */
