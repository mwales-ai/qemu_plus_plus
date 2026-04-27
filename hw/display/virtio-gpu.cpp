/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#include "qemu/units.h"
#include "qemu/iov.h"
#include "system/cpus.h"
#include "ui/console.h"
#include "ui/rect.h"
#include "trace.h"
#include "system/dma.h"
#include "system/system.h"
#include "hw/virtio/virtio.h"
#include "migration/qemu-file-types.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/memfd.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#define VIRTIO_GPU_VM_VERSION 1

/* Forward declarations */
static void virtio_gpu_reset_bh(void *opaque);
static void virtio_gpu_ctrl_bh(void *opaque);
static void virtio_gpu_cursor_bh(void *opaque);
static void virtio_gpu_handle_ctrl_cb(VirtIODevice *vdev, VirtQueue *vq);
static void virtio_gpu_handle_cursor_cb(VirtIODevice *vdev, VirtQueue *vq);

static uint32_t calc_image_hostmem(pixman_format_code_t pformat,
                                   uint32_t width, uint32_t height)
{
    /* Copied from pixman/pixman-bits-image.c, skip integer overflow check.
     * pixman_image_create_bits will fail in case it overflow.
     */

    int bpp = PIXMAN_FORMAT_BPP(pformat);
    int stride = ((width * bpp + 0x1f) >> 5) * sizeof(uint32_t);
    return height * stride;
}

static void virtio_unref_resource(pixman_image_t *image, void *data)
{
    pixman_image_unref(static_cast<pixman_image_t *>(data));
}

/*
 * VirtIOGPU methods
 */

void VirtIOGPU::updateCursorData(struct virtio_gpu_scanout *s,
                                 uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;
    uint32_t pixels;
    void *data;

    res = findCheckResource(resource_id, false, __func__, NULL);
    if (!res) {
        return;
    }

    if (res->blob_size) {
        if (res->blob_size < (s->current_cursor->width *
                              s->current_cursor->height * 4)) {
            return;
        }
        data = static_cast<uint8_t *>(res->blob);
    } else {
        if (pixman_image_get_width(res->image)  != s->current_cursor->width ||
            pixman_image_get_height(res->image) != s->current_cursor->height) {
            return;
        }
        data = pixman_image_get_data(res->image);
    }

    pixels = s->current_cursor->width * s->current_cursor->height;
    memcpy(s->current_cursor->data, data,
           pixels * sizeof(uint32_t));
}

void VirtIOGPU::updateCursor(struct virtio_gpu_update_cursor *cursor)
{
    struct virtio_gpu_scanout *s;
    VirtIOGPUClass *vgc = VIRTIO_GPU_GET_CLASS(this);
    bool move = cursor->hdr.type == VIRTIO_GPU_CMD_MOVE_CURSOR;

    if (cursor->pos.scanout_id >= parent_obj.conf.max_outputs) {
        return;
    }
    s = &parent_obj.scanout[cursor->pos.scanout_id];

    trace_virtio_gpu_update_cursor(cursor->pos.scanout_id,
                                   cursor->pos.x,
                                   cursor->pos.y,
                                   move ? "move" : "update",
                                   cursor->resource_id);

    if (!move) {
        if (!s->current_cursor) {
            s->current_cursor = cursor_alloc(64, 64);
        }

        s->current_cursor->hot_x = cursor->hot_x;
        s->current_cursor->hot_y = cursor->hot_y;

        if (cursor->resource_id > 0) {
            vgc->update_cursor_data(this, s, cursor->resource_id);
        }
        dpy_cursor_define(s->con, s->current_cursor);

        s->cursor = *cursor;
    } else {
        s->cursor.pos.x = cursor->pos.x;
        s->cursor.pos.y = cursor->pos.y;
    }
    dpy_mouse_set(s->con, cursor->pos.x, cursor->pos.y, cursor->resource_id);
}

struct virtio_gpu_simple_resource *
VirtIOGPU::findResource(uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;

    QTAILQ_FOREACH(res, &reslist, next) {
        if (res->resource_id == resource_id) {
            return res;
        }
    }
    return NULL;
}

struct virtio_gpu_simple_resource *
VirtIOGPU::findCheckResource(uint32_t resource_id,
                             bool require_backing,
                             const char *caller, uint32_t *error)
{
    struct virtio_gpu_simple_resource *res;

    res = findResource(resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid resource specified %d\n",
                      caller, resource_id);
        if (error) {
            *error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        }
        return NULL;
    }

    if (require_backing) {
        if (!res->iov || (!res->image && !res->blob)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: no backing storage %d\n",
                          caller, resource_id);
            if (error) {
                *error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            }
            return NULL;
        }
    }

    return res;
}

void VirtIOGPU::ctrlResponse(struct virtio_gpu_ctrl_command *cmd,
                             struct virtio_gpu_ctrl_hdr *resp,
                             size_t resp_len)
{
    size_t s;

    if (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
        resp->flags |= VIRTIO_GPU_FLAG_FENCE;
        resp->fence_id = cmd->cmd_hdr.fence_id;
        resp->ctx_id = cmd->cmd_hdr.ctx_id;
    }
    virtio_gpu_ctrl_hdr_bswap(resp);
    s = iov_from_buf(cmd->elem.in_sg, cmd->elem.in_num, 0, resp, resp_len);
    if (s != resp_len) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: response size incorrect %zu vs %zu\n",
                      __func__, s, resp_len);
    }
    virtqueue_push(cmd->vq, &cmd->elem, s);
    virtio_notify(reinterpret_cast<VirtIODevice *>(this), cmd->vq);
    cmd->finished = true;
}

void VirtIOGPU::ctrlResponseNodata(struct virtio_gpu_ctrl_command *cmd,
                                   enum virtio_gpu_ctrl_type type)
{
    struct virtio_gpu_ctrl_hdr resp;

    memset(&resp, 0, sizeof(resp));
    resp.type = type;
    ctrlResponse(cmd, &resp, sizeof(resp));
}

void VirtIOGPU::getDisplayInfo(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resp_display_info display_info;

    trace_virtio_gpu_cmd_get_display_info();
    memset(&display_info, 0, sizeof(display_info));
    display_info.hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
    virtio_gpu_base_fill_display_info(reinterpret_cast<VirtIOGPUBase *>(this), &display_info);
    ctrlResponse(cmd, &display_info.hdr, sizeof(display_info));
}

void VirtIOGPU::getEdid(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resp_edid edid;
    struct virtio_gpu_cmd_get_edid get_edid;
    VirtIOGPUBase *b = reinterpret_cast<VirtIOGPUBase *>(this);

    VIRTIO_GPU_FILL_CMD(get_edid);
    virtio_gpu_bswap_32(&get_edid, sizeof(get_edid));

    if (get_edid.scanout >= b->conf.max_outputs) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    trace_virtio_gpu_cmd_get_edid(get_edid.scanout);
    memset(&edid, 0, sizeof(edid));
    edid.hdr.type = VIRTIO_GPU_RESP_OK_EDID;
    virtio_gpu_base_generate_edid(reinterpret_cast<VirtIOGPUBase *>(this), get_edid.scanout, &edid);
    ctrlResponse(cmd, &edid.hdr, sizeof(edid));
}

