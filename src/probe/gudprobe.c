/* SPDX-License-Identifier: MIT */
/*
 * gudprobe -- a GUD device on Windows, without IddCx anywhere near it.
 *
 * Build this first and get it working before writing a line of the driver.
 *
 * The reason is the same one that made the device side's bulk endpoint take so
 * long: when a display driver produces no picture there are half a dozen
 * layers that could be at fault and no way to tell them apart from the
 * outside. IddCx adds three more -- the UMDF host process, Session 0, and the
 * DXGI surface -- and debugging a driver that runs in Session 0 means WinDbg
 * and a second machine.
 *
 * Everything this tool does, the driver does identically. If gudprobe can
 * enumerate the device, set a mode and put a test card on the CRT, then the
 * transport, the protocol layer, the format conversion and the compressor are
 * all proved, and anything that goes wrong afterwards is IddCx. That is worth
 * a day.
 *
 *   gudprobe                     enumerate and dump everything
 *   gudprobe --testcard          modeset to the preferred mode and draw
 *   gudprobe --testcard --mode 2 the same, using mode index 2
 *   gudprobe --testcard --raw    same but with compression off
 *   gudprobe --testcard --grey   greyscale card: a monotonic staircase rather
 *                                than colour bars. Easier to confirm on a CRT
 *                                -- a step out of order is a geometry fault
 *                                and any tint means the channels are crossed
 *   gudprobe --bounce            move a rect around, to exercise damage
 *   gudprobe --bounce --frames 500   the same, but stop rather than run
 *                                until interrupted -- scriptable, and gives
 *                                a per-rect average without a ctrl-c
 *   gudprobe --modeline "..."    set an unadvertised modeline, Switchres-style
 *   gudprobe --ini blitscrt.ini  load a modeline store and list what it holds
 */

#include "gud_host.h"
#include "convert.h"
#include "lz4enc.h"
#include "modeline.h"
#include "winusb_transport.h"
#include "gud_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define VID 0x1d50
#define PID 0x614d

/*
 * A test card in BGRA, so it goes through exactly the conversion path the
 * driver's frame loop uses. Drawing straight into RGB565 would prove less.
 */
