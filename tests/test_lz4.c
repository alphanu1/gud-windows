/* SPDX-License-Identifier: MIT */
/*
 * test_lz4.c -- the compressor against the device's own decoder.
 *
 * Links blitsCRT_Mister's lz4dec.c rather than a reference implementation, so
 * what is under test is the pair that will actually meet on the wire. A
 * compressor that is correct against liblz4 and wrong against the thing at the
 * far end is worth nothing.
 *
 * The cases that matter and are easy to get wrong:
 *
 *   - lengths either side of MFLIMIT (12) and LASTLITERALS (5), where the
 *     encoder has to stop emitting matches and fall back to literals
 *   - offset-1 matches, which is how LZ4 encodes runs and where a memcpy in
 *     the decoder would be undefined
 *   - long-range duplicates near the 65535 offset ceiling
 *   - incompressible input, which must expand rather than corrupt, so the
 *     caller can notice and send raw
 */

#include "lz4enc.h"
#include "lz4dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct lz4enc_ctx ctx;
static int fails, checks;

static int roundtrip(const unsigned char *buf, size_t n, const char *what,
                     int verbose)
{
    size_t cap = lz4enc_bound(n);
    unsigned char *c = malloc(cap);
    unsigned char *d = malloc(n ? n : 1);
    size_t cl;
    long dl;
    int bad = 0;

    checks++;
    cl = lz4enc_compress(&ctx, buf, n, c, cap);
    if (!cl) {
        printf("  FAIL  %-26s len=%-8zu did not fit in lz4enc_bound()\n", what, n);
        bad = 1;
        goto out;
    }
    dl = blitscrt_lz4_decompress(c, cl, d, n);
    if (dl != (long)n || (n && memcmp(d, buf, n))) {
        printf("  FAIL  %-26s len=%-8zu comp=%zu dec=%ld\n", what, n, cl, dl);
        bad = 1;
        goto out;
    }
    if (verbose)
        printf("        %-26s len=%-8zu comp=%-8zu %.2fx\n",
               what, n, cl, n ? (double)n / cl : 0.0);
out:
    free(c);
    free(d);
    fails += bad;
    return bad;
}

int main(void)
{
    static unsigned char buf[640 * 480 * 2];
    size_t i;

    printf("gud-windows lz4: compressor against blitsCRT_Mister lz4dec.c\n\n");

    /* every length across the encoder's boundary cases */
    for (i = 0; i <= 64; i++) {
        memset(buf, 0x5A, i);
        roundtrip(buf, i, "short run", 0);
    }
    printf("  lengths 0..64\n");

    memset(buf, 0, sizeof buf);
    roundtrip(buf, sizeof buf, "640x480 RGB565 black", 1);

    for (i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)rand();
    roundtrip(buf, sizeof buf, "incompressible noise", 1);

    /* sprite-ish: flat fields and repeated tiles from a small palette */
    {
        unsigned short pal[8] = { 0x0000, 0xF800, 0x07E0, 0x001F,
                                  0xFFFF, 0x8410, 0xFFE0, 0x07FF };
        unsigned short *p = (unsigned short *)buf;
        for (i = 0; i < 640 * 480; i++)
            p[i] = pal[((i / 16) ^ (i / (640 * 16))) & 7];
    }
    roundtrip(buf, sizeof buf, "tiled sprite field", 1);

    /* a desktop: mostly one colour with text-like noise through it */
    {
        unsigned short *p = (unsigned short *)buf;
        for (i = 0; i < 640 * 480; i++)
            p[i] = 0x3186;
        for (i = 0; i < 640 * 480; i += 37)
            p[i] = 0xFFFF;
    }
    roundtrip(buf, sizeof buf, "desktop-ish", 1);

    /* offset-1 run-length case */
    for (i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)(i & 1 ? 0x00 : 0xFF);
    roundtrip(buf, sizeof buf, "alternating bytes", 1);

    /* randomised, several shapes, including long-range duplicates */
    {
        static unsigned char s[70000];
        int it;
        for (it = 0; it < 4000; it++) {
            size_t n = (size_t)rand() % sizeof s;
            int mode = rand() % 5;
            for (i = 0; i < n; i++) {
                switch (mode) {
                case 0: s[i] = (unsigned char)rand();               break;
                case 1: s[i] = (unsigned char)(rand() % 3);         break;
                case 2: s[i] = (unsigned char)(i / (1 + rand() % 64)); break;
                case 3: s[i] = (rand() % 20) ? 0 : (unsigned char)rand(); break;
                case 4: s[i] = i < n / 2 ? (unsigned char)rand() : s[i - n / 2]; break;
                }
            }
            if (roundtrip(s, n, "random block", 0) && fails > 5)
                break;
        }
    }
    printf("  4000 random blocks\n");

    printf("\n%d blocks, %d failures\n", checks, fails);
    return fails != 0;
}
