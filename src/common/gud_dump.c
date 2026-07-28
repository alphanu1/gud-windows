/* SPDX-License-Identifier: MIT */
/*
 * gud_dump.c -- render a probed device as text.
 *
 * Lives in common rather than in the probe tool so the expected output can be
 * generated on any machine, from the loopback harness, by the same code the
 * exe runs. A golden reference nobody can reproduce is worth very little; one
 * that drifts from the tool it describes is worth less than nothing.
 */

#include "gud_dump.h"
#include <stdio.h>

const char *gud_format_name(uint8_t f)
{
    switch (f) {
    case GUD_PIXEL_FORMAT_R1:        return "R1";
    case GUD_PIXEL_FORMAT_R8:        return "R8";
    case GUD_PIXEL_FORMAT_XRGB1111:  return "XRGB1111";
    case GUD_PIXEL_FORMAT_RGB332:    return "RGB332";
    case GUD_PIXEL_FORMAT_RGB565:    return "RGB565";
    case GUD_PIXEL_FORMAT_RGB888:    return "RGB888";
    case GUD_PIXEL_FORMAT_XRGB8888:  return "XRGB8888";
    case GUD_PIXEL_FORMAT_ARGB8888:  return "ARGB8888";
    default:                         return "?";
    }
}

const char *gud_connector_name(uint8_t t)
{
    static const char *n[] = { "Panel", "VGA", "Composite", "S-Video",
                               "Component", "DVI", "DisplayPort", "HDMI" };
    return t < 8 ? n[t] : "?";
}

void gud_dump(FILE *out, const struct gud_device *d)
{
    unsigned i;

    fprintf(out, "descriptor\n");
    fprintf(out, "  magic            0x%08x %s\n", d->desc.magic,
           d->desc.magic == GUD_DISPLAY_MAGIC ? "(ok)" : "(WRONG)");
    fprintf(out, "  version          %u\n", d->desc.version);
    fprintf(out, "  flags            0x%08x%s%s\n", d->desc.flags,
           (d->desc.flags & GUD_DISPLAY_FLAG_STATUS_ON_SET) ? " STATUS_ON_SET" : "",
           (d->desc.flags & GUD_DISPLAY_FLAG_FULL_UPDATE)   ? " FULL_UPDATE" : "");
    fprintf(out, "  compression      0x%02x%s\n", d->desc.compression,
           (d->desc.compression & GUD_COMPRESSION_LZ4) ? " LZ4" : "");
    fprintf(out, "  max_buffer_size  %u\n", d->desc.max_buffer_size);
    fprintf(out, "  size             %u..%u x %u..%u\n",
           d->desc.min_width, d->desc.max_width,
           d->desc.min_height, d->desc.max_height);

    fprintf(out, "formats           ");
    for (i = 0; i < d->n_formats; i++)
        fprintf(out, " %s", gud_format_name(d->formats[i]));
    fprintf(out, "\n");

    fprintf(out, "connector 0        %s, flags 0x%08x%s%s\n",
           gud_connector_name(d->connector.connector_type), d->connector.flags,
           (d->connector.flags & GUD_CONNECTOR_FLAGS_INTERLACE)  ? " INTERLACE" : "",
           (d->connector.flags & GUD_CONNECTOR_FLAGS_POLL_STATUS) ? " POLL" : "");

    fprintf(out, "modes              %u\n", d->n_modes);
    for (i = 0; i < d->n_modes; i++) {
        const struct gud_display_mode_req *m = &d->modes[i];
        uint32_t r = gud_mode_refresh_mhz(m);
        double line_khz = m->htotal ? (double)m->clock / m->htotal : 0.0;

        fprintf(out, "  [%u] %4ux%-4u%s %7.3f Hz  %8.3f MHz  line %7.3f kHz%s\n",
               i, m->hdisplay, m->vdisplay,
               (m->flags & GUD_DISPLAY_MODE_FLAG_INTERLACE) ? "i" : "p",
               r / 1000.0, m->clock / 1000.0, line_khz,
               (m->flags & GUD_DISPLAY_MODE_FLAG_PREFERRED) ? "  PREFERRED" : "");
        fprintf(out, "      h %u %u %u %u   v %u %u %u %u   flags 0x%x\n",
               m->hdisplay, m->hsync_start, m->hsync_end, m->htotal,
               m->vdisplay, m->vsync_start, m->vsync_end, m->vtotal, m->flags);
    }
}

