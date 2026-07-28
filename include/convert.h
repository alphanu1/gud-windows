/* SPDX-License-Identifier: MIT */
#ifndef CONVERT_H
#define CONVERT_H

#include "gud.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * All of these read BGRA8888 at `src_pitch` bytes per row and write a tightly
 * packed rect, w*bpp per row, with no padding. `dst` must hold w*h*bpp bytes.
 */
void convert_bgra_to_rgb565  (const uint8_t *src, size_t src_pitch, uint8_t *dst,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void convert_bgra_to_rgb332  (const uint8_t *src, size_t src_pitch, uint8_t *dst,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void convert_bgra_to_rgb888  (const uint8_t *src, size_t src_pitch, uint8_t *dst,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void convert_bgra_to_xrgb8888(const uint8_t *src, size_t src_pitch, uint8_t *dst,
                              uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Dispatch on a GUD format code. -1 if the format is not one we can produce. */
int convert_rect(uint8_t format,
                 const uint8_t *src, size_t src_pitch, uint8_t *dst,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif
