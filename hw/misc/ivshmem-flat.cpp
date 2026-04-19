/*
 * Inter-VM Shared Memory Flat Device
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (c) 2023 Linaro Ltd.
 * Authors:
 *   Gustavo Romero
 *
 */

#include "qemu/osdep.h"

extern "C" {
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "trace.h"
}

#include "hw/irq.h"
#include "hw/qdev-properties-system.h"
#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "system/address-spaces.h"

#include "hw/misc/ivshmem-flat.h"

static int64_t ivshmem_flat_recv_msg(IvshmemFTState *s, int *pfd)
{
    int64_t msg;
    int n, ret;

    n = 0;
    do {
        ret = qemu_chr_fe_read_all(&s->server_chr, (uint8_t *)&msg + n,
                                   sizeof(msg) - n);
        if (ret < 0) {
            if (ret == -EINTR) {
                continue;
            }
            exit(1);
        }
        n += ret;
    } while (n < sizeof(msg));

    if (pfd) {
        *pfd = qemu_chr_fe_get_msgfd(&s->server_chr);
    }
    return le64_to_cpu(msg);
}

static void ivshmem_flat_irq_handler(void *opaque)
{
    VectorInfo *vi = static_cast<VectorInfo *>(opaque);
    EventNotifier *e = &vi->event_notifier;
    uint16_t vector_id;
    const VectorInfo (*v)[64];

    assert(e->initialized);

    vector_id = vi->id;

    /*
     * The vector info struct is passed to the handler via the 'opaque' pointer.
     * This struct pointer allows the retrieval of the vector ID and its
     * associated event notifier. However, for triggering an interrupt using
     * qemu_set_irq, it's necessary to also have a pointer to the device state,
     * i.e., a pointer to the IvshmemFTState struct. Since the vector info
     * struct is contained within the IvshmemFTState struct, its pointer can be
     * used to obtain the pointer to IvshmemFTState through simple pointer math.
     */
    v = reinterpret_cast<const VectorInfo (*)[64]>(vi - vector_id); /* v =  &IvshmemPeer->vector[0] */
    IvshmemPeer *own_peer = container_of(v, IvshmemPeer, vector);
    IvshmemFTState *s = container_of(own_peer, IvshmemFTState, own);

    /* Clear event  */
    if (!event_notifier_test_and_clear(e)) {
        return;
    }

    trace_ivshmem_flat_irq_handler(vector_id);

    /*
     * Toggle device's output line, which is connected to interrupt controller,
     * generating an interrupt request to the CPU.
     */
    qemu_irq_pulse(s->irq);
}

static IvshmemPeer *ivshmem_flat_find_peer(IvshmemFTState *s, uint16_t peer_id)
{
    IvshmemPeer *peer;

    /* Own ID */
    if (s->own.id == peer_id) {
        return &s->own;
    }

    /* Peer ID */
    QTAILQ_FOREACH(peer, &s->peer, next) {
        if (peer->id == peer_id) {
            return peer;
        }
    }

    return NULL;
}

static IvshmemPeer *ivshmem_flat_add_peer(IvshmemFTState *s, uint16_t peer_id)
{
    IvshmemPeer *new_peer;

    new_peer = static_cast<IvshmemPeer *>(g_malloc0(sizeof(*new_peer)));
    new_peer->id = peer_id;
    new_peer->vector_counter = 0;

    QTAILQ_INSERT_TAIL(&s->peer, new_peer, next);

    trace_ivshmem_flat_new_peer(peer_id);

    return new_peer;
}

static void ivshmem_flat_remove_peer(IvshmemFTState *s, uint16_t peer_id)
{
    IvshmemPeer *peer;

    peer = ivshmem_flat_find_peer(s, peer_id);
    assert(peer);

    QTAILQ_REMOVE(&s->peer, peer, next);
    for (int n = 0; n < peer->vector_counter; n++) {
        int efd;
        efd = event_notifier_get_fd(&(peer->vector[n].event_notifier));
        close(efd);
    }

    g_free(peer);
}

static void ivshmem_flat_add_vector(IvshmemFTState *s, IvshmemPeer *peer,
                                    int vector_fd)
{
    Error *err = NULL;

    if (peer->vector_counter >= IVSHMEM_MAX_VECTOR_NUM) {
        trace_ivshmem_flat_add_vector_failure(peer->vector_counter,
                                              vector_fd, peer->id);
        close(vector_fd);

        return;
    }

    trace_ivshmem_flat_add_vector_success(peer->vector_counter,
                                          vector_fd, peer->id);

    /*
     * Set vector ID and its associated eventfd notifier and add them to the
     * peer.
     */
    peer->vector[peer->vector_counter].id = peer->vector_counter;
    if (!qemu_set_blocking(vector_fd, false, &err)) {
        /* FIXME handle the error */
        warn_report_err(err);
    }
    event_notifier_init_fd(&peer->vector[peer->vector_counter].event_notifier,
                           vector_fd);

    /*
     * If it's the device's own ID, register also the handler for the eventfd
     * so the device can be notified by the other peers.
     */
    if (peer == &s->own) {
        qemu_set_fd_handler(vector_fd, ivshmem_flat_irq_handler, NULL,
                            &peer->vector);
    }

    peer->vector_counter++;
}

