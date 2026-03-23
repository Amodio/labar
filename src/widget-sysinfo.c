#define _GNU_SOURCE

#include "widget-sysinfo.h"
#include "config.h"
#include "widget-common.h"

#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int verbose;
extern int buffer_scale;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
typedef struct {
	// CPU idle accounting (from /proc/stat, "cpu" aggregate line)
	long long prev_idle;
	long long prev_total;
	// Last computed usage percentages (0–100)
	int cpu_pct;
	int ram_pct;
	// Cached label strings
	char cpu_label[16];
	char ram_label[16];
	int initialized;
} SysinfoState;

static SysinfoState g_si = {0};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/*
 * read_cpu_times — parse the first "cpu" line of /proc/stat.
 * Returns 0 on success, fills *idle_out and *total_out.
 */
static int
read_cpu_times(long long *idle_out, long long *total_out)
{
	FILE *fp = fopen("/proc/stat", "r");
	if (!fp)
		return -1;

	char line[256];
	int found = 0;
	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "cpu ", 4) != 0)
			continue;
		// Fields: user nice system idle iowait irq softirq steal guest
		// guest_nice
		long long f[10] = {0};
		sscanf(line + 4, "%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld",
			&f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8],
			&f[9]);
		*idle_out = f[3] + f[4]; // idle + iowait
		*total_out = 0;
		for (int i = 0; i < 10; i++)
			*total_out += f[i];
		found = 1;
		break;
	}
	fclose(fp);
	return found ? 0 : -1;
}

/*
 * read_ram_pct — parse /proc/meminfo for MemTotal and MemAvailable.
 * Returns used% as integer 0–100, or -1 on error.
 */
static int
read_ram_pct(void)
{
	FILE *fp = fopen("/proc/meminfo", "r");
	if (!fp)
		return -1;

	long long total = 0, avail = 0;
	char line[128];
	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, "MemTotal:", 9) == 0)
			sscanf(line + 9, "%lld", &total);
		else if (strncmp(line, "MemAvailable:", 13) == 0)
			sscanf(line + 13, "%lld", &avail);
		if (total && avail)
			break;
	}
	fclose(fp);

	if (total <= 0)
		return -1;
	long long used = total - avail;
	if (used < 0)
		used = 0;
	return (int)(100LL * used / total);
}

// ---------------------------------------------------------------------------
// sysinfo_widget_init
// ---------------------------------------------------------------------------
void
sysinfo_widget_init(void)
{
	memset(&g_si, 0, sizeof(g_si));
	read_cpu_times(&g_si.prev_idle, &g_si.prev_total);
	g_si.cpu_pct = 0;
	g_si.ram_pct = 0;
	snprintf(g_si.cpu_label, sizeof(g_si.cpu_label), "CPU   0%%");
	snprintf(g_si.ram_label, sizeof(g_si.ram_label), "RAM   0%%");
	g_si.initialized = 1;

	if (verbose >= 1)
		printf("[SYSINFO] widget initialized\n");
}

// ---------------------------------------------------------------------------
// sysinfo_compute_tile_size
// ---------------------------------------------------------------------------
int
sysinfo_compute_tile_size(const Config *cfg)
{
	double font_sz = ((cfg && cfg->sysinfo_font_size > 0) ?
							 (double)cfg->sysinfo_font_size :
							 (double)WIDGET_SYSINFO_FONT_SIZE) *
		buffer_scale;

	// Representative worst-case strings
	const char *sample_cpu = "CPU 100%";
	const char *sample_ram = "RAM 100%";

	cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(cs);
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, font_sz);

	cairo_text_extents_t ce, re;
	cairo_text_extents(cr, sample_cpu, &ce);
	cairo_text_extents(cr, sample_ram, &re);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	double max_w = ce.width > re.width ? ce.width : re.width;
	int w = (int)(max_w * 1.3) + 4;

	int icon_size = (cfg && cfg->icon_size > 0) ? cfg->icon_size : 64;
	if (w < icon_size)
		w = icon_size;

	if (verbose >= 4)
		printf("[SYSINFO] computed tile width: %d px\n", w);

	return w;
}

