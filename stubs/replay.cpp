#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "exec/replay-core.h"


void replay_finish(void)
{
}

void replay_save_random(int ret, void *buf, size_t len)
{
}

int replay_read_random(void *buf, size_t len)
{
    return 0;
}

bool replay_reverse_step(void)
{
    return false;
}

bool replay_reverse_continue(void)
{
    return false;
}


} /* extern "C" */
