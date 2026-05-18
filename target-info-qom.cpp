/*
 * QEMU binary/target API (QOM types)
 *
 *  Copyright (c) Linaro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "hw/arm/machines-qom.h"

#include "qom/cpp/object.h"

REGISTER_QEMU_INTERFACE_BARE(target_arm_machine, TYPE_TARGET_ARM_MACHINE)
REGISTER_QEMU_INTERFACE_BARE(target_aarch64_machine, TYPE_TARGET_AARCH64_MACHINE)
