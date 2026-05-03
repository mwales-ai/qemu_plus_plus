/*
 * filter-replay.c
 *
 * Copyright (c) 2010-2016 Institute for System Programming
 *                         of the Russian Academy of Sciences.
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
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"
#include "net/filter.h"
#include "system/replay.h"
#include "qom/object.h"

#define TYPE_FILTER_REPLAY "filter-replay"

OBJECT_DECLARE_SIMPLE_TYPE(NetFilterReplayState, FILTER_REPLAY)

struct NetFilterReplayState {
    NetFilterState nfs;
    ReplayNetState *rns;

    void init();
    void finalize();
};

static ssize_t filter_replay_receive_iov(NetFilterState *nf,
                                         NetClientState *sndr,
                                         unsigned flags,
                                         const struct iovec *iov,
                                         int iovcnt, NetPacketSent *sent_cb)
{
    NetFilterReplayState *nfrs = FILTER_REPLAY(nf);
    switch (replay_mode) {
    case REPLAY_MODE_RECORD:
        if (nf->netdev == sndr) {
            replay_net_packet_event(nfrs->rns, flags, iov, iovcnt);
            return iov_size(iov, iovcnt);
        }
        return 0;
    case REPLAY_MODE_PLAY:
        /* Drop all packets in replay mode.
           Packets from the log will be injected by the replay module. */
        return iov_size(iov, iovcnt);
    default:
        /* Pass all the packets. */
        return 0;
    }
}

void NetFilterReplayState::init()
{
    rns = replay_register_net(&nfs);
}

void NetFilterReplayState::finalize()
{
    replay_unregister_net(rns);
}

static void filter_replay_class_init(ObjectClass *oc, const void *data)
{
    NetFilterClass *nfc = NETFILTER_CLASS(oc);

    nfc->receive_iov = filter_replay_receive_iov;
}

} /* extern "C" */

#include "qom/cpp/object.h"

REGISTER_QEMU_OBJECT_CI(NetFilterReplayState, TYPE_FILTER_REPLAY,
                         TYPE_NETFILTER, filter_replay_class_init)
