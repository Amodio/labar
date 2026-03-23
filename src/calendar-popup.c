#define _GNU_SOURCE

#include "calendar-popup.h"
#include "config.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

// External globals provided by main.c
extern struct wl_display *display;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;
extern struct zwlr_layer_shell_v1 *layer_shell;
extern Config app_config;
extern int verbose;
extern int buffer_scale;

// ---------------------------------------------------------------------------
// Layout constants (logical pixels, scaled by buffer_scale for the buffer)
// ---------------------------------------------------------------------------
#define CAL_COLS 3		   // months per row
#define CAL_ROWS 4		   // rows of months
#define CAL_CELL_W 26	   // width of one day cell
#define CAL_CELL_H 20	   // height of one day cell
#define CAL_MONTH_PAD_X 12 // horizontal padding around a month grid
#define CAL_MONTH_PAD_Y 8  // vertical padding above/below a month grid
#define CAL_MONTH_HDR 22   // height of the month name header
#define CAL_DOW_HDR 18	   // height of the day-of-week row
#define CAL_GRID_W (7 * CAL_CELL_W)
#define CAL_MONTH_W (CAL_GRID_W + 2 * CAL_MONTH_PAD_X)
#define CAL_MONTH_H                                                            \
	(CAL_MONTH_HDR + CAL_DOW_HDR + 6 * CAL_CELL_H + 2 * CAL_MONTH_PAD_Y)
#define CAL_GAP_X 10
#define CAL_GAP_Y 10
#define CAL_BORDER 14 // outer padding
#define CAL_POPUP_W                                                            \
	(CAL_COLS * CAL_MONTH_W + (CAL_COLS - 1) * CAL_GAP_X + 2 * CAL_BORDER)
#define CAL_POPUP_H                                                            \
	(CAL_ROWS * CAL_MONTH_H + (CAL_ROWS - 1) * CAL_GAP_Y + 2 * CAL_BORDER +    \
		30) // +30 for year header

