#ifndef WIDGET_SYSINFO_H
#define WIDGET_SYSINFO_H

#include <stdint.h>
#include "config.h"

// ---------------------------------------------------------------------------
// System-info widget (CPU + RAM usage)
//
// Renders a text-only tile using Cairo:
//
//   Line 1 (top third)    – CPU usage      e.g. "CPU  42%"
//   Line 2 (middle third) – max CPU temp   e.g. "Temp 67°C"
//   Line 3 (bottom third) – RAM usage      e.g. "RAM  61%"
//
// CPU is sampled from /proc/stat (idle-time delta between two readings).
// RAM is read from /proc/meminfo (MemTotal – MemAvailable) / MemTotal.
//
// Config section: [widget-sysinfo]
//   cpu-color   #RRGGBB[AA]   color for the CPU line  (default: #FFEB3B)
//   tmp-color   #RRGGBB[AA]   color for the TMP line  (default: #FF7043)
//   ram-color   #RRGGBB[AA]   color for the RAM line  (default: #66BB6A)
//   size        N (pt)        font size                (default: 14)
//   bg-color    #RRGGBBAA     tile background          (default: transparent)
//
// In [global]:
//   show-sysinfo   true / false   (default: true)
//
// The tile is display-only — no mouse bindings are registered.
// sysinfo_widget_needs_repaint() fires once per second.
// ---------------------------------------------------------------------------

#define WIDGET_SYSINFO_CPU_COLOR 0xFFFFEB3B // yellow
#define WIDGET_SYSINFO_RAM_COLOR 0xFF66BB6A // green
#define WIDGET_SYSINFO_TMP_COLOR 0xFFFF7043 // deep orange
#define WIDGET_SYSINFO_FONT_SIZE 14			// pt

// ---------------------------------------------------------------------------
// sysinfo_widget_init
//
// Seed the CPU idle counters so the first sample produces 0 % instead of a
// garbage spike.  Call once after the config is loaded.
// ---------------------------------------------------------------------------
void sysinfo_widget_init(void);

// ---------------------------------------------------------------------------
// sysinfo_compute_tile_size
//
// Measure the two text lines with Cairo and return the width (pixels) the
// sysinfo slot needs along the bar axis.
// ---------------------------------------------------------------------------
int sysinfo_compute_tile_size(const Config *cfg);

// ---------------------------------------------------------------------------
// sysinfo_draw_tile
//
// Render the CPU/RAM widget into a caller-supplied ARGB8888 buffer.
// Clears to transparent before drawing.
// ---------------------------------------------------------------------------
void sysinfo_draw_tile(uint32_t *data, int width, int height, const Config *cfg,
	int corner_flags);

// ---------------------------------------------------------------------------
// sysinfo_widget_needs_repaint
//
// Sample /proc/stat and /proc/meminfo, update internal state.
// Returns 1 when the second has changed (always true after ≥ 1 s), 0 otherwise.
// Pass *last_second = -1 on first call to force an immediate repaint.
//
// percpu: if non-zero, CPU% is multiplied by the number of logical cores so
//         that one fully-loaded core shows 100% (matches top's per-process
//         style). If zero, shows system-wide aggregate (one core at 100% on
//         a 12-core machine shows ~8%).
// ---------------------------------------------------------------------------
int sysinfo_widget_needs_repaint(int *last_second, int percpu);

#endif /* WIDGET_SYSINFO_H */
