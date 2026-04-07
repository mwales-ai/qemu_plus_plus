/*
 * Inter-VM Shared Memory PCI device.
 *
 * Author:
 *      Cam Macdonell <cam@cs.ualberta.ca>
 *
 * Based On: cirrus_vga.c
 *          Copyright (c) 2004 Fabrice Bellard
 *          Copyright (c) 2004 Makoto Suzuki (suzu)
 *
 *      and rtl8139.c
 *          Copyright (c) 2006 Igor Kovalenko
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "system/kvm.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/event_notifier.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "chardev/char-fe.h"
#include "system/hostmem.h"
#include "qapi/visitor.h"

#include "hw/misc/ivshmem.h"
#include "qom/object.h"

#define PCI_VENDOR_ID_IVSHMEM   PCI_VENDOR_ID_REDHAT_QUMRANET
#define PCI_DEVICE_ID_IVSHMEM   0x1110

#define IVSHMEM_MAX_PEERS UINT16_MAX
#define IVSHMEM_IOEVENTFD   0
#define IVSHMEM_MSI     1

#define IVSHMEM_REG_BAR_SIZE 0x100

#define IVSHMEM_DEBUG 0
#define IVSHMEM_DPRINTF(fmt, ...)                       \
    do {                                                \
        if (IVSHMEM_DEBUG) {                            \
            printf("IVSHMEM: " fmt, ## __VA_ARGS__);    \
        }                                               \
    } while (0)

#define TYPE_IVSHMEM_COMMON "ivshmem-common"
typedef struct IVShmemState IVShmemState;
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM_COMMON,
                         TYPE_IVSHMEM_COMMON)

#define TYPE_IVSHMEM_PLAIN "ivshmem-plain"
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM_PLAIN,
                         TYPE_IVSHMEM_PLAIN)

#define TYPE_IVSHMEM_DOORBELL "ivshmem-doorbell"
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM_DOORBELL,
                         TYPE_IVSHMEM_DOORBELL)

#define TYPE_IVSHMEM "ivshmem"
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM,
                         TYPE_IVSHMEM)

typedef struct Peer {
    int nb_eventfds;
    EventNotifier *eventfds;
} Peer;

typedef struct MSIVector {
    PCIDevice *pdev;
    int virq;
    bool unmasked;
} MSIVector;

/* registers for the Inter-VM shared memory device */
enum ivshmem_registers {
    INTRMASK = 0,
    INTRSTATUS = 4,
    IVPOSITION = 8,
    DOORBELL = 12,
};

struct IVShmemState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    uint32_t features;

    /* exactly one of these two may be set */
    HostMemoryBackend *hostmem; /* with interrupts */
    CharFrontend server_chr; /* without interrupts */

    /* registers */
    uint32_t intrmask;
    uint32_t intrstatus;
    int vm_id;

    /* BARs */
    MemoryRegion ivshmem_mmio;  /* BAR 0 (registers) */
    MemoryRegion *ivshmem_bar2; /* BAR 2 (shared memory) */
    MemoryRegion server_bar2;   /* used with server_chr */

    /* interrupt support */
    Peer *peers;
    int nb_peers;               /* space in @peers[] */
    uint32_t vectors;
    MSIVector *msi_vectors;
    uint64_t msg_buf;           /* buffer for receiving server messages */
    int msg_buffered_bytes;     /* #bytes in @msg_buf */

    /* migration stuff */
    OnOffAuto master;
    Error *migration_blocker;

    /* ----- instance methods ----- */
    bool hasFeature(unsigned int feature);
    bool isMaster();
    void realize(PCIDevice *dev, Error **errp);
    void exit(PCIDevice *dev);
    void reset(DeviceState *d);
    void plainRealize(PCIDevice *dev, Error **errp);
    void doorbellRealize(PCIDevice *dev, Error **errp);
    void doorbellInit();

    /* static class init methods */
    static void commonClassInit(ObjectClass *klass, const void *data);
    static void plainClassInit(ObjectClass *klass, const void *data);
    static void doorbellClassInit(ObjectClass *klass, const void *data);

    /* static MMIO callbacks */
    static uint64_t mmioRead(void *opaque, hwaddr addr, unsigned size);
    static void mmioWrite(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size);

    /* static config callback */
    static void writeConfig(PCIDevice *pdev, uint32_t address,
                            uint32_t val, int len);

    /* static VMState callbacks */
    static int preLoad(void *opaque);
    static int postLoad(void *opaque, int version_id);

    /* static chardev callbacks */
    static int canReceive(void *opaque);
    static void readHandler(void *opaque, const uint8_t *buf, int size);

    /* static vector callback */
    static void vectorNotify(void *opaque);

    /* static MSI-X vector callbacks (need C function signature) */
    static int vectorUnmask(PCIDevice *dev, unsigned vector, MSIMessage msg);
    static void vectorMask(PCIDevice *dev, unsigned vector);
    static void vectorPoll(PCIDevice *dev, unsigned int vector_start,
                           unsigned int vector_end);

