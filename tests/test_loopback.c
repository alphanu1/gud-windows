/* SPDX-License-Identifier: MIT */
/*
 * test_loopback.c -- the host protocol layer against a real device.
 *
 * Not a mock. This links blitsCRT_Mister's own device.c, running headless
 * (fabric NULL), and routes gud_transport straight into blitscrt_handle_ctrl.
 * So both protocol implementations are under test at once, and a disagreement
 * about structure layout, request codes or the status handshake shows up here
 * rather than on a CRT.
 *
 * The bulk endpoint is checked for length and content only -- there is no
 * fabric to blit into -- but that is the part the host is responsible for.
 *
 * Build and run it with:
 *
 *     make test DEVICE=../blitsCRT_Mister/sw
 *
 */

#include "gud_host.h"
#include "modeline.h"
#include "convert.h"
#include "lz4enc.h"

/*
 * Both projects carry their own copy of the MIT protocol header and they
 * define the same structures under different include guards. Suppress the
 * device's copy so this file compiles, which means the device code below is
 * built against *our* definitions -- so if the two copies have drifted, the
 * loopback runs on ours and every field check in here is really a check that
 * they still agree.
 */
#define BLITSCRT_GUD_H
#include "device.h"        /* blitsCRT_Mister */
#include "lz4dec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

