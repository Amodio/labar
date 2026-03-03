#define _GNU_SOURCE

#include "widget-date.h"
#include "config.h"

#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// External verbose flag (defined in main.c)
extern int verbose;

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
	double date_sz = (cfg && cfg->date_date_size > 0) ?
		(double)cfg->date_date_size :
		(double)WIDGET_DATE_DATE_SIZE;
	double time_sz = (cfg && cfg->date_time_size > 0) ?
		(double)cfg->date_time_size :
		(double)WIDGET_DATE_TIME_SIZE;

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

	// Width only: widest line + 20 % padding each side
	double max_w = de.width > te.width ? de.width : te.width;
	int w = (int)(max_w * 1.4) + 4;

	// Never go narrower than icon_size
	int icon_size = (cfg && cfg->icon_size > 0) ? cfg->icon_size : 64;
	if (w < icon_size)
		w = icon_size;

	if (verbose >= 2)
		printf("[DATE] computed tile width: %d px  "
			   "(date_w=%.1f time_w=%.1f)\n",
			w, de.width, te.width);

	return w;
}

// ---------------------------------------------------------------------------
// date_draw_tile
// ---------------------------------------------------------------------------
void
date_draw_tile(uint32_t *data, int width, int height, const Config *cfg)
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
	double date_sz = (cfg && cfg->date_date_size > 0) ?
		(double)cfg->date_date_size :
		(double)WIDGET_DATE_DATE_SIZE;
	double time_sz = (cfg && cfg->date_time_size > 0) ?
		(double)cfg->date_time_size :
		(double)WIDGET_DATE_TIME_SIZE;

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

	if (verbose >= 2)
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

	// Paint the background if one is configured (alpha > 0)
	if (cfg && cfg->date_bg_color) {
		unsigned int bg = cfg->date_bg_color;
		double bg_a = ((bg >> 24) & 0xFF) / 255.0;
		double bg_r = ((bg >> 16) & 0xFF) / 255.0;
		double bg_g = ((bg >> 8) & 0xFF) / 255.0;
		double bg_b = ((bg) & 0xFF) / 255.0;
		cairo_set_source_rgba(cr, bg_r, bg_g, bg_b, bg_a);
		cairo_paint(cr);
	}

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

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
		if (verbose >= 3)
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
		if (verbose >= 3)
			printf("[DATE] second changed (%d → %d), scheduling repaint\n",
				*last_second, cur);
		*last_second = cur;
		return 1;
	}

	return 0;
}