private:
    void intrMaskWrite(uint32_t val);
    uint32_t intrMaskRead();
    void intrStatusWrite(uint32_t val);
    uint32_t intrStatusRead();
    void msixVectorUse();
    void watchVectorNotifier(EventNotifier *n, int vector);
    void addEventfd(int posn, int i);
    void delEventfd(int posn, int i);
    void closePeerEventfds(int posn);
    void resizePeers(int nb_peers);
    void addKvmMsiVirq(int vector, Error **errp);
    void setupInterrupt(int vector, Error **errp);
    void processMsgShmem(int fd, Error **errp);
    void processMsgDisconnect(uint16_t posn, Error **errp);
    void processMsgConnect(uint16_t posn, int fd, Error **errp);
    void processMsg(int64_t msg, int fd, Error **errp);
    int64_t recvMsg(int *pfd, Error **errp);
    void recvSetup(Error **errp);
    int setupInterrupts(Error **errp);
    void removeKvmMsiVirq(int vector);
    void enableIrqfd();
    void disableIrqfd();
};

bool IVShmemState::hasFeature(unsigned int feature)
{
    return (features & (1 << feature)) != 0;
}

bool IVShmemState::isMaster()
{
    assert(master != ON_OFF_AUTO_AUTO);
    return master == ON_OFF_AUTO_ON;
}

void IVShmemState::intrMaskWrite(uint32_t val)
{
    IVSHMEM_DPRINTF("IntrMask write(w) val = 0x%04x\n", val);
    intrmask = val;
}

uint32_t IVShmemState::intrMaskRead()
{
    uint32_t ret = intrmask;
    IVSHMEM_DPRINTF("intrmask read(w) val = 0x%04x\n", ret);
    return ret;
}

void IVShmemState::intrStatusWrite(uint32_t val)
{
    IVSHMEM_DPRINTF("IntrStatus write(w) val = 0x%04x\n", val);
    intrstatus = val;
}

uint32_t IVShmemState::intrStatusRead()
{
    uint32_t ret = intrstatus;
    /* reading ISR clears all interrupts */
    intrstatus = 0;
    return ret;
}

void IVShmemState::mmioWrite(void *opaque, hwaddr addr,
                              uint64_t val, unsigned size)
{
    IVShmemState *s = static_cast<IVShmemState *>(opaque);

    uint16_t dest = val >> 16;
    uint16_t vector = val & 0xff;

    addr &= 0xfc;

    IVSHMEM_DPRINTF("writing to addr " HWADDR_FMT_plx "\n", addr);
    switch (addr)
    {
        case INTRMASK:
            s->intrMaskWrite(val);
            break;

        case INTRSTATUS:
            s->intrStatusWrite(val);
            break;

        case DOORBELL:
            /* check that dest VM ID is reasonable */
            if (dest >= s->nb_peers) {
                IVSHMEM_DPRINTF("Invalid destination VM ID (%d)\n", dest);
                break;
            }

            /* check doorbell range */
            if (vector < s->peers[dest].nb_eventfds) {
                IVSHMEM_DPRINTF("Notifying VM %d on vector %d\n", dest, vector);
                event_notifier_set(&s->peers[dest].eventfds[vector]);
            } else {
                IVSHMEM_DPRINTF("Invalid destination vector %d on VM %d\n",
                                vector, dest);
            }
            break;
        default:
            IVSHMEM_DPRINTF("Unhandled write " HWADDR_FMT_plx "\n", addr);
    }
}

uint64_t IVShmemState::mmioRead(void *opaque, hwaddr addr,
                                 unsigned size)
{

    IVShmemState *s = static_cast<IVShmemState *>(opaque);
    uint32_t ret;

    switch (addr)
    {
        case INTRMASK:
            ret = s->intrMaskRead();
            break;

        case INTRSTATUS:
            ret = s->intrStatusRead();
            break;

        case IVPOSITION:
            ret = s->vm_id;
            break;

        default:
            IVSHMEM_DPRINTF("why are we reading " HWADDR_FMT_plx "\n", addr);
            ret = 0;
    }

    return ret;
}

