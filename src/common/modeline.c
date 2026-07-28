/* SPDX-License-Identifier: MIT */

#include "modeline.h"
#include "gud_host.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/*
 * strncasecmp is POSIX and MSVC does not have it. mingw and every Unix do, so
 * this compiles cleanly everywhere except the one compiler the driver is
 * actually built with.
 */
#if defined(_MSC_VER)
#  define strncasecmp _strnicmp
#endif

void modeline_store_reset(struct modeline_store *s)
{
    s->n = 0;
}

static int same_key(const struct modeline_entry *e,
                    uint32_t w, uint32_t h, uint32_t r, int interlace)
{
    uint32_t d = e->refresh_mhz > r ? e->refresh_mhz - r : r - e->refresh_mhz;
    int ei = (e->mode.flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? 1 : 0;

    if (e->mode.hdisplay != w || e->mode.vdisplay != h)
        return 0;
    if (d > MODELINE_REFRESH_TOL_MHZ)
        return 0;
    if (interlace >= 0 && ei != interlace)
        return 0;
    return 1;
}

int modeline_store_add(struct modeline_store *s,
                       const struct gud_display_mode_req *m,
                       const char *name, int from_device)
{
    uint32_t r = gud_mode_refresh_mhz(m);
    int interlace = (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? 1 : 0;
    struct modeline_entry *slot = NULL;
    unsigned i;

    for (i = 0; i < s->n; i++) {
        if (same_key(&s->e[i], m->hdisplay, m->vdisplay, r, interlace)) {
            slot = &s->e[i];
            break;
        }
    }
    if (!slot) {
        if (s->n >= MODELINE_MAX)
            return GUD_E_NOSPACE;
        slot = &s->e[s->n++];
    }

    memset(slot, 0, sizeof *slot);
    slot->mode        = *m;
    slot->refresh_mhz = r;
    slot->from_device = from_device;
    if (name && *name) {
        strncpy(slot->name, name, sizeof slot->name - 1);
    } else {
        snprintf(slot->name, sizeof slot->name, "%ux%u%s%u",
                 m->hdisplay, m->vdisplay, interlace ? "i" : "p",
                 (r + 500) / 1000);
    }
    return 0;
}

int modeline_store_load_device(struct modeline_store *s,
                               const struct gud_device *d)
{
    unsigned i;
    int err = 0;

    for (i = 0; i < d->n_modes; i++) {
        int e = modeline_store_add(s, &d->modes[i], NULL, 1);
        if (e && !err)
            err = e;
    }
    return err;
}

/* ---------------- parser ---------------- */

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

/*
 * Parse into a local and let the caller assign. Taking the address of a member
 * of a packed structure is undefined -- the pointer may be unaligned and on
 * ARM a 16-bit store through it can fault. The structures here are wire format
 * and packed by definition, so nothing in this file may point into one.
 */
static int take_u16(const char **p, uint16_t *out)
{
    char *end;
    unsigned long v;

    *p = skip_ws(*p);
    if (!isdigit((unsigned char)**p))
        return 0;
    v = strtoul(*p, &end, 10);
    if (end == *p || v > 0xffff)
        return 0;
    *p = end;
    *out = (uint16_t)v;
    return 1;
}

int modeline_parse(const char *line, struct gud_display_mode_req *m,
                   char *name, size_t name_len)
{
    const char *p = skip_ws(line);
    double mhz;
    char *end;
    uint32_t flags = 0;
    int have_h = 0, have_v = 0;

    memset(m, 0, sizeof *m);
    if (name && name_len)
        name[0] = '\0';

    /* optional `ModeLine` keyword */
    if (!strncasecmp(p, "modeline", 8) && isspace((unsigned char)p[8]))
        p = skip_ws(p + 8);

    /* optional quoted name */
    if (*p == '"') {
        const char *q = strchr(p + 1, '"');
        if (!q)
            return GUD_E_INVAL;
        if (name && name_len) {
            size_t n = (size_t)(q - p - 1);
            if (n >= name_len)
                n = name_len - 1;
            memcpy(name, p + 1, n);
            name[n] = '\0';
        }
        p = skip_ws(q + 1);
    }

    /*
     * Pixel clock in MHz, as X11 and Switchres write it. GUD carries kHz as an
     * integer, so this rounds. At 12.600 MHz one kHz is 79 ppm -- larger than
     * the 6 ppm the device's own PLL solver achieves, so writing more decimal
     * places than the kHz field can hold is silently pointless. Say so rather
     * than let someone chase it.
     */
    mhz = strtod(p, &end);
    if (end == p || mhz <= 0.0)
        return GUD_E_INVAL;
    p = end;
    m->clock = (uint32_t)(mhz * 1000.0 + 0.5);

    {
        uint16_t v[8];
        unsigned k;
        for (k = 0; k < 8; k++)
            if (!take_u16(&p, &v[k]))
                return GUD_E_INVAL;
        m->hdisplay = v[0]; m->hsync_start = v[1];
        m->hsync_end = v[2]; m->htotal = v[3];
        m->vdisplay = v[4]; m->vsync_start = v[5];
        m->vsync_end = v[6]; m->vtotal = v[7];
    }

    for (;;) {
        p = skip_ws(p);
        if (!*p)
            break;
        if (!strncasecmp(p, "+hsync", 6))     { flags |= GUD_DISPLAY_MODE_FLAG_PHSYNC; have_h = 1; p += 6; }
        else if (!strncasecmp(p, "-hsync", 6)) { flags |= GUD_DISPLAY_MODE_FLAG_NHSYNC; have_h = 1; p += 6; }
        else if (!strncasecmp(p, "+vsync", 6)) { flags |= GUD_DISPLAY_MODE_FLAG_PVSYNC; have_v = 1; p += 6; }
        else if (!strncasecmp(p, "-vsync", 6)) { flags |= GUD_DISPLAY_MODE_FLAG_NVSYNC; have_v = 1; p += 6; }
        else if (!strncasecmp(p, "interlace", 9))  { flags |= GUD_DISPLAY_MODE_FLAG_INTERLACE; p += 9; }
        else if (!strncasecmp(p, "doublescan", 10)) { flags |= GUD_DISPLAY_MODE_FLAG_DBLSCAN; p += 10; }
        else if (!strncasecmp(p, "csync", 5))      { flags |= GUD_DISPLAY_MODE_FLAG_CSYNC; p += 5; }
        else if (!strncasecmp(p, "preferred", 9))  { flags |= GUD_DISPLAY_MODE_FLAG_PREFERRED; p += 9; }
        else
            return GUD_E_INVAL;   /* an unrecognised trailing word is a typo,
                                   * and a typo in a modeline reaches a
                                   * deflection circuit. Refuse it. */
    }

    /* 15 kHz RGB and SCART want negative sync, and every mode this device
     * advertises uses it. Default rather than leave the flags empty, which a
     * device is entitled to read as "no sync at all". */
    if (!have_h) flags |= GUD_DISPLAY_MODE_FLAG_NHSYNC;
    if (!have_v) flags |= GUD_DISPLAY_MODE_FLAG_NVSYNC;

    m->flags = flags;

    if (m->hsync_start < m->hdisplay || m->hsync_end < m->hsync_start ||
        m->htotal < m->hsync_end ||
        m->vsync_start < m->vdisplay || m->vsync_end < m->vsync_start ||
        m->vtotal < m->vsync_end)
        return GUD_E_INVAL;

    return 0;
}

int modeline_format(const struct gud_display_mode_req *m, const char *name,
                    char *out, size_t out_len)
{
    int n = snprintf(out, out_len,
                     "ModeLine \"%s\" %.3f %u %u %u %u %u %u %u %u%s%s%s%s",
                     name ? name : "",
                     m->clock / 1000.0,
                     m->hdisplay, m->hsync_start, m->hsync_end, m->htotal,
                     m->vdisplay, m->vsync_start, m->vsync_end, m->vtotal,
                     (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? " interlace" : "",
                     (m->flags & GUD_DISPLAY_MODE_FLAG_DBLSCAN)   ? " doublescan" : "",
                     (m->flags & GUD_DISPLAY_MODE_FLAG_PHSYNC)    ? " +hsync" : " -hsync",
                     (m->flags & GUD_DISPLAY_MODE_FLAG_PVSYNC)    ? " +vsync" : " -vsync");
    return (n < 0 || (size_t)n >= out_len) ? GUD_E_NOSPACE : 0;
}

/* ---------------- ini ---------------- */

int modeline_store_load_ini(struct modeline_store *s, const char *path)
{
    char buf[512];
    FILE *f = fopen(path, "r");
    int in_section = 0;
    int line_no = 0;
    int err = 0;

    if (!f)
        return 0;   /* no file is the ordinary case, not a failure */

    while (fgets(buf, sizeof buf, f)) {
        char name[32] = { 0 };
        struct gud_display_mode_req m;
        char *p = buf, *nl;
        int e;

        line_no++;
        nl = strpbrk(p, "\r\n");
        if (nl)
            *nl = '\0';

        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p || *p == '#' || *p == ';')
            continue;

        if (*p == '[') {
            in_section = !strncasecmp(p, "[modelines]", 11);
            continue;
        }
        if (!in_section)
            continue;

        /*
         * `name = modeline`, or a bare modeline.
         *
         * Split on the first `=` only when it comes before any quote. A
         * modeline may carry its own quoted name and that name may contain an
         * `=` -- "384x224@60" does not, but "640x480@60=preferred" is the sort
         * of thing someone writes -- and splitting there would cut the line in
         * half. Guarding on "no quote anywhere" instead was the first attempt
         * and rejected every line in the example file, because they all carry
         * a quoted name.
         */
        {
            char *eq = strchr(p, '=');
            char *q  = strchr(p, '"');

            if (eq && (!q || eq < q)) {
                char *k = p, *ke = eq;
                while (ke > k && isspace((unsigned char)ke[-1]))
                    ke--;
                {
                    size_t kn = (size_t)(ke - k);
                    if (kn >= sizeof name)
                        kn = sizeof name - 1;
                    memcpy(name, k, kn);
                    name[kn] = '\0';
                }
                p = eq + 1;
            }
        }

        e = modeline_parse(p, &m, name[0] ? NULL : name, sizeof name);
        if (e) {
            fprintf(stderr, "%s:%d: bad modeline\n", path, line_no);
            err = e;
            continue;
        }
        modeline_store_add(s, &m, name, 0);
    }

    fclose(f);
    return err;
}

const struct modeline_entry *
modeline_store_find_exact(const struct modeline_store *s,
                          uint32_t clock_khz, uint32_t htotal, uint32_t vtotal,
                          uint32_t w, uint32_t h, int interlace)
{
    unsigned i;

    for (i = 0; i < s->n; i++) {
        const struct gud_display_mode_req *m = &s->e[i].mode;
        int ei = (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? 1 : 0;

        if (m->clock == clock_khz && m->htotal == htotal &&
            m->vtotal == vtotal && m->hdisplay == w && m->vdisplay == h &&
            (interlace < 0 || ei == interlace))
            return &s->e[i];
    }
    return NULL;
}

const struct modeline_entry *
modeline_store_find(const struct modeline_store *s,
                    uint32_t w, uint32_t h, uint32_t refresh_mhz,
                    int interlace)
{
    unsigned i;
    for (i = 0; i < s->n; i++)
        if (same_key(&s->e[i], w, h, refresh_mhz, interlace))
            return &s->e[i];
    return NULL;
}
