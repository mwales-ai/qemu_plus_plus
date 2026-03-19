/*
 * ARM multiprocessor CPU helpers
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef TARGET_ARM_MULTIPROCESSING_H
#define TARGET_ARM_MULTIPROCESSING_H

#include "target/arm/cpu-qom.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t arm_cpu_mp_affinity(ARMCPU *cpu);

#ifdef __cplusplus
}
#endif

#endif