static const MemoryRegionOps ivshmem_mmio_ops = {
    .read = IVShmemState::mmioRead,
    .write = IVShmemState::mmioWrite,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

void IVShmemState::vectorNotify(void *opaque)
{
    MSIVector *entry = static_cast<MSIVector *>(opaque);
    PCIDevice *pdev = entry->pdev;
    IVShmemState *s = reinterpret_cast<IVShmemState *>(pdev);
    int vector = entry - s->msi_vectors;
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];

    if (!event_notifier_test_and_clear(n)) {
        return;
    }

    IVSHMEM_DPRINTF("interrupt on vector %p %d\n", pdev, vector);
    if (s->hasFeature(IVSHMEM_MSI)) {
        if (msix_enabled(pdev)) {
            msix_notify(pdev, vector);
        }
    } else {
        s->intrStatusWrite(1);
    }
}

int IVShmemState::vectorUnmask(PCIDevice *dev, unsigned vector,
                                MSIMessage msg)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];
    MSIVector *v = &s->msi_vectors[vector];
    int ret;

    IVSHMEM_DPRINTF("vector unmask %p %d\n", dev, vector);
    if (!v->pdev) {
        error_report("ivshmem: vector %d route does not exist", vector);
        return -EINVAL;
    }
    assert(!v->unmasked);

    ret = kvm_irqchip_update_msi_route(kvm_state, v->virq, msg, dev);
    if (ret < 0) {
        return ret;
    }
    kvm_irqchip_commit_routes(kvm_state);

    ret = kvm_irqchip_add_irqfd_notifier_gsi(kvm_state, n, NULL, v->virq);
    if (ret < 0) {
        return ret;
    }
    v->unmasked = true;

    return 0;
}

void IVShmemState::vectorMask(PCIDevice *dev, unsigned vector)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];
    MSIVector *v = &s->msi_vectors[vector];
    int ret;

    IVSHMEM_DPRINTF("vector mask %p %d\n", dev, vector);
    if (!v->pdev) {
        error_report("ivshmem: vector %d route does not exist", vector);
        return;
    }
    assert(v->unmasked);

    ret = kvm_irqchip_remove_irqfd_notifier_gsi(kvm_state, n, v->virq);
    if (ret < 0) {
        error_report("remove_irqfd_notifier_gsi failed");
        return;
    }
    v->unmasked = false;
}

void IVShmemState::vectorPoll(PCIDevice *dev,
                               unsigned int vector_start,
                               unsigned int vector_end)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);
    unsigned int vector;

    IVSHMEM_DPRINTF("vector poll %p %d-%d\n", dev, vector_start, vector_end);

    vector_end = MIN(vector_end, s->vectors);

    for (vector = vector_start; vector < vector_end; vector++) {
        EventNotifier *notifier = &s->peers[s->vm_id].eventfds[vector];

        if (!msix_is_masked(dev, vector)) {
            continue;
        }

        if (event_notifier_test_and_clear(notifier)) {
            msix_set_pending(dev, vector);
        }
    }
}

void IVShmemState::watchVectorNotifier(EventNotifier *n, int vector)
{
    int eventfd = event_notifier_get_fd(n);

    assert(!msi_vectors[vector].pdev);
    msi_vectors[vector].pdev = reinterpret_cast<PCIDevice *>(this);

    qemu_set_fd_handler(eventfd, IVShmemState::vectorNotify,
                        NULL, &msi_vectors[vector]);
}

void IVShmemState::addEventfd(int posn, int i)
{
    memory_region_add_eventfd(&ivshmem_mmio,
                              DOORBELL,
                              4,
                              true,
                              (posn << 16) | i,
                              &peers[posn].eventfds[i]);
}

void IVShmemState::delEventfd(int posn, int i)
{
    memory_region_del_eventfd(&ivshmem_mmio,
                              DOORBELL,
                              4,
                              true,
                              (posn << 16) | i,
                              &peers[posn].eventfds[i]);
}

void IVShmemState::closePeerEventfds(int posn)
{
    int i, n;

    assert(posn >= 0 && posn < nb_peers);
    n = peers[posn].nb_eventfds;

    if (hasFeature(IVSHMEM_IOEVENTFD)) {
        memory_region_transaction_begin();
        for (i = 0; i < n; i++) {
            delEventfd(posn, i);
        }
        memory_region_transaction_commit();
    }

    for (i = 0; i < n; i++) {
        event_notifier_cleanup(&peers[posn].eventfds[i]);
    }

    g_free(peers[posn].eventfds);
    peers[posn].nb_eventfds = 0;
}

