#define _GNU_SOURCE

#include "widget-sysinfo.h"
#include "config.h"
#include "widget-common.h"

#include <cairo.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int verbose;
extern int buffer_scale;

#define MAX_CORES 512

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
typedef struct {
	// CPU idle accounting — aggregate line (system-wide mode)
	long long prev_idle;
	long long prev_total;
	// Per-core idle accounting (percpu mode) — avoids aggregate×ncpu rounding
	long long prev_core_idle[MAX_CORES];
	long long prev_core_total[MAX_CORES];
	// Last computed usage percentages
	int cpu_pct;
	int ram_pct;
	// Cached label strings (cpu_label needs room for percpu values e.g. "CPU
	// 1200%")
	char cpu_label[20];
	char ram_label[16];
	// Name of the most CPU-intensive and most RAM-intensive process
	char cpu_proc[32];
	char ram_proc[32];
	int initialized;
	// Number of logical CPU cores (counted from /proc/stat cpu0, cpu1, ...)
	int ncpu;
	// Per-CPU mode: if 1, cpu_pct is the busiest single core % (max across
	// cores)
	int percpu;
} SysinfoState;

static SysinfoState g_si = {0};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/*
 * read_cpu_times — parse /proc/stat.
 *
 * If core < 0, reads the aggregate "cpu " line (system-wide).
 * If core >= 0, reads the "cpuN" line for that specific core.
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
 * read_all_cpu_times — read per-core idle/total from /proc/stat.
 * Fills arrays of length *ncpu_out (allocated by caller, max MAX_CORES).
 * Returns number of cores found.
 */
static int
read_all_cpu_times(long long *idles, long long *totals, int max_cores)
{
	FILE *fp = fopen("/proc/stat", "r");
	if (!fp)
		return 0;

	char line[256];
	int n = 0;
	while (fgets(line, sizeof(line), fp) && n < max_cores) {
		// Match "cpuN " lines (per-core), skip the aggregate "cpu " line
		if (strncmp(line, "cpu", 3) != 0 || line[3] == ' ')
			continue;
		long long f[10] = {0};
		char *p = line + 3;
		while (*p && *p != ' ')
			p++; // skip the core number
		sscanf(p, "%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld", &f[0],
			&f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9]);
		idles[n] = f[3] + f[4];
		totals[n] = 0;
		for (int i = 0; i < 10; i++)
			totals[n] += f[i];
		n++;
	}
	fclose(fp);
	return n;
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

/*
 * read_top_cpu_proc — find the process name with the highest utime+stime
 * delta since last call by scanning /proc/[pid]/stat.
 * Writes into out_name (max out_len bytes, always NUL-terminated).
 *
 * We keep a small static table of previous utime+stime per PID so we can
 * compute deltas.  PIDs that vanish are simply ignored on the next call.
 */
#define PROC_TRACK_MAX 1024
static struct {
	int pid;
	long long prev_ticks;
} proc_cpu_track[PROC_TRACK_MAX];
static int proc_cpu_track_n = 0;

static void
read_top_cpu_proc(char *out_name, int out_len)
{
	DIR *d = opendir("/proc");
	if (!d) {
		snprintf(out_name, out_len, "?");
		return;
	}

	long best_delta = 0;
	char best_name[32] = {0};

	struct dirent *ent;
	while ((ent = readdir(d))) {
		// Only numeric entries are PIDs
		if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
			continue;
		int pid = atoi(ent->d_name);
		if (pid <= 0)
			continue;

		char path[64];
		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		FILE *fp = fopen(path, "r");
		if (!fp)
			continue;

		// /proc/pid/stat fields: pid (comm) state ppid ... utime stime ...
		// comm is field 2 (in parens), utime=14, stime=15 (1-indexed)
		char comm[32] = {0};
		long utime = 0, stime = 0;
		// Read the whole line; comm may contain spaces but is wrapped in ()
		char buf[512];
		if (!fgets(buf, sizeof(buf), fp)) {
			fclose(fp);
			continue;
		}
		fclose(fp);

		// Find the comm between first '(' and last ')'
		char *lp = strchr(buf, '(');
		char *rp = strrchr(buf, ')');
		if (!lp || !rp || rp <= lp)
			continue;
		int clen = (int)(rp - lp - 1);
		if (clen >= (int)sizeof(comm))
			clen = (int)sizeof(comm) - 1;
		memcpy(comm, lp + 1, clen);
		comm[clen] = '\0';

		// Parse remaining fields after ')'
		// state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt
		// cmajflt utime stime — that's 13 more fields after state
		char state;
		int ppid, pgrp, session, tty, tpgid;
		unsigned flags;
		long minflt, cminflt, majflt, cmajflt;
		sscanf(rp + 1, " %c %d %d %d %d %d %u %ld %ld %ld %ld %ld %ld", &state,
			&ppid, &pgrp, &session, &tty, &tpgid, &flags, &minflt, &cminflt,
			&majflt, &cmajflt, &utime, &stime);

		long long ticks = (long long)utime + stime;

		// Look up previous ticks for this PID
		long long prev_ticks = 0;
		int found = 0;
		for (int i = 0; i < proc_cpu_track_n; i++) {
			if (proc_cpu_track[i].pid == pid) {
				prev_ticks = proc_cpu_track[i].prev_ticks;
				proc_cpu_track[i].prev_ticks = ticks;
				found = 1;
				break;
			}
		}
		if (!found && proc_cpu_track_n < PROC_TRACK_MAX) {
			proc_cpu_track[proc_cpu_track_n].pid = pid;
			proc_cpu_track[proc_cpu_track_n].prev_ticks = ticks;
			proc_cpu_track_n++;
		}

		long delta = (long)(ticks - prev_ticks);
		if (delta > best_delta) {
			best_delta = delta;
			snprintf(best_name, sizeof(best_name), "%s", comm);
		}
	}
	closedir(d);

	if (best_name[0])
		snprintf(out_name, out_len, "%s", best_name);
	else
		snprintf(out_name, out_len, "-");
}

/*
 * read_top_ram_proc — find the process name with the highest RSS by reading
 * /proc/[pid]/statm (field 2 is RSS in pages).
 */
static void
read_top_ram_proc(char *out_name, int out_len)
{
	DIR *d = opendir("/proc");
	if (!d) {
		snprintf(out_name, out_len, "?");
		return;
	}

	long best_rss = 0;
	char best_name[32] = {0};

	struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] < '1' || ent->d_name[0] > '9')
			continue;
		int pid = atoi(ent->d_name);
		if (pid <= 0)
			continue;

		// Read comm
		char comm_path[64], statm_path[64];
		snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
		snprintf(statm_path, sizeof(statm_path), "/proc/%d/statm", pid);

		FILE *fp = fopen(statm_path, "r");
		if (!fp)
			continue;
		long vmsize = 0, rss = 0;
		if (fscanf(fp, "%ld %ld", &vmsize, &rss) != 2) {
			fclose(fp);
			continue;
		}
		fclose(fp);

		if (rss <= best_rss)
			continue;

		fp = fopen(comm_path, "r");
		if (!fp)
			continue;
		char comm[32] = {0};
		if (fgets(comm, sizeof(comm), fp)) {
			// Strip trailing newline
			comm[strcspn(comm, "\n")] = '\0';
		}
		fclose(fp);

		best_rss = rss;
		snprintf(best_name, sizeof(best_name), "%s", comm);
	}
	closedir(d);

	if (best_name[0])
		snprintf(out_name, out_len, "%s", best_name);
	else
		snprintf(out_name, out_len, "-");
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

	// Count logical CPU cores and seed per-core prev values
	int n = read_all_cpu_times(g_si.prev_core_idle, g_si.prev_core_total,
		MAX_CORES);
	g_si.ncpu = n > 0 ? n : 1;

	snprintf(g_si.cpu_label, sizeof(g_si.cpu_label), "CPU   0%%");
	snprintf(g_si.ram_label, sizeof(g_si.ram_label), "RAM   0%%");
	g_si.initialized = 1;

	if (verbose >= 1)
		printf("[SYSINFO] widget initialized (%d core%s)\n", g_si.ncpu,
			g_si.ncpu > 1 ? "s" : "");
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
	const char *cpu_proc = g_si.cpu_proc[0] ? g_si.cpu_proc : "";
	const char *ram_proc = g_si.ram_proc[0] ? g_si.ram_proc : "";

	if (verbose >= 4)
		printf("[SYSINFO] tile %dx%d  cpu='%s' (%s)  ram='%s' (%s)\n", width,
			height, cpu_str, cpu_proc, ram_str, ram_proc);

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

	// Process name is rendered at 65% of the main font size, dimmed
	double proc_font_sz = font_sz * 0.65;
	if (cpu_proc[0])
		proc_font_sz = fit_font_size(cr, cpu_proc, width, proc_font_sz);
	if (ram_proc[0])
		proc_font_sz = fit_font_size(cr, ram_proc, width, proc_font_sz);

	int mid = height / 2;

	// Each half is split: percentage at ~60%, process name at ~88%
	double cpu_baseline = mid * 0.60;
	double cpu_proc_baseline = mid * 0.88;
	double ram_baseline = mid + (height - mid) * 0.60;
	double ram_proc_baseline = mid + (height - mid) * 0.88;

	draw_centered_text(cr, width, cpu_str, cpu_baseline, font_sz, cpu_col);
	draw_centered_text(cr, width, ram_str, ram_baseline, font_sz, ram_col);

	// Process names: same hue as their line but at 60% alpha
	unsigned int cpu_proc_col = (cpu_col & 0x00FFFFFF) | 0x99000000;
	unsigned int ram_proc_col = (ram_col & 0x00FFFFFF) | 0x99000000;

	cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	draw_centered_text(cr, width, cpu_proc, cpu_proc_baseline, proc_font_sz,
		cpu_proc_col);
	draw_centered_text(cr, width, ram_proc, ram_proc_baseline, proc_font_sz,
		ram_proc_col);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);
}