void VirtIOGPU::resourceCreate2d(struct virtio_gpu_ctrl_command *cmd)
{
    Error *err = NULL;
    pixman_format_code_t pformat;
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_2d c2d;

    VIRTIO_GPU_FILL_CMD(c2d);
    virtio_gpu_bswap_32(&c2d, sizeof(c2d));
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    if (c2d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = findResource(c2d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c2d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_simple_resource, 1);

    res->width = c2d.width;
    res->height = c2d.height;
    res->format = c2d.format;
    res->resource_id = c2d.resource_id;

    pformat = virtio_gpu_get_pixman_format(c2d.format);
    if (!pformat) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: host couldn't handle guest format %d\n",
                      __func__, c2d.format);
        g_free(res);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    res->hostmem = calc_image_hostmem(pformat, c2d.width, c2d.height);
    if (res->hostmem + hostmem < conf_max_hostmem) {
        if (!qemu_pixman_image_new_shareable(
                &res->image,
                &res->share_handle,
                "virtio-gpu res",
                pformat,
                c2d.width,
                c2d.height,
                c2d.height ? res->hostmem / c2d.height : 0,
                &err)) {
            warn_report_err(err);
            goto end;
        }
    }

end:
    if (!res->image) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: resource creation failed %d %d %d\n",
                      __func__, c2d.resource_id, c2d.width, c2d.height);
        g_free(res);
        cmd->error = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
        return;
    }

    QTAILQ_INSERT_HEAD(&reslist, res, next);
    hostmem += res->hostmem;
}

void VirtIOGPU::resourceCreateBlob(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_create_blob cblob;
    int ret;

    VIRTIO_GPU_FILL_CMD(cblob);
    virtio_gpu_create_blob_bswap(&cblob);
    trace_virtio_gpu_cmd_res_create_blob(cblob.resource_id, cblob.size);

    if (cblob.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST &&
        cblob.blob_flags != VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid memory type\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    if (findResource(cblob.resource_id)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, cblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_simple_resource, 1);
    res->resource_id = cblob.resource_id;
    res->blob_size = cblob.size;

    ret = createMappingIov(cblob.nr_entries, sizeof(cblob),
                           cmd, &res->addrs, &res->iov,
                           &res->iov_cnt);
    if (ret != 0) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        g_free(res);
        return;
    }

    virtio_gpu_init_udmabuf(res);
    QTAILQ_INSERT_HEAD(&reslist, res, next);
}

void VirtIOGPU::disableScanout(int scanout_id)
{
    struct virtio_gpu_scanout *scanout = &parent_obj.scanout[scanout_id];
    struct virtio_gpu_simple_resource *res;

    if (scanout->resource_id == 0) {
        return;
    }

    res = findResource(scanout->resource_id);
    if (res) {
        res->scanout_bitmask &= ~(1 << scanout_id);
    }

    dpy_gfx_replace_surface(scanout->con, NULL);
    scanout->resource_id = 0;
    scanout->ds = NULL;
    scanout->width = 0;
    scanout->height = 0;
}

void VirtIOGPU::resourceDestroy(struct virtio_gpu_simple_resource *res,
                                Error **errp)
{
    int i;

    if (res->scanout_bitmask) {
        for (i = 0; i < parent_obj.conf.max_outputs; i++) {
            if (res->scanout_bitmask & (1 << i)) {
                disableScanout(i);
            }
        }
    }

    qemu_pixman_image_unref(res->image);
    cleanupMapping(res);
    QTAILQ_REMOVE(&reslist, res, next);
    hostmem -= res->hostmem;
    g_free(res);
}

void VirtIOGPU::resourceUnref(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_unref unref;

    VIRTIO_GPU_FILL_CMD(unref);
    virtio_gpu_bswap_32(&unref, sizeof(unref));
    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    res = findResource(unref.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, unref.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    /*
     * resourceDestroy does not set any errors, so pass a NULL errp
     * to ignore them.
     */
    resourceDestroy(res, NULL);
}

void VirtIOGPU::transferToHost2d(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    int h, bpp;
    uint32_t src_offset, dst_offset, stride;
    pixman_format_code_t format;
    struct virtio_gpu_transfer_to_host_2d t2d;
    void *img_data;

    VIRTIO_GPU_FILL_CMD(t2d);
    virtio_gpu_t2d_bswap(&t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

    res = findCheckResource(t2d.resource_id, true,
                            __func__, &cmd->error);
    if (!res || res->blob) {
        return;
    }

    if (t2d.r.x > res->width ||
        t2d.r.y > res->height ||
        t2d.r.width > res->width ||
        t2d.r.height > res->height ||
        t2d.r.x + t2d.r.width > res->width ||
        t2d.r.y + t2d.r.height > res->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: transfer bounds outside resource"
                      " bounds for resource %d: %d %d %d %d vs %d %d\n",
                      __func__, t2d.resource_id, t2d.r.x, t2d.r.y,
                      t2d.r.width, t2d.r.height, res->width, res->height);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    format = pixman_image_get_format(res->image);
    bpp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(format), 8);
    stride = pixman_image_get_stride(res->image);
    img_data = pixman_image_get_data(res->image);

    if (t2d.r.x || t2d.r.width != pixman_image_get_width(res->image)) {
        for (h = 0; h < t2d.r.height; h++) {
            src_offset = t2d.offset + stride * h;
            dst_offset = (t2d.r.y + h) * stride + (t2d.r.x * bpp);

            iov_to_buf(res->iov, res->iov_cnt, src_offset,
                       (uint8_t *)img_data + dst_offset,
                       t2d.r.width * bpp);
        }
    } else {
        src_offset = t2d.offset;
        dst_offset = t2d.r.y * stride + t2d.r.x * bpp;
        iov_to_buf(res->iov, res->iov_cnt, src_offset,
                   (uint8_t *)img_data + dst_offset,
                   stride * t2d.r.height);
    }
}

void VirtIOGPU::resourceFlush(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_flush rf;
    struct virtio_gpu_scanout *scanout;
    QemuRect flush_rect;
    bool within_bounds = false;
    bool update_submitted = false;
    int i;

    VIRTIO_GPU_FILL_CMD(rf);
    virtio_gpu_bswap_32(&rf, sizeof(rf));
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    res = findCheckResource(rf.resource_id, false,
                            __func__, &cmd->error);
    if (!res) {
        return;
    }

    if (res->blob) {
        for (i = 0; i < parent_obj.conf.max_outputs; i++) {
            scanout = &parent_obj.scanout[i];
            if (scanout->resource_id == res->resource_id &&
                rf.r.x < scanout->x + scanout->width &&
                rf.r.x + rf.r.width >= scanout->x &&
                rf.r.y < scanout->y + scanout->height &&
                rf.r.y + rf.r.height >= scanout->y) {
                within_bounds = true;

                if (console_has_gl(scanout->con)) {
                    dpy_gl_update(scanout->con, 0, 0, scanout->width,
                                  scanout->height);
                    update_submitted = true;
                }
            }
        }

        if (update_submitted) {
            return;
        }
        if (!within_bounds) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: flush bounds outside scanouts"
                          " bounds for flush %d: %d %d %d %d\n",
                          __func__, rf.resource_id, rf.r.x, rf.r.y,
                          rf.r.width, rf.r.height);
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            return;
        }
    }

    if (!res->blob &&
        (rf.r.x > res->width ||
        rf.r.y > res->height ||
        rf.r.width > res->width ||
        rf.r.height > res->height ||
        rf.r.x + rf.r.width > res->width ||
        rf.r.y + rf.r.height > res->height)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: flush bounds outside resource"
                      " bounds for resource %d: %d %d %d %d vs %d %d\n",
                      __func__, rf.resource_id, rf.r.x, rf.r.y,
                      rf.r.width, rf.r.height, res->width, res->height);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    qemu_rect_init(&flush_rect, rf.r.x, rf.r.y, rf.r.width, rf.r.height);
    for (i = 0; i < parent_obj.conf.max_outputs; i++) {
        QemuRect rect;

        if (!(res->scanout_bitmask & (1 << i))) {
            continue;
        }
        scanout = &parent_obj.scanout[i];

        qemu_rect_init(&rect, scanout->x, scanout->y,
                       scanout->width, scanout->height);

        /* work out the area we need to update for each console */
        if (qemu_rect_intersect(&flush_rect, &rect, &rect)) {
            qemu_rect_translate(&rect, -scanout->x, -scanout->y);
            dpy_gfx_update(parent_obj.scanout[i].con,
                           rect.x, rect.y, rect.width, rect.height);
        }
    }
}

