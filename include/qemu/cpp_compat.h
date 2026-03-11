/*
 * cpp_compat.h - C/C++ compatibility macros for the QEMU++ port
 *
 * This header provides macros to help with the incremental C-to-C++17 port.
 * Include this header in .cpp files that need to include C headers.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_CPP_COMPAT_H
#define QEMU_CPP_COMPAT_H

/*
 * QEMU_EXTERN_C_BEGIN / QEMU_EXTERN_C_END
 *
 * Wrap C header includes in .cpp files:
 *
 *   QEMU_EXTERN_C_BEGIN
 *   #include "qemu/osdep.h"
 *   #include "hw/qdev-core.h"
 *   QEMU_EXTERN_C_END
 */
#ifdef __cplusplus
#define QEMU_EXTERN_C_BEGIN  extern "C" {
#define QEMU_EXTERN_C_END    }
#else
#define QEMU_EXTERN_C_BEGIN
#define QEMU_EXTERN_C_END
#endif

/*
 * QEMU_EXTERN_C
 *
 * Mark a single function declaration as extern "C" in a header that
 * may be included from both C and C++:
 *
 *   QEMU_EXTERN_C void my_function(int arg);
 */
#ifdef __cplusplus
#define QEMU_EXTERN_C extern "C"
#else
#define QEMU_EXTERN_C
#endif

/*
 * QEMU_CAST(type, expr)
 *
 * A cast macro that uses static_cast in C++ and a C-style cast in C.
 * Use this in headers shared between C and C++ code.
 */
#ifdef __cplusplus
#define QEMU_CAST(type, expr) static_cast<type>(expr)
#else
#define QEMU_CAST(type, expr) ((type)(expr))
#endif

#endif /* QEMU_CPP_COMPAT_H */
