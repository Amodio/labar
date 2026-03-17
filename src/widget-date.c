#define _GNU_SOURCE

#include "widget-date.h"
#include "config.h"

#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// External verbose flag (defined in main.c)
extern int verbose;

// HiDPI scale factor (defined in main.c; 1 = normal, 2 = 2× HiDPI)
extern int buffer_scale;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/*
 * get_local_time — fill *tm_out with the current local time.
 * Returns 0 on success, -1 on error.
 */
static int
get_local_time(struct tm *tm_out)
{
	if (!tm_out)
		return -1;

	time_t now = time(NULL);
	if (now == (time_t)-1) {
		if (verbose >= 2)
			fprintf(stderr, "[DATE] time() failed\n");
		return -1;
	}

	if (!localtime_r(&now, tm_out)) {
		if (verbose >= 2)
			fprintf(stderr, "[DATE] localtime_r() failed\n");
		return -1;
	}

	return 0;
}

/*
 * format_time — run strftime into buf, falling back to fallback_fmt on error.
 */
static void
format_time(char *buf, int buf_len, const char *fmt, const struct tm *t,
	const char *fallback)
{
	if (!fmt || fmt[0] == '\0')
		fmt = fallback;

	if (strftime(buf, buf_len, fmt, t) == 0) {
		if (verbose >= 2)
			fprintf(stderr, "[DATE] strftime failed for format '%s'\n", fmt);
		snprintf(buf, buf_len, "%s", fallback);
	}
}

/*
 * fit_font_size
 *
 * Return the largest font size <= requested_size such that text fits within
 * max_width pixels (with ~12% horizontal padding).  Used to auto-scale text
 * on vertical bars where the tile is only icon_size wide.
 */
static double
fit_font_size(cairo_t *cr, const char *text, double max_width,
	double requested_size)
{
	double sz = requested_size;
	while (sz > 6.0) {
		cairo_set_font_size(cr, sz);
		cairo_text_extents_t ext;
		cairo_text_extents(cr, text, &ext);
		if (ext.width <= max_width * 0.88)
			return sz;
		sz -= 1.0;
	}
	return sz;
}

/*
 * draw_centered_text
 *
 * Draw a single line of text centered horizontally inside a Cairo context.
 * baseline_y is measured from the top of the surface (positive = down).
 * color is 0xAARRGGBB.
 */
static void
draw_centered_text(cairo_t *cr, int surface_width, const char *text,
	double baseline_y, double font_size, unsigned int color)
{
	if (!text || text[0] == '\0')
		return;

	cairo_set_font_size(cr, font_size);

	cairo_text_extents_t ext;
	cairo_text_extents(cr, text, &ext);

	double a = ((color >> 24) & 0xFF) / 255.0;
	double r = ((color >> 16) & 0xFF) / 255.0;
	double g = ((color >> 8) & 0xFF) / 255.0;
	double b = ((color) & 0xFF) / 255.0;
	cairo_set_source_rgba(cr, r, g, b, a);

	double x = (surface_width - ext.width) / 2.0 - ext.x_bearing;
	cairo_move_to(cr, x, baseline_y);
	cairo_show_text(cr, text);
}

// ---------------------------------------------------------------------------
// date_compute_tile_size
//
// Returns the width (pixels) the date slot needs along the bar axis.
// Height is always the caller's icon_size — this function does not touch it.
// ---------------------------------------------------------------------------
int
date_compute_tile_size(const Config *cfg)
{
	const char *date_fmt =
		(cfg && cfg->date_date_format && cfg->date_date_format[0]) ?
		cfg->date_date_format :
		WIDGET_DATE_DATE_FORMAT;
	const char *time_fmt =
		(cfg && cfg->date_time_format && cfg->date_time_format[0]) ?
		cfg->date_time_format :
		WIDGET_DATE_TIME_FORMAT;
	double date_sz =
		((cfg && cfg->date_date_size > 0) ? (double)cfg->date_date_size :
											(double)WIDGET_DATE_DATE_SIZE) *
		buffer_scale;
	double time_sz =
		((cfg && cfg->date_time_size > 0) ? (double)cfg->date_time_size :
											(double)WIDGET_DATE_TIME_SIZE) *
		buffer_scale;

	// Format representative strings with the current time
	struct tm t;
	char date_str[64] = "Xxx 00 Xxx";
	char time_str[64] = "00:00";
	if (get_local_time(&t) == 0) {
		format_time(date_str, sizeof(date_str), date_fmt, &t, "Xxx 00");
		format_time(time_str, sizeof(time_str), time_fmt, &t, "00:00");
	}

	// Measure on a tiny off-screen surface
	cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(cs);
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	cairo_set_font_size(cr, date_sz);
	cairo_text_extents_t de;
	cairo_text_extents(cr, date_str, &de);

	cairo_set_font_size(cr, time_sz);
	cairo_text_extents_t te;
	cairo_text_extents(cr, time_str, &te);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	// Width: use x_advance (total pen movement) which is more reliable than
	// ink width for multi-char strings.  Add 30% padding (15% each side).
	// Also account for x_bearing in case the first glyph has a left overhang.
	double d_full = de.x_advance - de.x_bearing;
	double t_full = te.x_advance - te.x_bearing;
	double max_w = d_full > t_full ? d_full : t_full;
	int w = (int)(max_w * 1.3) + 16; // explicit 8px padding each side

	// Never go narrower than icon_size
	int icon_size = (cfg && cfg->icon_size > 0) ? cfg->icon_size : 64;
	if (w < icon_size)
		w = icon_size;

	if (verbose >= 2)
		printf("[DATE] computed tile width: %d px  "
			   "(date_adv=%.1f time_adv=%.1f)\n",
			w, d_full, t_full);

	return w;
}

