/*
 * VMState interface
 *
 * Copyright (c) 2009-2019 Red Hat Inc
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif
#include "qom/cpp/object.h"

extern "C" {
#include "hw/vmstate-if.h"
} /* extern "C" */

REGISTER_QEMU_INTERFACE(VMStateIfClass, TYPE_VMSTATE_IF)
