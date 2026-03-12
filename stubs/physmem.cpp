#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "exec/cpu-common.h"


RAMBlock *qemu_ram_block_from_host(void *ptr, bool round_offset,
                                   ram_addr_t *offset)
{
    return NULL;
}

int qemu_ram_get_fd(RAMBlock *rb)
{
    return -1;
}


} /* extern "C" */