void IVShmemState::resizePeers(int new_nb_peers)
{
    int old_nb_peers = nb_peers;
    int i;

    assert(new_nb_peers > old_nb_peers);
    IVSHMEM_DPRINTF("bumping storage to %d peers\n", new_nb_peers);

    peers = g_renew(Peer, peers, new_nb_peers);
    nb_peers = new_nb_peers;

    for (i = old_nb_peers; i < new_nb_peers; i++) {
        peers[i].eventfds = g_new0(EventNotifier, vectors);
        peers[i].nb_eventfds = 0;
    }
}

void IVShmemState::addKvmMsiVirq(int vector, Error **errp)
{
    PCIDevice *pdev = reinterpret_cast<PCIDevice *>(this);
    KVMRouteChange c;
    int ret;

    IVSHMEM_DPRINTF("ivshmem_add_kvm_msi_virq vector:%d\n", vector);
    assert(!msi_vectors[vector].pdev);

    c = kvm_irqchip_begin_route_changes(kvm_state);
    ret = kvm_irqchip_add_msi_route(&c, vector, pdev);
    if (ret < 0) {
        error_setg(errp, "kvm_irqchip_add_msi_route failed");
        return;
    }
    kvm_irqchip_commit_route_changes(&c);

    msi_vectors[vector].virq = ret;
    msi_vectors[vector].pdev = pdev;
}

void IVShmemState::setupInterrupt(int vector, Error **errp)
{
    EventNotifier *n = &peers[vm_id].eventfds[vector];
    bool with_irqfd = kvm_msi_via_irqfd_enabled() &&
        hasFeature(IVSHMEM_MSI);
    PCIDevice *pdev = reinterpret_cast<PCIDevice *>(this);
    Error *err = NULL;

    IVSHMEM_DPRINTF("setting up interrupt for vector: %d\n", vector);

    if (!with_irqfd) {
        IVSHMEM_DPRINTF("with eventfd\n");
        watchVectorNotifier(n, vector);
    } else if (msix_enabled(pdev)) {
        IVSHMEM_DPRINTF("with irqfd\n");
        addKvmMsiVirq(vector, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }

        if (!msix_is_masked(pdev, vector)) {
            kvm_irqchip_add_irqfd_notifier_gsi(kvm_state, n, NULL,
                                               msi_vectors[vector].virq);
            /* TODO handle error */
        }
    } else {
        /* it will be delayed until msix is enabled, in write_config */
        IVSHMEM_DPRINTF("with irqfd, delayed until msix enabled\n");
    }
}

void IVShmemState::processMsgShmem(int fd, Error **errp)
{
    struct stat buf;
    size_t size;

    if (fd < 0) {
        error_setg(errp, "server didn't provide fd with shared memory message");
        return;
    }

    if (ivshmem_bar2) {
        error_setg(errp, "server sent unexpected shared memory message");
        close(fd);
        return;
    }

    if (fstat(fd, &buf) < 0) {
        error_setg_errno(errp, errno,
            "can't determine size of shared memory sent by server");
        close(fd);
        return;
    }

    size = buf.st_size;

    /* mmap the region and map into the BAR2 */
    if (!memory_region_init_ram_from_fd(&server_bar2, OBJECT(this),
                                        "ivshmem.bar2", size, RAM_SHARED,
                                        fd, 0, errp)) {
        return;
    }

    ivshmem_bar2 = &server_bar2;
}

void IVShmemState::processMsgDisconnect(uint16_t posn, Error **errp)
{
    IVSHMEM_DPRINTF("posn %d has gone away\n", posn);
    if (posn >= nb_peers || posn == vm_id) {
        error_setg(errp, "invalid peer %d", posn);
        return;
    }
    closePeerEventfds(posn);
}