static void ivshmem_flat_process_msg(IvshmemFTState *s, uint64_t msg, int fd)
{
    uint16_t peer_id;
    IvshmemPeer *peer;

    peer_id = msg & 0xFFFF;
    peer = ivshmem_flat_find_peer(s, peer_id);

    if (!peer) {
        peer = ivshmem_flat_add_peer(s, peer_id);
    }

    if (fd >= 0) {
        ivshmem_flat_add_vector(s, peer, fd);
    } else { /* fd == -1, which is received when peers disconnect. */
        ivshmem_flat_remove_peer(s, peer_id);
    }
}

static int ivshmem_flat_can_receive_data(void *opaque)
{
    IvshmemFTState *s = static_cast<IvshmemFTState *>(opaque);

    assert(s->msg_buffered_bytes < sizeof(s->msg_buf));
    return sizeof(s->msg_buf) - s->msg_buffered_bytes;
}

static void ivshmem_flat_read_msg(void *opaque, const uint8_t *buf, int size)
{
    IvshmemFTState *s = static_cast<IvshmemFTState *>(opaque);
    int fd;
    int64_t msg;

    assert(size >= 0 && s->msg_buffered_bytes + size <= sizeof(s->msg_buf));
    memcpy((unsigned char *)&s->msg_buf + s->msg_buffered_bytes, buf, size);
    s->msg_buffered_bytes += size;
    if (s->msg_buffered_bytes < sizeof(s->msg_buf)) {
        return;
    }
    msg = le64_to_cpu(s->msg_buf);
    s->msg_buffered_bytes = 0;

    fd = qemu_chr_fe_get_msgfd(&s->server_chr);

    ivshmem_flat_process_msg(s, msg, fd);
}

static uint64_t ivshmem_flat_iomem_read(void *opaque,
                                        hwaddr offset, unsigned size)
{
    IvshmemFTState *s = static_cast<IvshmemFTState *>(opaque);
    uint32_t ret;

    trace_ivshmem_flat_read_mmr(offset);

    switch (offset) {
    case INTMASK:
        ret = 0; /* Ignore read since all bits are reserved in rev 1. */
        break;
    case INTSTATUS:
        ret = 0; /* Ignore read since all bits are reserved in rev 1. */
        break;
    case IVPOSITION:
        ret = s->own.id;
        break;
    case DOORBELL:
        trace_ivshmem_flat_read_mmr_doorbell(); /* DOORBELL is write-only */
        ret = 0;
        break;
    default:
        /* Should never reach out here due to iomem map range being exact */
        trace_ivshmem_flat_read_write_mmr_invalid(offset);
        ret = 0;
    }

    return ret;
}

static int ivshmem_flat_interrupt_peer(IvshmemFTState *s,
                                       uint16_t peer_id, uint16_t vector_id)
{
    IvshmemPeer *peer;

    peer = ivshmem_flat_find_peer(s, peer_id);
    if (!peer) {
        trace_ivshmem_flat_interrupt_invalid_peer(peer_id);
        return 1;
    }

    event_notifier_set(&(peer->vector[vector_id].event_notifier));

    return 0;
}

static void ivshmem_flat_iomem_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    IvshmemFTState *s = static_cast<IvshmemFTState *>(opaque);
    uint16_t peer_id = (value >> 16) & 0xFFFF;
    uint16_t vector_id = value & 0xFFFF;

    trace_ivshmem_flat_write_mmr(offset);

    switch (offset) {
    case INTMASK:
        break;
    case INTSTATUS:
        break;
    case IVPOSITION:
        break;
    case DOORBELL:
        trace_ivshmem_flat_interrupt_peer(peer_id, vector_id);
        ivshmem_flat_interrupt_peer(s, peer_id, vector_id);
        break;
    default:
        /* Should never reach out here due to iomem map range being exact. */
        trace_ivshmem_flat_read_write_mmr_invalid(offset);
        break;
    }
}

