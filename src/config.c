#define _GNU_SOURCE

#include "config.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// XDG data directory helpers
// ---------------------------------------------------------------------------

// Return a NULL-terminated array of XDG data dirs.
// Source: $XDG_DATA_DIRS (colon-separated), or XDG_DATA_DIRS_DEFAULT.
// Caller frees with free_xdg_data_dirs().
char **
xdg_data_dirs(void)
{
	const char *env = getenv("XDG_DATA_DIRS");
	const char *src = (env && env[0]) ? env : XDG_DATA_DIRS_DEFAULT;

	// Count entries
	int count = 1;
	for (const char *p = src; *p; p++)
		if (*p == ':')
			count++;

	char **dirs = malloc((count + 1) * sizeof(char *));
	if (!dirs)
		return NULL;

	int n = 0;
	const char *start = src;
	for (;;) {
		const char *end = strchr(start, ':');
		size_t len = end ? (size_t)(end - start) : strlen(start);
		if (len > 0) {
			char *dir = malloc(len + 1);
			if (dir) {
				memcpy(dir, start, len);
				dir[len] = '\0';
				dirs[n++] = dir;
			}
		}
		if (!end)
			break;
		start = end + 1;
	}

	dirs[n] = NULL;
	return dirs;
}

void
free_xdg_data_dirs(char **dirs)
{
	if (!dirs)
		return;
	for (int i = 0; dirs[i]; i++)
		free(dirs[i]);
	free(dirs);
}

// ---------------------------------------------------------------------------
// .desktop file parser
//
// Parses key-value pairs from a .desktop file section.
// .desktop files are INI-style with [Desktop Entry] as the main section.
// ---------------------------------------------------------------------------

// Trim whitespace from both ends of a string (in-place)
static char *
trim_string(char *str)
{
	if (!str)
		return str;

	// Trim leading whitespace
	while (*str && isspace((unsigned char)*str))
		str++;

	// Trim trailing whitespace
	char *end = str + strlen(str) - 1;
	while (end >= str && isspace((unsigned char)*end))
		*end-- = '\0';

	return str;
}

