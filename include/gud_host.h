/* SPDX-License-Identifier: MIT */
/*
 * gud_host.h -- host side of GUD, with no USB in it.
 *
 * The device side of this project keeps its protocol layer free of USB so the
 * whole control surface can run under test. The same split is worth having
 * here for a stronger reason: on Windows the transport appears twice. The
 * probe tool opens the device with WinUsb_Initialize and the driver reaches it
 * through a WDFUSBDEVICE, and those two have different calling conventions and
 * different error types. Everything above this seam is identical either way.
 *
 * A transport supplies three operations. Implement them and the rest of the
 * protocol is free.
 */
#ifndef GUD_HOST_H
#define GUD_HOST_H

#include "gud.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transport.
 *
 * ctrl_in/ctrl_out issue a vendor control transfer to the interface:
 *
 *     bmRequestType = 0x41 | (in ? 0x80 : 0)   vendor, recipient interface
 *     bRequest      = request
 *     wValue        = value       (connector index where one applies, else 0)
 *     wIndex        = bInterfaceNumber
 *
 * That is what drivers/gpu/drm/gud/gud_drv.c does in gud_usb_control_msg():
 * USB_TYPE_VENDOR (0x40) | USB_RECIP_INTERFACE (0x01), plus USB_DIR_IN.
 *
 * The recipient nibble is not decoration. 0x40 is recipient *device*, and on
 * Linux the gadget core routes by recipient before the function is reached: a
 * device-recipient vendor request goes to the composite setup() and is stalled
 * there, never arriving at the code that would have answered it. This was
 * written as 0x40 and documented as "recipient interface", which is what it
 * had to be, and the two disagreed until real hardware stalled every request
 * with ERROR_GEN_FAILURE. The loopback harness cannot catch it: it routes
 * gud_transport straight into the device's handler and no bmRequestType is
 * ever constructed.
 *
 * Return the number of bytes transferred, or negative on failure. A short
 * read is not an error at this level; the callers check lengths themselves.
 */
struct gud_transport {
    int (*ctrl_in )(void *ctx, uint8_t request, uint16_t value,
                    void *buf, size_t len);
    int (*ctrl_out)(void *ctx, uint8_t request, uint16_t value,
                    const void *buf, size_t len);
    int (*bulk_out)(void *ctx, const void *buf, size_t len);
    void *ctx;
};

/* Negative return codes. Distinct from a transport's own errors, which are
 * passed through unchanged when they are already negative. */
#define GUD_E_IO        (-1)
#define GUD_E_PROTO     (-2)
#define GUD_E_INVAL     (-3)
#define GUD_E_NOTSUP    (-4)
#define GUD_E_BUSY      (-5)
#define GUD_E_NODEV     (-6)
#define GUD_E_NOSPACE   (-7)

struct gud_device {
    struct gud_transport t;

    struct gud_display_descriptor_req desc;

    uint8_t  formats[GUD_FORMATS_MAX_NUM];
    unsigned n_formats;

    struct gud_connector_descriptor_req connector;
    uint8_t  connector_index;

    struct gud_display_mode_req modes[GUD_CONNECTOR_MAX_NUM_MODES];
    unsigned n_modes;

    /* Negotiated. Set by gud_set_state(). */
    uint8_t  format;
    unsigned bpp;                /* bytes per pixel of `format` on the wire */
    int      compress;           /* LZ4 in use for buffer transfers */
    struct gud_display_mode_req active;
    int      active_valid;
};

/*
 * Enumeration. Reads the display descriptor, the format list, connector 0 and
 * its mode list, in the order the Linux driver does. Fails if the magic is
 * wrong, which is the only reliable way to tell a GUD device from any other
 * vendor-class interface that happens to carry the same VID:PID.
 */
int gud_probe(struct gud_device *d, const struct gud_transport *t);

/* Re-read the mode list. Cheap, and the answer changes: a device raises
 * GUD_CONNECTOR_STATUS_CHANGED when it has modes it did not have before, which
 * is how a runtime-added mode becomes visible without re-enumerating USB. */
int gud_read_modes(struct gud_device *d);

/* Returns 1 if the connector reported CHANGED since the last call. */
int gud_connector_status(struct gud_device *d, int *changed);

/*
 * Modeset. Sends SET_STATE_CHECK with the full modeline, then SET_STATE_COMMIT.
 *
 * The whole reason a Windows GUD driver is worth building sits in this one
 * call. `mode` carries porches, and nothing in IddCx does. Where the porches
 * come from is modeline.h's problem.
 */
int gud_set_state(struct gud_device *d,
                  const struct gud_display_mode_req *mode, uint8_t format);

int gud_set_controller_enable(struct gud_device *d, int on);
int gud_set_display_enable(struct gud_device *d, int on);

/*
 * Flush one rectangle. `pixels` is already in the negotiated wire format and
 * tightly packed at width*bpp per row -- conversion is convert.h's job, not
 * this one. If `compressed` is non-NULL it is LZ4 block data for the same
 * rect and `comp_len` its length.
 *
 * Sends SET_BUFFER, reads status if the device asked for STATUS_ON_SET, then
 * the bulk payload.
 */
int gud_flush_rect(struct gud_device *d,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const void *pixels, size_t len,
                   const void *compressed, size_t comp_len);

/* Bytes per pixel of a GUD format on the wire. 0 for formats we do not send. */
unsigned gud_format_bpp(uint8_t format);

/* True if the device advertised `format` in GET_FORMATS. */
int gud_has_format(const struct gud_device *d, uint8_t format);

/* Vertical refresh in millihertz, computed from the modeline the way DRM
 * does it: interlaced doubles, doublescan halves. Windows wants this number
 * and the device does not carry it. */
uint32_t gud_mode_refresh_mhz(const struct gud_display_mode_req *m);

const char *gud_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* GUD_HOST_H */
