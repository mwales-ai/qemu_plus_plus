/*
 * Xilinx CFI interface
 *
 * Copyright (C) 2023, Advanced Micro Devices, Inc.
 *
 * Written by Francisco Iglesias <francisco.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/xlnx-cfi-if.h"
#include "qom/cpp/object.h"

void xlnx_cfi_transfer_packet(XlnxCfiIf *cfi_if, XlnxCfiPacket *pkt)
{
    XlnxCfiIfClass *xcic = XLNX_CFI_IF_GET_CLASS(cfi_if);

    if (xcic->cfi_transfer_packet) {
        xcic->cfi_transfer_packet(cfi_if, pkt);
    }
}

REGISTER_QEMU_INTERFACE(XlnxCfiIfClass, TYPE_XLNX_CFI_IF)

