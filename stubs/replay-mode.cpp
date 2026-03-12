#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "system/replay.h"


ReplayMode replay_mode;


} /* extern "C" */
