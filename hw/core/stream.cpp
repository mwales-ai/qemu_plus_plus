#include "qemu/osdep.h"
#ifdef CONFIG_LINUX_IO_URING
#include <liburing.h>
#endif
#include "qom/cpp/object.h"

extern "C" {
#include "hw/stream.h"
#include "qemu/module.h"

size_t
stream_push(StreamSink *sink, uint8_t *buf, size_t len, bool eop)
{
    StreamSinkClass *k =  STREAM_SINK_GET_CLASS(sink);

    return k->push(sink, buf, len, eop);
}

bool
stream_can_push(StreamSink *sink, StreamCanPushNotifyFn notify,
                void *notify_opaque)
{
    StreamSinkClass *k =  STREAM_SINK_GET_CLASS(sink);

    return k->can_push ? k->can_push(sink, notify, notify_opaque) : true;
}

} /* extern "C" */

REGISTER_QEMU_INTERFACE(StreamSinkClass, TYPE_STREAM_SINK)
