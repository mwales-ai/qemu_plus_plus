#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {


#include "system/runstate.h"

bool runstate_check(RunState state)
{
    return state == RUN_STATE_PRELAUNCH;
}


} /* extern "C" */
