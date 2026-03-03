#ifndef WIDGET_DATE_H
#define WIDGET_DATE_H

#include <stdint.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Date / time widget
//
// Renders a pure-text tile — no PNG icon required.  The tile is divided into
// two lines drawn directly onto the shared-memory pixel buffer with Cairo:
//
//   Line 1 (upper half)  – date string, formatted with date_date_format
//                          (default: "%a %d")   e.g.  "Tue 03"
//   Line 2 (lower half)  – time string, formatted with date_time_format
//                          (default: "%H:%M")   e.g.  "14:37"
//
// Each line has its own color and font size, controlled by dedicated keys in
// the [global] section of labar.cfg:
//
//   date-date-format   strftime format for line 1  (default: "%a %d")
//   date-date-color    hex color  #RRGGBB[AA]       (default: #FFFFFF)
//   date-date-size     font size in points          (default: 10)
//   date-time-format   strftime format for line 2  (default: "%H:%M")
//   date-time-color    hex color  #RRGGBB[AA]       (default: #FFFFFF)
//   date-time-size     font size in points          (default: 14)
//
// The tile background is fully transparent.
// The widget is display-only — no mouse bindings are registered.
// Repainting is driven by date_widget_needs_repaint(), which fires once per
// minute.
// ---------------------------------------------------------------------------

// Default strftime(3) format strings
#define WIDGET_DATE_DATE_FORMAT "%a %d" // e.g. "Tue 03"
#define WIDGET_DATE_TIME_FORMAT "%H:%M" // e.g. "14:37"

// Default visual properties (used when config fields are 0 / NULL)
#define WIDGET_DATE_DATE_COLOR 0xFFFFFFFF // opaque white
#define WIDGET_DATE_DATE_SIZE 10		  // pt
#define WIDGET_DATE_TIME_COLOR 0xFFFFFFFF // opaque white
#define WIDGET_DATE_TIME_SIZE 14		  // pt

// ---------------------------------------------------------------------------
// date_compute_tile_size
//
// Measure both text lines with Cairo and return the width (in pixels) that
// the date slot needs along the bar axis so that neither line is clipped.
//
// The bar cross-dimension (height for a horizontal bar, width for a vertical
// bar) is always icon_size and is NOT affected by font size.
//
// Call this once after the config is loaded and store the result in
// cfg->date_tile_width.  All layout code uses that field.
// ---------------------------------------------------------------------------
int date_compute_tile_size(const Config *cfg);

// ---------------------------------------------------------------------------
// date_draw_tile
//
// Render the date+time widget into a caller-supplied pixel buffer.
// The buffer must be at least (width * height * 4) bytes.
// The function clears it to fully transparent before drawing.
//
// Parameters:
//   data   – ARGB8888 pixel buffer (width × height)
//   width  – tile width  in pixels
//   height – tile height in pixels
//   cfg    – application config (format strings, colors, sizes)
// ---------------------------------------------------------------------------
void date_draw_tile(uint32_t *data, int width, int height, const Config *cfg);

// ---------------------------------------------------------------------------
// date_get_tooltip
//
// Full human-readable date+time string for debug logging.
// Always uses the long format "%A, %B %d %Y  %H:%M:%S".
// ---------------------------------------------------------------------------
void date_get_tooltip(char *buf, int buf_len);

// ---------------------------------------------------------------------------
// date_widget_needs_repaint
//
// Returns 1 (and updates *last_minute) when the current minute has changed.
// Pass *last_minute = -1 on first call to force an immediate repaint.
// Use this when the time format has no seconds-level directive.
// ---------------------------------------------------------------------------
int date_widget_needs_repaint(int *last_minute);

// ---------------------------------------------------------------------------
// date_widget_needs_repaint_seconds
//
// Returns 1 (and updates *last_second) when the current second has changed.
// Pass *last_second = -1 on first call to force an immediate repaint.
// Use this when the time format includes %S, %T, %X, %c or %r.
// ---------------------------------------------------------------------------
int date_widget_needs_repaint_seconds(int *last_second);

#endif /* WIDGET_DATE_H */
