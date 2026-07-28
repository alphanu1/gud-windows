/* SPDX-License-Identifier: MIT */
/*
 * modeline.h -- where the porches come from.
 *
 * This is the whole difficulty of a Windows GUD host, so it is worth stating
 * plainly.
 *
 * GUD carries a complete DRM modeline in SET_STATE_CHECK: pixel clock, both
 * sync starts and ends, both totals, interlace. A device that synthesises its
 * own timing needs all of it. On Linux the path is direct -- an application
 * adds a modeline to a DRM connector, the kernel hands the mode to the gud
 * driver, and gud puts it on the wire unchanged.
 *
 * Windows has no such path. IddCx deals in a monitor mode list and EDID:
 * width, height, and a refresh rate expressed as a rational. Porches do not
 * exist anywhere in the API, because a normal indirect display does not
 * generate timing and has no use for them. This one does.
 *
 * So the driver keeps its own table, and every mode it reports to IddCx is
 * backed by an entry in it. The lookup runs the other way at commit time:
 * Windows says "640x480 at 59.94", and the table says which modeline that was.
 *
 * Two sources fill the table, in this order:
 *
 *   1. The device's own list, from GET_CONNECTOR_MODES. Every GUD device
 *      advertises one and every entry in it carries full timing. This is why
 *      the driver works with no configuration at all: plug the board in and
 *      the four modes it advertises are the four Windows offers.
 *
 *   2. A modeline store the user writes -- an INI on disk today, and the sink
 *      a Switchres backend pushes into later. Entries here override the
 *      device's when they collide, because someone who has written out a
 *      modeline by hand means it.
 *
 * The alternative -- reading what a Windows Switchres deposits somewhere and
 * pulling it in -- was considered and is worse. There is no such place: the
 * existing Windows backends (adl, powerstrip) push timing into a graphics
 * driver through a vendor interface and leave nothing behind to read. Making
 * this driver the sink rather than a scavenger means the Switchres side is a
 * small backend that writes modelines to a known destination, which is the
 * shape every other Switchres backend already has.
 */
#ifndef MODELINE_H
#define MODELINE_H

#include "gud.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MODELINE_MAX 128

/*
 * Refresh matching tolerance for the *fallback* lookup, in millihertz.
 *
 * 59.94 and 60.00 are 60 mHz apart and are different modelines on a CRT, so
 * anything at or above 30 mHz can conflate them. That rules out a comfortable
 * tolerance, which in turn rules out matching on refresh as the primary key:
 * there is not enough room between "survives rounding" and "does not conflate
 * two real modes" to be confident in either.
 *
 * So the primary lookup does not use this at all -- see
 * modeline_store_find_exact(). This value governs only the fallback, and it is
 * deliberately tight enough to fail rather than return the wrong modeline.
 */
#define MODELINE_REFRESH_TOL_MHZ 20

struct modeline_entry {
    struct gud_display_mode_req mode;
    uint32_t refresh_mhz;        /* derived, cached: the lookup key */
    char     name[32];           /* for logging; not part of the key */
    int      from_device;        /* 1 if it came from GET_CONNECTOR_MODES */
};

struct modeline_store {
    struct modeline_entry e[MODELINE_MAX];
    unsigned n;
};

void modeline_store_reset(struct modeline_store *s);

/*
 * Add one. Replaces an existing entry with the same key, which is how a store
 * entry overrides a device one -- load the device's list first.
 * Returns 0, or GUD_E_NOSPACE.
 */
int modeline_store_add(struct modeline_store *s,
                       const struct gud_display_mode_req *m,
                       const char *name, int from_device);

/* Load every mode the device advertises. */
struct gud_device;
int modeline_store_load_device(struct modeline_store *s,
                               const struct gud_device *d);

/*
 * Parse one modeline. Accepts the X11/Switchres spelling, with or without the
 * `ModeLine "name"` prefix, clock in MHz:
 *
 *   ModeLine "640x480i" 12.600 640 664 724 800 480 486 492 525 interlace -hsync -vsync
 *   12.600 640 664 724 800 480 486 492 525 interlace -hsync -vsync
 *
 * Recognised trailing flags: +hsync -hsync +vsync -vsync interlace doublescan
 * csync. Absent sync polarity defaults to negative, which is what 15 kHz RGB
 * and SCART want and what every mode this device advertises uses.
 *
 * Returns 0 on success, GUD_E_INVAL on a malformed line.
 */
int modeline_parse(const char *line, struct gud_display_mode_req *m,
                   char *name, size_t name_len);

/* Render a modeline back out in the same spelling. For logging and for
 * writing the store back. */
int modeline_format(const struct gud_display_mode_req *m, const char *name,
                    char *out, size_t out_len);

/*
 * Read an INI. Section [modelines], one modeline per line; a bare `name =`
 * key is optional and the value is parsed as above. Lines starting with # or ;
 * are comments. Missing file is not an error -- it is the ordinary case.
 */
int modeline_store_load_ini(struct modeline_store *s, const char *path);

/*
 * The primary lookup, and what runs at commit time.
 *
 * DISPLAYCONFIG_VIDEO_SIGNAL_INFO carries pixelRate and totalSize as well as
 * activeSize, and those came from us -- MakeTargetMode() put them there. So
 * Windows hands back four of the modeline's five identifying numbers exactly,
 * and matching on them is exact rather than approximate. Only the sync starts
 * and ends are missing, and those are what the store is for.
 *
 * This is worth insisting on. Matching by refresh rate alone cannot separate
 * 59.94 from 60.00 with any tolerance wide enough to survive rounding, and
 * returning the wrong modeline puts wrong timing on a fixed-frequency
 * deflection circuit. Exact keys have no such failure mode.
 *
 * `clock_khz` is the pixel clock in kHz, matching gud_display_mode_req.clock;
 * divide DISPLAYCONFIG's pixelRate by 1000.
 *
 * Returns the entry, or NULL.
 */
const struct modeline_entry *
modeline_store_find_exact(const struct modeline_store *s,
                          uint32_t clock_khz, uint32_t htotal, uint32_t vtotal,
                          uint32_t w, uint32_t h, int interlace);

/*
 * Fallback lookup, for a path that lost the totals.
 *
 * Matches on hdisplay, vdisplay and refresh within MODELINE_REFRESH_TOL_MHZ.
 * `interlace` is -1 for don't-care, 0 or 1 to require it: Windows reports a
 * 480i mode's height as 480 and so does DRM, so geometry alone cannot tell
 * 640x480p60 from 640x480i60 and the caller has to say which it meant.
 *
 * Also what modeline_store_add() uses to decide whether a new entry replaces
 * an existing one, which is the right granularity there: two modelines with
 * the same geometry and rate are the same mode expressed twice, and the later
 * one wins.
 *
 * Returns the entry, or NULL.
 */
const struct modeline_entry *
modeline_store_find(const struct modeline_store *s,
                    uint32_t w, uint32_t h, uint32_t refresh_mhz,
                    int interlace);

#ifdef __cplusplus
}
#endif

#endif /* MODELINE_H */
