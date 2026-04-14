#define _GNU_SOURCE

#include "widget-net.h"
#include "config.h"
#include "widget-common.h"

#include <cairo.h>
#include <ctype.h>
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
// Internal state
// ---------------------------------------------------------------------------

// Maximum length for an interface name (IFNAMSIZ = 16 on Linux)
#define IFACE_NAME_LEN 16

typedef struct {
	char iface[IFACE_NAME_LEN]; // resolved interface name
	// Values sampled on the previous tick
	long long prev_rx_bytes;
	long long prev_tx_bytes;
	struct timespec prev_ts;
	// Last computed speeds (bytes / second)
	double rx_bps;
	double tx_bps;
	// Cached label strings written by net_widget_needs_repaint()
	char rx_label[32];
	char tx_label[32];
	int initialized;
} NetState;

static NetState g_net = {0};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/*
 * read_proc_net_dev
 *
 * Parse /proc/net/dev and return the rx_bytes / tx_bytes for *iface*.
 * Returns 0 on success, -1 if the interface is not found.
 */
static int
read_proc_net_dev(const char *iface, long long *rx_out, long long *tx_out)
{
	FILE *fp = fopen("/proc/net/dev", "r");
	if (!fp)
		return -1;

	char line[256];
	int found = 0;

	while (fgets(line, sizeof(line), fp)) {
		// Lines look like:
		//   eth0:   123456  789  0  0  0  0  0  0   654321  ...
		char *colon = strchr(line, ':');
		if (!colon)
			continue;

		// Extract the interface name (trim leading spaces)
		char name[IFACE_NAME_LEN] = {0};
		char *p = line;
		while (*p == ' ')
			p++;
		int len = (int)(colon - p);
		if (len <= 0 || len >= IFACE_NAME_LEN)
			continue;
		memcpy(name, p, len);
		// rtrim
		while (len > 0 && name[len - 1] == ' ')
			name[--len] = '\0';

		if (strcmp(name, iface) != 0)
			continue;

		// Parse the 16 fields after the colon.
		// Field 0 = rx_bytes, field 8 = tx_bytes.
		long long fields[16] = {0};
		char *s = colon + 1;
		for (int i = 0; i < 16; i++) {
			while (*s == ' ')
				s++;
			fields[i] = strtoll(s, &s, 10);
		}
		*rx_out = fields[0];
		*tx_out = fields[8];
		found = 1;
		break;
	}

	fclose(fp);
	return found ? 0 : -1;
}

/*
 * find_default_iface
 *
 * Pick the first non-loopback interface in /proc/net/dev that has at
 * least one byte of traffic (rx or tx > 0).  Falls back to the first
 * non-loopback interface even if it is idle.
 */
static void
find_default_iface(char *out, int out_len)
{
	FILE *fp = fopen("/proc/net/dev", "r");
	if (!fp) {
		snprintf(out, out_len, "eth0");
		return;
	}

	char line[256];
	char first_non_lo[IFACE_NAME_LEN] = {0};
	out[0] = '\0';

	// Skip the two header lines
	fgets(line, sizeof(line), fp);
	fgets(line, sizeof(line), fp);

	while (fgets(line, sizeof(line), fp)) {
		char *colon = strchr(line, ':');
		if (!colon)
			continue;

		// Extract and trim interface name
		char name[IFACE_NAME_LEN] = {0};
		char *p = line;
		while (*p == ' ')
			p++;
		int len = (int)(colon - p);
		if (len <= 0 || len >= IFACE_NAME_LEN)
			continue;

		memcpy(name, p, len);
		name[len] = '\0';
		// rtrim spaces
		while (len > 0 && name[len - 1] == ' ')
			name[--len] = '\0';

		if (strcmp(name, "lo") == 0)
			continue;

		long long fields[16] = {0};
		char *s = colon + 1;
		for (int i = 0; i < 16; i++) {
			while (*s == ' ')
				s++;
			fields[i] = strtoll(s, &s, 10);
		}
		long long rx = fields[0], tx = fields[8];

		if (first_non_lo[0] == '\0')
			snprintf(first_non_lo, sizeof(first_non_lo), "%s", name);

		if (rx > 0 || tx > 0) {
			snprintf(out, out_len, "%s", name);
			fclose(fp);
			return;
		}
	}

	fclose(fp);

	if (out[0] == '\0' && first_non_lo[0] != '\0')
		snprintf(out, out_len, "%s", first_non_lo);

	if (out[0] == '\0')
		snprintf(out, out_len, "eth0");
}

