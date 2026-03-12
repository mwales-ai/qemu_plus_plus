#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "exec/gdbstub.h"       /* gdb_static_features */


const GDBFeature gdb_static_features[] = {
  { NULL }
};


} /* extern "C" */