#define CHECK(cond, ...) do {                       \
        checks++;                                   \
        if (!(cond)) {                              \
            fails++;                                \
            printf("  FAIL  ");                     \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

/* ---------------- the loopback transport ---------------- */

struct loop {
    struct blitscrt_dev dev;
    unsigned char last_bulk[1024 * 1024];
    size_t        last_bulk_len;
    unsigned      n_bulk;
    unsigned      n_ctrl_in, n_ctrl_out, n_status;
};

static int lb_ctrl_in(void *ctx, uint8_t r, uint16_t v, void *buf, size_t len)
{
    struct loop *L = ctx;
    int n;

    L->n_ctrl_in++;
    if (r == GUD_REQ_GET_STATUS)
        L->n_status++;

    n = blitscrt_handle_ctrl(&L->dev, r, v, 0, NULL, 0, buf, len);
    return n < 0 ? GUD_E_IO : n;
}

static int lb_ctrl_out(void *ctx, uint8_t r, uint16_t v, const void *buf, size_t len)
{
    struct loop *L = ctx;
    unsigned char scratch[512];
    int n;

    L->n_ctrl_out++;
    n = blitscrt_handle_ctrl(&L->dev, r, v, 0, buf, (uint16_t)len,
                             scratch, sizeof scratch);
    /*
     * A control OUT that the device refuses is a stall on the wire, but the
     * device sets last_status regardless and the host is expected to find out
     * from GET_STATUS. That is the point of STATUS_ON_SET. Report the transfer
     * as having happened, exactly as the hardware would.
     */
    (void)n;
    return (int)len;
}

static int lb_bulk_out(void *ctx, const void *buf, size_t len)
{
    struct loop *L = ctx;

    L->n_bulk++;
    if (len > sizeof L->last_bulk)
        return GUD_E_IO;
    memcpy(L->last_bulk, buf, len);
    L->last_bulk_len = len;
    return (int)len;
}

/* ---------------- tests ---------------- */

static struct loop L;
static struct gud_device host;
static struct lz4enc_ctx lz;

static void t_probe(void)
{
    struct gud_transport t = { lb_ctrl_in, lb_ctrl_out, lb_bulk_out, &L };
    int rc;

    blitscrt_dev_init(&L.dev, NULL);

    rc = gud_probe(&host, &t);
    CHECK(rc == 0, "probe: %s", gud_strerror(rc));
    CHECK(host.desc.magic == GUD_DISPLAY_MAGIC, "magic 0x%08x", host.desc.magic);
    CHECK(host.desc.version == 1, "version %u", host.desc.version);
    CHECK(host.desc.flags & GUD_DISPLAY_FLAG_STATUS_ON_SET, "STATUS_ON_SET not set");
    CHECK(host.desc.compression & GUD_COMPRESSION_LZ4, "LZ4 not offered");

    CHECK(host.n_formats == 2, "formats %u", host.n_formats);
    CHECK(gud_has_format(&host, GUD_PIXEL_FORMAT_RGB565), "no RGB565");
    CHECK(gud_has_format(&host, GUD_PIXEL_FORMAT_RGB332), "no RGB332");
    CHECK(!gud_has_format(&host, GUD_PIXEL_FORMAT_RGB888),
          "RGB888 offered -- the device cannot read three bytes per pixel");

    CHECK(host.connector.connector_type == GUD_CONNECTOR_TYPE_VGA,
          "connector type %u", host.connector.connector_type);
    CHECK(host.connector.flags & GUD_CONNECTOR_FLAGS_INTERLACE,
          "INTERLACE not set -- a host will not offer 480i at all");

    CHECK(host.n_modes == 4, "modes %u", host.n_modes);
}

static void t_modes(void)
{
    /* The four the daemon advertises, with the numbers the roadmap states. */
    struct { uint32_t clk, w, h; int inter; double hz, line_khz; } want[] = {
        { 12600, 640, 480, 1, 60.00, 15.750 },
        { 12500, 640, 576, 1, 50.00, 15.625 },
        {  6300, 320, 240, 0, 60.11, 15.750 },
        {  6250, 320, 288, 0, 50.08, 15.625 },
    };
    unsigned i;

    for (i = 0; i < 4 && i < host.n_modes; i++) {
        const struct gud_display_mode_req *m = &host.modes[i];
        double hz = gud_mode_refresh_mhz(m) / 1000.0;
        double line = (double)m->clock / m->htotal;
        int inter = (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? 1 : 0;

        CHECK(m->clock == want[i].clk, "mode %u clock %u want %u",
              i, m->clock, want[i].clk);
        CHECK(m->hdisplay == want[i].w && m->vdisplay == want[i].h,
              "mode %u geometry %ux%u", i, m->hdisplay, m->vdisplay);
        CHECK(inter == want[i].inter, "mode %u interlace %d", i, inter);
        CHECK(hz > want[i].hz - 0.02 && hz < want[i].hz + 0.02,
              "mode %u refresh %.3f want %.2f", i, hz, want[i].hz);
        CHECK(line > want[i].line_khz - 0.01 && line < want[i].line_khz + 0.01,
              "mode %u line %.3f kHz want %.3f", i, line, want[i].line_khz);
    }

    CHECK(host.modes[0].flags & GUD_DISPLAY_MODE_FLAG_PREFERRED,
          "640x480i60 is not flagged PREFERRED");
}

static void t_modeset(void)
{
    int rc;

    rc = gud_set_state(&host, &host.modes[0], GUD_PIXEL_FORMAT_RGB565);
    CHECK(rc == 0, "set_state: %s", gud_strerror(rc));
    CHECK(host.active_valid, "active not recorded");
    CHECK(host.bpp == 2, "bpp %u", host.bpp);

    rc = gud_set_controller_enable(&host, 1);
    CHECK(rc == 0, "controller enable: %s", gud_strerror(rc));
    rc = gud_set_display_enable(&host, 1);
    CHECK(rc == 0, "display enable: %s", gud_strerror(rc));

    /* A format the device does not offer must be refused here, not sent. */
    rc = gud_set_state(&host, &host.modes[0], GUD_PIXEL_FORMAT_XRGB8888);
    CHECK(rc == GUD_E_INVAL, "XRGB8888 accepted (%s)", gud_strerror(rc));

    /* An unadvertised but legal modeline: the Switchres case. 384x224p60,
     * a CPS2/Neo Geo timing, never in the advertised list. */
    {
        struct gud_display_mode_req m;
        char name[32];
        rc = modeline_parse("ModeLine \"384x224@60\" 7.560 384 400 432 480 "
                            "224 227 230 262 -hsync -vsync", &m, name, sizeof name);
        /* 7.560 MHz over an htotal of 480 is 15.750 kHz, inside the band.
         * 6.700 was tried first and the device refused it -- 13.958 kHz, well
         * under the 15.0 kHz floor. Worth leaving noted: a modeline that looks
         * right and is out of band is the exact thing profile_enforce and
         * mode_check exist to stop, and it is easy to write. */
        CHECK(rc == 0, "parse unadvertised modeline: %s", gud_strerror(rc));
        rc = gud_set_state(&host, &m, GUD_PIXEL_FORMAT_RGB565);
        CHECK(rc == 0, "unadvertised modeline refused: %s", gud_strerror(rc));
    }

    /* Something a 15 kHz CRT will not survive must be refused by the device
     * and reported as a failure, not silently accepted. */
    {
        struct gud_display_mode_req m;
        rc = modeline_parse("148.500 1920 2008 2052 2200 1080 1084 1089 1125 "
                            "+hsync +vsync", &m, NULL, 0);
        CHECK(rc == 0, "parse 1080p: %s", gud_strerror(rc));
        rc = gud_set_state(&host, &m, GUD_PIXEL_FORMAT_RGB565);
        CHECK(rc == GUD_E_INVAL, "1080p60 was accepted (%s)", gud_strerror(rc));
    }

    /* Back to a mode we can flush against. */
    rc = gud_set_state(&host, &host.modes[0], GUD_PIXEL_FORMAT_RGB565);
    CHECK(rc == 0, "restore mode: %s", gud_strerror(rc));
}

static void t_flush(void)
{
    static uint8_t bgra[640 * 480 * 4];
    static uint8_t conv[640 * 480 * 2];
    static uint8_t comp[640 * 480 * 2 + 65536];
    static uint8_t back[640 * 480 * 2];
    size_t len, clen;
    unsigned i;
    int rc;

    for (i = 0; i < sizeof bgra; i++)
        bgra[i] = (uint8_t)((i * 7) >> 3);

    /* full surface, raw */
    len = 640u * 480u * 2u;
    convert_rect(GUD_PIXEL_FORMAT_RGB565, bgra, 640 * 4, conv, 0, 0, 640, 480);
    rc = gud_flush_rect(&host, 0, 0, 640, 480, conv, len, NULL, 0);
    CHECK(rc == 0, "full flush raw: %s", gud_strerror(rc));
    CHECK(L.last_bulk_len == len, "raw bulk len %zu want %zu", L.last_bulk_len, len);
    CHECK(memcmp(L.last_bulk, conv, len) == 0, "raw bulk content differs");

    /* full surface, compressed -- and the device's own decoder gets it back */
    clen = lz4enc_compress(&lz, conv, len, comp, sizeof comp);
    CHECK(clen > 0 && clen < len, "compress produced %zu from %zu", clen, len);
    rc = gud_flush_rect(&host, 0, 0, 640, 480, conv, len, comp, clen);
    CHECK(rc == 0, "full flush lz4: %s", gud_strerror(rc));
    CHECK(L.last_bulk_len == clen, "lz4 bulk len %zu want %zu", L.last_bulk_len, clen);
    {
        long d = blitscrt_lz4_decompress(L.last_bulk, L.last_bulk_len,
                                         back, sizeof back);
        CHECK(d == (long)len, "device decoder returned %ld want %zu", d, len);
        CHECK(d == (long)len && memcmp(back, conv, len) == 0,
              "device decoder produced different pixels");
    }

    /* a damage rect, off-origin, odd size */
    {
        uint32_t x = 37, y = 91, w = 113, h = 67;
        size_t rl = (size_t)w * h * 2;
        convert_rect(GUD_PIXEL_FORMAT_RGB565, bgra, 640 * 4, conv, x, y, w, h);
        rc = gud_flush_rect(&host, x, y, w, h, conv, rl, NULL, 0);
        CHECK(rc == 0, "damage rect: %s", gud_strerror(rc));
        CHECK(L.last_bulk_len == rl, "rect bulk len %zu want %zu",
              L.last_bulk_len, rl);
    }

    /* a rect off the edge must be refused by the host, before the wire */
    rc = gud_flush_rect(&host, 600, 0, 100, 10, conv, 100u * 10u * 2u, NULL, 0);
    CHECK(rc == GUD_E_INVAL, "off-edge rect accepted (%s)", gud_strerror(rc));

    /* a wrong length must be refused too -- this is the fault that
     * desynchronises the bulk stream with nothing to recover against */
    rc = gud_flush_rect(&host, 0, 0, 64, 64, conv, 64u * 64u * 4u, NULL, 0);
    CHECK(rc == GUD_E_INVAL, "wrong-length rect accepted (%s)", gud_strerror(rc));

    /* an incompressible rect must go raw, not marked compressed */
    {
        uint32_t w = 128, h = 64;
        size_t rl = (size_t)w * h * 2;
        size_t big;
        for (i = 0; i < rl; i++)
            conv[i] = (uint8_t)rand();
        big = lz4enc_compress(&lz, conv, rl, comp, sizeof comp);
        rc = gud_flush_rect(&host, 0, 0, w, h, conv, rl, comp, big);
        CHECK(rc == 0, "incompressible rect: %s", gud_strerror(rc));
        CHECK(L.last_bulk_len == rl,
              "incompressible rect sent compressed: %zu bytes for %zu raw",
              L.last_bulk_len, rl);
    }
}

static void t_status_handshake(void)
{
    unsigned before = L.n_status;
    int rc = gud_set_controller_enable(&host, 1);

    CHECK(rc == 0, "controller enable: %s", gud_strerror(rc));
    CHECK(L.n_status > before,
          "no GET_STATUS after a SET, though STATUS_ON_SET is set -- "
          "every failed modeset would look like a success");
}

static void t_modeline_roundtrip(void)
{
    static struct modeline_store store;
    const struct modeline_entry *e;
    unsigned i;

    modeline_store_reset(&store);
    modeline_store_load_device(&store, &host);
    CHECK(store.n == host.n_modes, "store has %u of %u device modes",
          store.n, host.n_modes);

    /* Every device mode must be findable by the key the driver will look it
     * up with at commit time. If one is not, that mode is reportable to
     * Windows and unsettable, which is the worst possible failure. */
    for (i = 0; i < host.n_modes; i++) {
        const struct gud_display_mode_req *m = &host.modes[i];
        int inter = (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? 1 : 0;

        e = modeline_store_find(&store, m->hdisplay, m->vdisplay,
                                gud_mode_refresh_mhz(m), inter);
        CHECK(e != NULL, "mode %u not findable in the store", i);
        if (e)
            CHECK(memcmp(&e->mode, m, sizeof *m) == 0,
                  "mode %u came back changed", i);
    }

    /* The fallback survives a little rounding... */
    e = modeline_store_find(&store, 640, 480,
                            gud_mode_refresh_mhz(&host.modes[0]) + 12, 1);
    CHECK(e != NULL, "fallback lookup failed with 12 mHz of rounding error");

    /* ...but 59.94 and 60.00 are 60 mHz apart, are different modelines on a
     * CRT, and must not collide. This is why the fallback window is tight and
     * why it is a fallback: there is no tolerance that both survives arbitrary
     * rounding and keeps these two apart. */
    e = modeline_store_find(&store, 640, 480, 59940, 1);
    CHECK(e == NULL, "59.94 matched a 60.00 entry");

    /* The exact lookup: what EvtIddCxAdapterCommitModes really calls, keyed on
     * the numbers Windows hands back in DISPLAYCONFIG_VIDEO_SIGNAL_INFO
     * because MakeTargetMode put them there. Every device mode must be
     * findable this way or it is reportable and unsettable. */
    for (i = 0; i < host.n_modes; i++) {
        const struct gud_display_mode_req *m = &host.modes[i];
        int inter = (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? 1 : 0;

        e = modeline_store_find_exact(&store, m->clock, m->htotal, m->vtotal,
                                      m->hdisplay, m->vdisplay, inter);
        CHECK(e != NULL, "mode %u not findable by exact key", i);
        if (e)
            CHECK(memcmp(&e->mode, m, sizeof *m) == 0,
                  "exact lookup returned a different mode for %u", i);
    }

    /* The exact key separates what the fuzzy one cannot. 640x480i60 and a
     * hypothetical 640x480i59.94 differ in pixel clock, and that is enough. */
    e = modeline_store_find_exact(&store, host.modes[0].clock - 13,
                                  host.modes[0].htotal, host.modes[0].vtotal,
                                  640, 480, 1);
    CHECK(e == NULL, "exact lookup matched a different pixel clock");

    /* Progressive and interlaced at the same geometry must not collide. */
    e = modeline_store_find(&store, 640, 480,
                            gud_mode_refresh_mhz(&host.modes[0]), 0);
    CHECK(e == NULL, "640x480p matched the 640x480i entry");

    /* parse -> format -> parse must be a fixed point */
    for (i = 0; i < host.n_modes; i++) {
        char line[256];
        struct gud_display_mode_req m2;
        int rc;

        modeline_format(&host.modes[i], "x", line, sizeof line);
        rc = modeline_parse(line, &m2, NULL, 0);
        CHECK(rc == 0, "reparse of mode %u failed: %s", i, gud_strerror(rc));
        if (rc == 0) {
            CHECK(m2.clock == host.modes[i].clock &&
                  m2.htotal == host.modes[i].htotal &&
                  m2.vtotal == host.modes[i].vtotal &&
                  m2.hsync_start == host.modes[i].hsync_start &&
                  m2.vsync_start == host.modes[i].vsync_start,
                  "mode %u did not survive format/parse", i);
        }
    }

    /* a typo in a modeline reaches a deflection circuit; refuse it */
    {
        struct gud_display_mode_req m;
        CHECK(modeline_parse("12.600 640 664 724 800 480 486 492 525 interlce",
                             &m, NULL, 0) == GUD_E_INVAL,
              "a misspelled flag was accepted");
        CHECK(modeline_parse("12.600 640 664 724", &m, NULL, 0) == GUD_E_INVAL,
              "a truncated modeline was accepted");
        CHECK(modeline_parse("12.600 640 664 800 724 480 486 492 525",
                             &m, NULL, 0) == GUD_E_INVAL,
              "hsync_end past htotal was accepted");
    }
}

/*
 * The INI path. Regression cover for the key=value split, which rejected every
 * line in examples/modelines.ini on the first attempt: the guard was "no quote
 * anywhere on the line" and every real entry carries a quoted name.
 */
static void t_ini(void)
{
    static struct modeline_store store;
    const struct modeline_entry *e;
    const char *path = "/tmp/gudwin_test_modelines.ini";
    FILE *f = fopen(path, "w");

    if (!f) { CHECK(0, "cannot write %s", path); return; }
    fprintf(f,
        "# a comment\n"
        "; another\n"
        "[something_else]\n"
        "ignored = ModeLine \"nope\" 1.000 1 2 3 4 1 2 3 4\n"
        "[modelines]\n"
        "cps2 = ModeLine \"384x224@60\" 7.560 384 400 432 480 224 227 230 262 -hsync -vsync\n"
        "\n"
        "ModeLine \"bare\" 6.300 320 332 362 400 240 243 246 262 -hsync -vsync\n"
        "5.040 256 264 288 320 224 227 230 262\n"
        "; override the device's own 640x480i60 with a different front porch\n"
        "ntsc = ModeLine \"640x480@60\" 12.600 640 672 724 800 480 486 492 525 interlace -hsync -vsync\n");
    fclose(f);

    modeline_store_reset(&store);
    modeline_store_load_device(&store, &host);
    CHECK(modeline_store_load_ini(&store, path) == 0, "ini reported a parse error");

    /*
     * Four entries, two of them new.
     *
     * 384x224 and 256x224 are not advertised, so they are added. 320x240@60
     * and 640x480i60 both collide with a device mode and replace it, which is
     * the override working -- the ini is loaded second precisely so it can.
     * So the count grows by two, not four.
     */
    CHECK(store.n == host.n_modes + 2, "store has %u, expected %u",
          store.n, host.n_modes + 2);

    e = modeline_store_find(&store, 384, 224, 60115, 0);
    CHECK(e != NULL, "key=value entry with a quoted name did not load");
    if (e) CHECK(!e->from_device && strcmp(e->name, "cps2") == 0,
                 "cps2 name/origin wrong: '%s'", e->name);

    e = modeline_store_find(&store, 320, 240, 60115, 0);
    CHECK(e != NULL, "bare ModeLine entry did not load");
    if (e) {
        CHECK(strcmp(e->name, "bare") == 0, "quoted name lost: '%s'", e->name);
        CHECK(e->from_device == 0,
              "bare entry did not override the advertised 320x240p60");
    }

    e = modeline_store_find(&store, 256, 224, 60115, 0);
    CHECK(e != NULL, "bare numeric modeline did not load");
    if (e) CHECK(e->mode.flags & GUD_DISPLAY_MODE_FLAG_NHSYNC,
                 "sync polarity did not default to negative");

    /* the override: same geometry and rate as a device mode, different porch */
    e = modeline_store_find(&store, 640, 480,
                            gud_mode_refresh_mhz(&host.modes[0]), 1);
    CHECK(e != NULL, "640x480i60 vanished");
    if (e) {
        CHECK(e->from_device == 0, "ini entry did not override the device one");
        CHECK(e->mode.hsync_start == 672,
              "override kept the device's porch (%u)", e->mode.hsync_start);
    }

    /* a missing file is the ordinary case, not a failure */
    CHECK(modeline_store_load_ini(&store, "/tmp/gudwin_no_such_file.ini") == 0,
          "a missing ini was reported as an error");

    remove(path);
}

int main(void)
{
    printf("gud-windows loopback: host protocol against blitsCRT_Mister device.c\n\n");

    t_probe();               printf("  enumeration\n");
    t_modes();               printf("  advertised modes\n");
    t_modeset();             printf("  modeset, unadvertised and refused\n");
    t_flush();               printf("  flush: raw, LZ4, damage, bad input\n");
    t_status_handshake();    printf("  STATUS_ON_SET handshake\n");
    t_modeline_roundtrip();  printf("  modeline store\n");
    t_ini();                 printf("  ini parsing and override\n");

    printf("\n%d checks, %d failures\n", checks, fails);
    printf("%u control in, %u control out, %u bulk\n",
           L.n_ctrl_in, L.n_ctrl_out, L.n_bulk);
    return fails != 0;
}
