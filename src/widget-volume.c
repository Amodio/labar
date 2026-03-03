#define _GNU_SOURCE

#include "widget-volume.h"
#include "exec.h"

#include <alsa/asoundlib.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// External verbose flag (defined in main.c)
extern int verbose;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Name of the ALSA card / mixer device to target
#define ALSA_CARD "default"
// Preferred simple-element name; falls back to Master if absent
#define ALSA_ELEM "PCM"
#define ALSA_ELEM_FB "Master"

// Step size for scroll events (in percent of the full range)
#define VOLUME_STEP_PCT 4

/*
 * mixer_open_elem
 *
 * Open the default ALSA mixer, register the simple-element layer, load it,
 * and return a handle + the PCM (or Master) simple element.
 *
 * Both *handle_out and *elem_out must be non-NULL.
 * Returns 0 on success, -1 on any error.
 * On success the caller is responsible for calling
 * snd_mixer_close(*handle_out).
 */
static int
mixer_open_elem(snd_mixer_t **handle_out, snd_mixer_elem_t **elem_out)
{
	int rc;

	rc = snd_mixer_open(handle_out, 0);
	if (rc < 0) {
		if (verbose >= 2)
			fprintf(stderr, "[VOL] snd_mixer_open: %s\n", snd_strerror(rc));
		return -1;
	}

	rc = snd_mixer_attach(*handle_out, ALSA_CARD);
	if (rc < 0) {
		if (verbose >= 2)
			fprintf(stderr, "[VOL] snd_mixer_attach: %s\n", snd_strerror(rc));
		goto err_close;
	}

	rc = snd_mixer_selem_register(*handle_out, NULL, NULL);
	if (rc < 0) {
		if (verbose >= 2)
			fprintf(stderr, "[VOL] snd_mixer_selem_register: %s\n",
				snd_strerror(rc));
		goto err_close;
	}

	rc = snd_mixer_load(*handle_out);
	if (rc < 0) {
		if (verbose >= 2)
			fprintf(stderr, "[VOL] snd_mixer_load: %s\n", snd_strerror(rc));
		goto err_close;
	}

	// Locate the target simple element (PCM, fall back to Master)
	snd_mixer_selem_id_t *sid = NULL;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);

	snd_mixer_selem_id_set_name(sid, ALSA_ELEM);
	*elem_out = snd_mixer_find_selem(*handle_out, sid);

	if (!*elem_out) {
		snd_mixer_selem_id_set_name(sid, ALSA_ELEM_FB);
		*elem_out = snd_mixer_find_selem(*handle_out, sid);
	}

	if (!*elem_out) {
		if (verbose >= 2)
			fprintf(stderr, "[VOL] Could not find '%s' or '%s' element\n",
				ALSA_ELEM, ALSA_ELEM_FB);
		goto err_close;
	}

	return 0;

err_close:
	snd_mixer_close(*handle_out);
	*handle_out = NULL;
	*elem_out = NULL;
	return -1;
}

// ---------------------------------------------------------------------------
// volume_get_info
//
// Query ALSA for the current PCM volume level and mute state.
// Falls back from PCM → Master if the preferred element is absent.
//
// Parameters:
//   percent_out – receives the volume as a percentage (0–100)
//   muted_out   – receives 1 if the channel is muted, 0 otherwise
//
// Returns 0 on success, -1 on error.
// ---------------------------------------------------------------------------
int
volume_get_info(int *percent_out, int *muted_out)
{
	if (!percent_out || !muted_out)
		return -1;

	*percent_out = 0;
	*muted_out = 0;

	snd_mixer_t *handle = NULL;
	snd_mixer_elem_t *elem = NULL;

	if (mixer_open_elem(&handle, &elem) < 0)
		return -1;

	// --- Volume percentage ---
	long vol_min = 0, vol_max = 0, vol_cur = 0;
	snd_mixer_selem_get_playback_volume_range(elem, &vol_min, &vol_max);

	// Prefer FRONT_LEFT as the canonical channel; fall back to MONO
	snd_mixer_selem_channel_id_t ch =
		snd_mixer_selem_has_playback_channel(elem, SND_MIXER_SCHN_FRONT_LEFT) ?
		SND_MIXER_SCHN_FRONT_LEFT :
		SND_MIXER_SCHN_MONO;
	snd_mixer_selem_get_playback_volume(elem, ch, &vol_cur);

	if (vol_max > vol_min)
		*percent_out = (int)(100L * (vol_cur - vol_min) / (vol_max - vol_min));

	// --- Mute state (playback switch: 0 = muted, 1 = active) ---
	if (snd_mixer_selem_has_playback_switch(elem)) {
		int sw = 1;
		snd_mixer_selem_get_playback_switch(elem, ch, &sw);
		*muted_out = (sw == 0) ? 1 : 0;
	}

	if (verbose >= 2)
		printf("[VOL] volume=%d%% muted=%d\n", *percent_out, *muted_out);

	snd_mixer_close(handle);
	return 0;
}