void VirtIOGPU::updateScanout(uint32_t scanout_id,
                              struct virtio_gpu_simple_resource *res,
                              struct virtio_gpu_framebuffer *fb,
                              struct virtio_gpu_rect *r)
{
    struct virtio_gpu_simple_resource *ores;
    struct virtio_gpu_scanout *scanout;

    scanout = &parent_obj.scanout[scanout_id];
    ores = findResource(scanout->resource_id);
    if (ores) {
        ores->scanout_bitmask &= ~(1 << scanout_id);
    }

    res->scanout_bitmask |= (1 << scanout_id);
    scanout->resource_id = res->resource_id;
    scanout->x = r->x;
    scanout->y = r->y;
    scanout->width = r->width;
    scanout->height = r->height;
    scanout->fb = *fb;
}

bool VirtIOGPU::doSetScanout(uint32_t scanout_id,
                             struct virtio_gpu_framebuffer *fb,
                             struct virtio_gpu_simple_resource *res,
                             struct virtio_gpu_rect *r,
                             uint32_t *error)
{
    struct virtio_gpu_scanout *scanout;
    uint8_t *data;

    scanout = &parent_obj.scanout[scanout_id];

    if (r->x > fb->width ||
        r->y > fb->height ||
        r->width < 16 ||
        r->height < 16 ||
        r->width > fb->width ||
        r->height > fb->height ||
        r->x + r->width > fb->width ||
        r->y + r->height > fb->height) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout %d bounds for"
                      " resource %d, rect (%d,%d)+%d,%d, fb %d %d\n",
                      __func__, scanout_id, res->resource_id,
                      r->x, r->y, r->width, r->height,
                      fb->width, fb->height);
        *error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return false;
    }

    parent_obj.enable = 1;

    if (res->blob) {
        if (console_has_gl(scanout->con)) {
            if (!virtio_gpu_update_dmabuf(this, scanout_id, res, fb, r)) {
                updateScanout(scanout_id, res, fb, r);
            } else {
                *error = VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
                return false;
            }
            return true;
        }

        data = static_cast<uint8_t *>(res->blob);
    } else {
        data = (uint8_t *)pixman_image_get_data(res->image);
    }

    /* create a surface for this scanout */
    if ((res->blob && !console_has_gl(scanout->con)) ||
        !scanout->ds ||
        surface_data(scanout->ds) != data + fb->offset ||
        scanout->width != r->width ||
        scanout->height != r->height) {
        pixman_image_t *rect;
        void *ptr = data + fb->offset;
        rect = pixman_image_create_bits(static_cast<pixman_format_code_t>(fb->format), r->width, r->height,
                                        static_cast<uint32_t *>(ptr), fb->stride);

        if (res->image) {
            pixman_image_ref(res->image);
            pixman_image_set_destroy_function(rect, virtio_unref_resource,
                                              res->image);
        }

        /* realloc the surface ptr */
        scanout->ds = qemu_create_displaysurface_pixman(rect);
        qemu_displaysurface_set_share_handle(scanout->ds, res->share_handle, fb->offset);

        pixman_image_unref(rect);
        dpy_gfx_replace_surface(parent_obj.scanout[scanout_id].con,
                                scanout->ds);
    }

    updateScanout(scanout_id, res, fb, r);
    return true;
}

void VirtIOGPU::setScanout(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_framebuffer fb;
    memset(&fb, 0, sizeof(fb));
    struct virtio_gpu_set_scanout ss;

    VIRTIO_GPU_FILL_CMD(ss);
    virtio_gpu_bswap_32(&ss, sizeof(ss));
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);

    if (ss.scanout_id >= parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    if (ss.resource_id == 0) {
        disableScanout(ss.scanout_id);
        return;
    }

    res = findCheckResource(ss.resource_id, true,
                            __func__, &cmd->error);
    if (!res) {
        return;
    }

    fb.format = pixman_image_get_format(res->image);
    fb.bytes_pp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(fb.format), 8);
    fb.width  = pixman_image_get_width(res->image);
    fb.height = pixman_image_get_height(res->image);
    fb.stride = pixman_image_get_stride(res->image);
    fb.offset = ss.r.x * fb.bytes_pp + ss.r.y * fb.stride;

    doSetScanout(ss.scanout_id, &fb, res, &ss.r, &cmd->error);
}

bool virtio_gpu_scanout_blob_to_fb(struct virtio_gpu_framebuffer *fb,
                                   struct virtio_gpu_set_scanout_blob *ss,
                                   uint64_t blob_size)
{
    uint64_t fbend;

    fb->format = virtio_gpu_get_pixman_format(ss->format);
    if (!fb->format) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: host couldn't handle guest format %d\n",
                      __func__, ss->format);
        return false;
    }

    fb->bytes_pp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(fb->format), 8);
    fb->width = ss->width;
    fb->height = ss->height;
    fb->stride = ss->strides[0];
    fb->offset = ss->offsets[0] + ss->r.x * fb->bytes_pp + ss->r.y * fb->stride;

    fbend = fb->offset;
    fbend += (uint64_t) fb->stride * ss->r.height;

    if (fbend > blob_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: fb end out of range\n",
                      __func__);
        return false;
    }

    return true;
}

