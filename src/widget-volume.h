#ifndef VOLUME_H
#define VOLUME_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Volume widget
//
// Displays a volume icon from /usr/share/pixmaps/labar/ and overlays the
// current ALSA PCM volume percentage as a text label.
//
// All mixer operations are performed directly through alsa-lib
// (no external process / system() call).
//
// Mouse bindings:
//   Left click   : toggle PCM mute via alsa-lib
//   Right click  : spawn  foot -e alsamixer  (fork + execvp)
//   Scroll up    : raise PCM volume by VOLUME_STEP_PCT %
//   Scroll down  : lower PCM volume by VOLUME_STEP_PCT %
// ---------------------------------------------------------------------------

#define VOLUME_ICON_HIGH VOLUME_PIXMAP_DIR "/pnmixer-high.png"
#define VOLUME_ICON_MEDIUM VOLUME_PIXMAP_DIR "/pnmixer-medium.png"
#define VOLUME_ICON_LOW VOLUME_PIXMAP_DIR "/pnmixer-low.png"
#define VOLUME_ICON_OFF VOLUME_PIXMAP_DIR "/pnmixer-off.png"
#define VOLUME_ICON_MUTED VOLUME_PIXMAP_DIR "/pnmixer-muted.png"

// Thresholds that map a volume percentage to an icon
#define VOLUME_HIGH_THRESHOLD 75   // >= 75 % → high
#define VOLUME_MEDIUM_THRESHOLD 50 // >= 50 % → medium
#define VOLUME_LOW_THRESHOLD 25	   // >= 25 % → low, else off

// ---------------------------------------------------------------------------
// volume_get_info
//
// Query ALSA for the current PCM volume level and mute state.
// Tries the "PCM" simple element first; falls back to "Master".
//
// Parameters:
//   percent_out – receives the volume as a percentage (0–100)
//   muted_out   – receives 1 if the channel is muted, 0 otherwise
//
// Returns 0 on success, -1 on error.
// ---------------------------------------------------------------------------
int volume_get_info(int *percent_out, int *muted_out);

// ---------------------------------------------------------------------------
// volume_get_icon_path
//
// Return the path of the PNG icon that best represents the given state.
//
// Parameters:
//   percent – current volume percentage (0–100)
//   muted   – non-zero if the channel is muted
//
// Returns a string literal (no allocation needed).
// ---------------------------------------------------------------------------
const char *volume_get_icon_path(int percent, int muted);

// ---------------------------------------------------------------------------
// volume_get_label
//
// Format a human-readable label for the current volume state:
//   muted  → "muted"
//   active → "<percent> %"   (e.g. "42 %")
//
// Parameters:
//   buf     – destination buffer
//   buf_len – size of the buffer in bytes
//   percent – current volume percentage (0–100)
//   muted   – non-zero if the channel is muted
// ---------------------------------------------------------------------------
void volume_get_label(char *buf, int buf_len, int percent, int muted);

// ---------------------------------------------------------------------------
// volume_handle_click
//
// React to a mouse button press on the volume icon.
//   BTN_LEFT  → toggle PCM mute (alsa-lib)
//   BTN_RIGHT → launch exec_cmd (falls back to "foot -e alsamixer" if NULL)
//
// Parameters:
//   button    – Linux input event button code (BTN_LEFT / BTN_RIGHT)
//   exec_cmd  – right-click command from cfg->volume_exec (may be NULL)
// ---------------------------------------------------------------------------
void volume_handle_click(uint32_t button, const char *exec_cmd);

// ---------------------------------------------------------------------------
// volume_handle_scroll
//
// Raise or lower the PCM volume by VOLUME_STEP_PCT percent of the full
// hardware range using alsa-lib directly.
//
// Parameters:
//   up – non-zero to raise volume, zero to lower it
// ---------------------------------------------------------------------------
void volume_handle_scroll(int up);

#endif /* VOLUME_H */
