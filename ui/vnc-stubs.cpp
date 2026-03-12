#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "ui/console.h"
#include "qapi/error.h"

int vnc_display_password(const char *id, const char *password)
{
    return -ENODEV;
}
int vnc_display_pw_expire(const char *id, time_t expires)
{
    return -ENODEV;
};

} /* extern "C" */
