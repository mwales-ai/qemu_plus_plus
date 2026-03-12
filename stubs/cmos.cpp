#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "hw/block/fdc.h"


int cmos_get_fd_drive_type(FloppyDriveType fd0)
{
    return 0;
}


} /* extern "C" */
