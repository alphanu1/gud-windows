/* SPDX-License-Identifier: MIT */
#ifndef WINUSB_TRANSPORT_H
#define WINUSB_TRANSPORT_H

#include "gud_host.h"
#include <stdlib.h>

struct winusb_dev;

/* Opens the first present device matching vid:pid. NULL on failure, with a
 * reason written into err. */
struct winusb_dev *winusb_open(unsigned vid, unsigned pid,
                               char *err, size_t err_len);
void winusb_close(struct winusb_dev *d);

void winusb_transport(struct winusb_dev *d, struct gud_transport *t);

#endif