// ---------------------------------------------------------------------------
// volume_get_icon_path
// ---------------------------------------------------------------------------
const char *
volume_get_icon_path(int percent, int muted)
{
	if (muted)
		return VOLUME_ICON_MUTED;

	if (percent >= VOLUME_HIGH_THRESHOLD)
		return VOLUME_ICON_HIGH;

	if (percent >= VOLUME_MEDIUM_THRESHOLD)
		return VOLUME_ICON_MEDIUM;

	if (percent >= VOLUME_LOW_THRESHOLD)
		return VOLUME_ICON_LOW;

	return VOLUME_ICON_OFF;
}

// ---------------------------------------------------------------------------
// volume_get_label
// ---------------------------------------------------------------------------
void
volume_get_label(char *buf, int buf_len, int percent, int muted)
{
	if (!buf || buf_len <= 0)
		return;

	if (muted)
		snprintf(buf, buf_len, "muted");
	else
		snprintf(buf, buf_len, "%d %%", percent);
}

// ---------------------------------------------------------------------------
// volume_toggle_mute  (alsa-lib, no system())
//
// Reads the current playback-switch state across all channels and flips it:
// if every channel is muted we un-mute; otherwise we mute them all.
// ---------------------------------------------------------------------------
static void
volume_toggle_mute(void)
{
	snd_mixer_t *handle = NULL;
	snd_mixer_elem_t *elem = NULL;

	if (mixer_open_elem(&handle, &elem) < 0)
		return;

	if (!snd_mixer_selem_has_playback_switch(elem)) {
		if (verbose >= 2)
			fprintf(stderr, "[VOL] Element has no playback switch\n");
		snd_mixer_close(handle);
		return;
	}

	// Determine aggregate mute state: are ALL channels currently muted?
	int all_muted = 1;
	for (int ch = 0; ch <= SND_MIXER_SCHN_LAST; ch++) {
		if (!snd_mixer_selem_has_playback_channel(elem,
				(snd_mixer_selem_channel_id_t)ch))
			continue;
		int sw = 1;
		snd_mixer_selem_get_playback_switch(elem,
			(snd_mixer_selem_channel_id_t)ch, &sw);
		if (sw) {
			all_muted = 0;
			break;
		}
	}

	// Flip: all muted → unmute (sw=1), any active → mute (sw=0)
	int new_sw = all_muted ? 1 : 0;
	snd_mixer_selem_set_playback_switch_all(elem, new_sw);

	if (verbose)
		printf("[VOL] Mute toggled → %s\n", new_sw ? "unmuted" : "muted");

	snd_mixer_close(handle);
}

// ---------------------------------------------------------------------------
// volume_adjust  (alsa-lib, no system())
//
// Raise or lower the PCM volume by VOLUME_STEP_PCT percent of the full range.
// Clamps the result to [vol_min, vol_max].
//
// Parameters:
//   up – non-zero to raise, zero to lower
// ---------------------------------------------------------------------------
static void
volume_adjust(int up)
{
	snd_mixer_t *handle = NULL;
	snd_mixer_elem_t *elem = NULL;

	if (mixer_open_elem(&handle, &elem) < 0)
		return;

	long vol_min = 0, vol_max = 0, vol_cur = 0;
	snd_mixer_selem_get_playback_volume_range(elem, &vol_min, &vol_max);
	snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO, &vol_cur);

	long range = vol_max - vol_min;
	if (range <= 0) {
		snd_mixer_close(handle);
		return;
	}

	// Convert the percentage step to raw units, rounding up so a step is
	// never zero even on small-range controls.
	long step = (range * VOLUME_STEP_PCT + 99) / 100;

	long vol_new = up ? (vol_cur + step) : (vol_cur - step);

	// Clamp to the valid range
	if (vol_new < vol_min)
		vol_new = vol_min;
	if (vol_new > vol_max)
		vol_new = vol_max;

	snd_mixer_selem_set_playback_volume_all(elem, vol_new);

	if (verbose)
		printf("[VOL] Volume %s → %d%%\n", up ? "up" : "down",
			(int)(100L * (vol_new - vol_min) / range));

	snd_mixer_close(handle);
}

// ---------------------------------------------------------------------------
// volume_launch_alsamixer
//
// Launches  alsamixer  in the user's preferred terminal by constructing a
// synthetic DesktopEntry with terminal=true and handing it to launch_app(),
// which already handles double-fork, setsid, and /dev/null redirection.
// ---------------------------------------------------------------------------
static void
volume_launch_alsamixer(void)
{
	if (verbose)
		printf("[VOL] Right click → alsamixer (via launch_app)\n");

	DesktopEntry entry = {
		.name = "alsamixer",
		.exec = "alsamixer",
		.icon = NULL,
		.terminal = 1,
	};
	launch_app(&entry);
}

// ---------------------------------------------------------------------------
// volume_handle_click
// ---------------------------------------------------------------------------
void
volume_handle_click(uint32_t button)
{
	if (button == BTN_LEFT) {
		if (verbose)
			printf("[VOL] Left click → toggle mute\n");
		volume_toggle_mute();
	} else if (button == BTN_RIGHT) {
		volume_launch_alsamixer();
	}
}

// ---------------------------------------------------------------------------
// volume_handle_scroll
// ---------------------------------------------------------------------------
void
volume_handle_scroll(int up)
{
	if (verbose)
		printf("[VOL] Scroll %s → %d%% step\n", up ? "up" : "down",
			VOLUME_STEP_PCT);
	volume_adjust(up);
}
