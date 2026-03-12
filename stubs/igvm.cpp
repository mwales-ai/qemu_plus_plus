/*
 * QEMU IGVM, stubs
 *
 * Copyright (C) 2026 Red Hat
 *
 * Authors:
 *  Gerd Hoffmann <kraxel@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {


#include "system/igvm.h"


int qigvm_x86_get_mem_map_entry(int index,
                                ConfidentialGuestMemoryMapEntry *entry,
                                Error **errp)
{
    return -1;
}

int qigvm_x86_set_vp_context(void *data, int index, Error **errp)
{
    return -1;
}


} /* extern "C" */
