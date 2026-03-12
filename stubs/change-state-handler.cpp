#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "system/runstate.h"


VMChangeStateEntry *qemu_add_vm_change_state_handler(VMChangeStateHandler *cb,
                                                     void *opaque)
{
    return NULL;
}

void qemu_del_vm_change_state_handler(VMChangeStateEntry *e)
{
    /* Nothing to do. */
}


} /* extern "C" */
