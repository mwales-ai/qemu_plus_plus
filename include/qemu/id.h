#ifndef QEMU_ID_H
#define QEMU_ID_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IdSubSystems {
    ID_QDEV,
    ID_BLOCK,
    ID_CHR,
    ID_NET,
    ID_MAX      /* last element, used as array size */
} IdSubSystems;

char *id_generate(IdSubSystems id);
bool id_wellformed(const char *id);

#ifdef __cplusplus
}
#endif

#endif
