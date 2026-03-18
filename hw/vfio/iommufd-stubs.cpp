/*
 * Copyright (c) 2025 Oracle and/or its affiliates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "migration/cpr.h"
#include "migration/vmstate.h"

static const VMStateField vmstate_cpr_vfio_devices_fields[] = {
    VMSTATE_END_OF_LIST()
};

extern "C" const VMStateDescription vmstate_cpr_vfio_devices = {
    .name = CPR_STATE "/vfio devices",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = vmstate_cpr_vfio_devices_fields,
};
