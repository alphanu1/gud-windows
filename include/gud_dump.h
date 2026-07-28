/* SPDX-License-Identifier: MIT */
#ifndef GUD_DUMP_H
#define GUD_DUMP_H

#include "gud_host.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *gud_format_name(uint8_t format);
const char *gud_connector_name(uint8_t connector_type);

/* Everything gud_probe() learned, as text. This is what gudprobe prints. */
void gud_dump(FILE *out, const struct gud_device *d);

#ifdef __cplusplus
}
#endif
#endif
