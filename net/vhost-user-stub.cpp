/*
 * vhost-user-stub.c
 *
 * Copyright (c) 2018 Red Hat, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif

extern "C" {
#include "clients.h"
#include "net/vhost_net.h"
#include "qemu/error-report.h"
#include "qapi/error.h"

int net_init_vhost_user(const Netdev *netdev, const char *name,
                        NetClientState *peer, Error **errp)
{
    error_setg(errp, "vhost-user requires frontend driver virtio-net-*");
    return -1;
}

} /* extern "C" */
