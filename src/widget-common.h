#ifndef WIDGET_COMMON_H
#define WIDGET_COMMON_H

#include <cairo.h>

/*
 * fit_font_size
 *
 * Return the largest font size <= requested_size such that text fits within
 * max_width pixels (with ~12% horizontal padding).  Used to auto-scale text
 * on vertical bars where the tile is only icon_size wide.
 */
double fit_font_size(cairo_t *cr, const char *text, double max_width,
	double requested_size);

/*
 * draw_centered_text
 *
 * Draw a single line of text centered horizontally inside a Cairo context.
 * baseline_y is measured from the top of the surface (positive = down).
 * color is 0xAARRGGBB.
 */
void draw_centered_text(cairo_t *cr, int surface_width, const char *text,
	double baseline_y, double font_size, unsigned int color);

/*
 * draw_tile_background
 *
 * Draw a background rounded rect respecting corner_flags:
 *   TILE_ROUND_LEFT  (bit 0) — round the two left  corners
 *   TILE_ROUND_RIGHT (bit 1) — round the two right corners
 * Unrounded corners are drawn as right angles, so adjacent bg-enabled tiles
 * form a single visual pill.
 */
void draw_tile_background(cairo_t *cr, int width, int height,
	unsigned int bg_color, int corner_flags);

#endif /* WIDGET_COMMON_H */