// ---------------------------------------------------------------------------
// sysinfo_draw_tile
// ---------------------------------------------------------------------------
void
sysinfo_draw_tile(uint32_t *data, int width, int height, const Config *cfg,
	int corner_flags)
{
	if (!data || width <= 0 || height <= 0)
		return;

	unsigned int cpu_col = (cfg && cfg->sysinfo_cpu_color) ?
		cfg->sysinfo_cpu_color :
		WIDGET_SYSINFO_CPU_COLOR;
	unsigned int ram_col = (cfg && cfg->sysinfo_ram_color) ?
		cfg->sysinfo_ram_color :
		WIDGET_SYSINFO_RAM_COLOR;
	double font_sz = ((cfg && cfg->sysinfo_font_size > 0) ?
							 (double)cfg->sysinfo_font_size :
							 (double)WIDGET_SYSINFO_FONT_SIZE) *
		buffer_scale;

	const char *cpu_str = g_si.cpu_label[0] ? g_si.cpu_label : "CPU  --%";
	const char *ram_str = g_si.ram_label[0] ? g_si.ram_label : "RAM  --%";

	if (verbose >= 4)
		printf("[SYSINFO] tile %dx%d  cpu='%s'  ram='%s'\n", width, height,
			cpu_str, ram_str);

	cairo_surface_t *cs =
		cairo_image_surface_create_for_data((unsigned char *)data,
			CAIRO_FORMAT_ARGB32, width, height, width * 4);
	cairo_t *cr = cairo_create(cs);

	// Clear to fully transparent
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	// Optional tile background (corner rounding driven by corner_flags)
	if (cfg && cfg->sysinfo_bg_color)
		draw_tile_background(cr, width, height, cfg->sysinfo_bg_color,
			corner_flags);

	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_BOLD);

	// Auto-scale font size if text is wider than the tile (vertical bar).
	font_sz = fit_font_size(cr, cpu_str, width, font_sz);
	font_sz = fit_font_size(cr, ram_str, width, font_sz);

	int mid = height / 2;
	double cpu_baseline = mid * 0.82;
	double ram_baseline = mid + (height - mid) * 0.82;

	draw_centered_text(cr, width, cpu_str, cpu_baseline, font_sz, cpu_col);
	draw_centered_text(cr, width, ram_str, ram_baseline, font_sz, ram_col);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

// ---------------------------------------------------------------------------
// sysinfo_widget_needs_repaint
// ---------------------------------------------------------------------------
int
sysinfo_widget_needs_repaint(int *last_second)
{
	if (!last_second || !g_si.initialized)
		return 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int cur_second = (int)ts.tv_sec;

	if (cur_second == *last_second)
		return 0;

	// --- CPU ---
	long long idle = 0, total = 0;
	if (read_cpu_times(&idle, &total) == 0) {
		long long d_idle = idle - g_si.prev_idle;
		long long d_total = total - g_si.prev_total;
		if (d_total > 0) {
			if (d_idle < 0)
				d_idle = 0;
			g_si.cpu_pct = (int)(100LL * (d_total - d_idle) / d_total);
			if (g_si.cpu_pct < 0)
				g_si.cpu_pct = 0;
			if (g_si.cpu_pct > 100)
				g_si.cpu_pct = 100;
		}
		g_si.prev_idle = idle;
		g_si.prev_total = total;
	}

	// --- RAM ---
	int rp = read_ram_pct();
	if (rp >= 0)
		g_si.ram_pct = rp;

	snprintf(g_si.cpu_label, sizeof(g_si.cpu_label), "CPU %3d%%", g_si.cpu_pct);
	snprintf(g_si.ram_label, sizeof(g_si.ram_label), "RAM %3d%%", g_si.ram_pct);

	if (verbose >= 4)
		printf("[SYSINFO] cpu=%d%% ram=%d%%\n", g_si.cpu_pct, g_si.ram_pct);

	*last_second = cur_second;
	return 1;
}
