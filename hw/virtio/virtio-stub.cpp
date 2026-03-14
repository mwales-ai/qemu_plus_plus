#include "qemu/osdep.h"

extern "C" {
#include "qapi/error.h"
#include "qapi/qapi-commands-virtio.h"
}

static void *qmp_virtio_unsupported(Error **errp)
{
    error_setg(errp, "Virtio is disabled");
    return NULL;
}

extern "C" VirtioInfoList *qmp_x_query_virtio(Error **errp)
{
    return static_cast<VirtioInfoList *>(qmp_virtio_unsupported(errp));
}

extern "C" VirtioStatus *qmp_x_query_virtio_status(const char *path, Error **errp)
{
    return static_cast<VirtioStatus *>(qmp_virtio_unsupported(errp));
}

extern "C" VirtVhostQueueStatus *qmp_x_query_virtio_vhost_queue_status(const char *path,
                                                            uint16_t queue,
                                                            Error **errp)
{
    return static_cast<VirtVhostQueueStatus *>(qmp_virtio_unsupported(errp));
}

extern "C" VirtQueueStatus *qmp_x_query_virtio_queue_status(const char *path,
                                                 uint16_t queue,
                                                 Error **errp)
{
    return static_cast<VirtQueueStatus *>(qmp_virtio_unsupported(errp));
}

extern "C" VirtioQueueElement *qmp_x_query_virtio_queue_element(const char *path,
                                                     uint16_t queue,
                                                     bool has_index,
                                                     uint16_t index,
                                                     Error **errp)
{
    return static_cast<VirtioQueueElement *>(qmp_virtio_unsupported(errp));
}
