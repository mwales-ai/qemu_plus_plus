/*
 * QEMU target info helpers
 *
 *  Copyright (c) Linaro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/target-info.h"
#include "qemu/target-info-qapi.h"
#include "qemu/target-info-impl.h"
#include "qapi/error.h"
}

extern "C" const char *target_name(void)
{
    return target_info()->target_name;
}

extern "C" unsigned target_long_bits(void)
{
    return target_info()->long_bits;
}

extern "C" SysEmuTarget target_arch(void)
{
    SysEmuTarget arch = target_info()->target_arch;

    if (arch == SYS_EMU_TARGET__MAX) {
        arch = static_cast<SysEmuTarget>(
            qapi_enum_parse(&SysEmuTarget_lookup, target_name(), -1,
                            &error_abort));
    }
    return arch;
}

extern "C" const char *target_cpu_type(void)
{
    return target_info()->cpu_type;
}

extern "C" const char *target_machine_typename(void)
{
    return target_info()->machine_typename;
}

extern "C" EndianMode target_endian_mode(void)
{
    return target_info()->endianness;
}

extern "C" bool target_big_endian(void)
{
    return target_endian_mode() == ENDIAN_MODE_BIG;
}

extern "C" bool target_base_arm(void)
{
    switch (target_arch()) {
    case SYS_EMU_TARGET_ARM:
    case SYS_EMU_TARGET_AARCH64:
        return true;
    default:
        return false;
    }
}

extern "C" bool target_arm(void)
{
    return target_arch() == SYS_EMU_TARGET_ARM;
}

extern "C" bool target_aarch64(void)
{
    return target_arch() == SYS_EMU_TARGET_AARCH64;
}