void VirtIOGPU::setScanoutBlob(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_framebuffer fb;
    memset(&fb, 0, sizeof(fb));
    struct virtio_gpu_set_scanout_blob ss;

    VIRTIO_GPU_FILL_CMD(ss);
    virtio_gpu_scanout_blob_bswap(&ss);
    trace_virtio_gpu_cmd_set_scanout_blob(ss.scanout_id, ss.resource_id,
                                          ss.r.width, ss.r.height, ss.r.x,
                                          ss.r.y);

    if (ss.scanout_id >= parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    if (ss.resource_id == 0) {
        disableScanout(ss.scanout_id);
        return;
    }

    res = findCheckResource(ss.resource_id, true,
                            __func__, &cmd->error);
    if (!res) {
        return;
    }

    if (!virtio_gpu_scanout_blob_to_fb(&fb, &ss, res->blob_size)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    doSetScanout(ss.scanout_id, &fb, res, &ss.r, &cmd->error);
}

int VirtIOGPU::createMappingIov(uint32_t nr_entries, uint32_t offset,
                                struct virtio_gpu_ctrl_command *cmd,
                                uint64_t **addr, struct iovec **iov,
                                uint32_t *niov)
{
    struct virtio_gpu_mem_entry *ents;
    size_t esize, s;
    int e, v;

    if (nr_entries > 16384) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: nr_entries is too big (%d > 16384)\n",
                      __func__, nr_entries);
        return -1;
    }

    esize = sizeof(*ents) * nr_entries;
    ents = static_cast<struct virtio_gpu_mem_entry *>(g_malloc(esize));
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   offset, ents, esize);
    if (s != esize) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: command data size incorrect %zu vs %zu\n",
                      __func__, s, esize);
        g_free(ents);
        return -1;
    }

    *iov = NULL;
    if (addr) {
        *addr = NULL;
    }
    for (e = 0, v = 0; e < nr_entries; e++) {
        uint64_t a = le64_to_cpu(ents[e].addr);
        uint32_t l = le32_to_cpu(ents[e].length);
        hwaddr len;
        void *map;

        do {
            len = l;
            map = dma_memory_map(reinterpret_cast<VirtIODevice *>(this)->dma_as, a, &len,
                                 DMA_DIRECTION_TO_DEVICE,
                                 MEMTXATTRS_UNSPECIFIED);
            if (!map) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to map MMIO memory for"
                              " element %d\n", __func__, e);
                cleanupMappingIov(*iov, v);
                g_free(ents);
                *iov = NULL;
                if (addr) {
                    g_free(*addr);
                    *addr = NULL;
                }
                return -1;
            }

            if (!(v % 16)) {
                *iov = g_renew(struct iovec, *iov, v + 16);
                if (addr) {
                    *addr = g_renew(uint64_t, *addr, v + 16);
                }
            }
            (*iov)[v].iov_base = map;
            (*iov)[v].iov_len = len;
            if (addr) {
                (*addr)[v] = a;
            }

            a += len;
            l -= len;
            v += 1;
        } while (l > 0);
    }
    *niov = v;

    g_free(ents);
    return 0;
}

void VirtIOGPU::cleanupMappingIov(struct iovec *iov, uint32_t count)
{
    int i;

    for (i = 0; i < count; i++) {
        dma_memory_unmap(reinterpret_cast<VirtIODevice *>(this)->dma_as,
                         iov[i].iov_base, iov[i].iov_len,
                         DMA_DIRECTION_TO_DEVICE,
                         iov[i].iov_len);
    }
    g_free(iov);
}

void VirtIOGPU::cleanupMapping(struct virtio_gpu_simple_resource *res)
{
    cleanupMappingIov(res->iov, res->iov_cnt);
    res->iov = NULL;
    res->iov_cnt = 0;
    g_free(res->addrs);
    res->addrs = NULL;

    if (res->blob) {
        virtio_gpu_fini_udmabuf(res);
    }
}

