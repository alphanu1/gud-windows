/* SPDX-License-Identifier: MIT */
/*
 * convert.c -- desktop surface to wire format.
 *
 * IddCx hands over a DXGI surface, and in practice that is
 * DXGI_FORMAT_B8G8R8A8_UNORM: byte order B, G, R, A. Everything here reads
 * that and writes a tightly packed rect at width*bpp per row, which is what
 * gud_flush_rect() wants and what the device's blit expects.
 *
 * The source is strided -- a mapped staging texture's RowPitch is whatever the
 * driver chose and is never assumed to equal width*4 -- and the destination
 * never is. Getting that backwards produces a picture whose stride drifts
 * across every line, which is exactly the failure the device side saw when it
 * advertised RGB888 and a host packed at three bytes. It looks like a timing
 * fault and is not.
 */

#include "convert.h"
#include <string.h>

/*
 * RGB565.
 *
 * Straight truncation of the low bits rather than rounding. Rounding is
 * defensible on a panel and wrong here: the ladder is six bits a channel, so
 * red and blue arrive at the CRT with five bits of the six used and the sixth
 * dropped. Adding a rounding term shifts the whole ramp by half a step and
 * shows up as a black level that is not quite black. The greyscale ramp on the
 * device's own test card is the thing to check it against.
 */
void convert_bgra_to_rgb565(const uint8_t *src, size_t src_pitch,
                            uint8_t *dst,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t row, col;

    for (row = 0; row < h; row++) {
        const uint8_t *s = src + (size_t)(y + row) * src_pitch + (size_t)x * 4;
        uint8_t *d = dst + (size_t)row * w * 2;

        for (col = 0; col < w; col++, s += 4, d += 2) {
            uint16_t p = (uint16_t)(((uint16_t)(s[2] & 0xf8) << 8) |
                                    ((uint16_t)(s[1] & 0xfc) << 3) |
                                    ((uint16_t)(s[0]) >> 3));
            /* little endian on the wire, as DRM_FORMAT_RGB565 is */
            d[0] = (uint8_t)(p & 0xff);
            d[1] = (uint8_t)(p >> 8);
        }
    }
}

/* RGB332: 3-3-2, one byte. Halves everything at no CPU cost, and on sprite
 * content with a small palette it is less destructive than it sounds. */
void convert_bgra_to_rgb332(const uint8_t *src, size_t src_pitch,
                            uint8_t *dst,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t row, col;

    for (row = 0; row < h; row++) {
        const uint8_t *s = src + (size_t)(y + row) * src_pitch + (size_t)x * 4;
        uint8_t *d = dst + (size_t)row * w;

        for (col = 0; col < w; col++, s += 4, d++)
            *d = (uint8_t)((s[2] & 0xe0) | ((s[1] & 0xe0) >> 3) | (s[0] >> 6));
    }
}

/*
 * RGB888, three bytes, R G B in that order.
 *
 * The device does not offer this yet -- scanout_fetch.v cannot read three
 * bytes per pixel across a 64-bit beat boundary, which is its own M5 item --
 * so this is here for when it can, and for any other GUD device that does.
 * gud_probe() reads the format list and nothing calls this unless the device
 * asked for it.
 */
void convert_bgra_to_rgb888(const uint8_t *src, size_t src_pitch,
                            uint8_t *dst,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t row, col;

    for (row = 0; row < h; row++) {
        const uint8_t *s = src + (size_t)(y + row) * src_pitch + (size_t)x * 4;
        uint8_t *d = dst + (size_t)row * w * 3;

        for (col = 0; col < w; col++, s += 4, d += 3) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
    }
}

/* XRGB8888: the source layout unchanged, so this is a strided memcpy. */
void convert_bgra_to_xrgb8888(const uint8_t *src, size_t src_pitch,
                              uint8_t *dst,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t row;

    for (row = 0; row < h; row++)
        memcpy(dst + (size_t)row * w * 4,
               src + (size_t)(y + row) * src_pitch + (size_t)x * 4,
               (size_t)w * 4);
}

int convert_rect(uint8_t format,
                 const uint8_t *src, size_t src_pitch, uint8_t *dst,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    switch (format) {
    case GUD_PIXEL_FORMAT_RGB565:
        convert_bgra_to_rgb565(src, src_pitch, dst, x, y, w, h);   return 0;
    case GUD_PIXEL_FORMAT_RGB332:
        convert_bgra_to_rgb332(src, src_pitch, dst, x, y, w, h);   return 0;
    case GUD_PIXEL_FORMAT_RGB888:
        convert_bgra_to_rgb888(src, src_pitch, dst, x, y, w, h);   return 0;
    case GUD_PIXEL_FORMAT_XRGB8888:
        convert_bgra_to_xrgb8888(src, src_pitch, dst, x, y, w, h); return 0;
    default:
        return -1;
    }
}
