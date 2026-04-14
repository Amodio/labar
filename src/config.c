#define _GNU_SOURCE

#include "config.h"
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APPS_DIR "/usr/share/applications"

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

// Build a heap-allocated icon path; caller must free.
// Returns NULL on allocation failure.
static char *
make_icon_path(const char *dir, const char *size, const char *name,
	const char *ext)
{
	char *p = NULL;
	asprintf(&p, "%s/%s/apps/%s.%s", dir, size, name, ext);
	return p;
}

// Find all icon variants for an application
// Returns the best quality icon path, listing all candidates if verbose
// Caller must free the returned string
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

	const char *icon_base = "/usr/share/icons";
	DIR *base_dir = opendir(icon_base);
	if (!base_dir) {
		free(candidates);
		return NULL;
	}

	struct dirent *theme_entry;
	while ((theme_entry = readdir(base_dir))) {
		// Skip . and ..
		if (theme_entry->d_name[0] == '.')
			continue;

		char *theme_path = NULL;
		if (asprintf(&theme_path, "%s/%s", icon_base, theme_entry->d_name) < 0)
			continue;

		// Check if it's a directory
		DIR *theme_dir = opendir(theme_path);
		if (!theme_dir) {
			free(theme_path);
			continue;
		}

		struct dirent *size_entry;
		while ((size_entry = readdir(theme_dir))) {
			if (size_entry->d_name[0] == '.')
				continue;

			// Try SVG
			char *svg_path = make_icon_path(theme_path, size_entry->d_name,
				icon_name, "svg");
			if (svg_path) {
				if (access(svg_path, F_OK) == 0) {
					if (verbose >= 2)
						printf("[DBG²]   Found SVG: "
							   "%s\n",
							svg_path);
					if (candidate_count >= capacity) {
						capacity *= 2;
						IconCandidate *tmp = realloc(candidates,
							capacity * sizeof(IconCandidate));
						if (tmp)
							candidates = tmp;
						else {
							free(svg_path);
							goto cleanup;
						}
					}
					candidates[candidate_count].path = svg_path;
					candidates[candidate_count].size =
						999; // SVG is scalable (highest
							 // priority)
					candidate_count++;
				} else {
					if (verbose >= 4)
						printf("[I/O] ACCESS CHECK (stat): %s - NOT FOUND\n",
							svg_path);
					free(svg_path);
				}
			} // Try PNG
			char *png_path = make_icon_path(theme_path, size_entry->d_name,
				icon_name, "png");
			if (png_path) {
				if (access(png_path, F_OK) == 0) {
					if (verbose >= 4)
						printf("[I/O] ACCESS CHECK (stat): %s - EXISTS\n",
							png_path);
					// Extract size from directory name
					// (e.g., "256x256" -> 256)
					int size = 0;
					sscanf(size_entry->d_name, "%dx%d", &size, &size);

					if (verbose >= 2)
						printf("[DBG²]   Found PNG "
							   "(%dpx): %s\n",
							size, png_path);
					if (candidate_count >= capacity) {
						capacity *= 2;
						IconCandidate *tmp = realloc(candidates,
							capacity * sizeof(IconCandidate));
						if (tmp)
							candidates = tmp;
						else {
							free(png_path);
							goto cleanup;
						}
					}
					candidates[candidate_count].path = png_path;
					candidates[candidate_count].size = size;
					candidate_count++;
				} else {
					if (verbose >= 4)
						printf("[I/O] ACCESS CHECK (stat): %s - NOT FOUND\n",
							png_path);
					free(png_path);
				}
			}
		}

		closedir(theme_dir);
		free(theme_path);
	}

	if (verbose >= 4)
		printf("[I/O] CLOSEDIR: /usr/share/icons (base theme dir)\n");
	closedir(base_dir);

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

// List all applications from APPS_DIR/*.desktop
// Allocates an array of DesktopEntry pointers; caller must free all entries
// and the array itself.
DesktopEntry **
list_all_applications(int *count_out)
{
	const char *app_dir = APPS_DIR;
	if (verbose >= 4)
		printf("[I/O] OPENDIR: %s\n", app_dir);
	DIR *dir = opendir(app_dir);
	if (!dir) {
		if (verbose >= 4)
			printf("[I/O] OPENDIR FAILED: %s\n", app_dir);
		perror("opendir");
		*count_out = 0;
		return NULL;
	}

	// First pass: count valid .desktop files
	int capacity = 16;
	int count = 0;
	DesktopEntry **entries = malloc(capacity * sizeof(DesktopEntry *));
	if (!entries) {
		closedir(dir);
		*count_out = 0;
		return NULL;
	}

	struct dirent *entry;
	while ((entry = readdir(dir))) {
		// Only process .desktop files
		if (strlen(entry->d_name) < 9)
			continue;
		if (strcmp(entry->d_name + strlen(entry->d_name) - 8, ".desktop") != 0)
			continue;

		// Build full path
		char filepath[512];
		snprintf(filepath, sizeof(filepath), "%s/%s", app_dir, entry->d_name);

		// Parse the .desktop file
		DesktopEntry *parsed = parse_desktop_file(filepath);
		if (!parsed)
			continue;

		// Grow array if needed
		if (count >= capacity) {
			capacity *= 2;
			DesktopEntry **tmp =
				realloc(entries, capacity * sizeof(DesktopEntry *));
			if (!tmp) {
				free_desktop_entry(parsed);
				goto cleanup;
			}
			entries = tmp;
		}

		entries[count++] = parsed;
	}

	if (verbose >= 4)
		printf("[I/O] CLOSEDIR: %s\n", app_dir);
	closedir(dir);
	*count_out = count;
	return entries;

cleanup:
	for (int i = 0; i < count; i++)
		free_desktop_entry(entries[i]);
	free(entries);
	closedir(dir);
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
	cfg.sysinfo_cpu_color = 0;		   // Falls back to WIDGET_SYSINFO_CPU_COLOR
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
		"keepassxc",
		"thunderbird",
		"google-chrome",
		"chromium",
		"firefox",
		"foot",
		"xterm",
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
	fprintf(fp, "# cpu-color: color for the CPU usage line\n");
	fprintf(fp, "cpu-color=#FFEB3B\n");
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

		char *icon_path = find_best_icon(default_apps[i]);
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
		fprintf(stderr, "No applications found in " APPS_DIR "\n");
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
