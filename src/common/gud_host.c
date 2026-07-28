/* SPDX-License-Identifier: MIT */
/*
 * gud_host.c -- GUD protocol, host side, no USB.
 */

#include "gud_host.h"
#include <string.h>

/* ---------------- status handshake ---------------- */

static int status_to_err(uint8_t s)
{
    switch (s) {
    case GUD_STATUS_OK:                     return 0;
    case GUD_STATUS_BUSY:                   return GUD_E_BUSY;
    case GUD_STATUS_REQUEST_NOT_SUPPORTED:  return GUD_E_NOTSUP;
    case GUD_STATUS_PROTOCOL_ERROR:         return GUD_E_PROTO;
    case GUD_STATUS_INVALID_PARAMETER:      return GUD_E_INVAL;
    default:                                return GUD_E_IO;
    }
}

static int get_status(struct gud_device *d)
{
    uint8_t s = 0xff;
    int n = d->t.ctrl_in(d->t.ctx, GUD_REQ_GET_STATUS, 0, &s, 1);
    if (n < 0)
        return n;
    if (n != 1)
        return GUD_E_IO;
    return status_to_err(s);
}

/*
 * Every SET is followed by a status read when the device asked for it.
 *
 * The Linux gadget cannot control the status stage of a control OUT that
 * carries a payload -- the hardware has already ACKed by the time the function
 * driver sees the data -- so a device built on it sets STATUS_ON_SET and
 * reports the real answer to a separate GET_STATUS. Skipping that read means
 * every failed modeset looks like a success, which is a fault the device side
 * of this project already had once and fixed. Do not skip it here for
 * symmetry's sake.
 */
static int gud_set(struct gud_device *d, uint8_t request, uint16_t value,
                   const void *buf, size_t len)
{
    int n = d->t.ctrl_out(d->t.ctx, request, value, buf, len);
    if (n < 0)
        return n;
    if ((size_t)n != len)
        return GUD_E_IO;
    if (!(d->desc.flags & GUD_DISPLAY_FLAG_STATUS_ON_SET))
        return 0;
    return get_status(d);
}

/* ---------------- enumeration ---------------- */

int gud_probe(struct gud_device *d, const struct gud_transport *t)
{
    uint8_t fmts[GUD_FORMATS_MAX_NUM];
    int n;

    memset(d, 0, sizeof *d);
    d->t = *t;

    n = t->ctrl_in(t->ctx, GUD_REQ_GET_DESCRIPTOR, 0, &d->desc, sizeof d->desc);
    if (n < 0)
        return n;
    if ((size_t)n != sizeof d->desc)
        return GUD_E_PROTO;
    if (d->desc.magic != GUD_DISPLAY_MAGIC)
        return GUD_E_NODEV;
    if (!d->desc.version || !d->desc.max_width || !d->desc.max_height ||
        d->desc.min_width > d->desc.max_width ||
        d->desc.min_height > d->desc.max_height)
        return GUD_E_PROTO;

    n = t->ctrl_in(t->ctx, GUD_REQ_GET_FORMATS, 0, fmts, sizeof fmts);
    if (n < 0)
        return n;
    d->n_formats = (unsigned)n;
    memcpy(d->formats, fmts, (size_t)n);

    /*
     * Connector 0 only.
     *
     * GET_CONNECTORS returns an array and a device may carry several. This
     * driver takes the first, because IddCx models one monitor per adapter and
     * a second connector would need a second IddCx monitor with its own
     * swapchain. Not hard, but it is not the problem in front of us: no GUD
     * device in existence reports more than one.
     */
    n = t->ctrl_in(t->ctx, GUD_REQ_GET_CONNECTORS, 0,
                   &d->connector, sizeof d->connector);
    if (n < 0)
        return n;
    if ((size_t)n < sizeof d->connector)
        return GUD_E_PROTO;
    d->connector_index = 0;

    return gud_read_modes(d);
}

int gud_read_modes(struct gud_device *d)
{
    int n = d->t.ctrl_in(d->t.ctx, GUD_REQ_GET_CONNECTOR_MODES,
                         d->connector_index,
                         d->modes, sizeof d->modes);
    if (n < 0)
        return n;
    if ((size_t)n % sizeof(struct gud_display_mode_req))
        return GUD_E_PROTO;
    d->n_modes = (unsigned)((size_t)n / sizeof(struct gud_display_mode_req));
    return 0;
}

int gud_connector_status(struct gud_device *d, int *changed)
{
    uint8_t s = 0;
    int n = d->t.ctrl_in(d->t.ctx, GUD_REQ_GET_CONNECTOR_STATUS,
                         d->connector_index, &s, 1);
    if (n < 0)
        return n;
    if (n != 1)
        return GUD_E_IO;
    if (changed)
        *changed = (s & GUD_CONNECTOR_STATUS_CHANGED) ? 1 : 0;
    return (s & GUD_CONNECTOR_STATUS_CONNECTED_MASK) == GUD_CONNECTOR_STATUS_CONNECTED;
}

/* ---------------- modeset ---------------- */

