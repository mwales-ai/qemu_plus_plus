#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "system/hw_accel.h"


void cpu_synchronize_state(CPUState *cpu)
{
}
void cpu_synchronize_post_init(CPUState *cpu)
{
}


} /* extern "C" */