static MemoryRegionOps ivshmem_flat_ops = {
    .read = ivshmem_flat_iomem_read,
    .write = ivshmem_flat_iomem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void __attribute__((constructor)) init_ivshmem_flat_ops(void)
{
    /* Read/write aligned at 32 bits. */
    ivshmem_flat_ops.impl.min_access_size = 4;
    ivshmem_flat_ops.impl.max_access_size = 4;
}

void IvshmemFTState::init()
{
    Object *obj = OBJECT(this);
    SysBusDevice *sbd = SYS_BUS_DEVICE(this);

    memory_region_init_io(&iomem, obj, &ivshmem_flat_ops, this,
                          "ivshmem-mmio", 0x10);
    sysbus_init_mmio(sbd, &iomem);

    sysbus_init_irq(sbd, &irq);

    QTAILQ_INIT(&peer);
}

static bool ivshmem_flat_connect_server(DeviceState *dev, Error **errp)
{
    IvshmemFTState *s = IVSHMEM_FLAT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int64_t protocol_version, msg;
    int shmem_fd;
    uint16_t peer_id;
    struct stat fdstat;

    /* Check ivshmem server connection. */
    if (!qemu_chr_fe_backend_connected(&s->server_chr)) {
        error_setg(errp, "ivshmem server socket not specified or incorret."
                         " Can't create device.");
        return false;
    }

    /*
     * Message sequence from server on new connection:
     *  _____________________________________
     * |STEP| uint64_t msg  | int fd         |
     *  -------------------------------------
     *
     *  0    PROTOCOL        -1              \
     *  1    OWN PEER ID     -1               |-- Header/Greeting
     *  2    -1              shmem fd        /
     *
     *  3    PEER IDx        Other peer's Vector 0 eventfd
     *  4    PEER IDx        Other peer's Vector 1 eventfd
     *  .                    .
     *  .                    .
     *  .                    .
     *  N    PEER IDy        Other peer's Vector 0 eventfd
     *  N+1  PEER IDy        Other peer's Vector 1 eventfd
     *  .                    .
     *  .                    .
     *  .                    .
     *
     *  ivshmem_flat_recv_msg() calls return 'msg' and 'fd'.
     *
     *  See docs/specs/ivshmem-spec.rst for details on the protocol.
     */

    /* Step 0 */
    protocol_version = ivshmem_flat_recv_msg(s, NULL);

    /* Step 1 */
    msg = ivshmem_flat_recv_msg(s, NULL);
    peer_id = 0xFFFF & msg;
    s->own.id = peer_id;
    s->own.vector_counter = 0;

    trace_ivshmem_flat_proto_ver_own_id(protocol_version, s->own.id);

    /* Step 2 */
    msg = ivshmem_flat_recv_msg(s, &shmem_fd);
    /* Map shmem fd and MMRs into memory regions. */
    if (msg != -1 || shmem_fd < 0) {
        error_setg(errp, "Could not receive valid shmem fd."
                         " Can't create device!");
        return false;
    }

    if (fstat(shmem_fd, &fdstat) != 0) {
        error_setg(errp, "Could not determine shmem fd size."
                         " Can't create device!");
        return false;
    }
    trace_ivshmem_flat_shmem_size(shmem_fd, fdstat.st_size);

    /*
     * Shmem size provided by the ivshmem server must be equal to
     * device's shmem size.
     */
    if (fdstat.st_size != s->shmem_size) {
        error_setg(errp, "Can't map shmem fd: shmem size different"
                         " from device size!");
        return false;
    }

    /*
     * Beyond step 2 ivshmem_process_msg, called by ivshmem_flat_read_msg
     * handler -- when data is available on the server socket -- will handle
     * the additional messages that will be generated by the server as peers
     * connect or disconnect.
     */
    qemu_chr_fe_set_handlers(&s->server_chr, ivshmem_flat_can_receive_data,
                             ivshmem_flat_read_msg, NULL, NULL, s, NULL, true);

    memory_region_init_ram_from_fd(&s->shmem, OBJECT(s),
                                   "ivshmem-shmem", s->shmem_size,
                                   RAM_SHARED, shmem_fd, 0, NULL);
    sysbus_init_mmio(sbd, &s->shmem);

    return true;
}

void IvshmemFTState::realize(Error **errp)
{
    if (!ivshmem_flat_connect_server(DEVICE(this), errp)) {
        return;
    }
}

static const Property ivshmem_flat_props[] = {
    DEFINE_PROP_CHR("chardev", IvshmemFTState, server_chr),
    DEFINE_PROP_UINT32("shmem-size", IvshmemFTState, shmem_size, 4 * MiB),
};

void IvshmemFTState::classInit(DeviceClass *dc)
{
    dc->hotpluggable = true;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, ivshmem_flat_props);

    dc->user_creatable = false;
}

#include "qom/cpp/object.h"
REGISTER_QEMU_DEVICE(IvshmemFTState, TYPE_IVSHMEM_FLAT, TYPE_SYS_BUS_DEVICE)