static void draw_testcard(uint8_t *bgra, size_t pitch, uint32_t w, uint32_t h,
                          int grey)
{
    static const uint8_t bars[8][3] = {   /* R, G, B */
        {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
        {255,0,255},   {255,0,0},   {0,0,255},   {0,0,0},
    };
    uint32_t x, y;

    for (y = 0; y < h; y++) {
        uint8_t *row = bgra + (size_t)y * pitch;

        for (x = 0; x < w; x++) {
            uint8_t r, g, b;
            unsigned bar = (x * 8) / w;

            if (y < h * 2 / 3) {                       /* bars */
                if (grey) {
                    /* A monotonic staircase, black at the left. Easier to
                     * read than colour on a CRT: a step out of order is a
                     * geometry fault, and any tint at all means the BGRA to
                     * RGB565 conversion has the channels crossed. */
                    r = g = b = (uint8_t)(bar * 255 / 7);
                } else {
                    const uint8_t *c = bars[bar];
                    r = c[0]; g = c[1]; b = c[2];
                }
            } else if (y < h * 3 / 4) {                /* half amplitude */
                if (grey) {
                    r = g = b = (uint8_t)(bar * 255 / 7 / 2);
                } else {
                    const uint8_t *c = bars[bar];
                    r = c[0] / 2; g = c[1] / 2; b = c[2] / 2;
                }
            } else {                                   /* 16-step ramp */
                uint8_t v = (uint8_t)(((x * 16) / w) * 17);
                r = g = b = v;
            }

            /* one-pixel border and a centre crosshair, to check the raster
             * really starts where the porches say it does */
            if (x == 0 || y == 0 || x == w - 1 || y == h - 1 ||
                x == w / 2 || y == h / 2)
                r = g = b = 255;

            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 255;
        }
    }
}

static int push_rect(struct gud_device *d, struct lz4enc_ctx *lz,
                     const uint8_t *bgra, size_t pitch,
                     uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     int use_lz4)
{
    size_t len = (size_t)w * h * d->bpp;
    uint8_t *conv = malloc(len);
    uint8_t *comp = NULL;
    size_t comp_len = 0;
    int err;

    if (!conv)
        return GUD_E_NOSPACE;

    if (convert_rect(d->format, bgra, pitch, conv, x, y, w, h) < 0) {
        free(conv);
        return GUD_E_INVAL;
    }

    if (use_lz4 && d->compress) {
        size_t cap = lz4enc_bound(len);
        comp = malloc(cap);
        if (comp)
            comp_len = lz4enc_compress(lz, conv, len, comp, cap);
    }

    err = gud_flush_rect(d, x, y, w, h, conv, len, comp, comp_len);

    free(comp);
    free(conv);
    return err;
}

int main(int argc, char **argv)
{
    char err[256];
    struct winusb_dev *wd;
    struct gud_transport t;
    static struct gud_device d;
    static struct lz4enc_ctx lz;
    static struct modeline_store store;
    int do_testcard = 0, do_bounce = 0, use_lz4 = 1, mode_index = -1, grey = 0;
    int max_frames = 0;
    const char *ini = NULL, *modeline_str = NULL;
    const struct gud_display_mode_req *use = NULL;
    struct gud_display_mode_req custom;
    uint8_t format;
    int i, rc;

    /*
     * Line-buffered, so --bounce's progress appears as it happens even when
     * stdout is a file or a pipe. Fully buffered is the default off a
     * redirected handle, and a diagnostic that loops until interrupted then
     * loses its whole buffer on the interrupt is no diagnostic at all.
     *
     * BUFSIZ rather than 0. glibc reads a size of 0 as "pick one for me", and
     * MSVC's setvbuf validates the argument instead and rejects it -- the
     * invalid-parameter handler calls __fastfail and the process dies with
     * 0xC0000409 before main gets any further, which looks like a stack
     * overrun and is not one.
     */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

    for (i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--testcard"))  do_testcard = 1;
        else if (!strcmp(argv[i], "--bounce"))    do_bounce = do_testcard = 1;
        else if (!strcmp(argv[i], "--raw"))       use_lz4 = 0;
        else if (!strcmp(argv[i], "--grey") ||
                 !strcmp(argv[i], "--gray"))      grey = 1;
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode_index = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ini")  && i + 1 < argc) ini = argv[++i];
        else if (!strcmp(argv[i], "--modeline") && i + 1 < argc) modeline_str = argv[++i];
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }

    wd = winusb_open(VID, PID, err, sizeof err);
    if (!wd) {
        fprintf(stderr, "open: %s\n", err);
        fprintf(stderr, "\nSee docs/BRINGUP.md -- the device declares a "
                        "vendor-class interface with no Microsoft OS\n"
                        "descriptors, so Windows will not bind WinUSB to it "
                        "on its own.\n");
        return 1;
    }
    winusb_transport(wd, &t);

    rc = gud_probe(&d, &t);
    if (rc) {
        fprintf(stderr, "probe: %s\n", gud_strerror(rc));
        winusb_close(wd);
        return 1;
    }
    gud_dump(stdout, &d);

    modeline_store_reset(&store);
    modeline_store_load_device(&store, &d);
    if (ini) {
        modeline_store_load_ini(&store, ini);
        printf("\nmodeline store (%u entries)\n", store.n);
        for (i = 0; i < (int)store.n; i++) {
            char line[256];
            modeline_format(&store.e[i].mode, store.e[i].name, line, sizeof line);
            printf("  %-8s %s\n", store.e[i].from_device ? "device" : "ini", line);
        }
    }

    if (!do_testcard && !modeline_str) {
        winusb_close(wd);
        return 0;
    }

    /* ---- pick a mode ---- */
    if (modeline_str) {
        char name[32];
        rc = modeline_parse(modeline_str, &custom, name, sizeof name);
        if (rc) {
            fprintf(stderr, "modeline: %s\n", gud_strerror(rc));
            winusb_close(wd);
            return 1;
        }
        use = &custom;
        printf("\nusing supplied modeline\n");
    } else if (mode_index >= 0 && (unsigned)mode_index < d.n_modes) {
        use = &d.modes[mode_index];
    } else {
        for (i = 0; i < (int)d.n_modes; i++)
            if (d.modes[i].flags & GUD_DISPLAY_MODE_FLAG_PREFERRED)
                use = &d.modes[i];
        if (!use && d.n_modes)
            use = &d.modes[0];
    }
    if (!use) {
        fprintf(stderr, "no mode to set\n");
        winusb_close(wd);
        return 1;
    }

    format = gud_has_format(&d, GUD_PIXEL_FORMAT_RGB565)
           ? GUD_PIXEL_FORMAT_RGB565 : d.formats[0];

    printf("\nmodeset %ux%u%s at %.3f Hz, %s%s\n",
           use->hdisplay, use->vdisplay,
           (use->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? "i" : "p",
           gud_mode_refresh_mhz(use) / 1000.0, gud_format_name(format),
           use_lz4 ? ", LZ4" : ", raw");

    rc = gud_set_state(&d, use, format);
    if (rc) {
        fprintf(stderr, "set_state: %s\n", gud_strerror(rc));
        winusb_close(wd);
        return 1;
    }

    rc = gud_set_controller_enable(&d, 1);
    if (rc) { fprintf(stderr, "controller enable: %s\n", gud_strerror(rc)); }
    rc = gud_set_display_enable(&d, 1);
    if (rc) { fprintf(stderr, "display enable: %s\n", gud_strerror(rc)); }

    /* ---- draw ---- */
    {
        uint32_t w = use->hdisplay, h = use->vdisplay;
        size_t pitch = (size_t)w * 4;
        uint8_t *bgra = malloc(pitch * h);
        LARGE_INTEGER f, a, b;

        if (!bgra) { winusb_close(wd); return 1; }
        draw_testcard(bgra, pitch, w, h, grey);

        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        rc = push_rect(&d, &lz, bgra, pitch, 0, 0, w, h, use_lz4);
        QueryPerformanceCounter(&b);

        if (rc)
            fprintf(stderr, "flush: %s\n", gud_strerror(rc));
        else
            printf("full surface: %.2f ms (%u bytes converted)\n",
                   (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart,
                   (unsigned)((size_t)w * h * d.bpp));

        /*
         * Damage. A moving rect on a static background is the case GUD is
         * built for and the one where the numbers diverge most from a full
         * surface, so it is worth seeing separately: if a full frame works and
         * this does not, the fault is in the rect arithmetic rather than
         * anywhere in the transport.
         */
        if (do_bounce && !rc) {
            uint32_t bw = 64, bh = 48, bx = 0, by = 0;
            int dx = 4, dy = 3, frames = 0;
            double total = 0;

            printf("bouncing a %ux%u rect, ctrl-c to stop\n", bw, bh);
            for (;;) {
                uint32_t x, y;

                for (y = 0; y < bh; y++)
                    for (x = 0; x < bw; x++) {
                        uint8_t *p = bgra + (size_t)(by + y) * pitch + (bx + x) * 4;
                        p[0] = 0x40; p[1] = 0xff; p[2] = 0x40; p[3] = 255;
                    }

                QueryPerformanceCounter(&a);
                rc = push_rect(&d, &lz, bgra, pitch, bx, by, bw, bh, use_lz4);
                QueryPerformanceCounter(&b);
                if (rc) { fprintf(stderr, "flush: %s\n", gud_strerror(rc)); break; }
                total += (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;

                /* repaint what we are about to leave */
                draw_testcard(bgra, pitch, w, h, grey);
                push_rect(&d, &lz, bgra, pitch, bx, by, bw, bh, use_lz4);

                bx = (uint32_t)((int)bx + dx);
                by = (uint32_t)((int)by + dy);
                if (bx + bw >= w) { bx = w - bw - 1; dx = -dx; }
                if (by + bh >= h) { by = h - bh - 1; dy = -dy; }
                if ((int)bx <= 0) { bx = 0; dx = -dx; }
                if ((int)by <= 0) { by = 0; dy = -dy; }

                if (++frames % 120 == 0)
                    printf("  %d rects, %.3f ms each\n", frames, total / frames);
                if (max_frames && frames >= max_frames) {
                    printf("  stopping after %d rects, %.3f ms each\n",
                           frames, total / frames);
                    break;
                }
                Sleep(8);
            }
        }
        free(bgra);
    }

    winusb_close(wd);
    return rc ? 1 : 0;
}
