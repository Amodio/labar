#define _GNU_SOURCE

#include "widget-common.h"
#include "config.h"

#include <cairo.h>
#include <math.h>

// ---------------------------------------------------------------------------
// fit_font_size
// ---------------------------------------------------------------------------
double
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

// ---------------------------------------------------------------------------
// draw_centered_text
// ---------------------------------------------------------------------------
void
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
// draw_tile_background
// ---------------------------------------------------------------------------
void
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
