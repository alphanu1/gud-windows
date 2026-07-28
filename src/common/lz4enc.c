/* SPDX-License-Identifier: MIT */
/*
 * lz4enc.c -- LZ4 block compression, just enough for GUD.
 *
 * Carried here rather than linking liblz4, for the same reason the device
 * carries its own decoder: this is the only compression either end will ever
 * need, an IddCx driver has to ship as a self-contained package, and a driver
 * that pulls in a third-party library acquires that library's servicing
 * problem. The block format is small enough to hold in your head.
 *
 * Block format only. No frame header, no magic, no checksum -- GUD says how
 * many bytes follow in gud_set_buffer_req and the device expands exactly that.
 * Emitting a frame-format stream here would decode as garbage at the far end,
 * because the decoder would read the frame magic as a token.
 *
 * The algorithm is the ordinary greedy single-table one. Deliberately not the
 * high-compression variant: this runs once per damage rect at 60 Hz on the
 * host, and the measured ratio on this content is already well past what the
 * link needs. On real traffic the device side measured 2.58x against the 1.2x
 * the bandwidth arithmetic required, so spending host CPU to push that higher
 * buys nothing -- the frame is already inside budget and the CRT is the thing
 * setting the pace.
 *
 * Every write is bounds-checked and the function returns 0 rather than
 * overflowing. A 0 means "did not fit", and the caller sends the rect raw,
 * which is always legal: compression is declared per rect in the header.
 */

#include "lz4enc.h"
#include <string.h>

#define MINMATCH        4
#define MFLIMIT         12      /* last match must start this far from the end */
#define LASTLITERALS    5       /* a block ends in at least this many literals */
#define LZ4_MIN_LENGTH  (MFLIMIT + 1)
#define MAX_DISTANCE    65535
#define ML_BITS         4
#define ML_MASK         ((1u << ML_BITS) - 1)
#define RUN_BITS        (8 - ML_BITS)
#define RUN_MASK        ((1u << RUN_BITS) - 1)
#define SKIPSTRENGTH    6

static uint32_t read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static void write16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static uint32_t hash_pos(const uint8_t *p)
{
    return (read32(p) * 2654435761u) >> (32 - LZ4ENC_HASHLOG);
}

static size_t count_match(const uint8_t *a, const uint8_t *b,
                          const uint8_t *limit)
{
    const uint8_t *start = a;
    while (a < limit && *a == *b) { a++; b++; }
    return (size_t)(a - start);
}

size_t lz4enc_bound(size_t src_len)
{
    /* Worst case: every byte a literal, plus one token per 255. */
    return src_len + src_len / 255 + 16;
}

size_t lz4enc_compress(struct lz4enc_ctx *ctx,
                       const void *src, size_t src_len,
                       void *dst, size_t dst_cap)
{
    const uint8_t *const base   = (const uint8_t *)src;
    const uint8_t *const iend   = base + src_len;
    const uint8_t *const mflimit   = iend - MFLIMIT;
    const uint8_t *const matchlimit = iend - LASTLITERALS;
    const uint8_t *ip = base;
    const uint8_t *anchor = base;
    uint8_t *const ostart = (uint8_t *)dst;
    uint8_t *const oend = ostart + dst_cap;
    uint8_t *op = ostart;
    uint8_t *token = NULL;
    const uint8_t *match;
    size_t last_run;

    if (!ctx || !src || !dst)
        return 0;

    /*
     * The table holds positions into *this* buffer, so it has to be cleared
     * every call. Leaving stale positions from the previous frame would not
     * corrupt anything -- a candidate match is always verified with a 32-bit
     * compare before it is used -- but it would produce matches pointing
     * outside the current block, and the distance check would then reject
     * almost all of them. Slower and worse, silently.
     */
    memset(ctx->table, 0, sizeof ctx->table);

    if (src_len < LZ4_MIN_LENGTH)
        goto last_literals;

    ctx->table[hash_pos(ip)] = 0;
    ip++;

    for (;;) {
        /* ---- find a match ---- */
        {
            const uint8_t *forward = ip;
            unsigned search = 1u << SKIPSTRENGTH;
            unsigned step = 1;

            for (;;) {
                uint32_t h;

                ip = forward;
                forward += step;
                step = (search++ >> SKIPSTRENGTH);

                if (forward > mflimit)
                    goto last_literals;

                h = hash_pos(ip);
                match = base + ctx->table[h];
                ctx->table[h] = (uint32_t)(ip - base);

                if ((size_t)(ip - match) <= MAX_DISTANCE &&
                    read32(match) == read32(ip))
                    break;
            }
        }

        /* ---- back up over bytes that also match ---- */
        while (ip > anchor && match > base && ip[-1] == match[-1]) {
            ip--;
            match--;
        }

        /* ---- literals ---- */
        {
            size_t lit = (size_t)(ip - anchor);

            if (op + 1 + lit + (lit / 255) + 2 + 1 + LASTLITERALS > oend)
                return 0;

            token = op++;
            if (lit >= RUN_MASK) {
                size_t n = lit - RUN_MASK;
                *token = (uint8_t)(RUN_MASK << ML_BITS);
                for (; n >= 255; n -= 255)
                    *op++ = 255;
                *op++ = (uint8_t)n;
            } else {
                *token = (uint8_t)(lit << ML_BITS);
            }
            memcpy(op, anchor, lit);
            op += lit;
        }

    next_match:
        /* ---- offset ---- */
        write16le(op, (uint16_t)(ip - match));
        op += 2;

        /* ---- match length ---- */
        {
            size_t ml = count_match(ip + MINMATCH, match + MINMATCH, matchlimit);
            ip += MINMATCH + ml;

            if (op + 1 + (ml / 255) + LASTLITERALS > oend)
                return 0;

            if (ml >= ML_MASK) {
                size_t n = ml - ML_MASK;
                *token |= (uint8_t)ML_MASK;
                for (; n >= 255; n -= 255)
                    *op++ = 255;
                *op++ = (uint8_t)n;
            } else {
                *token |= (uint8_t)ml;
            }
        }

        anchor = ip;
        if (ip > mflimit)
            break;

        /* Index the position two back. It was skipped by the match and is
         * often the start of the next one. */
        ctx->table[hash_pos(ip - 2)] = (uint32_t)(ip - 2 - base);

        /* ---- try to continue straight into another match ---- */
        {
            uint32_t h = hash_pos(ip);
            match = base + ctx->table[h];
            ctx->table[h] = (uint32_t)(ip - base);

            if ((size_t)(ip - match) <= MAX_DISTANCE &&
                read32(match) == read32(ip)) {
                if (op + 1 > oend)
                    return 0;
                token = op++;
                *token = 0;          /* no literals between the two matches */
                goto next_match;
            }
        }

        /* anchor stays where the last match ended; the next search starts one
         * byte on, so that byte becomes a literal if a match is found later.
         * This is the ordinary LZ4 arrangement and getting it wrong costs
         * ratio rather than correctness, which makes it hard to notice. */
        ip++;
    }

last_literals:
    last_run = (size_t)(iend - anchor);

    if (op + 1 + last_run + (last_run + 255 - RUN_MASK) / 255 > oend)
        return 0;

    if (last_run >= RUN_MASK) {
        size_t n = last_run - RUN_MASK;
        *op++ = (uint8_t)(RUN_MASK << ML_BITS);
        for (; n >= 255; n -= 255)
            *op++ = 255;
        *op++ = (uint8_t)n;
    } else {
        *op++ = (uint8_t)(last_run << ML_BITS);
    }
    memcpy(op, anchor, last_run);
    op += last_run;

    return (size_t)(op - ostart);
}