// Colours (ARGB)
#define COL_BG 0xF0101820u		  // very dark blue-grey, nearly opaque
#define COL_BORDER 0xFF1E2D3Du	  // slightly lighter border
#define COL_YEAR 0xFFFFFFFFu	  // white
#define COL_MONTH_HDR 0xFF68D4FFu // light blue
#define COL_DOW 0xFF8090A0u		  // grey-blue
#define COL_DAY 0xFFCCCCCCu		  // light grey
#define COL_TODAY 0xFFFFEB3Bu	  // yellow
#define COL_TODAY_BG 0x3FFFFF00u  // translucent yellow bg
#define COL_WEEKEND 0xFFFF6B6Bu	  // soft red

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static struct {
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surf;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t pixels_size; // byte length of the mmap backing pop.pixels
	int w, h;
	int phys_w, phys_h;
	int open;
	int configured;
	int pending_close; // deferred destroy
} pop = {0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void
set_cairo_color(cairo_t *cr, unsigned int argb)
{
	double a = ((argb >> 24) & 0xFF) / 255.0;
	double r = ((argb >> 16) & 0xFF) / 255.0;
	double g = ((argb >> 8) & 0xFF) / 255.0;
	double b = ((argb) & 0xFF) / 255.0;
	cairo_set_source_rgba(cr, r, g, b, a);
}

// Days in month (1-indexed, year is full e.g. 2025)
static int
days_in_month(int month, int year)
{
	static const int days[] = {
		0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month == 2) {
		int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
		return 28 + leap;
	}
	return days[month];
}

// Day of week for 1st of month (0=Sun … 6=Sat)
static int
first_weekday(int month, int year)
{
	struct tm t = {0};
	t.tm_year = year - 1900;
	t.tm_mon = month - 1;
	t.tm_mday = 1;
	mktime(&t);
	return t.tm_wday; // 0=Sun
}

static void
draw_calendar(uint32_t *pixels, int pw, int ph)
{
	time_t now = time(NULL);
	struct tm *today = localtime(&now);
	int cur_year = today->tm_year + 1900;
	int cur_month = today->tm_mon + 1; // 1-12
	int cur_day = today->tm_mday;

	double sc = (double)buffer_scale;

	cairo_surface_t *cs =
		cairo_image_surface_create_for_data((unsigned char *)pixels,
			CAIRO_FORMAT_ARGB32, pw, ph, pw * 4);
	cairo_t *cr = cairo_create(cs);

	// Clear
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	// Background rounded rect
	double r = 12 * sc;
	double x0 = 0, y0 = 0, x1 = pw, y1 = ph;
	cairo_new_path(cr);
	cairo_arc(cr, x0 + r, y0 + r, r, M_PI, 3 * M_PI / 2);
	cairo_arc(cr, x1 - r, y0 + r, r, 3 * M_PI / 2, 2 * M_PI);
	cairo_arc(cr, x1 - r, y1 - r, r, 0, M_PI / 2);
	cairo_arc(cr, x0 + r, y1 - r, r, M_PI / 2, M_PI);
	cairo_close_path(cr);
	set_cairo_color(cr, COL_BG);
	cairo_fill(cr);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	// Year header
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 18 * sc);
	char year_str[12];
	snprintf(year_str, sizeof(year_str), "%d", cur_year);
	cairo_text_extents_t ye;
	cairo_text_extents(cr, year_str, &ye);
	set_cairo_color(cr, COL_YEAR);
	cairo_move_to(cr, (pw - ye.width) / 2.0 - ye.x_bearing,
		CAL_BORDER * sc + 20 * sc);
	cairo_show_text(cr, year_str);

	// Build locale-aware month names and 2-char day-of-week abbreviations
	// using strftime so they respect LC_TIME / LANG.
	char month_names[12][32];
	char dow_names[7][8]; // 2 visible chars + NUL, but strftime may give more
	{
		struct tm t = {0};
		t.tm_year = cur_year - 1900;
		t.tm_mday = 1;
		for (int m = 0; m < 12; m++) {
			t.tm_mon = m;
			mktime(&t);
			strftime(month_names[m], sizeof(month_names[m]), "%B", &t);
		}
		// Get abbreviated weekday names: Sunday=0 … Saturday=6
		// Use a known reference date: 2006-01-01 was a Sunday.
		struct tm ref = {0};
		ref.tm_year = 106;
		ref.tm_mon = 0;
		ref.tm_mday = 1;
		mktime(&ref);
		for (int d = 0; d < 7; d++) {
			char full[32];
			strftime(full, sizeof(full), "%a", &ref);
			// Keep at most 2 UTF-8 characters for the header cell
			int bytes = 0, chars = 0;
			unsigned char *p = (unsigned char *)full;
			while (*p && chars < 2) {
				int len = (*p < 0x80) ? 1 :
					(*p < 0xE0)		  ? 2 :
					(*p < 0xF0)		  ? 3 :
										4;
				bytes += len;
				chars++;
				p += len;
			}
			memcpy(dow_names[d], full, bytes);
			dow_names[d][bytes] = '\0';
			// Advance reference date by 1 day
			ref.tm_mday++;
			mktime(&ref);
		}
	}

	for (int m = 1; m <= 12; m++) {
		int col = (m - 1) % CAL_COLS;
		int row = (m - 1) / CAL_COLS;

		double mx = (CAL_BORDER + col * (CAL_MONTH_W + CAL_GAP_X)) * sc;
		double my = (CAL_BORDER + 30 + row * (CAL_MONTH_H + CAL_GAP_Y)) * sc;

		// Month name
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
			CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 11 * sc);
		cairo_text_extents_t me;
		cairo_text_extents(cr, month_names[m - 1], &me);
		set_cairo_color(cr, COL_MONTH_HDR);
		cairo_move_to(cr,
			mx + (CAL_MONTH_W * sc - me.width) / 2.0 - me.x_bearing,
			my + CAL_MONTH_PAD_Y * sc + 12 * sc);
		cairo_show_text(cr, month_names[m - 1]);

		// Day-of-week headers
		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
			CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size(cr, 9 * sc);
		double dow_y = my + (CAL_MONTH_PAD_Y + CAL_MONTH_HDR + 12) * sc;
		for (int d = 0; d < 7; d++) {
			cairo_text_extents_t de;
			cairo_text_extents(cr, dow_names[d], &de);
			int is_weekend = (d == 0 || d == 6);
			set_cairo_color(cr, is_weekend ? COL_WEEKEND : COL_DOW);
			double dx = mx + (CAL_MONTH_PAD_X + d * CAL_CELL_W) * sc +
				(CAL_CELL_W * sc - de.width) / 2.0 - de.x_bearing;
			cairo_move_to(cr, dx, dow_y);
			cairo_show_text(cr, dow_names[d]);
		}

		// Days
		int fd = first_weekday(m, cur_year);
		int nday = days_in_month(m, cur_year);
		int cell = fd; // 0-based cell index in a 7-col grid

		cairo_set_font_size(cr, 9 * sc);
		for (int day = 1; day <= nday; day++, cell++) {
			int dc = cell % 7; // column
			int dr = cell / 7; // row
			double dx = mx + (CAL_MONTH_PAD_X + dc * CAL_CELL_W) * sc;
			double dy = my +
				(CAL_MONTH_PAD_Y + CAL_MONTH_HDR + CAL_DOW_HDR +
					dr * CAL_CELL_H) *
					sc;

			int is_today = (m == cur_month && day == cur_day);
			int is_weekend = (dc == 0 || dc == 6);

			// Today highlight
			if (is_today) {
				double hr = CAL_CELL_H * sc * 0.42;
				cairo_new_path(cr);
				cairo_arc(cr, dx + CAL_CELL_W * sc / 2.0,
					dy + CAL_CELL_H * sc / 2.0, hr, 0, 2 * M_PI);
				set_cairo_color(cr, COL_TODAY_BG);
				cairo_fill(cr);
			}

			char day_str[12];
			snprintf(day_str, sizeof(day_str), "%d", day);
			cairo_text_extents_t te;
			cairo_text_extents(cr, day_str, &te);
			unsigned int col = is_today ? COL_TODAY :
				is_weekend				? COL_WEEKEND :
										  COL_DAY;
			set_cairo_color(cr, col);
			cairo_move_to(cr,
				dx + (CAL_CELL_W * sc - te.width) / 2.0 - te.x_bearing,
				dy + (CAL_CELL_H * sc + te.height) / 2.0);
			cairo_show_text(cr, day_str);
		}
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

// ---------------------------------------------------------------------------
// SHM buffer
// ---------------------------------------------------------------------------
static struct wl_buffer *
make_buffer(int pw, int ph, uint32_t **out, size_t *out_size)
{
	int size = pw * ph * 4;
	int fd = memfd_create("cal-popup", 0);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}
	void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	*out = (uint32_t *)m;
	*out_size = size;
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, pw, ph, pw * 4,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buf;
}