void VirtIOGPU::resourceAttachBacking(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_attach_backing ab;
    int ret;

    VIRTIO_GPU_FILL_CMD(ab);
    virtio_gpu_bswap_32(&ab, sizeof(ab));
    trace_virtio_gpu_cmd_res_back_attach(ab.resource_id);

    res = findResource(ab.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal resource specified %d\n",
                      __func__, ab.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (res->iov) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    ret = createMappingIov(ab.nr_entries, sizeof(ab), cmd,
                           &res->addrs, &res->iov, &res->iov_cnt);
    if (ret != 0) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
}

void VirtIOGPU::resourceDetachBacking(struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_simple_resource *res;
    struct virtio_gpu_resource_detach_backing detach;

    VIRTIO_GPU_FILL_CMD(detach);
    virtio_gpu_bswap_32(&detach, sizeof(detach));
    trace_virtio_gpu_cmd_res_back_detach(detach.resource_id);

    res = findCheckResource(detach.resource_id, true,
                            __func__, &cmd->error);
    if (!res) {
        return;
    }
    cleanupMapping(res);
}

void VirtIOGPU::simpleProcessCmd(struct virtio_gpu_ctrl_command *cmd)
{
    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);
    virtio_gpu_ctrl_hdr_bswap(&cmd->cmd_hdr);

    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        getDisplayInfo(cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        getEdid(cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        resourceCreate2d(cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        if (!virtio_gpu_blob_enabled(parent_obj.conf)) {
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        resourceCreateBlob(cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        resourceUnref(cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        resourceFlush(cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        transferToHost2d(cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        setScanout(cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        if (!virtio_gpu_blob_enabled(parent_obj.conf)) {
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        setScanoutBlob(cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        resourceAttachBacking(cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        resourceDetachBacking(cmd);
        break;
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }
    if (!cmd->finished) {
        if (!parent_obj.renderer_blocked) {
            ctrlResponseNodata(cmd,
                               static_cast<enum virtio_gpu_ctrl_type>(
                                   cmd->error ? cmd->error : VIRTIO_GPU_RESP_OK_NODATA));
        }
    }
}

void VirtIOGPU::processCmdq()
{
    struct virtio_gpu_ctrl_command *cmd;
    VirtIOGPUClass *vgc = VIRTIO_GPU_GET_CLASS(this);

    if (processing_cmdq) {
        return;
    }
    processing_cmdq = true;
    while (!QTAILQ_EMPTY(&cmdq)) {
        cmd = QTAILQ_FIRST(&cmdq);

        if (parent_obj.renderer_blocked) {
            break;
        }

        /* process command */
        vgc->process_cmd(this, cmd);

        /* command suspended */
        if (!cmd->finished && !(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
            trace_virtio_gpu_cmd_suspended(cmd->cmd_hdr.type);
            break;
        }

        QTAILQ_REMOVE(&cmdq, cmd, next);
        if (virtio_gpu_stats_enabled(parent_obj.conf)) {
            stats.requests++;
        }

        if (!cmd->finished) {
            QTAILQ_INSERT_TAIL(&fenceq, cmd, next);
            inflight++;
            if (virtio_gpu_stats_enabled(parent_obj.conf)) {
                if (stats.max_inflight < inflight) {
                    stats.max_inflight = inflight;
                }
                trace_virtio_gpu_inc_inflight_fences(inflight);
            }
        } else {
            g_free(cmd);
        }
    }
    processing_cmdq = false;
}

void VirtIOGPU::processFenceq()
{
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &fenceq, next, tmp) {
        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        ctrlResponseNodata(cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&fenceq, cmd, next);
        g_free(cmd);
        inflight--;
        if (virtio_gpu_stats_enabled(parent_obj.conf)) {
            trace_virtio_gpu_dec_inflight_fences(inflight);
        }
    }
}

bool VirtIOGPU::loadRestoreMapping(struct virtio_gpu_simple_resource *res)
{
    int i;

    for (i = 0; i < res->iov_cnt; i++) {
        hwaddr len = res->iov[i].iov_len;
        res->iov[i].iov_base =
            dma_memory_map(reinterpret_cast<VirtIODevice *>(this)->dma_as, res->addrs[i], &len,
                           DMA_DIRECTION_TO_DEVICE, MEMTXATTRS_UNSPECIFIED);

        if (!res->iov[i].iov_base || len != res->iov[i].iov_len) {
            /* Clean up the half-a-mapping we just created... */
            if (res->iov[i].iov_base) {
                dma_memory_unmap(reinterpret_cast<VirtIODevice *>(this)->dma_as, res->iov[i].iov_base,
                                 len, DMA_DIRECTION_TO_DEVICE, 0);
            }
            /* ...and the mappings for previous loop iterations */
            res->iov_cnt = i;
            cleanupMapping(res);
            return false;
        }
    }

    QTAILQ_INSERT_HEAD(&reslist, res, next);
    hostmem += res->hostmem;
    return true;
}

void VirtIOGPU::realize(DeviceState *qdev, Error **errp)
{
    VirtIODevice *vdev = reinterpret_cast<VirtIODevice *>(qdev);

    if (virtio_gpu_blob_enabled(parent_obj.conf)) {
        if (!virtio_gpu_rutabaga_enabled(parent_obj.conf) &&
            !virtio_gpu_virgl_enabled(parent_obj.conf) &&
            !virtio_gpu_have_udmabuf()) {
            error_setg(errp, "need rutabaga or udmabuf for blob resources");
            return;
        }

#ifdef VIRGL_VERSION_MAJOR
    #if VIRGL_VERSION_MAJOR < 1
        if (virtio_gpu_virgl_enabled(parent_obj.conf)) {
            error_setg(errp, "old virglrenderer, blob resources unsupported");
            return;
        }
    #endif
#endif
    }

    if (virtio_gpu_venus_enabled(parent_obj.conf)) {
#ifdef VIRGL_VERSION_MAJOR
    #if VIRGL_VERSION_MAJOR >= 1
        if (!virtio_gpu_blob_enabled(parent_obj.conf) ||
            !virtio_gpu_hostmem_enabled(parent_obj.conf)) {
            error_setg(errp, "venus requires enabled blob and hostmem options");
            return;
        }
    #else
        error_setg(errp, "old virglrenderer, venus unsupported");
        return;
    #endif
#endif
    }

    if (!virtio_gpu_base_device_realize(qdev,
                                        virtio_gpu_handle_ctrl_cb,
                                        virtio_gpu_handle_cursor_cb,
                                        errp)) {
        return;
    }

    ctrl_vq = virtio_get_queue(vdev, 0);
    cursor_vq = virtio_get_queue(vdev, 1);
    ctrl_bh = virtio_bh_new_guarded(qdev, virtio_gpu_ctrl_bh, this);
    cursor_bh = virtio_bh_new_guarded(qdev, virtio_gpu_cursor_bh, this);
    reset_bh = qemu_bh_new(virtio_gpu_reset_bh, this);
    qemu_cond_init(&reset_cond);
    QTAILQ_INIT(&reslist);
    QTAILQ_INIT(&cmdq);
    QTAILQ_INIT(&fenceq);
}

void VirtIOGPU::reset(void)
{
    VirtIODevice *vdev = reinterpret_cast<VirtIODevice *>(this);
    struct virtio_gpu_ctrl_command *cmd;

    if (qemu_in_vcpu_thread()) {
        reset_finished = false;
        qemu_bh_schedule(reset_bh);
        while (!reset_finished) {
            qemu_cond_wait_bql(&reset_cond);
        }
    } else {
        aio_bh_call(reset_bh);
    }

    while (!QTAILQ_EMPTY(&cmdq)) {
        cmd = QTAILQ_FIRST(&cmdq);
        QTAILQ_REMOVE(&cmdq, cmd, next);
        g_free(cmd);
    }

    while (!QTAILQ_EMPTY(&fenceq)) {
        cmd = QTAILQ_FIRST(&fenceq);
        QTAILQ_REMOVE(&fenceq, cmd, next);
        inflight--;
        g_free(cmd);
    }

    virtio_gpu_base_reset(reinterpret_cast<VirtIOGPUBase *>(vdev));
}

/*
 * extern "C" wrapper functions for the public API
 */

extern "C"
struct virtio_gpu_simple_resource *
virtio_gpu_find_resource(VirtIOGPU *g, uint32_t resource_id)
{
    return g->findResource(resource_id);
}

extern "C"
void virtio_gpu_ctrl_response(VirtIOGPU *g,
                              struct virtio_gpu_ctrl_command *cmd,
                              struct virtio_gpu_ctrl_hdr *resp,
                              size_t resp_len)
{
    g->ctrlResponse(cmd, resp, resp_len);
}

extern "C"
void virtio_gpu_ctrl_response_nodata(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd,
                                     enum virtio_gpu_ctrl_type type)
{
    g->ctrlResponseNodata(cmd, type);
}

extern "C"
void virtio_gpu_get_display_info(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    g->getDisplayInfo(cmd);
}

extern "C"
void virtio_gpu_get_edid(VirtIOGPU *g,
                         struct virtio_gpu_ctrl_command *cmd)
{
    g->getEdid(cmd);
}

extern "C"
int virtio_gpu_create_mapping_iov(VirtIOGPU *g,
                                  uint32_t nr_entries, uint32_t offset,
                                  struct virtio_gpu_ctrl_command *cmd,
                                  uint64_t **addr, struct iovec **iov,
                                  uint32_t *niov)
{
    return g->createMappingIov(nr_entries, offset, cmd, addr, iov, niov);
}

extern "C"
void virtio_gpu_cleanup_mapping_iov(VirtIOGPU *g,
                                    struct iovec *iov, uint32_t count)
{
    g->cleanupMappingIov(iov, count);
}

extern "C"
void virtio_gpu_cleanup_mapping(VirtIOGPU *g,
                                struct virtio_gpu_simple_resource *res)
{
    g->cleanupMapping(res);
}

extern "C"
void virtio_gpu_process_cmdq(VirtIOGPU *g)
{
    g->processCmdq();
}

extern "C"
void virtio_gpu_simple_process_cmd(VirtIOGPU *g,
                                   struct virtio_gpu_ctrl_command *cmd)
{
    g->simpleProcessCmd(cmd);
}

extern "C"
void virtio_gpu_update_cursor_data(VirtIOGPU *g,
                                   struct virtio_gpu_scanout *s,
                                   uint32_t resource_id)
{
    g->updateCursorData(s, resource_id);
}

extern "C"
void virtio_gpu_disable_scanout(VirtIOGPU *g, int scanout_id)
{
    g->disableScanout(scanout_id);
}

extern "C"
void virtio_gpu_update_scanout(VirtIOGPU *g,
                               uint32_t scanout_id,
                               struct virtio_gpu_simple_resource *res,
                               struct virtio_gpu_framebuffer *fb,
                               struct virtio_gpu_rect *r)
{
    g->updateScanout(scanout_id, res, fb, r);
}

extern "C"
void virtio_gpu_device_realize(DeviceState *qdev, Error **errp)
{
    VirtIOGPU *g = reinterpret_cast<VirtIOGPU *>(qdev);
    g->realize(qdev, errp);
}

extern "C"
void virtio_gpu_reset(VirtIODevice *vdev)
{
    VirtIOGPU *g = reinterpret_cast<VirtIOGPU *>(vdev);
    g->reset();
}

/*
 * VirtIO callbacks (kept as free functions - they receive VirtIODevice* or void*)
 */

static void virtio_gpu_handle_ctrl_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = reinterpret_cast<VirtIOGPU *>(vdev);
    qemu_bh_schedule(g->ctrl_bh);
}

static void virtio_gpu_handle_cursor_cb(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = reinterpret_cast<VirtIOGPU *>(vdev);
    qemu_bh_schedule(g->cursor_bh);
}

static void virtio_gpu_handle_gl_flushed(VirtIOGPUBase *b)
{
    VirtIOGPU *g = container_of(b, VirtIOGPU, parent_obj);

    g->processFenceq();
    g->processCmdq();
}

static void virtio_gpu_handle_ctrl(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = reinterpret_cast<VirtIOGPU *>(vdev);
    struct virtio_gpu_ctrl_command *cmd;

    if (!virtio_queue_ready(vq)) {
        return;
    }

    cmd = static_cast<struct virtio_gpu_ctrl_command *>(virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command)));
    while (cmd) {
        cmd->vq = vq;
        cmd->error = 0;
        cmd->finished = false;
        QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
        cmd = static_cast<struct virtio_gpu_ctrl_command *>(virtqueue_pop(vq, sizeof(struct virtio_gpu_ctrl_command)));
    }

    g->processCmdq();
}

static void virtio_gpu_ctrl_bh(void *opaque)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    VirtIOGPUClass *vgc = VIRTIO_GPU_GET_CLASS(g);

    vgc->handle_ctrl(reinterpret_cast<VirtIODevice *>(g), g->ctrl_vq);
}

static void virtio_gpu_handle_cursor(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOGPU *g = reinterpret_cast<VirtIOGPU *>(vdev);
    VirtQueueElement *elem;
    size_t s;
    struct virtio_gpu_update_cursor cursor_info;

    if (!virtio_queue_ready(vq)) {
        return;
    }
    for (;;) {
        elem = static_cast<VirtQueueElement *>(virtqueue_pop(vq, sizeof(VirtQueueElement)));
        if (!elem) {
            break;
        }

        s = iov_to_buf(elem->out_sg, elem->out_num, 0,
                       &cursor_info, sizeof(cursor_info));
        if (s != sizeof(cursor_info)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: cursor size incorrect %zu vs %zu\n",
                          __func__, s, sizeof(cursor_info));
        } else {
            virtio_gpu_bswap_32(&cursor_info, sizeof(cursor_info));
            g->updateCursor(&cursor_info);
        }
        virtqueue_push(vq, elem, 0);
        virtio_notify(vdev, vq);
        g_free(elem);
    }
}

static void virtio_gpu_cursor_bh(void *opaque)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    virtio_gpu_handle_cursor(&g->parent_obj.parent_obj, g->cursor_vq);
}

/*
 * VMState callbacks (kept as free functions since VMState uses void* opaque)
 */

static bool scanout_vmstate_after_v2(void *opaque, int version)
{
    struct VirtIOGPUBase *base = container_of(static_cast<const struct virtio_gpu_scanout (*)[16]>(opaque), VirtIOGPUBase, scanout);
    struct VirtIOGPU *gpu = container_of(base, VirtIOGPU, parent_obj);

    return gpu->scanout_vmstate_version >= 2;
}

static const VMStateField vmstate_virtio_gpu_scanout_fields[] = {
        VMSTATE_UINT32(resource_id, struct virtio_gpu_scanout),
        VMSTATE_UINT32(width, struct virtio_gpu_scanout),
        VMSTATE_UINT32(height, struct virtio_gpu_scanout),
        VMSTATE_INT32(x, struct virtio_gpu_scanout),
        VMSTATE_INT32(y, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.resource_id, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.hot_x, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.hot_y, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.pos.x, struct virtio_gpu_scanout),
        VMSTATE_UINT32(cursor.pos.y, struct virtio_gpu_scanout),
        VMSTATE_UINT32_TEST(fb.format, struct virtio_gpu_scanout,
                            scanout_vmstate_after_v2),
        VMSTATE_UINT32_TEST(fb.bytes_pp, struct virtio_gpu_scanout,
                            scanout_vmstate_after_v2),
        VMSTATE_UINT32_TEST(fb.width, struct virtio_gpu_scanout,
                            scanout_vmstate_after_v2),
        VMSTATE_UINT32_TEST(fb.height, struct virtio_gpu_scanout,
                            scanout_vmstate_after_v2),
        VMSTATE_UINT32_TEST(fb.stride, struct virtio_gpu_scanout,
                            scanout_vmstate_after_v2),
        VMSTATE_UINT32_TEST(fb.offset, struct virtio_gpu_scanout,
                            scanout_vmstate_after_v2),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_virtio_gpu_scanout = {
    .name = "virtio-gpu-one-scanout",
    .version_id = 1,
    .fields = vmstate_virtio_gpu_scanout_fields,
};

static const VMStateField vmstate_virtio_gpu_scanouts_fields[] = {
    VMSTATE_INT32(parent_obj.enable, struct VirtIOGPU),
    VMSTATE_UINT32_EQUAL(parent_obj.conf.max_outputs,
                         struct VirtIOGPU, NULL),
    VMSTATE_STRUCT_VARRAY_UINT32(parent_obj.scanout, struct VirtIOGPU,
                                 parent_obj.conf.max_outputs, 1,
                                 vmstate_virtio_gpu_scanout,
                                 struct virtio_gpu_scanout),
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription vmstate_virtio_gpu_scanouts = {
    .name = "virtio-gpu-scanouts",
    .version_id = 1,
    .fields = vmstate_virtio_gpu_scanouts_fields,
};

static int virtio_gpu_save(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field, JSONWriter *vmdesc)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    struct virtio_gpu_simple_resource *res;
    Error *err = NULL;
    int i, ret;

    /* in 2d mode we should never find unprocessed commands here */
    assert(QTAILQ_EMPTY(&g->cmdq));

    QTAILQ_FOREACH(res, &g->reslist, next) {
        if (res->blob_size) {
            continue;
        }
        qemu_put_be32(f, res->resource_id);
        qemu_put_be32(f, res->width);
        qemu_put_be32(f, res->height);
        qemu_put_be32(f, res->format);
        qemu_put_be32(f, res->iov_cnt);
        for (i = 0; i < res->iov_cnt; i++) {
            qemu_put_be64(f, res->addrs[i]);
            qemu_put_be32(f, res->iov[i].iov_len);
        }
        qemu_put_buffer(f, static_cast<const uint8_t *>(static_cast<void *>(pixman_image_get_data(res->image))),
                        pixman_image_get_stride(res->image) * res->height);
    }
    qemu_put_be32(f, 0); /* end of list */

    ret = vmstate_save_state(f, &vmstate_virtio_gpu_scanouts, g, NULL,
                             &err);
    if (ret < 0) {
        error_report_err(err);
    }
    return ret;
}

static int virtio_gpu_load(QEMUFile *f, void *opaque, size_t size,
                           const VMStateField *field)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    Error *err = NULL;
    struct virtio_gpu_simple_resource *res;
    uint32_t resource_id, pformat;
    int i, ret;

    g->hostmem = 0;

    resource_id = qemu_get_be32(f);
    while (resource_id != 0) {
        res = g->findResource(resource_id);
        if (res) {
            return -EINVAL;
        }

        res = g_new0(struct virtio_gpu_simple_resource, 1);
        res->resource_id = resource_id;
        res->width = qemu_get_be32(f);
        res->height = qemu_get_be32(f);
        res->format = qemu_get_be32(f);
        res->iov_cnt = qemu_get_be32(f);

        /* allocate */
        pformat = virtio_gpu_get_pixman_format(res->format);
        if (!pformat) {
            g_free(res);
            return -EINVAL;
        }

        res->hostmem = calc_image_hostmem(static_cast<pixman_format_code_t>(pformat), res->width, res->height);
        if (!qemu_pixman_image_new_shareable(&res->image,
                                             &res->share_handle,
                                             "virtio-gpu res",
                                             static_cast<pixman_format_code_t>(pformat),
                                             res->width,
                                             res->height,
                                             res->height ? res->hostmem / res->height : 0,
                                             &err)) {
            warn_report_err(err);
            g_free(res);
            return -EINVAL;
        }

        res->addrs = g_new(uint64_t, res->iov_cnt);
        res->iov = g_new(struct iovec, res->iov_cnt);

        /* read data */
        for (i = 0; i < res->iov_cnt; i++) {
            res->addrs[i] = qemu_get_be64(f);
            res->iov[i].iov_len = qemu_get_be32(f);
        }
        qemu_get_buffer(f, static_cast<uint8_t *>(static_cast<void *>(pixman_image_get_data(res->image))),
                        pixman_image_get_stride(res->image) * res->height);

        if (!g->loadRestoreMapping(res)) {
            pixman_image_unref(res->image);
            g_free(res);
            return -EINVAL;
        }

        resource_id = qemu_get_be32(f);
    }

    /* load & apply scanout state */
    ret = vmstate_load_state(f, &vmstate_virtio_gpu_scanouts, g, 1, &err);
    if (ret < 0) {
        error_report_err(err);
    }
    return ret;
}

static int virtio_gpu_blob_save(QEMUFile *f, void *opaque, size_t size,
                                const VMStateField *field, JSONWriter *vmdesc)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    struct virtio_gpu_simple_resource *res;
    int i;

    /* in 2d mode we should never find unprocessed commands here */
    assert(QTAILQ_EMPTY(&g->cmdq));

    QTAILQ_FOREACH(res, &g->reslist, next) {
        if (!res->blob_size) {
            continue;
        }
        assert(!res->image);
        qemu_put_be32(f, res->resource_id);
        qemu_put_be32(f, res->blob_size);
        qemu_put_be32(f, res->iov_cnt);
        for (i = 0; i < res->iov_cnt; i++) {
            qemu_put_be64(f, res->addrs[i]);
            qemu_put_be32(f, res->iov[i].iov_len);
        }
    }
    qemu_put_be32(f, 0); /* end of list */

    return 0;
}

static int virtio_gpu_blob_load(QEMUFile *f, void *opaque, size_t size,
                                const VMStateField *field)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    struct virtio_gpu_simple_resource *res;
    uint32_t resource_id;
    int i;

    resource_id = qemu_get_be32(f);
    while (resource_id != 0) {
        res = g->findResource(resource_id);
        if (res) {
            return -EINVAL;
        }

        res = g_new0(struct virtio_gpu_simple_resource, 1);
        res->resource_id = resource_id;
        res->blob_size = qemu_get_be32(f);
        res->iov_cnt = qemu_get_be32(f);
        res->addrs = g_new(uint64_t, res->iov_cnt);
        res->iov = g_new(struct iovec, res->iov_cnt);

        /* read data */
        for (i = 0; i < res->iov_cnt; i++) {
            res->addrs[i] = qemu_get_be64(f);
            res->iov[i].iov_len = qemu_get_be32(f);
        }

        if (!g->loadRestoreMapping(res)) {
            g_free(res);
            return -EINVAL;
        }

        virtio_gpu_init_udmabuf(res);

        resource_id = qemu_get_be32(f);
    }

    return 0;
}

static int virtio_gpu_post_load(void *opaque, int version_id)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    struct virtio_gpu_scanout *scanout;
    struct virtio_gpu_simple_resource *res;
    int i;

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        scanout = &g->parent_obj.scanout[i];
        if (!scanout->resource_id) {
            continue;
        }

        res = g->findResource(scanout->resource_id);
        if (!res) {
            return -EINVAL;
        }

        if (scanout->fb.format != 0) {
            uint32_t error = 0;
            struct virtio_gpu_rect r = {
                .x = scanout->x,
                .y = scanout->y,
                .width = scanout->width,
                .height = scanout->height
            };

            if (!g->doSetScanout(i, &scanout->fb, res, &r, &error)) {
                return -EINVAL;
            }
        } else {
            /* legacy v1 migration support */
            if (!res->image) {
                return -EINVAL;
            }
            scanout->ds = qemu_create_displaysurface_pixman(res->image);
            qemu_displaysurface_set_share_handle(scanout->ds, res->share_handle, 0);
            dpy_gfx_replace_surface(scanout->con, scanout->ds);
        }

        dpy_gfx_update_full(scanout->con);
        if (scanout->cursor.resource_id) {
            g->updateCursor(&scanout->cursor);
        }
        res->scanout_bitmask |= (1 << i);
    }

    return 0;
}

static void virtio_gpu_device_unrealize(DeviceState *qdev)
{
    VirtIOGPU *g = reinterpret_cast<VirtIOGPU *>(qdev);

    g_clear_pointer(&g->ctrl_bh, qemu_bh_delete);
    g_clear_pointer(&g->cursor_bh, qemu_bh_delete);
    g_clear_pointer(&g->reset_bh, qemu_bh_delete);
    qemu_cond_destroy(&g->reset_cond);
    virtio_gpu_base_device_unrealize(qdev);
}

static void virtio_gpu_reset_bh(void *opaque)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);
    VirtIOGPUClass *vgc = VIRTIO_GPU_GET_CLASS(g);
    struct virtio_gpu_simple_resource *res, *tmp;
    uint32_t resource_id;
    Error *local_err = NULL;
    int i = 0;

    QTAILQ_FOREACH_SAFE(res, &g->reslist, next, tmp) {
        resource_id = res->resource_id;
        vgc->resource_destroy(g, res, &local_err);
        if (local_err) {
            error_append_hint(&local_err, "%s: %s resource_destroy"
                              "for resource_id = %" PRIu32 " failed.\n",
                              __func__, object_get_typename(reinterpret_cast<Object *>(g)),
                              resource_id);
            /* error_report_err frees the error object for us */
            error_report_err(local_err);
            local_err = NULL;
        }
    }

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        dpy_gfx_replace_surface(g->parent_obj.scanout[i].con, NULL);
    }

    g->reset_finished = true;
    qemu_cond_signal(&g->reset_cond);
}