void IVShmemState::processMsgConnect(uint16_t posn, int fd, Error **errp)
{
    Peer *peer = &peers[posn];
    int vector;

    /*
     * The N-th connect message for this peer comes with the file
     * descriptor for vector N-1.  Count messages to find the vector.
     */
    if (peer->nb_eventfds >= static_cast<int>(vectors)) {
        error_setg(errp, "Too many eventfd received, device has %d vectors",
                   vectors);
        close(fd);
        return;
    }
    vector = peer->nb_eventfds++;

    IVSHMEM_DPRINTF("eventfds[%d][%d] = %d\n", posn, vector, fd);
    event_notifier_init_fd(&peer->eventfds[vector], fd);

    /* msix/irqfd poll non block */
    if (!qemu_set_blocking(fd, false, errp)) {
        close(fd);
        return;
    }

    if (posn == vm_id) {
        setupInterrupt(vector, errp);
        /* TODO do we need to handle the error? */
    }

    if (hasFeature(IVSHMEM_IOEVENTFD)) {
        addEventfd(posn, vector);
    }
}

void IVShmemState::processMsg(int64_t msg, int fd, Error **errp)
{
    IVSHMEM_DPRINTF("posn is %" PRId64 ", fd is %d\n", msg, fd);

    if (msg < -1 || msg > IVSHMEM_MAX_PEERS) {
        error_setg(errp, "server sent invalid message %" PRId64, msg);
        if (fd >= 0) {
            close(fd);
        }
        return;
    }

    if (msg == -1) {
        processMsgShmem(fd, errp);
        return;
    }

    if (msg >= nb_peers) {
        resizePeers(msg + 1);
    }

    if (fd >= 0) {
        processMsgConnect(msg, fd, errp);
    } else {
        processMsgDisconnect(msg, errp);
    }
}

int IVShmemState::canReceive(void *opaque)
{
    IVShmemState *s = static_cast<IVShmemState *>(opaque);

    assert(s->msg_buffered_bytes < sizeof(s->msg_buf));
    return sizeof(s->msg_buf) - s->msg_buffered_bytes;
}

void IVShmemState::readHandler(void *opaque, const uint8_t *buf, int size)
{
    IVShmemState *s = static_cast<IVShmemState *>(opaque);
    Error *err = NULL;
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

    s->processMsg(msg, fd, &err);
    if (err) {
        error_report_err(err);
    }
}

int64_t IVShmemState::recvMsg(int *pfd, Error **errp)
{
    int64_t msg;
    int n, ret;

    n = 0;
    do {
        ret = qemu_chr_fe_read_all(&server_chr, (uint8_t *)&msg + n,
                                   sizeof(msg) - n);
        if (ret < 0) {
            if (ret == -EINTR) {
                continue;
            }
            error_setg_errno(errp, -ret, "read from server failed");
            return INT64_MIN;
        }
        n += ret;
    } while (n < sizeof(msg));

    *pfd = qemu_chr_fe_get_msgfd(&server_chr);
    return le64_to_cpu(msg);
}