// ---------------------------------------------------------------------------
// Layer surface callbacks
// ---------------------------------------------------------------------------
static void
pop_configure(void *data, struct zwlr_layer_surface_v1 *surf, uint32_t serial,
	uint32_t w, uint32_t h)
{
	(void)data;
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(surf, serial);
	pop.configured = 1;

	pop.phys_w = pop.w * buffer_scale;
	pop.phys_h = pop.h * buffer_scale;

	if (pop.buffer) {
		if (pop.pixels && pop.pixels_size) {
			munmap(pop.pixels, pop.pixels_size);
			pop.pixels = NULL;
			pop.pixels_size = 0;
		}
		wl_buffer_destroy(pop.buffer);
		pop.buffer = NULL;
	}

	pop.buffer =
		make_buffer(pop.phys_w, pop.phys_h, &pop.pixels, &pop.pixels_size);
	if (!pop.buffer)
		return;

	draw_calendar(pop.pixels, pop.phys_w, pop.phys_h);

	wl_surface_attach(pop.surface, pop.buffer, 0, 0);
	if (buffer_scale > 1)
		wl_surface_set_buffer_scale(pop.surface, buffer_scale);
	wl_surface_damage(pop.surface, 0, 0, pop.w, pop.h);
	wl_surface_commit(pop.surface);
}

static void
pop_closed(void *data, struct zwlr_layer_surface_v1 *surf)
{
	(void)data;
	(void)surf;
	pop.open = 0;
	pop.pending_close = 1;
	if (verbose >= 1)
		printf("[CAL] popup closed by compositor (deferred)\n");
}