static void
virtio_gpu_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOGPUBase *g = reinterpret_cast<VirtIOGPUBase *>(vdev);

    memcpy(config, &g->virtio_config, sizeof(g->virtio_config));
}

static void
virtio_gpu_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIOGPUBase *g = reinterpret_cast<VirtIOGPUBase *>(vdev);
    const struct virtio_gpu_config *vgconfig =
        (const struct virtio_gpu_config *)config;

    if (vgconfig->events_clear) {
        g->virtio_config.events_read &= ~vgconfig->events_clear;
    }
}

static bool virtio_gpu_blob_state_needed(void *opaque)
{
    VirtIOGPU *g = static_cast<VirtIOGPU *>(opaque);

    return virtio_gpu_blob_enabled(g->parent_obj.conf);
}

static const VMStateInfo vmstate_info_virtio_gpu_blob = {
    .name = "blob",
    .get = virtio_gpu_blob_load,
    .put = virtio_gpu_blob_save,
};

static const VMStateField vmstate_virtio_gpu_blob_state_fields[] = {
    {
        .name = "virtio-gpu/blob",
        .info = &vmstate_info_virtio_gpu_blob,
        .flags = VMS_SINGLE,
    } /* device */,
    VMSTATE_END_OF_LIST()
};

const VMStateDescription vmstate_virtio_gpu_blob_state = {
    .name = "virtio-gpu/blob",
    .version_id = VIRTIO_GPU_VM_VERSION,
    .minimum_version_id = VIRTIO_GPU_VM_VERSION,
    .needed = virtio_gpu_blob_state_needed,
    .fields = vmstate_virtio_gpu_blob_state_fields,
};

