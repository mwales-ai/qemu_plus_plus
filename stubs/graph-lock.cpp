#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "block/graph-lock.h"


void register_aiocontext(AioContext *ctx)
{
}

void unregister_aiocontext(AioContext *ctx)
{
}


} /* extern "C" */