void IVShmemState::recvSetup(Error **errp)
{
    Error *err = NULL;
    int64_t msg;
    int fd;

    msg = recvMsg(&fd, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    if (msg != IVSHMEM_PROTOCOL_VERSION) {
        error_setg(errp, "server sent version %" PRId64 ", expecting %d",
                   msg, IVSHMEM_PROTOCOL_VERSION);
        return;
    }
    if (fd != -1) {
        error_setg(errp, "server sent invalid version message");
        return;
    }

    /*
     * ivshmem-server sends the remaining initial messages in a fixed
     * order, but the device has always accepted them in any order.
     * Stay as compatible as practical, just in case people use
     * servers that behave differently.
     */

    /*
     * ivshmem_device_spec.txt has always required the ID message
     * right here, and ivshmem-server has always complied.  However,
     * older versions of the device accepted it out of order, but
     * broke when an interrupt setup message arrived before it.
     */
    msg = recvMsg(&fd, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    if (fd != -1 || msg < 0 || msg > IVSHMEM_MAX_PEERS) {
        error_setg(errp, "server sent invalid ID message");
        return;
    }
    vm_id = msg;

    /*
     * Receive more messages until we got shared memory.
     */
    do {
        msg = recvMsg(&fd, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
        processMsg(msg, fd, &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    } while (msg != -1);

    /*
     * This function must either map the shared memory or fail.  The
     * loop above ensures that: it terminates normally only after it
     * successfully processed the server's shared memory message.
     * Assert that actually mapped the shared memory:
     */
    assert(ivshmem_bar2);
}

/* Select the MSI-X vectors used by device.
 * ivshmem maps events to vectors statically, so
 * we just enable all vectors on init and after reset. */
void IVShmemState::msixVectorUse()
{
    PCIDevice *d = reinterpret_cast<PCIDevice *>(this);
    int i;

    for (i = 0; i < static_cast<int>(vectors); i++) {
        msix_vector_use(d, i);
    }
}

static void ivshmem_reset_wrapper(DeviceState *d)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(d);
    s->reset(d);
}

void IVShmemState::reset(DeviceState *d)
{
    disableIrqfd();

    intrstatus = 0;
    intrmask = 0;
    if (hasFeature(IVSHMEM_MSI)) {
        msixVectorUse();
    }
}

int IVShmemState::setupInterrupts(Error **errp)
{
    /* allocate QEMU callback data for receiving interrupts */
    msi_vectors = g_new0(MSIVector, vectors);

    if (hasFeature(IVSHMEM_MSI)) {
        if (msix_init_exclusive_bar(reinterpret_cast<PCIDevice *>(this), vectors, 1, errp)) {
            return -1;
        }

        IVSHMEM_DPRINTF("msix initialized (%d vectors)\n", vectors);
        msixVectorUse();
    }

    return 0;
}

void IVShmemState::removeKvmMsiVirq(int vector)
{
    IVSHMEM_DPRINTF("ivshmem_remove_kvm_msi_virq vector:%d\n", vector);

    if (msi_vectors[vector].pdev == NULL) {
        return;
    }

    /* it was cleaned when masked in the frontend. */
    kvm_irqchip_release_virq(kvm_state, msi_vectors[vector].virq);

    msi_vectors[vector].pdev = NULL;
}

void IVShmemState::enableIrqfd()
{
    PCIDevice *pdev = reinterpret_cast<PCIDevice *>(this);
    int i;

    for (i = 0; i < peers[vm_id].nb_eventfds; i++) {
        Error *err = NULL;

        addKvmMsiVirq(i, &err);
        if (err) {
            error_report_err(err);
            goto undo;
        }
    }

    if (msix_set_vector_notifiers(pdev,
                                  IVShmemState::vectorUnmask,
                                  IVShmemState::vectorMask,
                                  IVShmemState::vectorPoll)) {
        error_report("ivshmem: msix_set_vector_notifiers failed");
        goto undo;
    }
    return;

 undo:
    while (--i >= 0) {
        removeKvmMsiVirq(i);
    }
}

void IVShmemState::disableIrqfd()
{
    PCIDevice *pdev = reinterpret_cast<PCIDevice *>(this);
    int i;

    if (!pdev->msix_vector_use_notifier) {
        return;
    }

    msix_unset_vector_notifiers(pdev);

    for (i = 0; i < peers[vm_id].nb_eventfds; i++) {
        /*
         * MSI-X is already disabled here so msix_unset_vector_notifiers()
         * didn't call our release notifier.  Do it now to keep our masks and
         * unmasks balanced.
         */
        if (msi_vectors[i].unmasked) {
            IVShmemState::vectorMask(pdev, i);
        }
        removeKvmMsiVirq(i);
    }

}

void IVShmemState::writeConfig(PCIDevice *pdev, uint32_t address,
                                uint32_t val, int len)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(pdev);
    int is_enabled, was_enabled = msix_enabled(pdev);

    pci_default_write_config(pdev, address, val, len);
    is_enabled = msix_enabled(pdev);

    if (kvm_msi_via_irqfd_enabled()) {
        if (!was_enabled && is_enabled) {
            s->enableIrqfd();
        } else if (was_enabled && !is_enabled) {
            s->disableIrqfd();
        }
    }
}

static void ivshmem_common_realize_wrapper(PCIDevice *dev, Error **errp)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);
    s->realize(dev, errp);
}