/*
 * For historical reasons virtio_gpu does not adhere to virtio migration
 * scheme as described in doc/virtio-migration.txt, in a sense that no
 * save/load callback are provided to the core. Instead the device data
 * is saved/loaded after the core data.
 *
 * Because of this we need a special vmsd.
 */
static const VMStateInfo vmstate_info_virtio_gpu = {
    .name = "virtio-gpu",
    .get = virtio_gpu_load,
    .put = virtio_gpu_save,
};

static const VMStateField vmstate_virtio_gpu_fields[] = {
    VMSTATE_VIRTIO_DEVICE /* core */,
    {
        .name = "virtio-gpu",
        .info = &vmstate_info_virtio_gpu,
        .flags = VMS_SINGLE,
    } /* device */,
    VMSTATE_END_OF_LIST()
};

static const VMStateDescription * const vmstate_virtio_gpu_subsections[] = {
    &vmstate_virtio_gpu_blob_state,
    NULL
};

static const VMStateDescription vmstate_virtio_gpu = {
    .name = "virtio-gpu",
    .version_id = VIRTIO_GPU_VM_VERSION,
    .minimum_version_id = VIRTIO_GPU_VM_VERSION,
    .post_load = virtio_gpu_post_load,
    .fields = vmstate_virtio_gpu_fields,
    .subsections = vmstate_virtio_gpu_subsections,
};

