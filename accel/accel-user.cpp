/*
 * QEMU accel class, user-mode components
 *
 * Copyright 2021 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "qemu/accel.h"
#include "accel-internal.h"

void accel_init_ops_interfaces(AccelClass *ac)
{
    /* nothing */
}

AccelState *current_accel(void)
{
    static AccelState *accel;

    if (!accel) {
        AccelClass *ac = accel_find("tcg");

        g_assert(ac != NULL);
        accel = ACCEL(object_new_with_class(OBJECT_CLASS(ac)));
    }
    return accel;
}

} /* extern "C" */
