// Xbox Dev Mode (UWP) port of xemu: Copyright (c) 2026 Skitzelles, https://github.com/skitzellez/xemu-uwp
// Free software under the GNU General Public License version 2 or later, like xemu itself;
// this notice stays in every copy.
/*
 * Audio output for the Xbox (UWP, Dev Mode) build of xemu.
 *
 * SDL3-uwp's WASAPI backend never finishes opening a device on the Xbox
 * (ActivateAudioInterfaceAsync does not complete while SDL waits for it), so
 * the APU still writes into an SDL_AudioStream, and a small thread plays that
 * stream through XAudio2 instead.
 */

#ifndef XEMU_UWP_AUDIO_H
#define XEMU_UWP_AUDIO_H

#include <stdbool.h>
#include <SDL3/SDL.h>

/* Creates the stream the APU writes to (48 kHz, signed 16-bit, stereo) and
 * starts playing it. Returns NULL if XAudio2 is unavailable. */
SDL_AudioStream *xemu_uwp_audio_start(void);

/* Headset (hw/xbox/xblc.c): the game's voice output (played next to the game's sound) and the
 * console's microphone, both 48 kHz mono signed 16-bit; xblc sets its own side's format.
 * Made by xemu_uwp_audio_start(); NULL without XAudio2. */
extern SDL_AudioStream *xemu_uwp_headset_out;
extern SDL_AudioStream *xemu_uwp_headset_in;
/* The headset is used only when the player turned it on in Game Vault's Xemu setup (headset=1 in the
 * launch link): a quick save made with it plugged in does not load without it, and the other way round. */
extern bool xemu_uwp_headset_on;
/* bytes the microphone has delivered so far (for the log) */
extern volatile unsigned xemu_uwp_mic_bytes;
/* Asks for the default microphone; call on the UI thread. Never waits. */
void xemu_uwp_mic_start(void);
/* every 3 s on the UI thread: asks again once the window is up, logs how the microphone does */
void xemu_uwp_mic_tick(void);

#endif
