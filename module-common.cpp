#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "qemu/module.h"

void qemu_module_dummy(void)
{
}

void DSO_STAMP_FUN(void)
{
}

} /* extern "C" */