// Returns a DesktopEntry on success, NULL on failure
static DesktopEntry *
parse_desktop_file(const char *filepath)
{
	if (verbose >= 3)
		printf("[I/O] FOPEN (read): %s\n", filepath);

	FILE *fp = fopen(filepath, "r");
	if (!fp) {
		if (verbose >= 3)
			printf("[I/O] FOPEN FAILED: %s\n", filepath);
		return NULL;
	}

	if (verbose >= 2)
		printf("[DBG²] Parsing .desktop file: %s\n", filepath);

	DesktopEntry *entry = calloc(1, sizeof(DesktopEntry));
	if (!entry) {
		fclose(fp);
		return NULL;
	}

	char line[512];
	int no_display = 0;
	int in_desktop_section = 0;

	while (fgets(line, sizeof(line), fp)) {
		// Remove trailing newline
		line[strcspn(line, "\n")] = '\0';

		// Skip empty lines and comments
		if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
			continue;

		// Check for [Desktop Entry] section
		if (strcmp(line, "[Desktop Entry]") == 0) {
			in_desktop_section = 1;
			continue;
		}

		// Stop parsing if we hit a different section
		if (line[0] == '[') {
			in_desktop_section = 0;
			continue;
		}

		if (!in_desktop_section)
			continue;

		// Parse key=value pairs
		char *eq = strchr(line, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char *key = trim_string(line);
		char *value = trim_string(eq + 1);

		if (strcasecmp(key, "Name") == 0) {
			free(entry->name);
			entry->name = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   Name: %s\n", value);
		} else if (strcasecmp(key, "Exec") == 0) {
			free(entry->exec);
			entry->exec = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   Exec: %s\n", value);
		} else if (strcasecmp(key, "Icon") == 0) {
			free(entry->icon);
			entry->icon = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   Icon: %s\n", value);
		} else if (strcasecmp(key, "NoDisplay") == 0) {
			no_display = (strcasecmp(value, "true") == 0);
			if (verbose >= 2)
				printf("[DBG²]   NoDisplay: %s\n", value);
		} else if (strcasecmp(key, "Terminal") == 0) {
			entry->terminal = (strcasecmp(value, "true") == 0);
			if (verbose >= 2) {
				printf("[DBG²]   terminal: %s\n",
					entry->terminal ? "true" : "false");
			}
		}
	}

	fclose(fp);

	if (verbose >= 3)
		printf("[I/O] FCLOSE: %s\n", filepath);

	// Valid entries need at least a Name and Exec
	if (!entry->name || !entry->exec || no_display) {
		if (entry->name)
			free(entry->name);
		if (entry->exec)
			free(entry->exec);
		if (entry->icon)
			free(entry->icon);
		free(entry);
		return NULL;
	}

	return entry;
}

static void
free_desktop_entry(DesktopEntry *entry)
{
	if (!entry)
		return;
	free(entry->name);
	free(entry->exec);
	free(entry->icon);
	free(entry);
}

// Find an application by name in the entries array
static DesktopEntry *
find_app_by_name(DesktopEntry **entries, int count, const char *name)
{
	for (int i = 0; i < count; i++) {
		if (strcasecmp(entries[i]->name, name) == 0)
			return entries[i];
	}
	return NULL;
}

// Icon surface cache entry
typedef struct {
	char *icon_path;
	int size;		  // Size in pixels
	uint32_t *pixels; // Decoded pixel data
} IconSurfaceCache;

// Icon candidate for sorting and selection
typedef struct {
	char *path;
	int size; // SVG: 999 (scalable), PNG: pixel dimension
} IconCandidate;

// Comparison function for sorting icons (best to worst)
// SVG (size=999) ranks highest, then PNG sorted by size descending
static int
compare_icons(const void *a, const void *b)
{
	IconCandidate *ic_a = (IconCandidate *)a;
	IconCandidate *ic_b = (IconCandidate *)b;

	// Larger size is better (SVG at 999 comes first)
	return ic_b->size - ic_a->size;
}

// Try to add a candidate icon at `path` to the candidates array.
// `size_str` is the directory component that may contain a numeric size
// (e.g. "48x48", "32", "scalable"); `is_svg` selects the priority value.
// Returns 0 on success, -1 on realloc failure (caller should goto cleanup).
static int
try_add_candidate(IconCandidate **candidates, int *count, int *capacity,
	const char *path, const char *size_str, int is_svg)
{
	if (access(path, F_OK) != 0)
		return 0;

	if (*count >= *capacity) {
		int newcap = *capacity * 2;
		IconCandidate *tmp =
			realloc(*candidates, newcap * sizeof(IconCandidate));
		if (!tmp)
			return -1;
		*candidates = tmp;
		*capacity = newcap;
	}

	int size = is_svg ? 999 : 0;
	if (!is_svg)
		sscanf(size_str, "%d", &size);

	(*candidates)[*count].path = strdup(path);
	if (!(*candidates)[*count].path)
		return -1;
	(*candidates)[(*count)++].size = size;
	return 0;
}

// Probe all known icon layout patterns for `icon_name` under `theme_path`,
// adding any found paths to `candidates`.
// Handles two common layouts:
//   hicolor: <theme>/<size>/apps/<name>.ext  (size dir at first level)
//   breeze:  <theme>/apps/<size>/<name>.ext  (category dir at first level)
// Returns -1 on realloc failure, 0 otherwise.
static int
probe_theme(const char *theme_path, const char *icon_name,
	IconCandidate **candidates, int *count, int *capacity)
{
	DIR *l1_dir = opendir(theme_path);
	if (!l1_dir)
		return 0;

	const char *exts[] = {"svg", "png", NULL};

	struct dirent *l1;
	while ((l1 = readdir(l1_dir))) {
		if (l1->d_name[0] == '.')
			continue;

		char *l1_path = NULL;
		if (asprintf(&l1_path, "%s/%s", theme_path, l1->d_name) < 0)
			continue;

		DIR *l2_dir = opendir(l1_path);
		if (!l2_dir) {
			free(l1_path);
			continue;
		}

		struct dirent *l2;
		while ((l2 = readdir(l2_dir))) {
			if (l2->d_name[0] == '.')
				continue;

			// Pattern A: <theme>/<l1>/<l2>/<name>.ext
			// Covers hicolor (<l1>=size, <l2>=apps) and
			//        breeze  (<l1>=apps, <l2>=size).
			// Extract numeric size from whichever of l1/l2 has one.
			const char *size_str = l1->d_name;
			{
				int n = 0;
				sscanf(l2->d_name, "%d", &n);
				if (n > 0)
					size_str = l2->d_name;
			}

			for (int e = 0; exts[e]; e++) {
				char path[PATH_MAX];
				snprintf(path, sizeof(path), "%s/%s/%s.%s", l1_path, l2->d_name,
					icon_name, exts[e]);
				int is_svg = (e == 0);
				if (try_add_candidate(candidates, count, capacity, path,
						size_str, is_svg) < 0) {
					closedir(l2_dir);
					free(l1_path);
					closedir(l1_dir);
					return -1;
				}
			}
		}
		closedir(l2_dir);
		free(l1_path);
	}
	closedir(l1_dir);
	return 0;
}

// Find all icon variants for an application
// Returns the best quality icon path, listing all candidates if verbose
// Caller must free the returned string
static char *find_best_icon(const char *icon_name);

char *
find_best_icon_for_name(const char *icon_name)
{
	return find_best_icon(icon_name);
}

static char *
find_best_icon(const char *icon_name)
{
	if (!icon_name || strlen(icon_name) == 0)
		return NULL;

	if (verbose >= 3)
		printf("[DBG³] Finding best icon for '%s'\n", icon_name);

	IconCandidate *candidates = NULL;
	int candidate_count = 0;
	int capacity = 16;
	char *best_path = NULL;

	candidates = malloc(capacity * sizeof(IconCandidate));
	if (!candidates)
		return NULL;

	char **data_dirs = xdg_data_dirs();
	if (!data_dirs) {
		free(candidates);
		return NULL;
	}

	for (int d = 0; data_dirs[d]; d++) {
		char *icon_base = NULL;
		if (asprintf(&icon_base, "%s/" ICONS_SUBDIR, data_dirs[d]) < 0)
			continue;

		DIR *base_dir = opendir(icon_base);
		if (!base_dir) {
			free(icon_base);
			continue;
		}

		struct dirent *theme_entry;
		while ((theme_entry = readdir(base_dir))) {
			if (theme_entry->d_name[0] == '.')
				continue;

			char *theme_path = NULL;
			if (asprintf(&theme_path, "%s/%s", icon_base, theme_entry->d_name) <
				0)
				continue;

			if (probe_theme(theme_path, icon_name, &candidates,
					&candidate_count, &capacity) < 0) {
				free(theme_path);
				closedir(base_dir);
				free(icon_base);
				goto cleanup;
			}

			free(theme_path);
		}

		if (verbose >= 4)
			printf("[I/O] CLOSEDIR: %s (icons dir)\n", icon_base);
		closedir(base_dir);
		free(icon_base);

		// Also check <datadir>/pixmaps as a fallback within this data dir
		char *pixmaps_base = NULL;
		if (asprintf(&pixmaps_base, "%s/" PIXMAPS_SUBDIR, data_dirs[d]) >= 0) {
			const char *exts[] = {"svg", "png", NULL};
			for (int e = 0; exts[e]; e++) {
				char *p = NULL;
				if (asprintf(&p, "%s/%s.%s", pixmaps_base, icon_name, exts[e]) <
					0)
					continue;
				if (access(p, F_OK) == 0) {
					if (verbose >= 2)
						printf("[DBG²]   Found pixmap: %s\n", p);
					if (candidate_count >= capacity) {
						capacity *= 2;
						IconCandidate *tmp = realloc(candidates,
							capacity * sizeof(IconCandidate));
						if (tmp)
							candidates = tmp;
						else {
							free(p);
							free(pixmaps_base);
							goto cleanup;
						}
					}
					// Pixmaps rank below sized PNGs but above 0
					candidates[candidate_count].path = p;
					candidates[candidate_count].size =
						(e == 0) ? 998 : 1; // svg > png
					candidate_count++;
				} else {
					free(p);
				}
			}
			free(pixmaps_base);
		}
	}

	free_xdg_data_dirs(data_dirs);

	if (candidate_count == 0) {
		if (verbose >= 2)
			printf("[DBG²] No icon candidates found for '%s'\n", icon_name);
		free(candidates);
		return NULL;
	}

	if (verbose >= 2)
		printf("[DBG²] Found %d candidate icon(s) for '%s'\n", candidate_count,
			icon_name);

	// Sort candidates from best to worst quality
	qsort(candidates, candidate_count, sizeof(IconCandidate), compare_icons);

	// List all candidates if verbose
	if (verbose >= 2) {
		printf("[DBG²] Available icons for '%s' (best to worst):\n", icon_name);
		for (int i = 0; i < candidate_count; i++) {
			const char *type = (candidates[i].size == 999) ? "SVG" : "PNG";
			printf("[DBG²]   #%d %s (%s, %dpx)\n", i, candidates[i].path, type,
				candidates[i].size);
		}
	}

	// Return the best one
	best_path = strdup(candidates[0].path);

	if (verbose >= 2)
		printf("[DBG²] Best icon selected: %s\n", best_path);

cleanup:
	for (int i = 0; i < candidate_count; i++)
		free(candidates[i].path);
	free(candidates);

	return best_path;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// List all applications from <datadir>/applications/*.desktop across all
// XDG data dirs.  Allocates an array of DesktopEntry pointers; caller must
// free all entries and the array itself using free_applications().
DesktopEntry **
list_all_applications(int *count_out)
{
	int capacity = 16;
	int count = 0;
	DesktopEntry **entries = malloc(capacity * sizeof(DesktopEntry *));
	if (!entries) {
		*count_out = 0;
		return NULL;
	}

	char **data_dirs = xdg_data_dirs();
	if (!data_dirs) {
		free(entries);
		*count_out = 0;
		return NULL;
	}

	for (int d = 0; data_dirs[d]; d++) {
		char *app_dir = NULL;
		if (asprintf(&app_dir, "%s/" APPS_SUBDIR, data_dirs[d]) < 0)
			continue;

		if (verbose >= 4)
			printf("[I/O] OPENDIR: %s\n", app_dir);
		DIR *dir = opendir(app_dir);
		if (!dir) {
			if (verbose >= 4)
				printf("[I/O] OPENDIR FAILED: %s\n", app_dir);
			free(app_dir);
			continue;
		}

		struct dirent *entry;
		while ((entry = readdir(dir))) {
			// Only process .desktop files
			if (strlen(entry->d_name) < 9)
				continue;
			if (strcmp(entry->d_name + strlen(entry->d_name) - 8, ".desktop") !=
				0)
				continue;

			// Build full path
			char filepath[512];
			snprintf(filepath, sizeof(filepath), "%s/%s", app_dir,
				entry->d_name);

			// Parse the .desktop file
			DesktopEntry *parsed = parse_desktop_file(filepath);
			if (!parsed)
				continue;

			// Skip duplicates: a .desktop file with the same name in an
			// earlier data dir takes precedence (XDG spec)
			int dup = 0;
			for (int i = 0; i < count; i++) {
				if (strcasecmp(entries[i]->name, parsed->name) == 0) {
					dup = 1;
					break;
				}
			}
			if (dup) {
				free_desktop_entry(parsed);
				continue;
			}

			// Grow array if needed
			if (count >= capacity) {
				capacity *= 2;
				DesktopEntry **tmp =
					realloc(entries, capacity * sizeof(DesktopEntry *));
				if (!tmp) {
					free_desktop_entry(parsed);
					closedir(dir);
					free(app_dir);
					goto cleanup;
				}
				entries = tmp;
			}

			entries[count++] = parsed;
		}

		if (verbose >= 4)
			printf("[I/O] CLOSEDIR: %s\n", app_dir);
		closedir(dir);
		free(app_dir);
	}

	free_xdg_data_dirs(data_dirs);
	*count_out = count;
	return entries;

cleanup:
	free_xdg_data_dirs(data_dirs);
	for (int i = 0; i < count; i++)
		free_desktop_entry(entries[i]);
	free(entries);
	*count_out = 0;
	return NULL;
}

void
free_applications(DesktopEntry **entries, int count)
{
	if (!entries)
		return;
	for (int i = 0; i < count; i++)
		free_desktop_entry(entries[i]);
	free(entries);
}

// Free a Config struct
void
free_config(Config *cfg)
{
	if (!cfg)
		return;
	free_applications(cfg->apps, cfg->count);
	cfg->apps = NULL;
	cfg->count = 0;
	free(cfg->date_date_format);
	cfg->date_date_format = NULL;
	free(cfg->date_time_format);
	cfg->date_time_format = NULL;
	free(cfg->volume_exec);
	cfg->volume_exec = NULL;
	free(cfg->net_iface);
	cfg->net_iface = NULL;
	free(cfg->output_name);
	cfg->output_name = NULL;
	free(cfg->sysinfo_exec);
	cfg->sysinfo_exec = NULL;
}

// Check if the config file exists
int
config_file_exists(void)
{
	const char *home = getenv("HOME");
	if (!home)
		return 0;

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/" CONFIG_DIR "/" CONFIG_NAME,
		home);

	if (verbose >= 4)
		printf("[I/O] ACCESS CHECK (stat): %s\n", config_path);
	return access(config_path, F_OK) == 0;
}

// Parse the labar.cfg config file and load applications
// Returns a Config struct with loaded applications
static Config
parse_config_file(FILE *fp)
{
	Config cfg = {0};
	cfg.apps = malloc(10 * sizeof(DesktopEntry *));
	cfg.icon_size = 64;				   // Default icon size
	cfg.icon_spacing = 0;			   // No spacing between icons by default
	cfg.exclusive_zone = 0;			   // No exclusive zone by default
	cfg.border_space = 0;			   // No border space by default
	cfg.label_mode = LABEL_MODE_HOVER; // Show label on hover by default
	cfg.label_color = 0xFFFFFFFF;	   // Opaque white by default
	cfg.label_size = 10;			   // 10 pt font by default
	cfg.label_offset = 10;			   // Baseline 10 px above the bottom edge
	cfg.position = POSITION_BOTTOM;	   // Bar at the bottom by default
	cfg.layer = LAYER_TOP;			   // Layer-shell top layer by default
	cfg.output_name = NULL;			   // Use compositor default output
	cfg.show_date = 0;				   // Date widget off by default
	cfg.show_sysinfo = 1;			   // Sysinfo widget on by default
	cfg.sysinfo_percpu = 1;			   // top-style (unnormalized) by default
	cfg.sysinfo_show_temp = 1;		   // show temperature line by default
	cfg.sysinfo_show_proc = 1;		   // show process name sub-lines by default
	cfg.sysinfo_cpu_color = 0;		   // Falls back to WIDGET_SYSINFO_CPU_COLOR
	cfg.sysinfo_tmp_color = 0;		   // Falls back to WIDGET_SYSINFO_TMP_COLOR
	cfg.sysinfo_ram_color = 0;		   // Falls back to WIDGET_SYSINFO_RAM_COLOR
	cfg.sysinfo_font_size = 0;		   // Falls back to WIDGET_SYSINFO_FONT_SIZE
	cfg.sysinfo_bg_color = 0;		   // Transparent by default
	cfg.sysinfo_tile_width = 0;
	cfg.sysinfo_exec = strdup("foot -e btop"); // default click command
	cfg.show_net = 1;						   // Network widget on by default
	cfg.net_iface = NULL;					   // Auto-detect interface
	cfg.net_rx_color = 0;			// Falls back to WIDGET_NET_RX_COLOR
	cfg.net_tx_color = 0;			// Falls back to WIDGET_NET_TX_COLOR
	cfg.net_font_size = 0;			// Falls back to WIDGET_NET_FONT_SIZE
	cfg.net_bg_color = 0x33000000;	// dark, 80% transparent by default
	cfg.net_tile_width = 0;			// Set after load by net_compute_tile_size()
	cfg.show_volume = 0;			// Volume widget off by default
	cfg.volume_exec = NULL;			// Falls back to "foot -e alsamixer"
	cfg.date_date_format = NULL;	// Falls back to WIDGET_DATE_DATE_FORMAT
	cfg.date_date_color = 0;		// Falls back to WIDGET_DATE_DATE_COLOR
	cfg.date_date_size = 0;			// Falls back to WIDGET_DATE_DATE_SIZE
	cfg.date_time_format = NULL;	// Falls back to WIDGET_DATE_TIME_FORMAT
	cfg.date_time_color = 0;		// Falls back to WIDGET_DATE_TIME_COLOR
	cfg.date_time_size = 0;			// Falls back to WIDGET_DATE_TIME_SIZE
	cfg.date_bg_color = 0x33000000; // dark, 80% transparent by default
	// Default order: sysinfo(0), net(1), apps(2), volume(3), date(4)
	cfg.widget_order[0] = 0; // sysinfo
	cfg.widget_order[1] = 1; // net
	cfg.widget_order[2] = 2; // apps
	cfg.widget_order[3] = 3; // volume
	cfg.widget_order[4] = 4; // date
	if (!cfg.apps)
		return cfg;

	int capacity = 10;
	char line[512];
	DesktopEntry *current_entry = NULL;
	int in_apps_section = 0;
	int in_global_section = 0;
	int in_widget_date_section = 0;
	int in_widget_net_section = 0;
	int in_widget_sysinfo_section = 0;
	int in_widget_volume_section = 0;

	// Track section-declaration order to derive widget_order[].
	// Each element is a widget ID (0–4) in the order its section first appears.
	// -1 marks an unfilled slot.
	int section_order[5] = {-1, -1, -1, -1, -1};
	int section_order_count = 0;

	// Helper: record a widget/apps ID the first time its section is seen.
#define RECORD_SECTION(id)                                                     \
	do {                                                                       \
		int _already = 0;                                                      \
		for (int _s = 0; _s < section_order_count; _s++)                       \
			if (section_order[_s] == (id)) {                                   \
				_already = 1;                                                  \
				break;                                                         \
			}                                                                  \
		if (!_already && section_order_count < 5)                              \
			section_order[section_order_count++] = (id);                       \
	} while (0)

	if (verbose >= 2)
		printf("[DBG²] Parsing config file\n");

	while (fgets(line, sizeof(line), fp)) {
		// Remove trailing newline
		line[strcspn(line, "\n")] = '\0';

		// Skip empty lines and comments
		if (line[0] == '\0' || line[0] == '#')
			continue;

		if (verbose >= 2)
			printf("[DBG²] Config line: %s\n", line);

		// Check for section header
		if (line[0] == '[') {
			char *end = strchr(line, ']');
			if (end) {
				// Save previous entry if any
				if (current_entry) {
					// Only add if all required fields are
					// present
					if (current_entry->name && current_entry->exec &&
						current_entry->icon) {
						if (cfg.count >= capacity) {
							capacity *= 2;
							DesktopEntry **tmp = realloc(cfg.apps,
								capacity * sizeof(DesktopEntry *));
							if (!tmp) {
								free_desktop_entry(current_entry);
								return cfg;
							}
							cfg.apps = tmp;
						}
						cfg.apps[cfg.count++] = current_entry;
						if (verbose >= 2)
							printf("[DBG²] Added "
								   "app: %s\n",
								current_entry->name);
						if (verbose >= 2)
							printf("[DBG²]   name: %s, icon: %s,%s exec: %s\n",
								current_entry->name, current_entry->icon,
								current_entry->terminal ? " terminal: true," :
														  "",
								current_entry->exec);
					} else {
						if (verbose)
							printf("[DBG] Skipping app with missing fields\n");
						if (verbose) {
							const char *n = current_entry->name ?
								current_entry->name :
								"(missing)";
							const char *ic = current_entry->icon ?
								current_entry->icon :
								"(missing)";
							const char *ex = current_entry->exec ?
								current_entry->exec :
								"(missing)";
							printf("[DBG]   name: %s, icon: %s, exec: %s\n", n,
								ic, ex);
						}
						free_desktop_entry(current_entry);
					}
					current_entry = NULL;
				} // Check which section we're entering
				if (strncmp(line, "[global]", 8) == 0) {
					in_global_section = 1;
					in_apps_section = 0;
					in_widget_date_section = 0;
					in_widget_net_section = 0;
					in_widget_sysinfo_section = 0;
					in_widget_volume_section = 0;
					if (verbose >= 2)
						printf("[DBG²] Entering "
							   "[global] section\n");
					if (verbose >= 2)
						printf("[DBG²] Processing "
							   "global "
							   "configuration\n");
				} else if (strncmp(line, "[widget-sysinfo]", 16) == 0) {
					in_global_section = 0;
					in_apps_section = 0;
					in_widget_date_section = 0;
					in_widget_net_section = 0;
					in_widget_sysinfo_section = 1;
					in_widget_volume_section = 0;
					RECORD_SECTION(0); // WIDGET_ID_SYSINFO
					if (verbose >= 2)
						printf("[DBG²] Entering [widget-sysinfo] section\n");
				} else if (strncmp(line, "[widget-net]", 12) == 0) {
					in_global_section = 0;
					in_apps_section = 0;
					in_widget_date_section = 0;
					in_widget_net_section = 1;
					in_widget_sysinfo_section = 0;
					in_widget_volume_section = 0;
					RECORD_SECTION(1); // WIDGET_ID_NET
					if (verbose >= 2)
						printf("[DBG²] Entering [widget-net] section\n");
				} else if (strncmp(line, "[apps]", 6) == 0) {
					in_global_section = 0;
					in_apps_section = 1;
					in_widget_date_section = 0;
					in_widget_net_section = 0;
					in_widget_sysinfo_section = 0;
					in_widget_volume_section = 0;
					RECORD_SECTION(2); // WIDGET_ID_APPS
					if (verbose >= 2)
						printf("[DBG²] Entering [apps] "
							   "section\n");
					if (verbose >= 2)
						printf("[DBG²] Processing "
							   "applications\n");
				} else if (strncmp(line, "[widget-volume]", 15) == 0) {
					in_global_section = 0;
					in_apps_section = 0;
					in_widget_date_section = 0;
					in_widget_net_section = 0;
					in_widget_sysinfo_section = 0;
					in_widget_volume_section = 1;
					RECORD_SECTION(3); // WIDGET_ID_VOLUME
					if (verbose >= 2)
						printf("[DBG²] Entering [widget-volume] section\n");
				} else if (strncmp(line, "[widget-date]", 13) == 0) {
					in_global_section = 0;
					in_apps_section = 0;
					in_widget_date_section = 1;
					in_widget_net_section = 0;
					in_widget_sysinfo_section = 0;
					in_widget_volume_section = 0;
					RECORD_SECTION(4); // WIDGET_ID_DATE
					if (verbose >= 2)
						printf("[DBG²] Entering [widget-date] section\n");
				} else {
					in_global_section = 0;
					in_apps_section = 0;
					in_widget_date_section = 0;
					in_widget_net_section = 0;
					in_widget_sysinfo_section = 0;
					in_widget_volume_section = 0;
					in_widget_sysinfo_section = 0;
					if (verbose >= 2)
						printf("[DBG²] Unknown "
							   "section: %s\n",
							line);
				}
			}
			continue;
		}

		// Parse key=value pairs
		char *eq = strchr(line, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char *key = trim_string(line);
		char *value = trim_string(eq + 1);

		if (verbose >= 2)
			printf("[DBG²]   key='%s', value='%s'\n", key, value);

		// Handle global section
		if (in_global_section) {
			if (strcmp(key, "icon-size") == 0) {
				cfg.icon_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   icon-size: %d\n", cfg.icon_size);
			} else if (strcmp(key, "icon-spacing") == 0) {
				cfg.icon_spacing = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   icon-spacing: %d\n", cfg.icon_spacing);
			} else if (strcmp(key, "exclusive-zone") == 0) {
				cfg.exclusive_zone = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   exclusive-zone: %d\n", cfg.exclusive_zone);
			} else if (strcmp(key, "border-space") == 0) {
				cfg.border_space = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   border-space: %d\n", cfg.border_space);
			} else if (strcmp(key, "label-mode") == 0) {
				if (strcmp(value, "hover") == 0)
					cfg.label_mode = LABEL_MODE_HOVER;
				else if (strcmp(value, "never") == 0)
					cfg.label_mode = LABEL_MODE_NEVER;
				else
					cfg.label_mode = LABEL_MODE_ALWAYS;
				if (verbose >= 2)
					printf("[DBG²]   label-mode: %s\n", value);
			} else if (strcmp(key, "label-color") == 0) {
				// Accept #RRGGBB or #RRGGBBAA
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.label_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.label_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   label-color: 0x%08X\n", cfg.label_color);
			} else if (strcmp(key, "label-size") == 0) {
				cfg.label_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   label-size: %d\n", cfg.label_size);
			} else if (strcmp(key, "label-offset") == 0) {
				cfg.label_offset = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   label-offset: %d\n", cfg.label_offset);
			} else if (strcmp(key, "position") == 0) {
				if (strcmp(value, "top") == 0)
					cfg.position = POSITION_TOP;
				else if (strcmp(value, "left") == 0)
					cfg.position = POSITION_LEFT;
				else if (strcmp(value, "right") == 0)
					cfg.position = POSITION_RIGHT;
				else
					cfg.position = POSITION_BOTTOM; // Default
				if (verbose >= 2)
					printf("[DBG²]   position: %s\n", value);
			} else if (strcmp(key, "layer") == 0) {
				if (strcmp(value, "background") == 0)
					cfg.layer = LAYER_BACKGROUND;
				else if (strcmp(value, "bottom") == 0)
					cfg.layer = LAYER_BOTTOM;
				else if (strcmp(value, "overlay") == 0)
					cfg.layer = LAYER_OVERLAY;
				else
					cfg.layer = LAYER_TOP; // Default
			} else if (strcmp(key, "output") == 0) {
				free(cfg.output_name);
				cfg.output_name = value[0] ? strdup(value) : NULL;
				if (verbose >= 2)
					printf("[DBG²]   output: %s\n",
						cfg.output_name ? cfg.output_name : "(auto)");
			} else if (strcmp(key, "show-date") == 0) {
				cfg.show_date =
					(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
				if (verbose >= 2)
					printf("[DBG²]   show-date: %s\n",
						cfg.show_date ? "true" : "false");
			} else if (strcmp(key, "show-sysinfo") == 0) {
				cfg.show_sysinfo =
					(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
				if (verbose >= 2)
					printf("[DBG²]   show-sysinfo: %s\n",
						cfg.show_sysinfo ? "true" : "false");
			} else if (strcmp(key, "show-net") == 0) {
				cfg.show_net =
					(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
				if (verbose >= 2)
					printf("[DBG²]   show-net: %s\n",
						cfg.show_net ? "true" : "false");
			} else if (strcmp(key, "show-volume") == 0) {
				cfg.show_volume =
					(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
				if (verbose >= 2)
					printf("[DBG²]   show-volume: %s\n",
						cfg.show_volume ? "true" : "false");
			} else if (strcmp(key, "widget-net-bg-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.net_bg_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.net_bg_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-net-bg-color: 0x%08X\n",
						cfg.net_bg_color);
			}
			continue;
		}

		// Handle [widget-volume] section
		if (in_widget_volume_section) {
			if (strcmp(key, "exec") == 0) {
				free(cfg.volume_exec);
				cfg.volume_exec = value[0] ? strdup(value) : NULL;
				if (verbose >= 2)
					printf("[DBG²]   widget-volume exec: %s\n",
						cfg.volume_exec ? cfg.volume_exec : "(none)");
			}
			continue;
		}

		// Handle [widget-sysinfo] section
		if (in_widget_sysinfo_section) {
			if (strcmp(key, "percpu") == 0) {
				cfg.sysinfo_percpu = (strcmp(value, "true") == 0);
				if (verbose >= 2)
					printf("[DBG²]   sysinfo percpu: %s\n", value);
			} else if (strcmp(key, "show-temp") == 0) {
				cfg.sysinfo_show_temp =
					(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
				if (verbose >= 2)
					printf("[DBG²]   sysinfo show-temp: %s\n", value);
			} else if (strcmp(key, "show-proc") == 0) {
				cfg.sysinfo_show_proc =
					(strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
				if (verbose >= 2)
					printf("[DBG²]   sysinfo show-proc: %s\n", value);
			} else if (strcmp(key, "cpu-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.sysinfo_cpu_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.sysinfo_cpu_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-sysinfo cpu-color: 0x%08X\n",
						cfg.sysinfo_cpu_color);
			} else if (strcmp(key, "tmp-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.sysinfo_tmp_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.sysinfo_tmp_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-sysinfo tmp-color: 0x%08X\n",
						cfg.sysinfo_tmp_color);
			} else if (strcmp(key, "ram-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.sysinfo_ram_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.sysinfo_ram_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-sysinfo ram-color: 0x%08X\n",
						cfg.sysinfo_ram_color);
			} else if (strcmp(key, "size") == 0) {
				cfg.sysinfo_font_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   widget-sysinfo size: %d\n",
						cfg.sysinfo_font_size);
			} else if (strcmp(key, "bg-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.sysinfo_bg_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.sysinfo_bg_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-sysinfo bg-color: 0x%08X\n",
						cfg.sysinfo_bg_color);
			} else if (strcmp(key, "exec") == 0) {
				free(cfg.sysinfo_exec);
				cfg.sysinfo_exec = value[0] ? strdup(value) : NULL;
				if (verbose >= 2)
					printf("[DBG²]   widget-sysinfo exec: %s\n",
						cfg.sysinfo_exec ? cfg.sysinfo_exec : "(none)");
			}
			continue;
		}

		// Handle [widget-net] section
		if (in_widget_net_section) {
			if (strcmp(key, "iface") == 0) {
				free(cfg.net_iface);
				cfg.net_iface = strdup(value);
				if (verbose >= 2)
					printf("[DBG²]   widget-net iface: %s\n", cfg.net_iface);
			} else if (strcmp(key, "rx-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.net_rx_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.net_rx_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-net rx-color: 0x%08X\n",
						cfg.net_rx_color);
			} else if (strcmp(key, "tx-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.net_tx_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.net_tx_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-net tx-color: 0x%08X\n",
						cfg.net_tx_color);
			} else if (strcmp(key, "size") == 0) {
				cfg.net_font_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   widget-net size: %d\n", cfg.net_font_size);
			} else if (strcmp(key, "bg-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.net_bg_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.net_bg_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-net bg-color: 0x%08X\n",
						cfg.net_bg_color);
			}
			continue;
		}

		// Handle [widget-date] section
		if (in_widget_date_section) {
			if (strcmp(key, "format") == 0) {
				free(cfg.date_date_format);
				cfg.date_date_format = strdup(value);
				if (verbose >= 2)
					printf("[DBG²]   widget-date format: %s\n",
						cfg.date_date_format);
			} else if (strcmp(key, "color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.date_date_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.date_date_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-date color: 0x%08X\n",
						cfg.date_date_color);
			} else if (strcmp(key, "size") == 0) {
				cfg.date_date_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   widget-date size: %d\n",
						cfg.date_date_size);
			} else if (strcmp(key, "time-format") == 0) {
				free(cfg.date_time_format);
				cfg.date_time_format = strdup(value);
				if (verbose >= 2)
					printf("[DBG²]   widget-date time-format: %s\n",
						cfg.date_time_format);
			} else if (strcmp(key, "time-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.date_time_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.date_time_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-date time-color: 0x%08X\n",
						cfg.date_time_color);
			} else if (strcmp(key, "time-size") == 0) {
				cfg.date_time_size = atoi(value);
				if (verbose >= 2)
					printf("[DBG²]   widget-date time-size: %d\n",
						cfg.date_time_size);
			} else if (strcmp(key, "bg-color") == 0) {
				const char *hex = value;
				if (hex[0] == '#')
					hex++;
				unsigned long parsed = strtoul(hex, NULL, 16);
				if (strlen(hex) <= 6)
					cfg.date_bg_color = 0xFF000000 | (unsigned int)parsed;
				else
					cfg.date_bg_color =
						((parsed & 0xFF) << 24) | ((parsed >> 8) & 0xFFFFFF);
				if (verbose >= 2)
					printf("[DBG²]   widget-date bg-color: 0x%08X\n",
						cfg.date_bg_color);
			}
			continue;
		}

		// Handle apps section
		if (!in_apps_section)
			continue;

		// Check for app entry start (name field indicates a new app)
		if (strcmp(key, "name") == 0) {
			// Save previous app entry if complete
			if (current_entry) {
				if (current_entry->name && current_entry->exec &&
					current_entry->icon) {
					if (cfg.count >= capacity) {
						capacity *= 2;
						DesktopEntry **tmp = realloc(cfg.apps,
							capacity * sizeof(DesktopEntry *));
						if (!tmp) {
							free_desktop_entry(current_entry);
							return cfg;
						}
						cfg.apps = tmp;
					}
					cfg.apps[cfg.count++] = current_entry;
					if (verbose >= 2)
						printf("[DBG²] Added app: %s\n", current_entry->name);
				} else {
					if (verbose >= 2)
						printf("[DBG²] Skipping app "
							   "with missing fields\n");
					free_desktop_entry(current_entry);
				}
			}
			current_entry = calloc(1, sizeof(DesktopEntry));
			if (!current_entry)
				return cfg;
		}

		if (!current_entry)
			continue;

		if (strcmp(key, "name") == 0) {
			free(current_entry->name);
			current_entry->name = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   name: %s\n", value);
		} else if (strcmp(key, "terminal") == 0) {
			current_entry->terminal = (strcasecmp(value, "true") == 0);
			if (verbose >= 2)
				printf("[DBG²]   terminal: %s\n",
					current_entry->terminal ? "true" : "false");
		} else if (strcmp(key, "exec") == 0) {
			free(current_entry->exec);
			current_entry->exec = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   exec: %s\n", value);
		} else if (strcmp(key, "icon") == 0) {
			free(current_entry->icon);
			current_entry->icon = strdup(value);
			if (verbose >= 2)
				printf("[DBG²]   icon: %s (size: will be set "
					   "during render)\n",
					value);
		}
	}

	// Don't forget the last entry
	if (current_entry) {
		if (current_entry->name && current_entry->exec && current_entry->icon) {
			if (cfg.count >= capacity) {
				capacity *= 2;
				DesktopEntry **tmp =
					realloc(cfg.apps, capacity * sizeof(DesktopEntry *));
				if (!tmp) {
					free_desktop_entry(current_entry);
					return cfg;
				}
				cfg.apps = tmp;
			}
			cfg.apps[cfg.count++] = current_entry;
			if (verbose >= 2)
				printf("[DBG²] Added app: %s\n", current_entry->name);
		} else {
			if (verbose >= 2)
				printf("[DBG²] Skipping app with missing "
					   "fields\n");
			free_desktop_entry(current_entry);
		}
	}

	if (verbose >= 2)
		printf("[DBG²] Config file parsed: %d application(s) loaded\n",
			cfg.count);

	if (verbose >= 2) {
		printf("[DBG²] Parsing complete - icon_size: %d\n", cfg.icon_size);
		for (int i = 0; i < cfg.count; i++) {
			printf("[DBG²]   App #%d: %s -> %s\n", i, cfg.apps[i]->name,
				cfg.apps[i]->exec);
		}
	}

	// Apply section-declaration order to widget_order[].
	// Any widget IDs not seen in the file are appended in their default order.
	if (section_order_count > 0) {
		// Build a list of IDs missing from the file (in default order)
		static const int default_order[5] = {0, 1, 2, 3, 4};
		int filled = 0;
		int result[5];
		// First, copy the IDs seen in the file
		for (int i = 0; i < section_order_count; i++)
			result[filled++] = section_order[i];
		// Then append any IDs not yet present, preserving default relative
		// order
		for (int d = 0; d < 5; d++) {
			int id = default_order[d];
			int seen = 0;
			for (int k = 0; k < filled; k++)
				if (result[k] == id) {
					seen = 1;
					break;
				}
			if (!seen)
				result[filled++] = id;
		}
		for (int i = 0; i < 5; i++)
			cfg.widget_order[i] = result[i];
		if (verbose >= 2) {
			static const char *wnames[5] = {
				"sysinfo", "net", "apps", "volume", "date"};
			printf("[DBG²]   widget-order (from sections): "
				   "%s,%s,%s,%s,%s\n",
				wnames[cfg.widget_order[0]], wnames[cfg.widget_order[1]],
				wnames[cfg.widget_order[2]], wnames[cfg.widget_order[3]],
				wnames[cfg.widget_order[4]]);
		}
	}

#undef RECORD_SECTION

	return cfg;
}

// Load the config file or create it if it doesn't exist
// If the file doesn't exist, calls init_config() to create it
// Returns a Config struct with loaded or created applications
Config
load_config(void)
{
	Config cfg = {0};

	// Try to open existing config file
	const char *home = getenv("HOME");
	if (!home)
		return cfg;

	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/" CONFIG_DIR "/" CONFIG_NAME,
		home);

	if (verbose >= 3)
		printf("[I/O] FOPEN (read): %s\n", config_path);
	FILE *fp = fopen(config_path, "r");
	if (!fp) {
		if (verbose >= 3)
			printf("[I/O] FOPEN FAILED (will create): %s\n", config_path);
		// File doesn't exist, create it
		if (init_config() == 0) {
			// Successfully created, reload
			if (verbose >= 3)
				printf("[I/O] FOPEN (read, after create): %s\n", config_path);
			fp = fopen(config_path, "r");
			if (!fp)
				return cfg;
		} else {
			// Failed to create config
			return cfg;
		}
	}

	// Parse the config file
	cfg = parse_config_file(fp);
	if (verbose >= 3)
		printf("[I/O] FCLOSE: %s\n", config_path);
	fclose(fp);
	return cfg;
}

// Write default config to ~/.config/labar.cfg
int
write_default_config(DesktopEntry **entries, int count)
{
	const char *home = getenv("HOME");
	if (!home)
		return 0;

	// List of default applications to include
	const char *default_apps[] = {
		// misc: pw manager + email client
		"KeePassXC",
		"Thunderbird",
		// web browsers
		"Firefox",
		"google-chrome",
		"Chromium",
		// file managers
		"Dolphin",
		"File Manager PCManFM",
		"Thunar File Manager",
		// terminals
		"Alacritty",
		"kitty",
		"Konsole",
		"Foot",
		"XTerm",
	};
	int default_count = sizeof(default_apps) / sizeof(default_apps[0]);

	// Create ~/.config directory if it doesn't exist
	char config_dir[512];
	snprintf(config_dir, sizeof(config_dir), "%s/" CONFIG_DIR, home);
	if (verbose >= 3)
		printf("[I/O] MKDIR: %s\n", config_dir);
	mkdir(config_dir, 0755);

	// Write config file
	char config_path[512];
	snprintf(config_path, sizeof(config_path), "%s/" CONFIG_DIR "/" CONFIG_NAME,
		home);

	if (verbose >= 3)
		printf("[I/O] FOPEN (write): %s\n", config_path);
	FILE *fp = fopen(config_path, "w");
	if (!fp) {
		perror("fopen");
		return 0;
	}

	fprintf(fp, "[global]\n");
	fprintf(fp, "icon-size=64\n");
	fprintf(fp, "# icon-spacing: spacing between icons in pixels\n");
	fprintf(fp, "icon-spacing=0\n");
	fprintf(fp, "# exclusive-zone: interaction with other surfaces\n");
	fprintf(fp, "#    0: surface will be moved to avoid occluding\n");
	fprintf(fp, "#       surfaces with positive exclusive zone\n");
	fprintf(fp, "#   >0: surface reserves space (e.g., panel=10 prevents\n");
	fprintf(fp, "#       maximized windows from overlapping)\n");
	fprintf(fp, "#   -1: surface stretches to edges, ignoring other\n");
	fprintf(fp, "#       surfaces (e.g., wallpaper, lock screen)\n");
	fprintf(fp, "exclusive-zone=64\n");
	fprintf(fp, "# border-space: pixels between the bar and the screen edge\n");
	fprintf(fp, "border-space=0\n");
	fprintf(fp, "# label-mode: always | hover | never\n");
	fprintf(fp, "label-mode=hover\n");
	fprintf(fp, "# label-color: hex color #RRGGBB or #RRGGBBAA\n");
	fprintf(fp, "label-color=#FFFFFF\n");
	fprintf(fp, "# label-size: font size in points for the app-name label\n");
	fprintf(fp, "label-size=10\n");
	fprintf(fp,
		"# label_offset: pixels from the bottom edge of the icon "
		"to the text baseline\n");
	fprintf(fp,
		"#   0 = bottom edge (descenders clipped), icon-size = top "
		"edge (text invisible)\n");
	fprintf(fp, "#   recommended range: 4-16\n");
	fprintf(fp, "label-offset=10\n");
	fprintf(fp, "# position: where to place the bar on screen\n");
	fprintf(fp, "#   bottom (default): horizontal bar at the bottom\n");
	fprintf(fp, "#   top:              horizontal bar at the top\n");
	fprintf(fp, "#   left:             vertical bar on the left\n");
	fprintf(fp, "#   right:            vertical bar on the right\n");
	fprintf(fp, "position=bottom\n");
	fprintf(fp, "# layer: layer-shell layer for the surface\n");
	fprintf(fp, "#   background:       beneath everything (for wallpapers)\n");
	fprintf(fp, "#   bottom:           below normal windows (for docks)\n");
	fprintf(fp, "#   top (default):    above normal windows\n");
	fprintf(fp, "#   overlay:          on top of everything\n");
	fprintf(fp, "layer=top\n");
	fprintf(fp, "# output: output name to place the bar on (empty = auto)\n");
	fprintf(fp, "# output=eDP-1\n");
	fprintf(fp, "output=\n");
	fprintf(fp, "# show-sysinfo: show the CPU/RAM usage widget\n");
	fprintf(fp, "show-sysinfo=true\n");
	fprintf(fp, "# show-net: show the network speed widget\n");
	fprintf(fp, "show-net=true\n");
	fprintf(fp, "# show-volume: show the volume widget (after apps)\n");
	fprintf(fp, "show-volume=true\n");
	fprintf(fp, "# show-date: show the date/time widget (last slot)\n");
	fprintf(fp, "show-date=true\n");
	fprintf(fp, "\n[widget-sysinfo]\n");
	fprintf(fp, "# percpu: true = per-core %% like top, false = system-wide\n");
	fprintf(fp, "percpu=true\n");
	fprintf(fp, "# show-temp: show the CPU temperature line\n");
	fprintf(fp, "show-temp=true\n");
	fprintf(fp, "# show-proc: show process name sub-lines\n");
	fprintf(fp, "show-proc=true\n");
	fprintf(fp, "# cpu-color: color for the CPU usage line\n");
	fprintf(fp, "cpu-color=#FFEB3B\n");
	fprintf(fp, "# tmp-color: color for the CPU temperature line\n");
	fprintf(fp, "tmp-color=#FF7043\n");
	fprintf(fp, "# ram-color: color for the RAM usage line\n");
	fprintf(fp, "ram-color=#66BB6A\n");
	fprintf(fp, "# size: font size in pt\n");
	fprintf(fp, "size=14\n");
	fprintf(fp, "# bg-color: tile background color (#RRGGBBAA)\n");
	fprintf(fp, "#   #00000094 = black 42%% transparent (default)\n");
	fprintf(fp, "#   #00000000 = fully transparent\n");
	fprintf(fp, "bg-color=#00000094\n");
	fprintf(fp, "# exec: command to run on left-click (empty to disable)\n");
	fprintf(fp, "exec=foot -e btop\n");
	fprintf(fp, "\n[widget-net]\n");
	fprintf(fp,
		"# iface: network interface to monitor (omit for auto-detect)\n");
	fprintf(fp, "# iface=eth0\n");
	fprintf(fp, "# rx-color: color for the receive (down) speed line\n");
	fprintf(fp, "rx-color=#FF3FFA\n");
	fprintf(fp, "# tx-color: color for the transmit (up) speed line\n");
	fprintf(fp, "tx-color=#3AFFFD\n");
	fprintf(fp, "# size: font size in pt for the speed lines\n");
	fprintf(fp, "size=14\n");
	fprintf(fp, "# bg-color: tile background color (#RRGGBBAA)\n");
	fprintf(fp, "#   #00000094 = black 42%% transparent (default)\n");
	fprintf(fp, "#   #00000000 = fully transparent\n");
	fprintf(fp, "bg-color=#00000094\n");
	fprintf(fp, "\n[apps]\n");

	int written = 0;

	for (int i = 0; i < default_count; i++) {
		DesktopEntry *app = find_app_by_name(entries, count, default_apps[i]);

		if (!app)
			continue;

		char *icon_path = find_best_icon(app->icon);
		if (!icon_path)
			continue;

		fprintf(fp, "name=%s\n", app->name);
		fprintf(fp, "icon=%s\n", icon_path);
		if (app->terminal)
			fprintf(fp, "terminal=true\n");
		fprintf(fp, "exec=%s\n\n", app->exec);

		free(icon_path);
		written++;
	}

	fprintf(fp, "\n[widget-volume]\n");
	fprintf(fp, "# exec: command to run on right-click (empty to disable)\n");
	fprintf(fp, "exec=foot -e alsamixer\n");
	fprintf(fp, "\n[widget-date]\n");
	fprintf(fp, "# Date line (upper half of the tile)\n");
	fprintf(fp, "# format: strftime(3) format string, e.g. \"%%a %%d %%B\"\n");
	fprintf(fp, "format=%%a %%d %%B\n");
	fprintf(fp, "color=#68FF3A\n");
	fprintf(fp, "size=16\n");
	fprintf(fp, "# Time line (lower half of the tile)\n");
	fprintf(fp, "# time-format: strftime(3) format string, e.g. \"%%H:%%M\"\n");
	fprintf(fp, "time-format=%%H:%%M\n");
	fprintf(fp, "time-color=#FF0000\n");
	fprintf(fp, "time-size=36\n");
	fprintf(fp, "# bg-color: tile background color (#RRGGBBAA)\n");
	fprintf(fp, "#   #00000094 = black 42%% transparent (default)\n");
	fprintf(fp, "#   #00000000 = fully transparent\n");
	fprintf(fp, "bg-color=#00000094\n");

	if (verbose >= 3)
		printf("[I/O] FCLOSE: %s\n", config_path);
	fclose(fp);

	// Return success only if at least one app was written
	return written > 0;
}

// Initialize the config file on first run
// If config doesn't exist, tries to create it
// Returns 0 if successful, 1 if unable to proceed
int
init_config(void)
{
	// Load all applications
	int app_count = 0;
	DesktopEntry **apps = list_all_applications(&app_count);

	if (app_count == 0) {
		const char *xdg = getenv("XDG_DATA_DIRS");
		fprintf(stderr, "No applications found in %s/" APPS_SUBDIR "\n",
			xdg && xdg[0] ? xdg : XDG_DATA_DIRS_DEFAULT);
		return 1;
	}

	// Try to write default config
	if (write_default_config(apps, app_count)) {
		printf("Created default config at ~/" CONFIG_DIR "/" CONFIG_NAME "\n");
		free_applications(apps, app_count);
		return 0;
	}

	// If we get here, firefox and foot weren't found - list available apps
	printf("No config file found at ~/" CONFIG_DIR "/" CONFIG_NAME "\n");
	printf("Available applications (%d found):\n\n", app_count);

	for (int i = 0; i < app_count; i++) {
		printf("#%d %s\n", i + 1, apps[i]->name);
		if (apps[i]->icon)
			printf("    Icon: %s\n", apps[i]->icon);
		printf("    Exec: %s\n\n", apps[i]->exec);
	}

	free_applications(apps, app_count);

	fprintf(stderr,
		"\nPlease create a config file at ~/" CONFIG_DIR "/" CONFIG_NAME "\n");
	return 1;
}