/*
 * format_speed
 *
 * Format bytes-per-second into a human-readable string.
 * Always uses KB/MB/GB — never raw bytes.
 * Fixed-width output so both lines stay aligned (max 999.9 per unit):
 *   "↓ 123.4 GB/s"   "↑   0.1 KB/s"
 */
static void
format_speed(char *buf, int buf_len, double bps, const char *arrow)
{
	double kbps = bps / 1e3;
	if (kbps >= 1e6)
		snprintf(buf, buf_len, "%s %5.1f GB/s", arrow, kbps / 1e6);
	else if (kbps >= 1e3)
		snprintf(buf, buf_len, "%s %5.1f MB/s", arrow, kbps / 1e3);
	else
		snprintf(buf, buf_len, "%s %5.1f KB/s", arrow, kbps);
}

// ---------------------------------------------------------------------------
// net_widget_init
// ---------------------------------------------------------------------------
void
net_widget_init(Config *cfg)
{
	memset(&g_net, 0, sizeof(g_net));

	// Resolve interface name
	if (cfg && cfg->net_iface && cfg->net_iface[0]) {
		strncpy(g_net.iface, cfg->net_iface, IFACE_NAME_LEN - 1);
	} else {
		find_default_iface(g_net.iface, IFACE_NAME_LEN);
		// Store the resolved name back into the config so layout code and
		// debug prints can reference it.
		if (cfg) {
			free(cfg->net_iface);
			cfg->net_iface = strdup(g_net.iface);
		}
	}

	if (verbose >= 1)
		printf("[NET] Using interface: %s\n", g_net.iface);

	// Seed the previous-tick counters so the first speed calculation
	// produces 0 B/s rather than a huge spike.
	read_proc_net_dev(g_net.iface, &g_net.prev_rx_bytes, &g_net.prev_tx_bytes);
	clock_gettime(CLOCK_MONOTONIC, &g_net.prev_ts);

	snprintf(g_net.rx_label, sizeof(g_net.rx_label), "↓   0.0 KB/s");
	snprintf(g_net.tx_label, sizeof(g_net.tx_label), "↑   0.0 KB/s");

	g_net.initialized = 1;
}

// ---------------------------------------------------------------------------
// net_compute_tile_size
// ---------------------------------------------------------------------------
int
net_compute_tile_size(const Config *cfg)
{
	double font_sz =
		((cfg && cfg->net_font_size > 0) ? (double)cfg->net_font_size :
										   (double)WIDGET_NET_FONT_SIZE) *
		buffer_scale;

	// Use representative worst-case strings
	const char *sample_rx = "↓ 999.9 MB/s";
	const char *sample_tx = "↑ 999.9 MB/s";

	cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(cs);
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, font_sz);

	cairo_text_extents_t re, te;
	cairo_text_extents(cr, sample_rx, &re);
	cairo_text_extents(cr, sample_tx, &te);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	double max_w = re.width > te.width ? re.width : te.width;
	int w = (int)(max_w * 1.3) + 4;

	int icon_size = (cfg && cfg->icon_size > 0) ? cfg->icon_size : 64;
	if (w < icon_size)
		w = icon_size;

	if (verbose >= 4)
		printf("[NET] computed tile width: %d px  "
			   "(rx_w=%.1f tx_w=%.1f)\n",
			w, re.width, te.width);

	return w;
}

