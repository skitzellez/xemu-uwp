/*
 * xemu on Xbox Dev Mode: the TV's picture mode (HDMI)
 *
 * Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_UWP_HDMI_H
#define XEMU_UWP_HDMI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Asks the console for the TV's current picture size at want_hz (e.g. 120); true when it is already there or
 * was asked for. Logs the modes the TV offers above 100 Hz. Must run on the app's window thread. */
bool xemu_uwp_hdmi_request_hz(double want_hz);
/* The refresh rate in use as last seen or asked for; 0 when not known. */
double xemu_uwp_hdmi_refresh_rate(void);

#ifdef __cplusplus
}
#endif

#endif