static const Property virtio_gpu_properties[] = {
    VIRTIO_GPU_BASE_PROPERTIES(VirtIOGPU, parent_obj.conf),
    DEFINE_PROP_SIZE("max_hostmem", VirtIOGPU, conf_max_hostmem,
                     256 * MiB),
    DEFINE_PROP_BIT("blob", VirtIOGPU, parent_obj.conf.flags,
                    VIRTIO_GPU_FLAG_BLOB_ENABLED, false),
    DEFINE_PROP_SIZE("hostmem", VirtIOGPU, parent_obj.conf.hostmem, 0),
    DEFINE_PROP_UINT8("x-scanout-vmstate-version", VirtIOGPU, scanout_vmstate_version, 2),
};

/*
 * Class init - wrappers delegate to methods via the vgc function pointers
 */

/* These wrappers are used as vgc->process_cmd etc. They call through methods. */
static void virtio_gpu_simple_process_cmd_wrapper(VirtIOGPU *g,
                                                  struct virtio_gpu_ctrl_command *cmd)
{
    g->simpleProcessCmd(cmd);
}

static void virtio_gpu_update_cursor_data_wrapper(VirtIOGPU *g,
                                                  struct virtio_gpu_scanout *s,
                                                  uint32_t resource_id)
{
    g->updateCursorData(s, resource_id);
}

static void virtio_gpu_resource_destroy_wrapper(VirtIOGPU *g,
                                                struct virtio_gpu_simple_resource *res,
                                                Error **errp)
{
    g->resourceDestroy(res, errp);
}

void VirtIOGPU::classInit(DeviceClass *dc)
{
    ObjectClass *klass = reinterpret_cast<ObjectClass *>(dc);
    VirtioDeviceClass *vdc = reinterpret_cast<VirtioDeviceClass *>(klass);
    VirtIOGPUClass *vgc = reinterpret_cast<VirtIOGPUClass *>(klass);
    VirtIOGPUBaseClass *vgbc = &vgc->parent;

    vgc->handle_ctrl = virtio_gpu_handle_ctrl;
    vgc->process_cmd = virtio_gpu_simple_process_cmd_wrapper;
    vgc->update_cursor_data = virtio_gpu_update_cursor_data_wrapper;
    vgc->resource_destroy = virtio_gpu_resource_destroy_wrapper;
    vgbc->gl_flushed = virtio_gpu_handle_gl_flushed;

    vdc->realize = virtio_gpu_device_realize;
    vdc->unrealize = virtio_gpu_device_unrealize;
    vdc->reset = virtio_gpu_reset;
    vdc->get_config = virtio_gpu_get_config;
    vdc->set_config = virtio_gpu_set_config;

    dc->vmsd = &vmstate_virtio_gpu;
    device_class_set_props(dc, virtio_gpu_properties);
}

#include "qom/cpp/object.h"
module_obj(TYPE_VIRTIO_GPU);
module_kconfig(VIRTIO_GPU);

REGISTER_QEMU_DEVICE_CLASS_SIZE(VirtIOGPU, VirtIOGPUClass,
                                TYPE_VIRTIO_GPU, TYPE_VIRTIO_GPU_BASE)