// ---------------------------------------------------------------------------
// net_draw_tile
// ---------------------------------------------------------------------------
void
net_draw_tile(uint32_t *data, int width, int height, const Config *cfg,
	int corner_flags)
{
	if (!data || width <= 0 || height <= 0)
		return;

	unsigned int rx_col =
		(cfg && cfg->net_rx_color) ? cfg->net_rx_color : WIDGET_NET_RX_COLOR;
	unsigned int tx_col =
		(cfg && cfg->net_tx_color) ? cfg->net_tx_color : WIDGET_NET_TX_COLOR;
	double font_sz =
		((cfg && cfg->net_font_size > 0) ? (double)cfg->net_font_size :
										   (double)WIDGET_NET_FONT_SIZE) *
		buffer_scale;

	const char *rx_str = g_net.rx_label[0] ? g_net.rx_label : "↓";
	const char *tx_str = g_net.tx_label[0] ? g_net.tx_label : "↑";

	if (verbose >= 4)
		printf("[NET] tile %dx%d  rx='%s'  tx='%s'\n", width, height, rx_str,
			tx_str);

	// Set up Cairo surface over the caller's pixel buffer
	cairo_surface_t *cs =
		cairo_image_surface_create_for_data((unsigned char *)data,
			CAIRO_FORMAT_ARGB32, width, height, width * 4);
	cairo_t *cr = cairo_create(cs);

	// Clear to fully transparent
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	// Optional tile background (corner rounding driven by corner_flags)
	if (cfg && cfg->net_bg_color)
		draw_tile_background(cr, width, height, cfg->net_bg_color,
			corner_flags);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	// Auto-scale font size if text is wider than the tile (vertical bar).
	font_sz = fit_font_size(cr, rx_str, width, font_sz);
	font_sz = fit_font_size(cr, tx_str, width, font_sz);

	// Upper band → RX, lower band → TX
	int mid = height / 2;
	double rx_baseline = mid * 0.82;
	double tx_baseline = mid + (height - mid) * 0.82;

	draw_centered_text(cr, width, rx_str, rx_baseline, font_sz, rx_col);
	draw_centered_text(cr, width, tx_str, tx_baseline, font_sz, tx_col);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

// ---------------------------------------------------------------------------
// net_widget_needs_repaint
// ---------------------------------------------------------------------------
int
net_widget_needs_repaint(int *last_second)
{
	if (!last_second)
		return 0;

	if (!g_net.initialized)
		return 0;

	// Get current time
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);

	int cur_second = (int)ts.tv_sec;

	// Only sample once per second
	if (cur_second == *last_second)
		return 0;

	// Compute elapsed time since last sample
	double dt = (double)(ts.tv_sec - g_net.prev_ts.tv_sec) +
		(double)(ts.tv_nsec - g_net.prev_ts.tv_nsec) * 1e-9;

	long long rx_bytes = 0, tx_bytes = 0;
	if (read_proc_net_dev(g_net.iface, &rx_bytes, &tx_bytes) == 0 && dt > 0.0) {
		long long d_rx = rx_bytes - g_net.prev_rx_bytes;
		long long d_tx = tx_bytes - g_net.prev_tx_bytes;

		// Guard against counter wraps or interface resets
		if (d_rx < 0)
			d_rx = 0;
		if (d_tx < 0)
			d_tx = 0;

		g_net.rx_bps = (double)d_rx / dt;
		g_net.tx_bps = (double)d_tx / dt;

		g_net.prev_rx_bytes = rx_bytes;
		g_net.prev_tx_bytes = tx_bytes;
		g_net.prev_ts = ts;

		format_speed(g_net.rx_label, sizeof(g_net.rx_label), g_net.rx_bps, "↓");
		format_speed(g_net.tx_label, sizeof(g_net.tx_label), g_net.tx_bps, "↑");

		if (verbose >= 4)
			printf("[NET] %s  rx=%.0f B/s  tx=%.0f B/s\n", g_net.iface,
				g_net.rx_bps, g_net.tx_bps);
	}

	*last_second = cur_second;
	return 1;
}