void IVShmemState::realize(PCIDevice *dev, Error **errp)
{
    ERRP_GUARD();
    uint8_t *pci_conf;
    Error *err = NULL;

    /* IRQFD requires MSI */
    if (hasFeature(IVSHMEM_IOEVENTFD) &&
        !hasFeature(IVSHMEM_MSI)) {
        error_setg(errp, "ioeventfd/irqfd requires MSI");
        return;
    }

    pci_conf = dev->config;
    pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

    memory_region_init_io(&ivshmem_mmio, OBJECT(this), &ivshmem_mmio_ops, this,
                          "ivshmem-mmio", IVSHMEM_REG_BAR_SIZE);

    /* region for registers*/
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
                     &ivshmem_mmio);

    if (hostmem != NULL) {
        IVSHMEM_DPRINTF("using hostmem\n");

        ivshmem_bar2 = host_memory_backend_get_memory(hostmem);
        host_memory_backend_set_mapped(hostmem, true);
    } else {
        Chardev *chr = qemu_chr_fe_get_driver(&server_chr);
        assert(chr);

        IVSHMEM_DPRINTF("using shared memory server (socket = %s)\n",
                        chr->filename);

        /* we allocate enough space for 16 peers and grow as needed */
        resizePeers(16);

        /*
         * Receive setup messages from server synchronously.
         * Older versions did it asynchronously, but that creates a
         * number of entertaining race conditions.
         */
        recvSetup(&err);
        if (err) {
            error_propagate(errp, err);
            return;
        }

        if (master == ON_OFF_AUTO_ON && vm_id != 0) {
            error_setg(errp,
                       "master must connect to the server before any peers");
            return;
        }

        qemu_chr_fe_set_handlers(&server_chr, IVShmemState::canReceive,
                                 IVShmemState::readHandler, NULL, NULL,
                                 this, NULL, true);

        if (setupInterrupts(errp) < 0) {
            error_prepend(errp, "Failed to initialize interrupts: ");
            return;
        }
    }

    if (master == ON_OFF_AUTO_AUTO) {
        master = vm_id == 0 ? ON_OFF_AUTO_ON : ON_OFF_AUTO_OFF;
    }

    if (!isMaster()) {
        error_setg(&migration_blocker,
                   "Migration is disabled when using feature 'peer mode' in device 'ivshmem'");
        if (migrate_add_blocker(&migration_blocker, errp) < 0) {
            return;
        }
    }

    vmstate_register_ram(ivshmem_bar2, reinterpret_cast<DeviceState *>(this));
    pci_register_bar(reinterpret_cast<PCIDevice *>(this), 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     ivshmem_bar2);
}

static void ivshmem_exit_wrapper(PCIDevice *dev)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);
    s->exit(dev);
}

void IVShmemState::exit(PCIDevice *dev)
{
    int i;

    migrate_del_blocker(&migration_blocker);

    if (memory_region_is_mapped(ivshmem_bar2)) {
        if (!hostmem) {
            void *addr = memory_region_get_ram_ptr(ivshmem_bar2);
            int fd;

            if (munmap(addr, memory_region_size(ivshmem_bar2) == -1)) {
                error_report("Failed to munmap shared memory %s",
                             strerror(errno));
            }

            fd = memory_region_get_fd(ivshmem_bar2);
            close(fd);
        }

        vmstate_unregister_ram(ivshmem_bar2, reinterpret_cast<DeviceState *>(dev));
    }

    if (hostmem) {
        host_memory_backend_set_mapped(hostmem, false);
    }

    if (peers) {
        for (i = 0; i < nb_peers; i++) {
            closePeerEventfds(i);
        }
        g_free(peers);
    }

    if (hasFeature(IVSHMEM_MSI)) {
        msix_uninit_exclusive_bar(dev);
    }

    g_free(msi_vectors);
}

int IVShmemState::preLoad(void *opaque)
{
    IVShmemState *s = static_cast<IVShmemState *>(opaque);

    if (!s->isMaster()) {
        error_report("'peer' devices are not migratable");
        return -EINVAL;
    }

    return 0;
}

int IVShmemState::postLoad(void *opaque, int version_id)
{
    IVShmemState *s = static_cast<IVShmemState *>(opaque);

    if (s->hasFeature(IVSHMEM_MSI)) {
        s->msixVectorUse();
    }
    return 0;
}

void IVShmemState::commonClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ivshmem_common_realize_wrapper;
    k->exit = ivshmem_exit_wrapper;
    k->config_write = IVShmemState::writeConfig;
    k->vendor_id = PCI_VENDOR_ID_IVSHMEM;
    k->device_id = PCI_DEVICE_ID_IVSHMEM;
    k->class_id = PCI_CLASS_MEMORY_RAM;
    k->revision = 1;
    device_class_set_legacy_reset(dc, ivshmem_reset_wrapper);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "Inter-VM shared memory";
}

static const InterfaceInfo ivshmem_common_interfaces[] = {
    { INTERFACE_CONVENTIONAL_PCI_DEVICE },
    { },
};

static const TypeInfo ivshmem_common_info = {
    .name          = TYPE_IVSHMEM_COMMON,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IVShmemState),
    .is_abstract      = true,
    .class_init    = IVShmemState::commonClassInit,
    .interfaces = ivshmem_common_interfaces,
};