static const struct zwlr_layer_surface_v1_listener pop_listener = {
	.configure = pop_configure,
	.closed = pop_closed,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/* Actually destroy all Wayland objects. Called from calendar_popup_dispatch
 * (main loop) never from within a Wayland event callback, to avoid
 * destroying proxies while libwayland is still processing their events. */
static void
pop_destroy(void)
{
	if (pop.buffer) {
		if (pop.pixels && pop.pixels_size) {
			munmap(pop.pixels, pop.pixels_size);
			pop.pixels = NULL;
			pop.pixels_size = 0;
		}
		wl_buffer_destroy(pop.buffer);
		pop.buffer = NULL;
	}
	if (pop.layer_surf) {
		zwlr_layer_surface_v1_destroy(pop.layer_surf);
		pop.layer_surf = NULL;
	}
	if (pop.surface) {
		wl_surface_destroy(pop.surface);
		pop.surface = NULL;
	}
	pop.open = 0;
	pop.configured = 0;
	pop.pending_close = 0;
	if (verbose >= 1)
		printf("[CAL] popup closed\n");
}

void
calendar_popup_toggle(void)
{
	if (pop.open) {
		/* Don't destroy Wayland objects here — we may be inside a Wayland
		 * event callback (pointer_button on the popup surface). Destroying
		 * proxies while libwayland is dispatching their events causes
		 * wl_proxy_get_version() segfaults on the pending leave/enter events.
		 * Instead, mark for deferred destruction and hide the surface now. */
		pop.open = 0; // make get_surface() return NULL immediately
		pop.pending_close = 1;
		if (verbose >= 1)
			printf("[CAL] popup closing (deferred)\n");
		return;
	}

	if (pop.pending_close) {
		/* Still waiting to be destroyed — don't re-open yet */
		return;
	}

	// Open
	pop.w = CAL_POPUP_W;
	pop.h = CAL_POPUP_H;

	pop.surface = wl_compositor_create_surface(compositor);
	if (!pop.surface)
		return;

	// Determine which Wayland layer to use (one above the bar)
	uint32_t wl_layer;
	switch (app_config.layer) {
	case 0:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
		break;
	case 1:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
		break;
	default:
		wl_layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
		break;
	}

	pop.layer_surf = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
		pop.surface, NULL, wl_layer, "labar-calendar");
	if (!pop.layer_surf) {
		wl_surface_destroy(pop.surface);
		pop.surface = NULL;
		return;
	}

	// Anchor to the same edge as the bar so the popup appears near the date
	uint32_t anchor = 0;
	switch (app_config.position) {
	case 0: // bottom
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		break;
	case 1: // top
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		break;
	case 2: // left
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		break;
	case 3: // right
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
			ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		break;
	}
	zwlr_layer_surface_v1_set_anchor(pop.layer_surf, anchor);
	zwlr_layer_surface_v1_set_size(pop.layer_surf, pop.w, pop.h);
	zwlr_layer_surface_v1_set_exclusive_zone(pop.layer_surf, -1);
	// Request keyboard interactivity so Escape can close it
	zwlr_layer_surface_v1_set_keyboard_interactivity(pop.layer_surf,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);

	zwlr_layer_surface_v1_add_listener(pop.layer_surf, &pop_listener, NULL);
	wl_surface_commit(pop.surface);

	pop.open = 1;
	if (verbose >= 1)
		printf("[CAL] popup opened (%dx%d logical)\n", pop.w, pop.h);
}

int
calendar_popup_dispatch(void)
{
	/* Perform deferred destruction now that we're safely in the main loop,
	 * outside any Wayland event callback. */
	if (pop.pending_close)
		pop_destroy();
	return pop.open || pop.pending_close;
}

int
calendar_popup_is_open(void)
{
	return pop.open; // pending_close already set open=0
}

struct wl_surface *
calendar_popup_get_surface(void)
{
	/* Return the surface while pending_close too so seat.c can still
	 * match pointer_surface and suppress spurious bar events during
	 * the close transition. */
	return (pop.open || pop.pending_close) ? pop.surface : NULL;
}
