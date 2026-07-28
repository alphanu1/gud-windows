/* SPDX-License-Identifier: MIT */
/*
 * gud.h -- Generic USB Display protocol, host side.
 *
 * Derived from include/drm/gud.h in the Linux kernel:
 *
 *     Copyright 2020 Noralf Tronnes
 *     SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * The structures are wire format and must not be padded. GCC's
 * __attribute__((packed)) has no MSVC equivalent, so #pragma pack is used and
 * every structure is size-asserted below. If one of those asserts fires the
 * compiler has laid the structure out differently from every other GUD
 * implementation and nothing will work.
 */
#ifndef GUD_H
#define GUD_H

#include <stdint.h>

#if defined(_MSC_VER)
#  define GUD_PACK_PUSH __pragma(pack(push, 1))
#  define GUD_PACK_POP  __pragma(pack(pop))
#  define GUD_PACKED
#else
#  define GUD_PACK_PUSH
#  define GUD_PACK_POP
#  define GUD_PACKED __attribute__((packed))
#endif

#define GUD_DISPLAY_MAGIC               0x1d50614du

GUD_PACK_PUSH

struct gud_display_descriptor_req {
    uint32_t magic;
    uint8_t  version;
    uint32_t flags;
    uint8_t  compression;
    uint32_t max_buffer_size;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
} GUD_PACKED;

struct gud_property_req {
    uint16_t prop;
    uint64_t val;
} GUD_PACKED;

struct gud_display_mode_req {
    uint32_t clock;             /* kHz */
    uint16_t hdisplay, hsync_start, hsync_end, htotal;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal;
    uint32_t flags;
} GUD_PACKED;

struct gud_connector_descriptor_req {
    uint8_t  connector_type;
    uint32_t flags;
} GUD_PACKED;

struct gud_set_buffer_req {
    uint32_t x, y, width, height;
    uint32_t length;
    uint8_t  compression;
    uint32_t compressed_length;
} GUD_PACKED;

struct gud_state_req {
    struct gud_display_mode_req mode;
    uint8_t format;
    uint8_t connector;
    /* struct gud_property_req properties[]; */
} GUD_PACKED;

GUD_PACK_POP

#define GUD_DISPLAY_FLAG_STATUS_ON_SET  (1u << 0)
#define GUD_DISPLAY_FLAG_FULL_UPDATE    (1u << 1)
#define GUD_COMPRESSION_LZ4             (1u << 0)

#define GUD_DISPLAY_MODE_FLAG_PHSYNC    (1u << 0)
#define GUD_DISPLAY_MODE_FLAG_NHSYNC    (1u << 1)
#define GUD_DISPLAY_MODE_FLAG_PVSYNC    (1u << 2)
#define GUD_DISPLAY_MODE_FLAG_NVSYNC    (1u << 3)
#define GUD_DISPLAY_MODE_FLAG_INTERLACE (1u << 4)
#define GUD_DISPLAY_MODE_FLAG_DBLSCAN   (1u << 5)
#define GUD_DISPLAY_MODE_FLAG_CSYNC     (1u << 6)
#define GUD_DISPLAY_MODE_FLAG_PREFERRED (1u << 10)

#define GUD_CONNECTOR_TYPE_PANEL        0
#define GUD_CONNECTOR_TYPE_VGA          1
#define GUD_CONNECTOR_TYPE_COMPOSITE    2
#define GUD_CONNECTOR_TYPE_SVIDEO       3
#define GUD_CONNECTOR_TYPE_COMPONENT    4
#define GUD_CONNECTOR_TYPE_DVI          5
#define GUD_CONNECTOR_TYPE_DISPLAYPORT  6
#define GUD_CONNECTOR_TYPE_HDMI         7

#define GUD_CONNECTOR_FLAGS_POLL_STATUS (1u << 0)
#define GUD_CONNECTOR_FLAGS_INTERLACE   (1u << 1)
#define GUD_CONNECTOR_FLAGS_DOUBLESCAN  (1u << 2)

