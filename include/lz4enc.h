/* SPDX-License-Identifier: MIT */
#ifndef LZ4ENC_H
#define LZ4ENC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 16-bit hash table, 256 KB. Larger than LZ4's default for a reason: the
 * buffers here are whole 640x480 surfaces at 600 KB and a smaller table
 * collides badly on flat colour fields, which is most of what sprite content
 * is. The table is cleared per call, so this is also a 256 KB memset per rect
 * -- tens of microseconds against a 16.7 ms frame, and the ratio is worth it.
 */
#define LZ4ENC_HASHLOG 16

struct lz4enc_ctx {
    uint32_t table[1u << LZ4ENC_HASHLOG];
};

/* Worst-case output size for an incompressible input. */
size_t lz4enc_bound(size_t src_len);

/*
 * Compress one block. Returns the compressed length, or 0 if it did not fit
 * in dst_cap -- send the rect raw in that case, which is always legal.
 *
 * `ctx` is caller-allocated so the driver can keep one per swapchain thread
 * rather than putting 256 KB on the stack.
 */
size_t lz4enc_compress(struct lz4enc_ctx *ctx,
                       const void *src, size_t src_len,
                       void *dst, size_t dst_cap);

#ifdef __cplusplus
}
#endif

#endif
