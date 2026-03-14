#ifndef QEMU_PROGRESS_H
#define QEMU_PROGRESS_H

#ifdef __cplusplus
extern "C" {
#endif

void qemu_progress_init(int enabled, float min_skip);
void qemu_progress_end(void);
void qemu_progress_print(float delta, int max);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_PROGRESS_H */