// ---------------------------------------------------------------------------
// sysinfo_widget_needs_repaint
// ---------------------------------------------------------------------------
int
sysinfo_widget_needs_repaint(int *last_second, int percpu)
{
	if (!last_second || !g_si.initialized)
		return 0;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	int cur_second = (int)ts.tv_sec;

	if (cur_second == *last_second)
		return 0;

	// --- CPU ---
	if (percpu) {
		// Per-core mode: find the busiest single core.
		// Reading individual cpuN lines avoids the aggregate×ncpu rounding
		// error that caused values like 108% on a 12-core machine.
		long long cur_idles[MAX_CORES], cur_totals[MAX_CORES];
		int n = read_all_cpu_times(cur_idles, cur_totals, MAX_CORES);
		int max_pct = 0;
		for (int i = 0; i < n && i < g_si.ncpu; i++) {
			long long d_idle = cur_idles[i] - g_si.prev_core_idle[i];
			long long d_total = cur_totals[i] - g_si.prev_core_total[i];
			if (d_total > 0) {
				if (d_idle < 0)
					d_idle = 0;
				int pct = (int)(100LL * (d_total - d_idle) / d_total);
				if (pct > max_pct)
					max_pct = pct;
			}
			g_si.prev_core_idle[i] = cur_idles[i];
			g_si.prev_core_total[i] = cur_totals[i];
		}
		if (max_pct > 100)
			max_pct = 100;
		g_si.cpu_pct = max_pct;
	} else {
		// System-wide mode: aggregate line gives total across all cores
		long long idle = 0, total = 0;
		if (read_cpu_times(&idle, &total) == 0) {
			long long d_idle = idle - g_si.prev_idle;
			long long d_total = total - g_si.prev_total;
			if (d_total > 0) {
				if (d_idle < 0)
					d_idle = 0;
				int pct = (int)(100LL * (d_total - d_idle) / d_total);
				if (pct < 0)
					pct = 0;
				if (pct > 100)
					pct = 100;
				g_si.cpu_pct = pct;
			}
			g_si.prev_idle = idle;
			g_si.prev_total = total;
		}
	}

	// --- RAM ---
	int rp = read_ram_pct();
	if (rp >= 0)
		g_si.ram_pct = rp;

	// --- Top processes ---
	read_top_cpu_proc(g_si.cpu_proc, sizeof(g_si.cpu_proc));
	read_top_ram_proc(g_si.ram_proc, sizeof(g_si.ram_proc));

	snprintf(g_si.cpu_label, sizeof(g_si.cpu_label), "CPU %3d%%", g_si.cpu_pct);
	snprintf(g_si.ram_label, sizeof(g_si.ram_label), "RAM %3d%%", g_si.ram_pct);

	if (verbose >= 4)
		printf("[SYSINFO] cpu=%d%% ram=%d%% (percpu=%d ncpu=%d)\n",
			g_si.cpu_pct, g_si.ram_pct, percpu, g_si.ncpu);

	*last_second = cur_second;
	return 1;
}
