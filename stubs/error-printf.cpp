#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {

#include "qemu/error-report.h"
#include "monitor/monitor.h"


int error_vprintf(const char *fmt, va_list ap)
{
    int ret;

    if (g_test_initialized() && !g_test_subprocess() &&
        getenv("QTEST_SILENT_ERRORS")) {
        char *msg = g_strdup_vprintf(fmt, ap);
        g_test_message("%s", msg);
        ret = strlen(msg);
        g_free(msg);
        return ret;
    }
    return vfprintf(stderr, fmt, ap);
}

int error_vprintf_unless_qmp(const char *fmt, va_list ap)
{
    return error_vprintf(fmt, ap);
}


} /* extern "C" */
