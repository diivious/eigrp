// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt-local read-only operational interface. Copyright (C) 2026 Donnie V. Savage */
#ifndef EIGRP_OWRT_MGNT_H_
#define EIGRP_OWRT_MGNT_H_

#include <stdio.h>

/* The daemon owns the listener and executes handlers on its control thread.
 * The protocol snapshot APIs are the sole data source. */
int eigrp_owrt_mgnt_listen(const char *path);
void eigrp_owrt_mgnt_handle(int listener);
void eigrp_owrt_mgnt_close(int listener, const char *path);
int eigrp_owrt_mgnt_show(FILE *output, const char *command);

#endif