int gud_set_state(struct gud_device *d,
                  const struct gud_display_mode_req *mode, uint8_t format)
{
    struct gud_state_req req;
    unsigned bpp = gud_format_bpp(format);
    int err;

    if (!bpp)
        return GUD_E_INVAL;
    if (!gud_has_format(d, format))
        return GUD_E_INVAL;

    memset(&req, 0, sizeof req);
    req.mode      = *mode;
    req.format    = format;
    req.connector = d->connector_index;

    err = gud_set(d, GUD_REQ_SET_STATE_CHECK, 0, &req, sizeof req);
    if (err)
        return err;

    err = gud_set(d, GUD_REQ_SET_STATE_COMMIT, 0, NULL, 0);
    if (err) {
        /* The device leaves its previous mode running on a failed commit and
         * says so. Mirror that: do not record a mode we are not driving. */
        return err;
    }

    d->format       = format;
    d->bpp          = bpp;
    d->active       = *mode;
    d->active_valid = 1;
    d->compress     = (d->desc.compression & GUD_COMPRESSION_LZ4) ? 1 : 0;
    return 0;
}

int gud_set_controller_enable(struct gud_device *d, int on)
{
    uint8_t v = on ? 1 : 0;
    return gud_set(d, GUD_REQ_SET_CONTROLLER_ENABLE, 0, &v, 1);
}

int gud_set_display_enable(struct gud_device *d, int on)
{
    uint8_t v = on ? 1 : 0;
    return gud_set(d, GUD_REQ_SET_DISPLAY_ENABLE, 0, &v, 1);
}

/* ---------------- pixels ---------------- */

int gud_flush_rect(struct gud_device *d,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const void *pixels, size_t len,
                   const void *compressed, size_t comp_len)
{
    struct gud_set_buffer_req req;
    const void *payload;
    size_t trlen;
    int err, n;

    if (!d->active_valid)
        return GUD_E_PROTO;
    if (x + w > d->active.hdisplay || y + h > d->active.vdisplay)
        return GUD_E_INVAL;
    if (len != (size_t)w * h * d->bpp)
        return GUD_E_INVAL;

    memset(&req, 0, sizeof req);
    req.x = x; req.y = y; req.width = w; req.height = h;
    req.length = (uint32_t)len;

    /*
     * Compression is decided per rect and declared in the header, so an
     * incompressible one costs nothing but the attempt. Send raw whenever the
     * compressor did not actually win -- the device has to expand anything
     * marked compressed, and paying 2 ms of ARM time to expand data that got
     * bigger is the worst of both.
     */
    if (compressed && comp_len && comp_len < len) {
        req.compression        = GUD_COMPRESSION_LZ4;
        req.compressed_length  = (uint32_t)comp_len;
        payload = compressed;
        trlen   = comp_len;
    } else {
        payload = pixels;
        trlen   = len;
    }

    /*
     * FULL_UPDATE devices take one rect per frame and infer it, so SET_BUFFER
     * is skipped. Nothing implements that today, but the flag exists and the
     * Linux driver honours it.
     */
    if (!(d->desc.flags & GUD_DISPLAY_FLAG_FULL_UPDATE)) {
        err = gud_set(d, GUD_REQ_SET_BUFFER, 0, &req, sizeof req);
        if (err)
            return err;
    }

    n = d->t.bulk_out(d->t.ctx, payload, trlen);
    if (n < 0)
        return n;
    if ((size_t)n != trlen)
        return GUD_E_IO;
    return 0;
}

/* ---------------- helpers ---------------- */

unsigned gud_format_bpp(uint8_t format)
{
    switch (format) {
    case GUD_PIXEL_FORMAT_RGB332:   return 1;
    case GUD_PIXEL_FORMAT_R8:       return 1;
    case GUD_PIXEL_FORMAT_RGB565:   return 2;
    case GUD_PIXEL_FORMAT_RGB888:   return 3;
    case GUD_PIXEL_FORMAT_XRGB8888: return 4;
    case GUD_PIXEL_FORMAT_ARGB8888: return 4;
    default:                        return 0;   /* R1, XRGB1111: sub-byte */
    }
}

int gud_has_format(const struct gud_device *d, uint8_t format)
{
    unsigned i;
    for (i = 0; i < d->n_formats; i++)
        if (d->formats[i] == format)
            return 1;
    return 0;
}

uint32_t gud_mode_refresh_mhz(const struct gud_display_mode_req *m)
{
    uint64_t num;
    uint32_t den;

    if (!m->htotal || !m->vtotal)
        return 0;

    /* clock is kHz; want mHz. kHz * 1e6 / (htotal*vtotal) = mHz. */
    num = (uint64_t)m->clock * 1000000ull;
    den = (uint32_t)m->htotal * (uint32_t)m->vtotal;

    if (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE)
        num *= 2;
    if (m->flags & GUD_DISPLAY_MODE_FLAG_DBLSCAN)
        num /= 2;

    return (uint32_t)((num + den / 2) / den);
}

const char *gud_strerror(int err)
{
    switch (err) {
    case 0:             return "ok";
    case GUD_E_IO:      return "transfer failed or short";
    case GUD_E_PROTO:   return "protocol error";
    case GUD_E_INVAL:   return "invalid parameter";
    case GUD_E_NOTSUP:  return "request not supported";
    case GUD_E_BUSY:    return "device busy";
    case GUD_E_NODEV:   return "not a GUD device (bad magic)";
    case GUD_E_NOSPACE: return "buffer too small";
    default:            return "transport error";
    }
}
