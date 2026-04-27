/*
 * QEMU Firmware configuration device emulation (QOM interfaces)
 *
 * Copyright 2020 Red Hat, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/nvram/fw_cfg.h"
#include "qom/cpp/object.h"

REGISTER_QEMU_INTERFACE(FWCfgDataGeneratorClass, TYPE_FW_CFG_DATA_GENERATOR_INTERFACE)
