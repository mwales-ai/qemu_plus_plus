#ifndef QEMU_SHADER_H
#define QEMU_SHADER_H

#include <epoxy/gl.h>

typedef struct QemuGLShader QemuGLShader;

#ifdef __cplusplus
extern "C" {
#endif

void qemu_gl_run_texture_blit(QemuGLShader *gls, bool flip);

QemuGLShader *qemu_gl_init_shader(void);
void qemu_gl_fini_shader(QemuGLShader *gls);

#ifdef __cplusplus
}
#endif

#endif /* QEMU_SHADER_H */
