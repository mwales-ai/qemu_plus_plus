#ifndef QEMU_HW_H
#define QEMU_HW_H

#ifdef __cplusplus
extern "C" {
#endif

G_NORETURN void hw_error(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

#ifdef __cplusplus
}
#endif

#endif