/* control requests */
#define GUD_REQ_GET_STATUS                      0x00
#define GUD_STATUS_OK                           0x00
#define GUD_STATUS_BUSY                         0x01
#define GUD_STATUS_REQUEST_NOT_SUPPORTED        0x02
#define GUD_STATUS_PROTOCOL_ERROR               0x03
#define GUD_STATUS_INVALID_PARAMETER            0x04
#define GUD_STATUS_ERROR                        0x05

#define GUD_REQ_GET_DESCRIPTOR                  0x01
#define GUD_REQ_GET_FORMATS                     0x40
#define GUD_FORMATS_MAX_NUM                     32
#define GUD_PIXEL_FORMAT_R1                     0x01
#define GUD_PIXEL_FORMAT_R8                     0x08
#define GUD_PIXEL_FORMAT_XRGB1111               0x20
#define GUD_PIXEL_FORMAT_RGB332                 0x30
#define GUD_PIXEL_FORMAT_RGB565                 0x40
#define GUD_PIXEL_FORMAT_RGB888                 0x50
#define GUD_PIXEL_FORMAT_XRGB8888               0x80
#define GUD_PIXEL_FORMAT_ARGB8888               0x81

#define GUD_REQ_GET_PROPERTIES                  0x41
#define GUD_REQ_GET_CONNECTORS                  0x50
#define GUD_REQ_GET_CONNECTOR_PROPERTIES        0x51
#define GUD_REQ_GET_CONNECTOR_TV_MODE_VALUES    0x52
#define GUD_REQ_SET_CONNECTOR_FORCE_DETECT      0x53

#define GUD_REQ_GET_CONNECTOR_STATUS            0x54
#define GUD_CONNECTOR_STATUS_DISCONNECTED       0x00
#define GUD_CONNECTOR_STATUS_CONNECTED          0x01
#define GUD_CONNECTOR_STATUS_UNKNOWN            0x02
#define GUD_CONNECTOR_STATUS_CONNECTED_MASK     0x03
#define GUD_CONNECTOR_STATUS_CHANGED            (1u << 7)

#define GUD_REQ_GET_CONNECTOR_MODES             0x55
#define GUD_CONNECTOR_MAX_NUM_MODES             128
#define GUD_REQ_GET_CONNECTOR_EDID              0x56
#define GUD_REQ_SET_BUFFER                      0x60
#define GUD_REQ_SET_STATE_CHECK                 0x61
#define GUD_REQ_SET_STATE_COMMIT                0x62
#define GUD_REQ_SET_CONTROLLER_ENABLE           0x63
#define GUD_REQ_SET_DISPLAY_ENABLE              0x64

/*
 * Wire layout. These are the sizes the Linux driver and every device
 * implementation agree on; a mismatch means the compiler padded something.
 */
#define GUD_CAT_(a, b) a##b
#define GUD_CAT(a, b)  GUD_CAT_(a, b)

#if defined(__cplusplus) && __cplusplus >= 201103L
#  define GUD_SASSERT(c, m) static_assert(c, m)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define GUD_SASSERT(c, m) _Static_assert(c, m)
#else
/* MSVC in its default C mode defines no __STDC_VERSION__ and has no
 * _Static_assert, so this fallback is the path it takes -- which makes it worth
 * getting right. The concatenation needs the extra level of indirection: with
 * a bare `gud_sassert_##__LINE__` the preprocessor pastes before expanding and
 * every assertion below declares the same typedef name. Six identical typedefs
 * are legal in C11 and an error in C89, so it would compile here and fail on
 * the compiler this file exists for. */
#  define GUD_SASSERT(c, m) typedef char GUD_CAT(gud_sassert_, __LINE__)[(c) ? 1 : -1]
#endif

GUD_SASSERT(sizeof(struct gud_display_descriptor_req)  == 30, "descriptor padded");
GUD_SASSERT(sizeof(struct gud_display_mode_req)        == 24, "mode padded");
GUD_SASSERT(sizeof(struct gud_connector_descriptor_req) == 5, "connector padded");
GUD_SASSERT(sizeof(struct gud_set_buffer_req)          == 25, "set_buffer padded");
GUD_SASSERT(sizeof(struct gud_state_req)               == 26, "state padded");
GUD_SASSERT(sizeof(struct gud_property_req)            == 10, "property padded");

#endif /* GUD_H */