static const VMStateField ivshmem_plain_vmsd_fields[] = {
    VMSTATE_PCI_DEVICE(parent_obj, IVShmemState),
    VMSTATE_UINT32(intrstatus, IVShmemState),
    VMSTATE_UINT32(intrmask, IVShmemState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription ivshmem_plain_vmsd = {
    .name = TYPE_IVSHMEM_PLAIN,
    .version_id = 0,
    .minimum_version_id = 0,
    .pre_load = IVShmemState::preLoad,
    .post_load = IVShmemState::postLoad,
    .fields = ivshmem_plain_vmsd_fields,
};

static const Property ivshmem_plain_properties[] = {
    DEFINE_PROP_ON_OFF_AUTO("master", IVShmemState, master, ON_OFF_AUTO_OFF),
    DEFINE_PROP_LINK("memdev", IVShmemState, hostmem, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *),
};

void IVShmemState::plainRealize(PCIDevice *dev, Error **errp)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);

    if (!s->hostmem) {
        error_setg(errp, "You must specify a 'memdev'");
        return;
    } else if (host_memory_backend_is_mapped(s->hostmem)) {
        error_setg(errp, "can't use already busy memdev: %s",
                   object_get_canonical_path_component(OBJECT(s->hostmem)));
        return;
    }

    ivshmem_common_realize_wrapper(dev, errp);
}

static void ivshmem_plain_realize(PCIDevice *dev, Error **errp)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);
    s->plainRealize(dev, errp);
}

void IVShmemState::plainClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ivshmem_plain_realize;
    device_class_set_props(dc, ivshmem_plain_properties);
    dc->vmsd = &ivshmem_plain_vmsd;
}

static const TypeInfo ivshmem_plain_info = {
    .name          = TYPE_IVSHMEM_PLAIN,
    .parent        = TYPE_IVSHMEM_COMMON,
    .instance_size = sizeof(IVShmemState),
    .class_init    = IVShmemState::plainClassInit,
};

static const VMStateField ivshmem_doorbell_vmsd_fields[] = {
    VMSTATE_PCI_DEVICE(parent_obj, IVShmemState),
    VMSTATE_MSIX(parent_obj, IVShmemState),
    VMSTATE_UINT32(intrstatus, IVShmemState),
    VMSTATE_UINT32(intrmask, IVShmemState),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription ivshmem_doorbell_vmsd = {
    .name = TYPE_IVSHMEM_DOORBELL,
    .version_id = 0,
    .minimum_version_id = 0,
    .pre_load = IVShmemState::preLoad,
    .post_load = IVShmemState::postLoad,
    .fields = ivshmem_doorbell_vmsd_fields,
};

static const Property ivshmem_doorbell_properties[] = {
    DEFINE_PROP_CHR("chardev", IVShmemState, server_chr),
    DEFINE_PROP_UINT32("vectors", IVShmemState, vectors, 1),
    DEFINE_PROP_BIT("ioeventfd", IVShmemState, features, IVSHMEM_IOEVENTFD,
                    true),
    DEFINE_PROP_ON_OFF_AUTO("master", IVShmemState, master, ON_OFF_AUTO_OFF),
};

void IVShmemState::doorbellInit()
{
    features |= (1 << IVSHMEM_MSI);
}

static void ivshmem_doorbell_init(Object *obj)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(obj);
    s->doorbellInit();
}

void IVShmemState::doorbellRealize(PCIDevice *dev, Error **errp)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);

    if (!qemu_chr_fe_backend_connected(&s->server_chr)) {
        error_setg(errp, "You must specify a 'chardev'");
        return;
    }

    ivshmem_common_realize_wrapper(dev, errp);
}

static void ivshmem_doorbell_realize(PCIDevice *dev, Error **errp)
{
    IVShmemState *s = reinterpret_cast<IVShmemState *>(dev);
    s->doorbellRealize(dev, errp);
}

void IVShmemState::doorbellClassInit(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ivshmem_doorbell_realize;
    device_class_set_props(dc, ivshmem_doorbell_properties);
    dc->vmsd = &ivshmem_doorbell_vmsd;
}

static const TypeInfo ivshmem_doorbell_info = {
    .name          = TYPE_IVSHMEM_DOORBELL,
    .parent        = TYPE_IVSHMEM_COMMON,
    .instance_size = sizeof(IVShmemState),
    .instance_init = ivshmem_doorbell_init,
    .class_init    = IVShmemState::doorbellClassInit,
};

static void ivshmem_register_types(void)
{
    type_register_static(&ivshmem_common_info);
    type_register_static(&ivshmem_plain_info);
    type_register_static(&ivshmem_doorbell_info);
}

type_init(ivshmem_register_types)