// ---------------------------------------------------------------------------
// draw_tile_background
//
// Draws a background rounded rect respecting corner_flags:
//   TILE_ROUND_LEFT  (bit 0) — round the two left  corners
//   TILE_ROUND_RIGHT (bit 1) — round the two right corners
// Unrounded corners are drawn as right angles, so adjacent tiles with
// backgrounds form a single visual pill.
// ---------------------------------------------------------------------------
static void
draw_tile_background(cairo_t *cr, int width, int height, unsigned int bg_color,
	int corner_flags)
{
	if (!bg_color)
		return;

	double bg_a = ((bg_color >> 24) & 0xFF) / 255.0;
	double bg_r = ((bg_color >> 16) & 0xFF) / 255.0;
	double bg_g = ((bg_color >> 8) & 0xFF) / 255.0;
	double bg_b = ((bg_color) & 0xFF) / 255.0;

	double r = (width < height ? width : height) * 0.25;
	double x0 = 0, y0 = 0, x1 = width, y1 = height;
	int rl = (corner_flags & TILE_ROUND_LEFT);
	int rr = (corner_flags & TILE_ROUND_RIGHT);

	cairo_new_path(cr);

	/* Start: top-left */
	if (rl) {
		/* arc from 180° to 270° (top-left corner) */
		cairo_arc(cr, x0 + r, y0 + r, r, M_PI, 3.0 * M_PI / 2.0);
	} else {
		cairo_move_to(cr, x0, y0);
	}

	/* Top edge → top-right */
	if (rr) {
		cairo_line_to(cr, x1 - r, y0);
		cairo_arc(cr, x1 - r, y0 + r, r, 3.0 * M_PI / 2.0, 2.0 * M_PI);
	} else {
		cairo_line_to(cr, x1, y0);
	}

	/* Right edge → bottom-right */
	if (rr) {
		cairo_line_to(cr, x1, y1 - r);
		cairo_arc(cr, x1 - r, y1 - r, r, 0, M_PI / 2.0);
	} else {
		cairo_line_to(cr, x1, y1);
	}

	/* Bottom edge → bottom-left */
	if (rl) {
		cairo_line_to(cr, x0 + r, y1);
		cairo_arc(cr, x0 + r, y1 - r, r, M_PI / 2.0, M_PI);
	} else {
		cairo_line_to(cr, x0, y1);
	}

	/* Left edge back to start */
	cairo_close_path(cr);

	cairo_set_source_rgba(cr, bg_r, bg_g, bg_b, bg_a);
	cairo_fill(cr);
}
// ---------------------------------------------------------------------------
// date_draw_tile
// ---------------------------------------------------------------------------
void
date_draw_tile(uint32_t *data, int width, int height, const Config *cfg,
	int corner_flags)
{
	if (!data || width <= 0 || height <= 0)
		return;

	// Resolve config values, falling back to compiled-in defaults
	const char *date_fmt =
		(cfg && cfg->date_date_format && cfg->date_date_format[0]) ?
		cfg->date_date_format :
		WIDGET_DATE_DATE_FORMAT;
	const char *time_fmt =
		(cfg && cfg->date_time_format && cfg->date_time_format[0]) ?
		cfg->date_time_format :
		WIDGET_DATE_TIME_FORMAT;
	unsigned int date_col = (cfg && cfg->date_date_color) ?
		cfg->date_date_color :
		WIDGET_DATE_DATE_COLOR;
	unsigned int time_col = (cfg && cfg->date_time_color) ?
		cfg->date_time_color :
		WIDGET_DATE_TIME_COLOR;
	double date_sz =
		((cfg && cfg->date_date_size > 0) ? (double)cfg->date_date_size :
											(double)WIDGET_DATE_DATE_SIZE) *
		buffer_scale;
	double time_sz =
		((cfg && cfg->date_time_size > 0) ? (double)cfg->date_time_size :
											(double)WIDGET_DATE_TIME_SIZE) *
		buffer_scale;

	// Obtain current time
	struct tm t;
	const char *fallback_date = "---";
	const char *fallback_time = "--:--";
	char date_str[64];
	char time_str[64];

	if (get_local_time(&t) == 0) {
		format_time(date_str, sizeof(date_str), date_fmt, &t, fallback_date);
		format_time(time_str, sizeof(time_str), time_fmt, &t, fallback_time);
	} else {
		snprintf(date_str, sizeof(date_str), "%s", fallback_date);
		snprintf(time_str, sizeof(time_str), "%s", fallback_time);
	}

	if (verbose >= 4)
		printf("[DATE] tile %dx%d  date='%s'  time='%s'\n", width, height,
			date_str, time_str);

	// Set up Cairo surface over the pixel buffer
	cairo_surface_t *cs =
		cairo_image_surface_create_for_data((unsigned char *)data,
			CAIRO_FORMAT_ARGB32, width, height, width * 4);
	cairo_t *cr = cairo_create(cs);

	// Clear to fully transparent
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	// Paint background (corner rounding driven by corner_flags)
	if (cfg && cfg->date_bg_color)
		draw_tile_background(cr, width, height, cfg->date_bg_color,
			corner_flags);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	// Auto-scale font sizes down if the text is wider than the tile.
	// This happens on vertical bars where tile width == icon_size.
	date_sz = fit_font_size(cr, date_str, width, date_sz);
	time_sz = fit_font_size(cr, time_str, width, time_sz);

	// -----------------------------------------------------------------------
	// Layout: divide the tile height into two horizontal bands.
	//
	//   Upper band  [0 .. mid)       → date line
	//   Lower band  [mid .. height)  → time line
	//
	// The baseline of each line is placed at 80 % of its band height so
	// that ascenders sit comfortably inside the band and descenders (g, y…)
	// remain mostly visible.
	// -----------------------------------------------------------------------
	int mid = height / 2;

	double date_baseline = mid * 0.82;
	double time_baseline = mid + (height - mid) * 0.82;

	draw_centered_text(cr, width, date_str, date_baseline, date_sz, date_col);
	draw_centered_text(cr, width, time_str, time_baseline, time_sz, time_col);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

// ---------------------------------------------------------------------------
// date_get_tooltip
// ---------------------------------------------------------------------------
void
date_get_tooltip(char *buf, int buf_len)
{
	if (!buf || buf_len <= 0)
		return;

	struct tm t;
	if (get_local_time(&t) < 0) {
		snprintf(buf, buf_len, "(unknown)");
		return;
	}

	strftime(buf, buf_len, "%A, %B %d %Y  %H:%M:%S", &t);
}

// ---------------------------------------------------------------------------
// date_widget_needs_repaint
// ---------------------------------------------------------------------------
int
date_widget_needs_repaint(int *last_minute)
{
	if (!last_minute)
		return 0;

	struct tm t;
	if (get_local_time(&t) < 0)
		return 0;

	int cur = t.tm_hour * 60 + t.tm_min;

	if (cur != *last_minute) {
		if (verbose >= 4)
			printf("[DATE] minute changed (%d → %d), scheduling repaint\n",
				*last_minute, cur);
		*last_minute = cur;
		return 1;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// date_widget_needs_repaint_seconds
// ---------------------------------------------------------------------------
int
date_widget_needs_repaint_seconds(int *last_second)
{
	if (!last_second)
		return 0;

	struct tm t;
	if (get_local_time(&t) < 0)
		return 0;

	int cur = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;

	if (cur != *last_second) {
		if (verbose >= 4)
			printf("[DATE] second changed (%d → %d), scheduling repaint\n",
				*last_second, cur);
		*last_second = cur;
		return 1;
	}

	return 0;
}
