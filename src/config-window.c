#define _GNU_SOURCE

#include "config-window.h"
#include "config.h"

#include <dirent.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int verbose;

/* =========================================================================
 * Forward declarations
 * ======================================================================= */
typedef struct _CfgWin CfgWin;
static void refresh_apps_list(CfgWin *w);
static void refresh_widgets_list(CfgWin *w);
static void harvest_widget_fields(CfgWin *w);

static void on_icon_preview_slot_free(gpointer data);
static void on_icon_entry_changed(GtkEditable *editable, gpointer user_data);
static void on_icon_path_btn_clicked(GtkButton *btn, gpointer user_data);

/* =========================================================================
 * Local icon resolution
 *
 * The icon field in a DesktopEntry from list_all_applications() is a logical
 * icon name (e.g. "firefox"), not a path.  write_default_config() calls the
 * private find_best_icon() to turn it into an absolute path before writing.
 * We replicate a simplified version here so we can:
 *   (a) show a preview in the Add App dialog, and
 *   (b) write the resolved absolute path into labar.cfg.
 * ======================================================================= */

/* Find the best icon for icon_name by delegating to the shared
 * find_best_icon_for_name() in config.c, which handles both the hicolor
 * (<theme>/<size>/apps/<n>.ext) and breeze (<theme>/apps/<size>/<n>.ext)
 * layouts across all XDG data dirs.  Returns a heap-allocated path or NULL.
 * Already-absolute paths are returned as-is via strdup. */
static char *
local_find_best_icon(const char *icon_name)
{
	if (!icon_name || icon_name[0] == '\0')
		return NULL;
	if (icon_name[0] == '/')
		return strdup(icon_name);
	return find_best_icon_for_name(icon_name);
}

/* Collect ALL icon paths for <icon_name> across every theme/size directory.
 * Returns a NULL-terminated heap-allocated array of heap-allocated strings.
 * Caller must free each string and the array itself.
 * The list is deduplicated and sorted largest→smallest (SVG first). */
static char **
local_find_all_icons(const char *icon_name)
{
	if (!icon_name || icon_name[0] == '\0')
		return NULL;

	/* If given an absolute path, derive the logical name (basename minus
	 * extension) so we can still do a full theme scan.  We'll also include
	 * the original absolute path in the results. */
	char logical[256] = {0};
	const char *absolute_seed = NULL; /* include verbatim if non-NULL */

	if (icon_name[0] == '/') {
		absolute_seed = icon_name;
		const char *slash = strrchr(icon_name, '/');
		const char *base = slash ? slash + 1 : icon_name;
		const char *dot = strrchr(base, '.');
		size_t len = dot ? (size_t)(dot - base) : strlen(base);
		if (len == 0 || len >= sizeof(logical)) {
			/* Can't derive a logical name — return just the path */
			char **r = malloc(2 * sizeof(char *));
			if (!r)
				return NULL;
			r[0] = strdup(icon_name);
			r[1] = NULL;
			return r;
		}
		memcpy(logical, base, len);
		logical[len] = '\0';
		icon_name = logical;
	}

	/* Dynamic array of (path, size) pairs before deduplication */
	typedef struct {
		char *path;
		int sz;
	} IconEntry;
	IconEntry *found = NULL;
	int nfound = 0, cap = 0;

	/* Helper: append a path+size, deduplicating */
#define APPEND_ICON(p, s)                                                      \
	do {                                                                       \
		int _dup = 0;                                                          \
		for (int _k = 0; _k < nfound; _k++)                                    \
			if (strcmp(found[_k].path, (p)) == 0) {                            \
				_dup = 1;                                                      \
				break;                                                         \
			}                                                                  \
		if (!_dup) {                                                           \
			if (nfound >= cap) {                                               \
				cap = cap ? cap * 2 : 8;                                       \
				found = realloc(found, (size_t)cap * sizeof(IconEntry));       \
				if (!found) {                                                  \
					free(p);                                                   \
					goto done_scan;                                            \
				}                                                              \
			}                                                                  \
			found[nfound].path = (p);                                          \
			found[nfound].sz = (s);                                            \
			nfound++;                                                          \
		} else {                                                               \
			free(p);                                                           \
		}                                                                      \
	} while (0)

	char **data_dirs = xdg_data_dirs();
	if (data_dirs) {
		for (int d = 0; data_dirs[d]; d++) {
			/* Search <datadir>/icons */
			char *icons_base = NULL;
			if (asprintf(&icons_base, "%s/" ICONS_SUBDIR, data_dirs[d]) < 0)
				continue;
			DIR *bd = opendir(icons_base);
			if (bd) {
				struct dirent *l1e;
				while ((l1e = readdir(bd))) {
					if (l1e->d_name[0] == '.')
						continue;
					char *l1path = NULL;
					if (asprintf(&l1path, "%s/%s", icons_base, l1e->d_name) < 0)
						continue;
					DIR *l1d = opendir(l1path);
					if (!l1d) {
						free(l1path);
						continue;
					}
					struct dirent *l2e;
					while ((l2e = readdir(l1d))) {
						if (l2e->d_name[0] == '.')
							continue;
						char *l2path = NULL;
						if (asprintf(&l2path, "%s/%s", l1path, l2e->d_name) < 0)
							continue;
						DIR *l2d = opendir(l2path);
						if (!l2d) {
							free(l2path);
							continue;
						}
						/* Size is whichever of l1/l2 parses as a
						 * positive integer (handles both hicolor
						 * <theme>/<size>/apps/ and breeze
						 * <theme>/apps/<size>/). */
						int sz_val = -1;
						sscanf(l2e->d_name, "%d", &sz_val);
						if (sz_val <= 0)
							sscanf(l1e->d_name, "%d", &sz_val);
						size_t prefix_len = strlen(icon_name);
						struct dirent *fe;
						while ((fe = readdir(l2d))) {
							if (fe->d_name[0] == '.')
								continue;
							/* Match exact name or "<n>-…" / "<n>.…" */
							if (strncmp(fe->d_name, icon_name, prefix_len) != 0)
								continue;
							char next = fe->d_name[prefix_len];
							if (next != '\0' && next != '-' && next != '.')
								continue;
							const char *dot = strrchr(fe->d_name, '.');
							if (!dot)
								continue;
							int is_svg = (strcmp(dot, ".svg") == 0);
							int is_png = (strcmp(dot, ".png") == 0);
							if (!is_svg && !is_png)
								continue;
							char *spath = NULL;
							if (asprintf(&spath, "%s/%s", l2path, fe->d_name) <
								0)
								continue;
							int sv = is_svg ?
								(sz_val < 0 ? 9999 : sz_val + 5000) :
								(sz_val < 0 ? 0 : sz_val);
							APPEND_ICON(spath, sv);
						}
						closedir(l2d);
						free(l2path);
					}
					closedir(l1d);
					free(l1path);
				}
				closedir(bd);
			}
			free(icons_base);

			/* Pixmaps fallback for this data dir */
			{
				const char *exts2[] = {"png", "svg", NULL};
				for (int e = 0; exts2[e]; e++) {
					char *p = NULL;
					if (asprintf(&p, "%s/" PIXMAPS_SUBDIR "/%s.%s",
							data_dirs[d], icon_name, exts2[e]) < 0)
						continue;
					if (access(p, F_OK) == 0) {
						int sv = (e == 1) ? 9999 : 0;
						APPEND_ICON(p, sv);
					} else {
						free(p);
					}
				}
			}
		}
		free_xdg_data_dirs(data_dirs);
	}
done_scan:;

	/* Include the original absolute path (if any) — it may not be in the
	 * theme tree (e.g. a custom path the user typed in) */
	if (absolute_seed) {
		char *p = strdup(absolute_seed);
		if (p)
			APPEND_ICON(p, -1); /* sort to end */
	}

#undef APPEND_ICON

	if (nfound == 0) {
		free(found);
		return NULL;
	}

	/* Sort: largest size first (SVGs in scalable dirs → 9999+, sized PNGs
	 * below) */
	for (int i = 0; i < nfound - 1; i++)
		for (int j = i + 1; j < nfound; j++)
			if (found[j].sz > found[i].sz) {
				IconEntry tmp = found[i];
				found[i] = found[j];
				found[j] = tmp;
			}

	char **result = malloc((size_t)(nfound + 1) * sizeof(char *));
	if (result) {
		for (int i = 0; i < nfound; i++)
			result[i] = found[i].path;
		result[nfound] = NULL;
	} else {
		for (int i = 0; i < nfound; i++)
			free(found[i].path);
	}
	free(found);
	return result;
}

/* =========================================================================
 * Main window state
 * ======================================================================= */
struct _CfgWin {
	Config cfg;

	GtkApplication *gapp;
	GtkWindow *window;

	/* Global */
	GtkSpinButton *icon_size_spin;
	GtkSpinButton *icon_spacing_spin;
	GtkSpinButton *exclusive_zone_spin;
	GtkSpinButton *border_space_spin;
	GtkDropDown *label_mode_drop;
	GtkColorDialog *color_dialog; /* unref'd in config_window_run */
	GtkColorDialogButton *label_color_btn;
	GtkSpinButton *label_size_spin;
	GtkSpinButton *label_offset_spin;
	GtkDropDown *position_drop;
	GtkDropDown *layer_drop;
	GtkEntry *output_entry;

	/* Widgets — reorderable rows */
	GtkBox *widgets_box; // container rebuilt by refresh_widgets_list()
	/* Per-widget controls (rebuilt each time; kept here for write_config) */
	GtkCheckButton *show_volume_check;
	GtkEntry *volume_exec_entry;
	GtkCheckButton *show_date_check;
	GtkCheckButton *show_net_check;
	GtkCheckButton *show_sysinfo_check;
	GtkEntry *date_date_format_entry;
	GtkColorDialogButton *date_date_color_btn;
	GtkSpinButton *date_date_size_spin;
	GtkEntry *date_time_format_entry;
	GtkColorDialogButton *date_time_color_btn;
	GtkSpinButton *date_time_size_spin;
	GtkColorDialogButton *date_bg_color_btn;
	GtkEntry *net_iface_entry;
	GtkColorDialogButton *net_rx_color_btn;
	GtkColorDialogButton *net_tx_color_btn;
	GtkSpinButton *net_size_spin;
	GtkColorDialogButton *net_bg_color_btn;
	GtkColorDialogButton *sysinfo_cpu_color_btn;
	GtkColorDialogButton *sysinfo_tmp_color_btn;
	GtkColorDialogButton *sysinfo_ram_color_btn;
	GtkSpinButton *sysinfo_size_spin;
	GtkColorDialogButton *sysinfo_bg_color_btn;
	GtkEntry *sysinfo_exec_entry;
	GtkCheckButton *sysinfo_percpu_check;
	GtkCheckButton *sysinfo_show_temp_check;
	GtkCheckButton *sysinfo_show_proc_check;

	/* Apps */
	GtkBox *apps_box;
	GtkLabel *status_label;
};

/* =========================================================================
 * mark_unsaved and change-detection helpers
 *
 * Placed here so CfgWin is complete when they are compiled.
 * ======================================================================= */

/* Clear the "saved" status label — called whenever the user makes a change. */
static void
mark_unsaved(CfgWin *w)
{
	if (w && w->status_label)
		gtk_label_set_text(GTK_LABEL(w->status_label), "");
}

/* Generic signal callbacks that forward to mark_unsaved */
static void
on_any_changed(GObject *obj, GParamSpec *ps, gpointer data)
{
	(void)obj;
	(void)ps;
	mark_unsaved((CfgWin *)data);
}
static void
on_any_text_changed(GtkEditable *e, gpointer data)
{
	(void)e;
	mark_unsaved((CfgWin *)data);
}
/* Used for "value-changed" (GtkSpinButton) and "toggled" (GtkCheckButton).
 * Both deliver (widget*, user_data) so a single void* first-arg works. */
static void
on_any_widget_changed(gpointer widget, gpointer data)
{
	(void)widget;
	mark_unsaved((CfgWin *)data);
}

/* Connect mark_unsaved to a widget depending on its type.
 * Returns the signal handler ID so the caller can disconnect before
 * the widget is destroyed (prevents callbacks firing during teardown). */
static gulong
connect_mark_unsaved(GtkWidget *widget, CfgWin *w)
{
	if (GTK_IS_SPIN_BUTTON(widget))
		return g_signal_connect(widget, "value-changed",
			G_CALLBACK(on_any_widget_changed), w);
	else if (GTK_IS_COLOR_DIALOG_BUTTON(widget))
		return g_signal_connect(widget, "notify::rgba",
			G_CALLBACK(on_any_changed), w);
	else if (GTK_IS_ENTRY(widget))
		return g_signal_connect(widget, "changed",
			G_CALLBACK(on_any_text_changed), w);
	else if (GTK_IS_DROP_DOWN(widget))
		return g_signal_connect(widget, "notify::selected",
			G_CALLBACK(on_any_changed), w);
	else if (GTK_IS_CHECK_BUTTON(widget))
		return g_signal_connect(widget, "toggled",
			G_CALLBACK(on_any_widget_changed), w);
	return 0;
}

/* Store a (GObject*, handler_id) pair on a frame widget so that
 * refresh_widgets_list() can disconnect the handler before destroying
 * the row, preventing callbacks from firing during widget teardown. */
static void
store_signal_pair(GtkWidget *frame, GObject *obj, gulong id)
{
	if (!id)
		return;
	GPtrArray *pairs = g_object_get_data(G_OBJECT(frame), "signal_pairs");
	if (!pairs) {
		pairs = g_ptr_array_new();
		g_object_set_data_full(G_OBJECT(frame), "signal_pairs", pairs,
			(GDestroyNotify)g_ptr_array_unref);
	}
	g_ptr_array_add(pairs, obj);
	g_ptr_array_add(pairs, (gpointer)(gintptr)id);
}
static gboolean
idle_refresh_widgets(gpointer data)
{
	refresh_widgets_list((CfgWin *)data);
	return G_SOURCE_REMOVE;
}

/* =========================================================================
 * Colour helpers  0xAARRGGBB <-> GdkRGBA
 *
 * File format (matching write_default_config / parse_config_file):
 *   - label-color      : #RRGGBB   (parser treats 6-digit as fully opaque)
 *   - widget colours   : #RRGGBBAA (parser: last byte = alpha, internal
 * 0xAARRGGBB)
 *
 * We write #RRGGBB when alpha==0xFF, #RRGGBBAA otherwise.
 * ======================================================================= */
static GdkRGBA
argb_to_rgba(unsigned int argb)
{
	return (GdkRGBA){
		.red = ((argb >> 16) & 0xFF) / 255.0,
		.green = ((argb >> 8) & 0xFF) / 255.0,
		.blue = ((argb) & 0xFF) / 255.0,
		.alpha = ((argb >> 24) & 0xFF) / 255.0,
	};
}

static unsigned int
rgba_to_argb(const GdkRGBA *c)
{
	/* Use lround to avoid float precision errors (e.g. 1.0*255 -> 254.999) */
	unsigned int a = (unsigned int)lround(c->alpha * 255.0) & 0xFF;
	unsigned int r = (unsigned int)lround(c->red * 255.0) & 0xFF;
	unsigned int g = (unsigned int)lround(c->green * 255.0) & 0xFF;
	unsigned int b = (unsigned int)lround(c->blue * 255.0) & 0xFF;
	return (a << 24) | (r << 16) | (g << 8) | b;
}

static GtkColorDialogButton *
make_color_btn(GtkColorDialog *dlg, unsigned int argb)
{
	GtkColorDialogButton *btn =
		GTK_COLOR_DIALOG_BUTTON(gtk_color_dialog_button_new(dlg));
	GdkRGBA c = argb_to_rgba(argb);
	gtk_color_dialog_button_set_rgba(btn, &c);
	return btn;
}

static unsigned int
read_color_btn(GtkColorDialogButton *btn)
{
	const GdkRGBA *c = gtk_color_dialog_button_get_rgba(btn);
	return c ? rgba_to_argb(c) : 0xFFFFFFFF;
}

/* Write a colour exactly as parse_config_file() expects:
 *   fully opaque (AA=FF) → #RRGGBB
 *   any other alpha      → #RRGGBBAA  (AA = alpha byte, last) */
static void
fprint_color(FILE *fp, const char *key, unsigned int argb)
{
	unsigned int r = (argb >> 16) & 0xFF;
	unsigned int g = (argb >> 8) & 0xFF;
	unsigned int b = (argb) & 0xFF;
	unsigned int a = (argb >> 24) & 0xFF;
	if (a == 0xFF)
		fprintf(fp, "%s=#%02X%02X%02X\n", key, r, g, b);
	else
		fprintf(fp, "%s=#%02X%02X%02X%02X\n", key, r, g, b, a);
}

/* =========================================================================
 * Generic widget helpers
 * ======================================================================= */
static GtkSpinButton *
make_spin(double min, double max, double val)
{
	GtkSpinButton *s =
		GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(min, max, 1.0));
	gtk_spin_button_set_value(s, val);
	gtk_spin_button_set_digits(s, 0);
	gtk_widget_set_hexpand(GTK_WIDGET(s), TRUE);
	return s;
}

static void
grid_row(GtkGrid *grid, int row, const char *lbl_text, GtkWidget *widget)
{
	GtkWidget *lbl = gtk_label_new(lbl_text);
	gtk_label_set_xalign(GTK_LABEL(lbl), 1.0);
	gtk_grid_attach(grid, lbl, 0, row, 1, 1);
	gtk_widget_set_hexpand(widget, TRUE);
	gtk_grid_attach(grid, widget, 1, row, 1, 1);
}

static GtkWidget *
section_label(const char *markup)
{
	GtkWidget *lbl = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(lbl), markup);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtk_widget_set_margin_top(lbl, 12);
	gtk_widget_set_margin_bottom(lbl, 4);
	return lbl;
}

static GtkDropDown *
make_dropdown(const char *const *items, int active)
{
	int n = 0;
	while (items[n])
		n++;
	GtkStringList *sl = gtk_string_list_new(NULL);
	for (int i = 0; i < n; i++)
		gtk_string_list_append(sl, items[i]);
	GtkDropDown *dd = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(sl), NULL));
	gtk_drop_down_set_selected(dd, (guint)(active >= 0 ? active : 0));
	gtk_widget_set_hexpand(GTK_WIDGET(dd), TRUE);
	return dd;
}

/* =========================================================================
 * Write config — matches write_default_config() format exactly
 * ======================================================================= */
static int
write_config(CfgWin *w)
{
	const char *home = getenv("HOME");
	if (!home)
		return -1;

	char path[512];
	snprintf(path, sizeof(path), "%s/" CONFIG_DIR "/" CONFIG_NAME, home);

	FILE *fp = fopen(path, "w");
	if (!fp) {
		perror("fopen");
		return -1;
	}

	Config *c = &w->cfg;

	/* Harvest widget-section values (rebuilding lost pointers) */
	harvest_widget_fields(w);

	/* Harvest global widget values */
	c->icon_size = (int)gtk_spin_button_get_value(w->icon_size_spin);
	c->icon_spacing = (int)gtk_spin_button_get_value(w->icon_spacing_spin);
	c->exclusive_zone = (int)gtk_spin_button_get_value(w->exclusive_zone_spin);
	c->border_space = (int)gtk_spin_button_get_value(w->border_space_spin);
	c->label_mode = (LabelMode)gtk_drop_down_get_selected(w->label_mode_drop);
	c->label_color = read_color_btn(w->label_color_btn);
	c->label_size = (int)gtk_spin_button_get_value(w->label_size_spin);
	c->label_offset = (int)gtk_spin_button_get_value(w->label_offset_spin);
	c->position = (Position)gtk_drop_down_get_selected(w->position_drop);
	c->layer = (Layer)gtk_drop_down_get_selected(w->layer_drop);
	if (w->output_entry) {
		free(c->output_name);
		const char *v = gtk_editable_get_text(GTK_EDITABLE(w->output_entry));
		c->output_name = (v && v[0]) ? strdup(v) : NULL;
	}
	c->show_volume = gtk_check_button_get_active(w->show_volume_check);
	if (w->volume_exec_entry) {
		free(c->volume_exec);
		const char *v =
			gtk_editable_get_text(GTK_EDITABLE(w->volume_exec_entry));
		c->volume_exec = (v && v[0]) ? strdup(v) : NULL;
	}
	c->show_date = gtk_check_button_get_active(w->show_date_check);
	c->show_net = gtk_check_button_get_active(w->show_net_check);
	if (w->show_sysinfo_check)
		c->show_sysinfo = gtk_check_button_get_active(w->show_sysinfo_check);

	/* Net widget fields */
	const char *niface =
		gtk_editable_get_text(GTK_EDITABLE(w->net_iface_entry));
	free(c->net_iface);
	c->net_iface = (niface && niface[0]) ? strdup(niface) : NULL;
	c->net_rx_color = read_color_btn(w->net_rx_color_btn);
	c->net_tx_color = read_color_btn(w->net_tx_color_btn);
	c->net_font_size = (int)gtk_spin_button_get_value(w->net_size_spin);
	c->net_bg_color = read_color_btn(w->net_bg_color_btn);

	/* Sysinfo widget fields */
	if (w->sysinfo_cpu_color_btn)
		c->sysinfo_cpu_color = read_color_btn(w->sysinfo_cpu_color_btn);
	if (w->sysinfo_tmp_color_btn)
		c->sysinfo_tmp_color = read_color_btn(w->sysinfo_tmp_color_btn);
	if (w->sysinfo_ram_color_btn)
		c->sysinfo_ram_color = read_color_btn(w->sysinfo_ram_color_btn);
	if (w->sysinfo_size_spin)
		c->sysinfo_font_size =
			(int)gtk_spin_button_get_value(w->sysinfo_size_spin);
	if (w->sysinfo_bg_color_btn)
		c->sysinfo_bg_color = read_color_btn(w->sysinfo_bg_color_btn);
	if (w->sysinfo_exec_entry) {
		free(c->sysinfo_exec);
		const char *v =
			gtk_editable_get_text(GTK_EDITABLE(w->sysinfo_exec_entry));
		c->sysinfo_exec = (v && v[0]) ? strdup(v) : NULL;
	}
	if (w->sysinfo_percpu_check)
		c->sysinfo_percpu =
			gtk_check_button_get_active(w->sysinfo_percpu_check);
	if (w->sysinfo_show_temp_check)
		c->sysinfo_show_temp =
			gtk_check_button_get_active(w->sysinfo_show_temp_check);
	if (w->sysinfo_show_proc_check)
		c->sysinfo_show_proc =
			gtk_check_button_get_active(w->sysinfo_show_proc_check);

	char *ddf =
		strdup(gtk_editable_get_text(GTK_EDITABLE(w->date_date_format_entry)));
	free(c->date_date_format);
	c->date_date_format = ddf;
	c->date_date_color = read_color_btn(w->date_date_color_btn);
	c->date_date_size = (int)gtk_spin_button_get_value(w->date_date_size_spin);

	char *dtf =
		strdup(gtk_editable_get_text(GTK_EDITABLE(w->date_time_format_entry)));
	free(c->date_time_format);
	c->date_time_format = dtf;
	c->date_time_color = read_color_btn(w->date_time_color_btn);
	c->date_time_size = (int)gtk_spin_button_get_value(w->date_time_size_spin);
	c->date_bg_color = read_color_btn(w->date_bg_color_btn);

	static const char *lm_str[] = {"always", "hover", "never"};
	static const char *pos_str[] = {"bottom", "top", "left", "right"};
	static const char *lay_str[] = {"background", "bottom", "top", "overlay"};

	fprintf(fp, "[global]\n");
	fprintf(fp, "icon-size=%d\n", c->icon_size);
	fprintf(fp, "# icon-spacing: spacing between icons in pixels\n");
	fprintf(fp, "icon-spacing=%d\n", c->icon_spacing);
	fprintf(fp, "# exclusive-zone: interaction with other surfaces\n");
	fprintf(fp, "#    0: surface will be moved to avoid occluding\n");
	fprintf(fp, "#       surfaces with positive exclusive zone\n");
	fprintf(fp, "#   >0: surface reserves space (e.g., panel=10 prevents\n");
	fprintf(fp, "#       maximized windows from overlapping)\n");
	fprintf(fp, "#   -1: surface stretches to edges, ignoring other\n");
	fprintf(fp, "#       surfaces (e.g., wallpaper, lock screen)\n");
	fprintf(fp, "exclusive-zone=%d\n", c->exclusive_zone);
	fprintf(fp, "# border-space: pixels between the bar and the screen edge\n");
	fprintf(fp, "border-space=%d\n", c->border_space);
	fprintf(fp, "# label-mode: always | hover | never\n");
	fprintf(fp, "label-mode=%s\n",
		(unsigned)c->label_mode < 3 ? lm_str[c->label_mode] : "hover");
	fprintf(fp, "# label-color: hex color #RRGGBB or #RRGGBBAA\n");
	fprint_color(fp, "label-color", c->label_color);
	fprintf(fp, "# label-size: font size in points for the app-name label\n");
	fprintf(fp, "label-size=%d\n", c->label_size);
	fprintf(fp,
		"# label_offset: pixels from the bottom edge of the icon"
		" to the text baseline\n");
	fprintf(fp,
		"#   0 = bottom edge (descenders clipped),"
		" icon-size = top edge (text invisible)\n");
	fprintf(fp, "#   recommended range: 4-16\n");
	fprintf(fp, "label-offset=%d\n", c->label_offset);
	fprintf(fp, "# position: where to place the bar on screen\n");
	fprintf(fp, "#   bottom (default): horizontal bar at the bottom\n");
	fprintf(fp, "#   top:              horizontal bar at the top\n");
	fprintf(fp, "#   left:             vertical bar on the left\n");
	fprintf(fp, "#   right:            vertical bar on the right\n");
	fprintf(fp, "position=%s\n",
		(unsigned)c->position < 4 ? pos_str[c->position] : "bottom");
	fprintf(fp, "# layer: layer-shell layer for the surface\n");
	fprintf(fp, "#   background:       beneath everything (for wallpapers)\n");
	fprintf(fp, "#   bottom:           below normal windows (for docks)\n");
	fprintf(fp, "#   top (default):    above normal windows\n");
	fprintf(fp, "#   overlay:          on top of everything\n");
	fprintf(fp, "layer=%s\n",
		(unsigned)c->layer < 4 ? lay_str[c->layer] : "top");
	if (w->output_entry) {
		free(c->output_name);
		const char *v = gtk_editable_get_text(GTK_EDITABLE(w->output_entry));
		c->output_name = (v && v[0]) ? strdup(v) : NULL;
	}
	fprintf(fp, "# output: output name to place the bar on (empty = auto)\n");
	fprintf(fp, "# output=eDP-1\n");
	fprintf(fp, "output=%s\n", c->output_name ? c->output_name : "");
	fprintf(fp, "# show-sysinfo: show the CPU/RAM usage widget\n");
	fprintf(fp, "show-sysinfo=%s\n", c->show_sysinfo ? "true" : "false");
	fprintf(fp, "# show-net: show the network speed widget\n");
	fprintf(fp, "show-net=%s\n", c->show_net ? "true" : "false");
	fprintf(fp, "# show-volume: show the volume widget (after apps)\n");
	fprintf(fp, "show-volume=%s\n", c->show_volume ? "true" : "false");
	fprintf(fp, "# show-date: show the date/time widget (last slot)\n");
	fprintf(fp, "show-date=%s\n", c->show_date ? "true" : "false");

	/* Walk widget_order[5] in order, emitting each section.
	 * WIDGET_ID_APPS (2) triggers the [apps] block. */
	static const char *wnames[5] = {"sysinfo", "net", "apps", "volume", "date"};

#define EMIT_WIDGET_SECTION(wid)                                                 \
	do {                                                                         \
		if ((wid) == 0) {                                                        \
			fprintf(fp, "\n[widget-sysinfo]\n");                                 \
			fprintf(fp,                                                          \
				"# percpu: true = per-core %% like top, false = system-wide\n"); \
			fprintf(fp, "percpu=%s\n", c->sysinfo_percpu ? "true" : "false");    \
			fprintf(fp, "# show-temp: show the CPU temperature line\n");         \
			fprintf(fp, "show-temp=%s\n",                                        \
				c->sysinfo_show_temp ? "true" : "false");                        \
			fprintf(fp, "# show-proc: show process name sub-lines\n");           \
			fprintf(fp, "show-proc=%s\n",                                        \
				c->sysinfo_show_proc ? "true" : "false");                        \
			fprintf(fp, "# cpu-color: color for the CPU usage line\n");          \
			fprint_color(fp, "cpu-color",                                        \
				c->sysinfo_cpu_color ? c->sysinfo_cpu_color : 0xFFFFEB3B);       \
			fprintf(fp, "# tmp-color: color for the CPU temperature line\n");    \
			fprint_color(fp, "tmp-color",                                        \
				c->sysinfo_tmp_color ? c->sysinfo_tmp_color : 0xFFFF7043);       \
			fprintf(fp, "# ram-color: color for the RAM usage line\n");          \
			fprint_color(fp, "ram-color",                                        \
				c->sysinfo_ram_color ? c->sysinfo_ram_color : 0xFF66BB6A);       \
			fprintf(fp, "# size: font size in pt\n");                            \
			fprintf(fp, "size=%d\n",                                             \
				c->sysinfo_font_size > 0 ? c->sysinfo_font_size : 14);           \
			fprintf(fp, "# bg-color: tile background color (#RRGGBBAA)\n");      \
			fprintf(fp, "#   #00000094 = black 42%% transparent (default)\n");   \
			fprintf(fp, "#   #00000000 = fully transparent\n");                  \
			fprint_color(fp, "bg-color",                                         \
				c->sysinfo_bg_color ? c->sysinfo_bg_color : 0x94000000);         \
			fprintf(fp,                                                          \
				"# exec: command to run on left-click"                           \
				" (empty to disable)\n");                                        \
			fprintf(fp, "exec=%s\n", c->sysinfo_exec ? c->sysinfo_exec : "");    \
		} else if ((wid) == 1) {                                                 \
			fprintf(fp, "\n[widget-net]\n");                                     \
			fprintf(fp,                                                          \
				"# iface: network interface to monitor"                          \
				" (omit for auto-detect)\n");                                    \
			if (c->net_iface && c->net_iface[0])                                 \
				fprintf(fp, "iface=%s\n", c->net_iface);                         \
			else                                                                 \
				fprintf(fp, "# iface=eth0\n");                                   \
			fprintf(fp,                                                          \
				"# rx-color: color for the receive (down) speed line\n");        \
			fprint_color(fp, "rx-color",                                         \
				c->net_rx_color ? c->net_rx_color : 0xFFFF3FFA);                 \
			fprintf(fp,                                                          \
				"# tx-color: color for the transmit (up) speed line\n");         \
			fprint_color(fp, "tx-color",                                         \
				c->net_tx_color ? c->net_tx_color : 0xFF3AFFFD);                 \
			fprintf(fp, "# size: font size in pt for the speed lines\n");        \
			fprintf(fp, "size=%d\n",                                             \
				c->net_font_size > 0 ? c->net_font_size : 14);                   \
			fprintf(fp, "# bg-color: tile background color (#RRGGBBAA)\n");      \
			fprintf(fp, "#   #00000094 = black 42%% transparent (default)\n");   \
			fprintf(fp, "#   #00000000 = fully transparent\n");                  \
			fprint_color(fp, "bg-color",                                         \
				c->net_bg_color ? c->net_bg_color : 0x94000000);                 \
		} else if ((wid) == 2) {                                                 \
			fprintf(fp, "\n[apps]\n");                                           \
			GtkWidget *_ch =                                                     \
				gtk_widget_get_first_child(GTK_WIDGET(w->apps_box));             \
			while (_ch) {                                                        \
				GtkWidget *_ne =                                                 \
					g_object_get_data(G_OBJECT(_ch), "name_entry");              \
				GtkWidget *_ie =                                                 \
					g_object_get_data(G_OBJECT(_ch), "icon_entry");              \
				GtkWidget *_ee =                                                 \
					g_object_get_data(G_OBJECT(_ch), "exec_entry");              \
				GtkWidget *_tcb =                                                \
					g_object_get_data(G_OBJECT(_ch), "terminal_check");          \
				if (_ne && _ie && _ee) {                                         \
					const char *_nm =                                            \
						gtk_editable_get_text(GTK_EDITABLE(_ne));                \
					const char *_ic =                                            \
						gtk_editable_get_text(GTK_EDITABLE(_ie));                \
					const char *_ex =                                            \
						gtk_editable_get_text(GTK_EDITABLE(_ee));                \
					int _tr = _tcb ?                                             \
						gtk_check_button_get_active(GTK_CHECK_BUTTON(_tcb)) :    \
						0;                                                       \
					if (_nm && _nm[0] && _ic && _ic[0] && _ex && _ex[0]) {       \
						fprintf(fp, "name=%s\n", _nm);                           \
						fprintf(fp, "icon=%s\n", _ic);                           \
						if (_tr)                                                 \
							fprintf(fp, "terminal=true\n");                      \
						fprintf(fp, "exec=%s\n\n", _ex);                         \
					}                                                            \
				}                                                                \
				_ch = gtk_widget_get_next_sibling(_ch);                          \
			}                                                                    \
		} else if ((wid) == 3) {                                                 \
			fprintf(fp, "\n[widget-volume]\n");                                  \
			fprintf(fp,                                                          \
				"# exec: command to run on right-click"                          \
				" (empty to disable)\n");                                        \
			fprintf(fp, "exec=%s\n", c->volume_exec ? c->volume_exec : "");      \
		} else if ((wid) == 4) {                                                 \
			fprintf(fp, "\n[widget-date]\n");                                    \
			fprintf(fp, "# Date line (upper half of the tile)\n");               \
			fprintf(fp,                                                          \
				"# format: strftime(3) format string,"                           \
				" e.g. \"%%a %%d %%B\"\n");                                      \
			fprintf(fp, "format=%s\n",                                           \
				c->date_date_format && c->date_date_format[0] ?                  \
					c->date_date_format :                                        \
					"%a %d %B");                                                 \
			fprint_color(fp, "color",                                            \
				c->date_date_color ? c->date_date_color : 0xFF68FF3A);           \
			fprintf(fp, "size=%d\n",                                             \
				c->date_date_size > 0 ? c->date_date_size : 16);                 \
			fprintf(fp, "# Time line (lower half of the tile)\n");               \
			fprintf(fp,                                                          \
				"# time-format: strftime(3) format string,"                      \
				" e.g. \"%%H:%%M\"\n");                                          \
			fprintf(fp, "time-format=%s\n",                                      \
				c->date_time_format && c->date_time_format[0] ?                  \
					c->date_time_format :                                        \
					"%H:%M");                                                    \
			fprint_color(fp, "time-color",                                       \
				c->date_time_color ? c->date_time_color : 0xFFFF0000);           \
			fprintf(fp, "time-size=%d\n",                                        \
				c->date_time_size > 0 ? c->date_time_size : 36);                 \
			fprintf(fp, "# bg-color: tile background color (#RRGGBBAA)\n");      \
			fprintf(fp, "#   #00000094 = black 42%% transparent (default)\n");   \
			fprintf(fp, "#   #00000000 = fully transparent\n");                  \
			fprint_color(fp, "bg-color",                                         \
				c->date_bg_color ? c->date_bg_color : 0x94000000);               \
		}                                                                        \
	} while (0)

	for (int _i = 0; _i < 5; _i++)
		EMIT_WIDGET_SECTION(c->widget_order[_i]);

#undef EMIT_WIDGET_SECTION
	(void)wnames;

	fclose(fp);
	return 0;
}

/* =========================================================================
 * "Add application" dialog
 *
 * Uses GtkStringList as the filterable model — no custom GObject needed,
 * which eliminates all GType-registration race conditions and segfaults.
 * A parallel GPtrArray holds the DesktopEntry* pointers at the same indices.
 * The factory bind callback uses gtk_list_item_get_position() to index into
 * the visible-indices array to retrieve the right DesktopEntry.
 * ======================================================================= */

typedef struct {
	CfgWin *win;
	GtkWindow *dialog;

	/* All desktop entries (owned via all_apps / all_count) */
	DesktopEntry **all_apps;
	int all_count;

	/* Parallel array of DesktopEntry* — borrowed from all_apps */
	GPtrArray *entries;

	/* Visible subset: indices into entries that match the search query */
	GArray *visible; /* element type: guint */

	/* GtkStringList used as the list-view model (names of visible entries) */
	GtkStringList *str_model;
	GtkSingleSelection *selection; /* wraps str_model; owned by list_view */

	GtkEntry *search_entry;
	GtkLabel *count_label;
} AddAppCtx;

/* Rebuild visible[] and str_model from the current search text */
static void
rebuild_filter(AddAppCtx *ctx)
{
	const char *q = gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry));
	gboolean any = (!q || q[0] == '\0');

	g_array_set_size(ctx->visible, 0);
	for (guint i = 0; i < ctx->entries->len; i++) {
		DesktopEntry *de = g_ptr_array_index(ctx->entries, i);
		if (any || (de->name && strcasestr(de->name, q)) ||
			(de->exec && strcasestr(de->exec, q)))
			g_array_append_val(ctx->visible, i);
	}

	/* Replace the string list in one splice call */
	guint old_n = g_list_model_get_n_items(G_LIST_MODEL(ctx->str_model));
	const char **names = calloc(ctx->visible->len + 1, sizeof(char *));
	for (guint i = 0; i < ctx->visible->len; i++) {
		guint idx = g_array_index(ctx->visible, guint, i);
		DesktopEntry *de = g_ptr_array_index(ctx->entries, idx);
		names[i] = de->name ? de->name : "";
	}
	gtk_string_list_splice(ctx->str_model, 0, old_n, names);
	free(names);

	char buf[64];
	snprintf(buf, sizeof(buf), "%u application%s", ctx->visible->len,
		ctx->visible->len == 1 ? "" : "s");
	gtk_label_set_text(ctx->count_label, buf);
}

static void
on_app_search_changed(GtkEditable *e, gpointer data)
{
	(void)e;
	rebuild_filter((AddAppCtx *)data);
}

/* Factory: setup — create the row widget skeleton (no data yet) */
static void
app_factory_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
	(void)f;
	(void)d;
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(row, 6);
	gtk_widget_set_margin_end(row, 6);
	gtk_widget_set_margin_top(row, 4);
	gtk_widget_set_margin_bottom(row, 4);

	GtkWidget *img = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
	gtk_widget_set_size_request(img, 36, 36);
	gtk_box_append(GTK_BOX(row), img);

	GtkWidget *lbl = gtk_label_new("");
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(lbl, TRUE);
	gtk_box_append(GTK_BOX(row), lbl);

	gtk_list_item_set_child(li, row);
}

/* Factory: bind — fill row with data for position pos.
 *
 * The model is GtkStringList so gtk_list_item_get_item() returns a
 * GtkStringObject.  We use the position to look up the DesktopEntry
 * directly — no custom GObject, no G_TYPE_CHECK, no crashes.
 */
static void
app_factory_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data)
{
	(void)f;
	AddAppCtx *ctx = (AddAppCtx *)data;

	GtkWidget *row = gtk_list_item_get_child(li);
	if (!row)
		return;

	GtkWidget *img = gtk_widget_get_first_child(row);
	GtkWidget *lbl = img ? gtk_widget_get_next_sibling(img) : NULL;
	if (!img || !lbl)
		return;

	guint pos = gtk_list_item_get_position(li);
	if (pos >= ctx->visible->len)
		return;

	guint idx = g_array_index(ctx->visible, guint, pos);
	DesktopEntry *de = g_ptr_array_index(ctx->entries, idx);

	gtk_label_set_text(GTK_LABEL(lbl), de->name ? de->name : "");

	/* gtk_image_set_from_icon_name / _from_file always produce a visible
	 * result (broken-image placeholder on miss) unlike the lookup APIs
	 * which silently return NULL and leave the GtkImage blank. */
	if (de->icon && de->icon[0]) {
		if (de->icon[0] == '/') {
			gtk_image_set_from_file(GTK_IMAGE(img), de->icon);
		} else {
			gtk_image_set_from_icon_name(GTK_IMAGE(img), de->icon);
		}
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(img),
			"application-x-executable");
	}
}

/* Append the currently selected entry to cfg.apps and close the dialog */
static void
commit_selected(AddAppCtx *ctx)
{
	guint pos = gtk_single_selection_get_selected(ctx->selection);
	if (pos == GTK_INVALID_LIST_POSITION || pos >= ctx->visible->len)
		return;

	guint idx = g_array_index(ctx->visible, guint, pos);
	DesktopEntry *de = g_ptr_array_index(ctx->entries, idx);

	CfgWin *w = ctx->win;
	DesktopEntry **tmp = realloc(w->cfg.apps,
		(size_t)(w->cfg.count + 1) * sizeof(DesktopEntry *));
	if (!tmp)
		return;
	w->cfg.apps = tmp;

	DesktopEntry *entry = calloc(1, sizeof(DesktopEntry));
	entry->name = strdup(de->name ? de->name : "");
	entry->exec = strdup(de->exec ? de->exec : "");
	entry->terminal = de->terminal;
	/* Resolve to absolute icon path (matches write_default_config) */
	entry->icon = local_find_best_icon(de->icon);
	if (!entry->icon && de->icon && de->icon[0])
		entry->icon = strdup(de->icon);

	w->cfg.apps[w->cfg.count++] = entry;
	mark_unsaved(w);
	refresh_apps_list(w);
	gtk_window_destroy(ctx->dialog);
	/* ctx freed by on_add_dialog_destroy — do not touch after this point */
}

static void
on_add_btn_clicked(GtkButton *btn, gpointer data)
{
	(void)btn;
	commit_selected((AddAppCtx *)data);
}

static void
on_list_activate(GtkListView *lv, guint position, gpointer data)
{
	(void)lv;
	(void)position;
	commit_selected((AddAppCtx *)data);
}

/* Dialog destroy — free everything we own */
static void
on_add_dialog_destroy(GtkWidget *widget, gpointer data)
{
	(void)widget;
	AddAppCtx *ctx = (AddAppCtx *)data;
	/*
	 * selection is owned by list_view (widget tree) → do NOT unref.
	 * str_model is owned by selection               → do NOT unref.
	 * entries holds borrowed ptrs                   → free the GPtrArray only.
	 * all_apps owns the DesktopEntry allocations    → free via
	 * free_applications.
	 */
	g_ptr_array_free(ctx->entries, TRUE);
	g_array_free(ctx->visible, TRUE);
	free_applications(ctx->all_apps, ctx->all_count);
	free(ctx);
}

static void
open_add_app_dialog(CfgWin *w)
{
	AddAppCtx *ctx = calloc(1, sizeof(AddAppCtx));
	ctx->win = w;

	ctx->all_apps = list_all_applications(&ctx->all_count);

	ctx->entries = g_ptr_array_sized_new((guint)ctx->all_count);
	for (int i = 0; i < ctx->all_count; i++)
		g_ptr_array_add(ctx->entries, ctx->all_apps[i]);

	/* All entries visible initially */
	ctx->visible = g_array_new(FALSE, FALSE, sizeof(guint));
	for (guint i = 0; i < ctx->entries->len; i++)
		g_array_append_val(ctx->visible, i);

	/* Populate string model with all names */
	ctx->str_model = gtk_string_list_new(NULL);
	{
		const char **names = calloc(ctx->entries->len + 1, sizeof(char *));
		for (guint i = 0; i < ctx->entries->len; i++) {
			DesktopEntry *de = g_ptr_array_index(ctx->entries, i);
			names[i] = de->name ? de->name : "";
		}
		gtk_string_list_splice(ctx->str_model, 0, 0, names);
		free(names);
	}

	/* selection wraps str_model; list_view will take ownership of selection */
	ctx->selection = gtk_single_selection_new(G_LIST_MODEL(ctx->str_model));
	gtk_single_selection_set_autoselect(ctx->selection, TRUE);

	/* Dialog */
	ctx->dialog = GTK_WINDOW(gtk_window_new());
	gtk_window_set_title(ctx->dialog, "Add Application");
	gtk_window_set_default_size(ctx->dialog, 520, 500);
	gtk_window_set_transient_for(ctx->dialog, w->window);
	gtk_window_set_modal(ctx->dialog, TRUE);
	g_signal_connect(ctx->dialog, "destroy", G_CALLBACK(on_add_dialog_destroy),
		ctx);

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(ctx->dialog, outer);

	/* Search bar */
	GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(sb, 10);
	gtk_widget_set_margin_end(sb, 10);
	gtk_widget_set_margin_top(sb, 8);
	gtk_widget_set_margin_bottom(sb, 4);
	gtk_box_append(GTK_BOX(outer), sb);

	gtk_box_append(GTK_BOX(sb), gtk_label_new("Search:"));
	ctx->search_entry = GTK_ENTRY(gtk_entry_new());
	gtk_widget_set_hexpand(GTK_WIDGET(ctx->search_entry), TRUE);
	gtk_entry_set_placeholder_text(ctx->search_entry,
		"Filter by name or command\xe2\x80\xa6");
	gtk_box_append(GTK_BOX(sb), GTK_WIDGET(ctx->search_entry));
	g_signal_connect(ctx->search_entry, "changed",
		G_CALLBACK(on_app_search_changed), ctx);

	char cbuf[64];
	snprintf(cbuf, sizeof(cbuf), "%u applications", ctx->entries->len);
	ctx->count_label = GTK_LABEL(gtk_label_new(cbuf));
	gtk_widget_add_css_class(GTK_WIDGET(ctx->count_label), "dim-label");
	gtk_box_append(GTK_BOX(sb), GTK_WIDGET(ctx->count_label));

	/* List view */
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(app_factory_setup), NULL);
	g_signal_connect(factory, "bind", G_CALLBACK(app_factory_bind), ctx);

	/* list_view takes ownership of selection */
	GtkWidget *list_view =
		gtk_list_view_new(GTK_SELECTION_MODEL(ctx->selection), factory);
	gtk_list_view_set_show_separators(GTK_LIST_VIEW(list_view), TRUE);
	g_signal_connect(list_view, "activate", G_CALLBACK(on_list_activate), ctx);

	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list_view);
	gtk_box_append(GTK_BOX(outer), scroll);

	gtk_box_append(GTK_BOX(outer),
		gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

	/* Buttons */
	GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(btn_row, 10);
	gtk_widget_set_margin_end(btn_row, 10);
	gtk_widget_set_margin_top(btn_row, 6);
	gtk_widget_set_margin_bottom(btn_row, 8);
	gtk_box_append(GTK_BOX(outer), btn_row);

	GtkWidget *hint = gtk_label_new("Double-click or press Add to insert");
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_widget_set_hexpand(hint, TRUE);
	gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
	gtk_box_append(GTK_BOX(btn_row), hint);

	GtkWidget *cancel = gtk_button_new_with_label("Cancel");
	gtk_box_append(GTK_BOX(btn_row), cancel);
	g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy),
		ctx->dialog);

	GtkWidget *add_btn = gtk_button_new_with_label("\xe2\x9e\x95  Add");
	gtk_widget_add_css_class(add_btn, "suggested-action");
	gtk_box_append(GTK_BOX(btn_row), add_btn);
	g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_btn_clicked), ctx);

	gtk_window_present(ctx->dialog);
}

static void
on_add_app_clicked(GtkButton *btn, gpointer data)
{
	(void)btn;
	open_add_app_dialog((CfgWin *)data);
}

static void refresh_widgets_list(CfgWin *w);

/* Harvest current widget-field values from the live GTK widgets into cfg,
 * so that refresh_widgets_list() (which rebuilds them) picks up the right
 * defaults on the next build.  Called before any reorder. */
static void
harvest_widget_fields(CfgWin *w)
{
	if (w->show_volume_check)
		w->cfg.show_volume = gtk_check_button_get_active(w->show_volume_check);
	if (w->volume_exec_entry) {
		free(w->cfg.volume_exec);
		const char *v =
			gtk_editable_get_text(GTK_EDITABLE(w->volume_exec_entry));
		w->cfg.volume_exec = (v && v[0]) ? strdup(v) : NULL;
	}
	if (w->show_date_check)
		w->cfg.show_date = gtk_check_button_get_active(w->show_date_check);
	if (w->show_net_check)
		w->cfg.show_net = gtk_check_button_get_active(w->show_net_check);

	if (w->date_date_format_entry) {
		free(w->cfg.date_date_format);
		w->cfg.date_date_format = strdup(
			gtk_editable_get_text(GTK_EDITABLE(w->date_date_format_entry)));
	}
	if (w->date_date_color_btn)
		w->cfg.date_date_color = read_color_btn(w->date_date_color_btn);
	if (w->date_date_size_spin)
		w->cfg.date_date_size =
			(int)gtk_spin_button_get_value(w->date_date_size_spin);
	if (w->date_time_format_entry) {
		free(w->cfg.date_time_format);
		w->cfg.date_time_format = strdup(
			gtk_editable_get_text(GTK_EDITABLE(w->date_time_format_entry)));
	}
	if (w->date_time_color_btn)
		w->cfg.date_time_color = read_color_btn(w->date_time_color_btn);
	if (w->date_time_size_spin)
		w->cfg.date_time_size =
			(int)gtk_spin_button_get_value(w->date_time_size_spin);
	if (w->date_bg_color_btn)
		w->cfg.date_bg_color = read_color_btn(w->date_bg_color_btn);

	if (w->net_iface_entry) {
		free(w->cfg.net_iface);
		const char *v = gtk_editable_get_text(GTK_EDITABLE(w->net_iface_entry));
		w->cfg.net_iface = (v && v[0]) ? strdup(v) : NULL;
	}
	if (w->net_rx_color_btn)
		w->cfg.net_rx_color = read_color_btn(w->net_rx_color_btn);
	if (w->net_tx_color_btn)
		w->cfg.net_tx_color = read_color_btn(w->net_tx_color_btn);
	if (w->net_size_spin)
		w->cfg.net_font_size = (int)gtk_spin_button_get_value(w->net_size_spin);
	if (w->net_bg_color_btn)
		w->cfg.net_bg_color = read_color_btn(w->net_bg_color_btn);
	if (w->show_sysinfo_check)
		w->cfg.show_sysinfo =
			gtk_check_button_get_active(w->show_sysinfo_check);
	if (w->sysinfo_cpu_color_btn)
		w->cfg.sysinfo_cpu_color = read_color_btn(w->sysinfo_cpu_color_btn);
	if (w->sysinfo_ram_color_btn)
		w->cfg.sysinfo_ram_color = read_color_btn(w->sysinfo_ram_color_btn);
	if (w->sysinfo_size_spin)
		w->cfg.sysinfo_font_size =
			(int)gtk_spin_button_get_value(w->sysinfo_size_spin);
	if (w->sysinfo_bg_color_btn)
		w->cfg.sysinfo_bg_color = read_color_btn(w->sysinfo_bg_color_btn);
	if (w->sysinfo_exec_entry) {
		free(w->cfg.sysinfo_exec);
		const char *v =
			gtk_editable_get_text(GTK_EDITABLE(w->sysinfo_exec_entry));
		w->cfg.sysinfo_exec = (v && v[0]) ? strdup(v) : NULL;
	}
	if (w->sysinfo_percpu_check)
		w->cfg.sysinfo_percpu =
			gtk_check_button_get_active(w->sysinfo_percpu_check);
}

static void
on_widget_move_up(GtkButton *btn, gpointer data)
{
	(void)btn;
	CfgWin *w = (CfgWin *)data;
	int pos = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "widget_pos"));
	if (pos <= 0)
		return;
	harvest_widget_fields(w);
	int tmp = w->cfg.widget_order[pos - 1];
	w->cfg.widget_order[pos - 1] = w->cfg.widget_order[pos];
	w->cfg.widget_order[pos] = tmp;
	mark_unsaved(w);
	g_idle_add(idle_refresh_widgets, w);
}

static void
on_widget_move_down(GtkButton *btn, gpointer data)
{
	(void)btn;
	CfgWin *w = (CfgWin *)data;
	int pos = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "widget_pos"));
	if (pos >= 4)
		return;
	harvest_widget_fields(w);
	int tmp = w->cfg.widget_order[pos + 1];
	w->cfg.widget_order[pos + 1] = w->cfg.widget_order[pos];
	w->cfg.widget_order[pos] = tmp;
	mark_unsaved(w);
	g_idle_add(idle_refresh_widgets, w);
}

static void
on_widget_show_toggled(GtkCheckButton *cb, gpointer data)
{
	/* Settings box is stored as "settings_box" on the checkbox */
	GtkWidget *box =
		GTK_WIDGET(g_object_get_data(G_OBJECT(cb), "settings_box"));
	if (box)
		gtk_widget_set_visible(box, gtk_check_button_get_active(cb));
	CfgWin *w = (CfgWin *)data;
	mark_unsaved(w);
}

/* Build one reorderable widget row for position pos in widget_order. */
static GtkWidget *
make_widget_row(CfgWin *w, int pos)
{
	int wid = w->cfg.widget_order[pos];
	int total = 5;

	/* Outer frame */
	GtkWidget *frame = gtk_frame_new(NULL);
	gtk_widget_set_margin_start(frame, 4);
	gtk_widget_set_margin_end(frame, 4);
	gtk_widget_set_margin_top(frame, 3);
	gtk_widget_set_margin_bottom(frame, 3);
	/* Tag the frame with the widget ID+1 so refresh_widgets_list can find it.
	 * We store wid+1 because GINT_TO_POINTER(0)==NULL which g_object_get_data
	 * returns for unset keys, making wid=0 indistinguishable from "not set". */
	g_object_set_data(G_OBJECT(frame), "widget_id", GINT_TO_POINTER(wid + 1));

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start(vbox, 8);
	gtk_widget_set_margin_end(vbox, 8);
	gtk_widget_set_margin_top(vbox, 6);
	gtk_widget_set_margin_bottom(vbox, 6);
	gtk_frame_set_child(GTK_FRAME(frame), vbox);

	/* Header row: [checkbox] [spacer] [▲ Up] [▼ Down] */
	GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(vbox), hdr);

	const char *label_text = NULL;
	int show_val = 0;
	switch (wid) {
	case 0:
		label_text = "CPU/RAM usage";
		show_val = w->cfg.show_sysinfo;
		break;
	case 1:
		label_text = "Network speed widget";
		show_val = w->cfg.show_net;
		break;
	case 2:
		label_text = "App icons";
		show_val = 1;
		break;
	case 3:
		label_text = "Volume widget";
		show_val = w->cfg.show_volume;
		break;
	case 4:
		label_text = "Date/time widget";
		show_val = w->cfg.show_date;
		break;
	}

	GtkCheckButton *cb =
		GTK_CHECK_BUTTON(gtk_check_button_new_with_label(label_text));
	gtk_check_button_set_active(cb, show_val);
	gtk_widget_set_hexpand(GTK_WIDGET(cb), TRUE);
	gtk_box_append(GTK_BOX(hdr), GTK_WIDGET(cb));

	GtkWidget *up = gtk_button_new_with_label("\xe2\x96\xb2 Up");
	gtk_widget_set_sensitive(up, pos > 0);
	g_object_set_data(G_OBJECT(up), "widget_pos", GINT_TO_POINTER(pos));
	g_signal_connect(up, "clicked", G_CALLBACK(on_widget_move_up), w);
	gtk_box_append(GTK_BOX(hdr), up);

	GtkWidget *dn = gtk_button_new_with_label("\xe2\x96\xbc Down");
	gtk_widget_set_sensitive(dn, pos < total - 1);
	g_object_set_data(G_OBJECT(dn), "widget_pos", GINT_TO_POINTER(pos));
	g_signal_connect(dn, "clicked", G_CALLBACK(on_widget_move_down), w);
	gtk_box_append(GTK_BOX(hdr), dn);

	/* Store button pointers on frame so refresh_widgets_list can update
	 * their sensitivity without rebuilding the rows */
	g_object_set_data(G_OBJECT(frame), "up_btn", up);
	g_object_set_data(G_OBJECT(frame), "dn_btn", dn);

	/* Settings revealer */
	GtkRevealer *rev = GTK_REVEALER(gtk_revealer_new());
	gtk_revealer_set_transition_type(rev,
		GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration(rev, 200);
	gtk_revealer_set_reveal_child(rev, show_val);
	gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(rev));

	g_object_set_data(G_OBJECT(cb), "revealer", rev);
	store_signal_pair(frame, G_OBJECT(cb),
		g_signal_connect(cb, "toggled", G_CALLBACK(on_widget_show_toggled), w));

	/* Settings grid */
	GtkWidget *sg = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(sg), 12);
	gtk_grid_set_row_spacing(GTK_GRID(sg), 6);
	gtk_widget_set_margin_start(sg, 16);
	gtk_widget_set_margin_end(sg, 4);
	gtk_widget_set_margin_top(sg, 4);
	gtk_widget_set_margin_bottom(sg, 4);
	gtk_revealer_set_child(rev, sg);
	int r = 0;

	switch (wid) {
	case 0: { /* Sysinfo */
		w->show_sysinfo_check = cb;

		w->sysinfo_cpu_color_btn = make_color_btn(w->color_dialog,
			w->cfg.sysinfo_cpu_color ? w->cfg.sysinfo_cpu_color : 0xFFFFEB3B);
		grid_row(GTK_GRID(sg), r++,
			"CPU color:", GTK_WIDGET(w->sysinfo_cpu_color_btn));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_cpu_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_cpu_color_btn), w));

		w->sysinfo_tmp_color_btn = make_color_btn(w->color_dialog,
			w->cfg.sysinfo_tmp_color ? w->cfg.sysinfo_tmp_color : 0xFFFF7043);
		grid_row(GTK_GRID(sg), r++,
			"Temp color:", GTK_WIDGET(w->sysinfo_tmp_color_btn));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_tmp_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_tmp_color_btn), w));

		w->sysinfo_ram_color_btn = make_color_btn(w->color_dialog,
			w->cfg.sysinfo_ram_color ? w->cfg.sysinfo_ram_color : 0xFF66BB6A);
		grid_row(GTK_GRID(sg), r++,
			"RAM color:", GTK_WIDGET(w->sysinfo_ram_color_btn));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_ram_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_ram_color_btn), w));

		w->sysinfo_size_spin = make_spin(4, 128,
			w->cfg.sysinfo_font_size > 0 ? w->cfg.sysinfo_font_size : 14);
		grid_row(GTK_GRID(sg), r++,
			"Font size (pt):", GTK_WIDGET(w->sysinfo_size_spin));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_size_spin),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_size_spin), w));

		w->sysinfo_bg_color_btn = make_color_btn(w->color_dialog,
			w->cfg.sysinfo_bg_color ? w->cfg.sysinfo_bg_color : 0x94000000);
		grid_row(GTK_GRID(sg), r++,
			"Tile background:", GTK_WIDGET(w->sysinfo_bg_color_btn));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_bg_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_bg_color_btn), w));

		w->sysinfo_exec_entry = GTK_ENTRY(gtk_entry_new());
		gtk_editable_set_text(GTK_EDITABLE(w->sysinfo_exec_entry),
			w->cfg.sysinfo_exec ? w->cfg.sysinfo_exec : "");
		gtk_entry_set_placeholder_text(w->sysinfo_exec_entry,
			"empty to disable");
		gtk_widget_set_hexpand(GTK_WIDGET(w->sysinfo_exec_entry), TRUE);
		grid_row(GTK_GRID(sg), r++,
			"Left-click command:", GTK_WIDGET(w->sysinfo_exec_entry));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_exec_entry),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_exec_entry), w));

		w->sysinfo_percpu_check = GTK_CHECK_BUTTON(
			gtk_check_button_new_with_label("Busiest core % (top-style)"));
		gtk_check_button_set_active(w->sysinfo_percpu_check,
			w->cfg.sysinfo_percpu);
		grid_row(GTK_GRID(sg), r++,
			"CPU mode:", GTK_WIDGET(w->sysinfo_percpu_check));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_percpu_check),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_percpu_check), w));

		w->sysinfo_show_temp_check = GTK_CHECK_BUTTON(
			gtk_check_button_new_with_label("Show temperature line"));
		gtk_check_button_set_active(w->sysinfo_show_temp_check,
			w->cfg.sysinfo_show_temp);
		grid_row(GTK_GRID(sg), r++,
			"Temperature:", GTK_WIDGET(w->sysinfo_show_temp_check));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_show_temp_check),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_show_temp_check), w));

		w->sysinfo_show_proc_check = GTK_CHECK_BUTTON(
			gtk_check_button_new_with_label("Show process names"));
		gtk_check_button_set_active(w->sysinfo_show_proc_check,
			w->cfg.sysinfo_show_proc);
		grid_row(GTK_GRID(sg), r++,
			"Process names:", GTK_WIDGET(w->sysinfo_show_proc_check));
		store_signal_pair(frame, G_OBJECT(w->sysinfo_show_proc_check),
			connect_mark_unsaved(GTK_WIDGET(w->sysinfo_show_proc_check), w));
		break;
	}
	case 1: { /* Net */
		w->show_net_check = cb;

		w->net_iface_entry = GTK_ENTRY(gtk_entry_new());
		gtk_editable_set_text(GTK_EDITABLE(w->net_iface_entry),
			w->cfg.net_iface ? w->cfg.net_iface : "");
		gtk_entry_set_placeholder_text(w->net_iface_entry, "auto-detect");
		gtk_widget_set_hexpand(GTK_WIDGET(w->net_iface_entry), TRUE);
		grid_row(GTK_GRID(sg), r++,
			"Interface:", GTK_WIDGET(w->net_iface_entry));
		store_signal_pair(frame, G_OBJECT(w->net_iface_entry),
			connect_mark_unsaved(GTK_WIDGET(w->net_iface_entry), w));

		w->net_rx_color_btn = make_color_btn(w->color_dialog,
			w->cfg.net_rx_color ? w->cfg.net_rx_color : 0xFFFF3FFA);
		grid_row(GTK_GRID(sg), r++,
			"Down speed color:", GTK_WIDGET(w->net_rx_color_btn));
		store_signal_pair(frame, G_OBJECT(w->net_rx_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->net_rx_color_btn), w));

		w->net_tx_color_btn = make_color_btn(w->color_dialog,
			w->cfg.net_tx_color ? w->cfg.net_tx_color : 0xFF3AFFFD);
		grid_row(GTK_GRID(sg), r++,
			"Up speed color:", GTK_WIDGET(w->net_tx_color_btn));
		store_signal_pair(frame, G_OBJECT(w->net_tx_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->net_tx_color_btn), w));

		w->net_size_spin = make_spin(4, 128,
			w->cfg.net_font_size > 0 ? w->cfg.net_font_size : 14);
		grid_row(GTK_GRID(sg), r++,
			"Font size (pt):", GTK_WIDGET(w->net_size_spin));
		store_signal_pair(frame, G_OBJECT(w->net_size_spin),
			connect_mark_unsaved(GTK_WIDGET(w->net_size_spin), w));

		w->net_bg_color_btn = make_color_btn(w->color_dialog,
			w->cfg.net_bg_color ? w->cfg.net_bg_color : 0x94000000);
		grid_row(GTK_GRID(sg), r++,
			"Tile background:", GTK_WIDGET(w->net_bg_color_btn));
		store_signal_pair(frame, G_OBJECT(w->net_bg_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->net_bg_color_btn), w));
		break;
	}
	case 2: /* Apps — no sub-settings; checkbox is non-interactive */
		gtk_widget_set_sensitive(GTK_WIDGET(cb), FALSE);
		break;
	case 3: { /* Volume */
		w->show_volume_check = cb;

		w->volume_exec_entry = GTK_ENTRY(gtk_entry_new());
		gtk_editable_set_text(GTK_EDITABLE(w->volume_exec_entry),
			w->cfg.volume_exec ? w->cfg.volume_exec : "");
		gtk_entry_set_placeholder_text(w->volume_exec_entry,
			"empty to disable");
		gtk_widget_set_hexpand(GTK_WIDGET(w->volume_exec_entry), TRUE);
		grid_row(GTK_GRID(sg), r++,
			"Right-click command:", GTK_WIDGET(w->volume_exec_entry));
		store_signal_pair(frame, G_OBJECT(w->volume_exec_entry),
			connect_mark_unsaved(GTK_WIDGET(w->volume_exec_entry), w));
		break;
	}
	case 4: { /* Date */
		w->show_date_check = cb;

		w->date_date_format_entry = GTK_ENTRY(gtk_entry_new());
		gtk_editable_set_text(GTK_EDITABLE(w->date_date_format_entry),
			w->cfg.date_date_format ? w->cfg.date_date_format : "%a %d %B");
		gtk_widget_set_hexpand(GTK_WIDGET(w->date_date_format_entry), TRUE);
		grid_row(GTK_GRID(sg), r++,
			"Date format (strftime):", GTK_WIDGET(w->date_date_format_entry));
		store_signal_pair(frame, G_OBJECT(w->date_date_format_entry),
			connect_mark_unsaved(GTK_WIDGET(w->date_date_format_entry), w));

		w->date_date_color_btn = make_color_btn(w->color_dialog,
			w->cfg.date_date_color ? w->cfg.date_date_color : 0xFF68FF3A);
		grid_row(GTK_GRID(sg), r++,
			"Date color:", GTK_WIDGET(w->date_date_color_btn));
		store_signal_pair(frame, G_OBJECT(w->date_date_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->date_date_color_btn), w));

		w->date_date_size_spin = make_spin(4, 128,
			w->cfg.date_date_size > 0 ? w->cfg.date_date_size : 16);
		grid_row(GTK_GRID(sg), r++,
			"Date font size (pt):", GTK_WIDGET(w->date_date_size_spin));
		store_signal_pair(frame, G_OBJECT(w->date_date_size_spin),
			connect_mark_unsaved(GTK_WIDGET(w->date_date_size_spin), w));

		w->date_time_format_entry = GTK_ENTRY(gtk_entry_new());
		gtk_editable_set_text(GTK_EDITABLE(w->date_time_format_entry),
			w->cfg.date_time_format ? w->cfg.date_time_format : "%H:%M");
		gtk_widget_set_hexpand(GTK_WIDGET(w->date_time_format_entry), TRUE);
		grid_row(GTK_GRID(sg), r++,
			"Time format (strftime):", GTK_WIDGET(w->date_time_format_entry));
		store_signal_pair(frame, G_OBJECT(w->date_time_format_entry),
			connect_mark_unsaved(GTK_WIDGET(w->date_time_format_entry), w));

		w->date_time_color_btn = make_color_btn(w->color_dialog,
			w->cfg.date_time_color ? w->cfg.date_time_color : 0xFFFF0000);
		grid_row(GTK_GRID(sg), r++,
			"Time color:", GTK_WIDGET(w->date_time_color_btn));
		store_signal_pair(frame, G_OBJECT(w->date_time_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->date_time_color_btn), w));

		w->date_time_size_spin = make_spin(4, 128,
			w->cfg.date_time_size > 0 ? w->cfg.date_time_size : 36);
		grid_row(GTK_GRID(sg), r++,
			"Time font size (pt):", GTK_WIDGET(w->date_time_size_spin));
		store_signal_pair(frame, G_OBJECT(w->date_time_size_spin),
			connect_mark_unsaved(GTK_WIDGET(w->date_time_size_spin), w));

		w->date_bg_color_btn = make_color_btn(w->color_dialog,
			w->cfg.date_bg_color ? w->cfg.date_bg_color : 0x94000000);
		grid_row(GTK_GRID(sg), r++,
			"Tile background:", GTK_WIDGET(w->date_bg_color_btn));
		store_signal_pair(frame, G_OBJECT(w->date_bg_color_btn),
			connect_mark_unsaved(GTK_WIDGET(w->date_bg_color_btn), w));
		break;
	}
	}

	return frame;
}

static void
refresh_widgets_list(CfgWin *w)
{
	gboolean is_empty =
		gtk_widget_get_first_child(GTK_WIDGET(w->widgets_box)) == NULL;

	if (is_empty) {
		/* Initial population — build the 5 rows in widget_order sequence */
		for (int i = 0; i < 5; i++)
			gtk_box_append(w->widgets_box, make_widget_row(w, i));
		return;
	}

	/* Collect the existing row frames into an array indexed by widget ID */
	GtkWidget *rows[5] = {NULL, NULL, NULL, NULL, NULL};
	GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(w->widgets_box));
	while (child) {
		int wid =
			GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "widget_id")) -
			1;
		if (wid >= 0 && wid < 5)
			rows[wid] = child;
		child = gtk_widget_get_next_sibling(child);
	}

	/* Re-insert in widget_order sequence (last-to-first prepend) */
	for (int i = 4; i >= 0; i--) {
		int wid = w->cfg.widget_order[i];
		GtkWidget *row = rows[wid];
		if (!row)
			continue;

		g_object_ref(row);
		gtk_box_remove(w->widgets_box, row);
		gtk_box_prepend(w->widgets_box, row);
		g_object_unref(row);
	}

	/* Update the Up/Down button sensitivity and widget_pos to match new
	 * positions */
	child = gtk_widget_get_first_child(GTK_WIDGET(w->widgets_box));
	int pos = 0;
	while (child) {
		GtkWidget *up = g_object_get_data(G_OBJECT(child), "up_btn");
		GtkWidget *dn = g_object_get_data(G_OBJECT(child), "dn_btn");
		if (up) {
			gtk_widget_set_sensitive(up, pos > 0);
			g_object_set_data(G_OBJECT(up), "widget_pos", GINT_TO_POINTER(pos));
		}
		if (dn) {
			gtk_widget_set_sensitive(dn, pos < 4);
			g_object_set_data(G_OBJECT(dn), "widget_pos", GINT_TO_POINTER(pos));
		}
		child = gtk_widget_get_next_sibling(child);
		pos++;
	}
}

/* Both the Close button and the window's own × button route through here,
 * calling g_application_quit instead of gtk_window_destroy to avoid a crash
 * caused by GTK tearing down the widget tree while background threads still
 * hold GObject references. Returning TRUE suppresses the default handler. */
static gboolean
on_close_request(GtkWindow *win, gpointer data)
{
	(void)win;
	g_application_quit(G_APPLICATION(((CfgWin *)data)->gapp));
	return TRUE;
}

/* =========================================================================
 * App row management (main window)
 * ======================================================================= */
static void
on_remove_clicked(GtkButton *btn, gpointer data)
{
	(void)btn;
	GtkWidget *row = GTK_WIDGET(data);
	CfgWin *w = g_object_get_data(G_OBJECT(row), "cfgwin");
	int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "app_index"));
	if (idx < 0 || idx >= w->cfg.count)
		return;
	free(w->cfg.apps[idx]->name);
	free(w->cfg.apps[idx]->exec);
	free(w->cfg.apps[idx]->icon);
	free(w->cfg.apps[idx]);
	for (int i = idx; i < w->cfg.count - 1; i++)
		w->cfg.apps[i] = w->cfg.apps[i + 1];
	w->cfg.count--;
	mark_unsaved(w);
	refresh_apps_list(w);
}

static void
on_move_up_clicked(GtkButton *btn, gpointer data)
{
	(void)btn;
	GtkWidget *row = GTK_WIDGET(data);
	CfgWin *w = g_object_get_data(G_OBJECT(row), "cfgwin");
	int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "app_index"));
	if (idx <= 0 || idx >= w->cfg.count)
		return;
	DesktopEntry *t = w->cfg.apps[idx - 1];
	w->cfg.apps[idx - 1] = w->cfg.apps[idx];
	w->cfg.apps[idx] = t;
	mark_unsaved(w);
	refresh_apps_list(w);
}

static void
on_move_down_clicked(GtkButton *btn, gpointer data)
{
	(void)btn;
	GtkWidget *row = GTK_WIDGET(data);
	CfgWin *w = g_object_get_data(G_OBJECT(row), "cfgwin");
	int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "app_index"));
	if (idx < 0 || idx >= w->cfg.count - 1)
		return;
	DesktopEntry *t = w->cfg.apps[idx + 1];
	w->cfg.apps[idx + 1] = w->cfg.apps[idx];
	w->cfg.apps[idx] = t;
	mark_unsaved(w);
	refresh_apps_list(w);
}

/* Free the preview weak-pointer slot, removing the weak ref first if the
 * preview widget is still alive, to avoid a dangling notify after free. */
static void
on_icon_preview_slot_free(gpointer data)
{
	GtkWidget **slot = (GtkWidget **)data;
	if (*slot)
		g_object_remove_weak_pointer(G_OBJECT(*slot), (gpointer *)slot);
	g_free(slot);
}

/* Update the preview image when the icon entry text changes. */
static void
on_icon_entry_changed(GtkEditable *editable, gpointer user_data)
{
	(void)user_data;
	GtkWidget **slot = g_object_get_data(G_OBJECT(editable), "preview_slot");
	GtkWidget *preview = slot ? *slot : NULL;
	if (!preview)
		return;
	const char *path = gtk_editable_get_text(editable);
	if (path && path[0]) {
		GdkTexture *tex = gdk_texture_new_from_filename(path, NULL);
		if (tex) {
			gtk_image_set_from_paintable(GTK_IMAGE(preview),
				GDK_PAINTABLE(tex));
			g_object_unref(tex);
			return;
		}
	}
	gtk_image_set_from_paintable(GTK_IMAGE(preview), NULL);
}

/* Copy the clicked button's label (the icon path) into the icon entry. */
static void
on_icon_path_btn_clicked(GtkButton *btn, gpointer user_data)
{
	const char *path = gtk_button_get_label(btn);
	if (path && path[0])
		gtk_editable_set_text(GTK_EDITABLE((GtkWidget *)user_data), path);
	/* Close the popover */
	GtkWidget *w = gtk_widget_get_parent(GTK_WIDGET(btn)); /* box */
	w = w ? gtk_widget_get_parent(w) : NULL;			   /* scrolled window */
	w = w ? gtk_widget_get_parent(w) : NULL;			   /* popover */
	if (w && GTK_IS_POPOVER(w))
		gtk_popover_popdown(GTK_POPOVER(w));
}

static GtkWidget *
make_app_row(CfgWin *w, int idx)
{
	DesktopEntry *app = w->cfg.apps[idx];

	GtkWidget *frame = gtk_frame_new(NULL);
	gtk_widget_set_margin_start(frame, 4);
	gtk_widget_set_margin_end(frame, 4);
	gtk_widget_set_margin_top(frame, 3);
	gtk_widget_set_margin_bottom(frame, 3);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start(vbox, 8);
	gtk_widget_set_margin_end(vbox, 8);
	gtk_widget_set_margin_top(vbox, 6);
	gtk_widget_set_margin_bottom(vbox, 6);
	gtk_frame_set_child(GTK_FRAME(frame), vbox);

	GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_append(GTK_BOX(vbox), hdr);

	char lbuf[32];
	snprintf(lbuf, sizeof(lbuf), "<b>#%d</b>", idx + 1);
	GtkWidget *il = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(il), lbuf);
	gtk_box_append(GTK_BOX(hdr), il);

	GtkWidget *sp = gtk_label_new("");
	gtk_widget_set_hexpand(sp, TRUE);
	gtk_box_append(GTK_BOX(hdr), sp);

	GtkWidget *up = gtk_button_new_with_label("\xe2\x96\xb2 Up");
	gtk_widget_set_sensitive(up, idx > 0);
	gtk_box_append(GTK_BOX(hdr), up);

	GtkWidget *dn = gtk_button_new_with_label("\xe2\x96\xbc Down");
	gtk_widget_set_sensitive(dn, idx < w->cfg.count - 1);
	gtk_box_append(GTK_BOX(hdr), dn);

	GtkWidget *rm = gtk_button_new_with_label("\xe2\x9c\x95 Remove");
	gtk_box_append(GTK_BOX(hdr), rm);

	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_box_append(GTK_BOX(vbox), grid);

	GtkWidget *ne = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(ne), app->name ? app->name : "");
	gtk_widget_set_hexpand(ne, TRUE);

	GtkWidget *ie = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(ie), app->icon ? app->icon : "");
	gtk_widget_set_hexpand(ie, TRUE);

	GtkWidget *ee = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(ee), app->exec ? app->exec : "");
	gtk_widget_set_hexpand(ee, TRUE);

	grid_row(GTK_GRID(grid), 0, "Name:", ne);

	/* ---- Icon row: [preview] [entry] [▾ detected icons] ---- */
	{
		char **icon_paths = local_find_all_icons(app->icon);

		GtkWidget *icon_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_widget_set_hexpand(icon_hbox, TRUE);

		/* Preview image — updated when the entry text changes */
		GtkWidget *preview = gtk_image_new();
		gtk_image_set_pixel_size(GTK_IMAGE(preview), 32);
		gtk_widget_set_size_request(preview, 36, 36);
		if (app->icon && app->icon[0]) {
			GdkTexture *tex = gdk_texture_new_from_filename(app->icon, NULL);
			if (tex) {
				gtk_image_set_from_paintable(GTK_IMAGE(preview),
					GDK_PAINTABLE(tex));
				g_object_unref(tex);
			}
		}
		gtk_box_append(GTK_BOX(icon_hbox), preview);

		/* Store preview as a weak pointer so the "changed" callback
		 * doesn't touch it if it happens to be destroyed first. */
		GtkWidget **preview_slot = g_new(GtkWidget *, 1);
		*preview_slot = preview;
		g_object_add_weak_pointer(G_OBJECT(preview), (gpointer *)preview_slot);
		g_object_set_data_full(G_OBJECT(ie), "preview_slot", preview_slot,
			on_icon_preview_slot_free);
		g_signal_connect(ie, "changed", G_CALLBACK(on_icon_entry_changed),
			NULL);

		gtk_box_append(GTK_BOX(icon_hbox), ie);

		/* ▾ menu button showing all detected icon paths */
		if (icon_paths && icon_paths[0]) {
			int nicons = 0;
			while (icon_paths[nicons])
				nicons++;

			GtkWidget *pop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
			gtk_widget_set_margin_start(pop_box, 4);
			gtk_widget_set_margin_end(pop_box, 4);
			gtk_widget_set_margin_top(pop_box, 4);
			gtk_widget_set_margin_bottom(pop_box, 4);

			for (int k = 0; k < nicons; k++) {
				GtkWidget *btn = gtk_button_new_with_label(icon_paths[k]);
				gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
				GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(btn));
				gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_START);
				gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
				g_signal_connect(btn, "clicked",
					G_CALLBACK(on_icon_path_btn_clicked), ie);
				gtk_box_append(GTK_BOX(pop_box), btn);
				free(icon_paths[k]);
			}

			GtkWidget *pop_scroll = gtk_scrolled_window_new();
			gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pop_scroll),
				GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
			gtk_scrolled_window_set_kinetic_scrolling(
				GTK_SCROLLED_WINDOW(pop_scroll), FALSE);
			gtk_widget_set_size_request(pop_scroll, 420, 200);
			gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pop_scroll),
				pop_box);

			GtkWidget *popover = gtk_popover_new();
			gtk_popover_set_child(GTK_POPOVER(popover), pop_scroll);

			GtkWidget *menu_btn = gtk_menu_button_new();
			gtk_menu_button_set_label(GTK_MENU_BUTTON(menu_btn),
				"\xe2\x96\xbe");
			gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_btn), popover);
			gtk_widget_set_tooltip_text(menu_btn, "Show detected icons");
			gtk_box_append(GTK_BOX(icon_hbox), menu_btn);
		}
		free(icon_paths);

		grid_row(GTK_GRID(grid), 1, "Icon:", icon_hbox);
	}

	grid_row(GTK_GRID(grid), 2, "Exec:", ee);

	GtkWidget *tcb = gtk_check_button_new_with_label("Run in terminal");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(tcb), app->terminal);
	gtk_grid_attach(GTK_GRID(grid), tcb, 1, 3, 1, 1);

	g_object_set_data(G_OBJECT(frame), "name_entry", ne);
	g_object_set_data(G_OBJECT(frame), "icon_entry", ie);
	g_object_set_data(G_OBJECT(frame), "exec_entry", ee);
	g_object_set_data(G_OBJECT(frame), "terminal_check", tcb);
	g_object_set_data(G_OBJECT(frame), "cfgwin", (gpointer)w);
	g_object_set_data(G_OBJECT(frame), "app_index", GINT_TO_POINTER(idx));

	g_signal_connect(up, "clicked", G_CALLBACK(on_move_up_clicked), frame);
	g_signal_connect(dn, "clicked", G_CALLBACK(on_move_down_clicked), frame);
	g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_clicked), frame);

	return frame;
}

static void
refresh_apps_list(CfgWin *w)
{
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(GTK_WIDGET(w->apps_box))))
		gtk_box_remove(w->apps_box, child);
	for (int i = 0; i < w->cfg.count; i++)
		gtk_box_append(w->apps_box, make_app_row(w, i));
}

static void
on_save_clicked(GtkButton *btn, gpointer data)
{
	(void)btn;
	CfgWin *w = (CfgWin *)data;
	if (write_config(w) == 0)
		gtk_label_set_markup(w->status_label,
			"<span foreground='green'>\xe2\x9c\x94  Configuration saved.</span>");
	else
		gtk_label_set_markup(w->status_label,
			"<span foreground='red'>\xe2\x9c\x98  Failed to save configuration.</span>");
}

static void
on_close_clicked(GtkButton *btn, gpointer data)
{
	(void)btn;
	g_application_quit(G_APPLICATION(((CfgWin *)data)->gapp));
}

/* =========================================================================
 * GApplication activate — build and present the main window
 * ======================================================================= */
static void
on_activate(GApplication *gapp, gpointer data)
{
	CfgWin *w = (CfgWin *)data;
	w->gapp = GTK_APPLICATION(gapp);

	w->window = GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(gapp)));
	gtk_window_set_title(w->window, "labar — Configuration");
	gtk_window_set_default_size(w->window, 720, 680);
	g_signal_connect(w->window, "close-request", G_CALLBACK(on_close_request),
		w);

	w->color_dialog = gtk_color_dialog_new();
	gtk_color_dialog_set_with_alpha(w->color_dialog, TRUE);
	g_object_ref_sink(w->color_dialog); /* take explicit owned ref */

	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_window_set_child(w->window, outer);

	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(scroll),
		FALSE);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(outer), scroll);

	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_start(content, 14);
	gtk_widget_set_margin_end(content, 14);
	gtk_widget_set_margin_top(content, 10);
	gtk_widget_set_margin_bottom(content, 10);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);

	/* ---- Global Settings ---- */
	gtk_box_append(GTK_BOX(content), section_label("<b>Global Settings</b>"));

	GtkWidget *gg = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(gg), 12);
	gtk_grid_set_row_spacing(GTK_GRID(gg), 6);
	gtk_box_append(GTK_BOX(content), gg);
	int r = 0;

	w->icon_size_spin =
		make_spin(8, 512, w->cfg.icon_size > 0 ? w->cfg.icon_size : 64);
	grid_row(GTK_GRID(gg), r++,
		"Icon size (px):", GTK_WIDGET(w->icon_size_spin));
	connect_mark_unsaved(GTK_WIDGET(w->icon_size_spin), w);

	w->icon_spacing_spin = make_spin(0, 256, w->cfg.icon_spacing);
	grid_row(GTK_GRID(gg), r++,
		"Icon spacing (px):", GTK_WIDGET(w->icon_spacing_spin));
	connect_mark_unsaved(GTK_WIDGET(w->icon_spacing_spin), w);

	w->exclusive_zone_spin = make_spin(-1, 2048, w->cfg.exclusive_zone);
	grid_row(GTK_GRID(gg), r++,
		"Exclusive zone:", GTK_WIDGET(w->exclusive_zone_spin));
	connect_mark_unsaved(GTK_WIDGET(w->exclusive_zone_spin), w);

	w->border_space_spin = make_spin(0, 2048, w->cfg.border_space);
	grid_row(GTK_GRID(gg), r++,
		"Border space (px):", GTK_WIDGET(w->border_space_spin));
	connect_mark_unsaved(GTK_WIDGET(w->border_space_spin), w);

	{
		static const char *const lm[] = {"always", "hover", "never", NULL};
		w->label_mode_drop = make_dropdown(lm, (int)w->cfg.label_mode);
		grid_row(GTK_GRID(gg), r++,
			"Label mode:", GTK_WIDGET(w->label_mode_drop));
		connect_mark_unsaved(GTK_WIDGET(w->label_mode_drop), w);
	}

	w->label_color_btn = make_color_btn(w->color_dialog,
		w->cfg.label_color ? w->cfg.label_color : 0xFFFFFFFF);
	grid_row(GTK_GRID(gg), r++, "Label color:", GTK_WIDGET(w->label_color_btn));
	connect_mark_unsaved(GTK_WIDGET(w->label_color_btn), w);

	w->label_size_spin =
		make_spin(4, 128, w->cfg.label_size > 0 ? w->cfg.label_size : 10);
	grid_row(GTK_GRID(gg), r++,
		"Label font size (pt):", GTK_WIDGET(w->label_size_spin));
	connect_mark_unsaved(GTK_WIDGET(w->label_size_spin), w);

	w->label_offset_spin = make_spin(0, 512, w->cfg.label_offset);
	grid_row(GTK_GRID(gg), r++,
		"Label offset (px from bottom):", GTK_WIDGET(w->label_offset_spin));
	connect_mark_unsaved(GTK_WIDGET(w->label_offset_spin), w);

	{
		static const char *const pos[] = {
			"bottom", "top", "left", "right", NULL};
		w->position_drop = make_dropdown(pos, (int)w->cfg.position);
		grid_row(GTK_GRID(gg), r++, "Position:", GTK_WIDGET(w->position_drop));
		connect_mark_unsaved(GTK_WIDGET(w->position_drop), w);
	}

	{
		static const char *const lay[] = {
			"background", "bottom", "top", "overlay", NULL};
		w->layer_drop = make_dropdown(lay, (int)w->cfg.layer);
		grid_row(GTK_GRID(gg), r++, "Layer:", GTK_WIDGET(w->layer_drop));
		connect_mark_unsaved(GTK_WIDGET(w->layer_drop), w);
	}

	{
		w->output_entry = GTK_ENTRY(gtk_entry_new());
		gtk_editable_set_text(GTK_EDITABLE(w->output_entry),
			w->cfg.output_name ? w->cfg.output_name : "");
		gtk_entry_set_placeholder_text(w->output_entry, "eDP-1 (empty = auto)");
		gtk_widget_set_hexpand(GTK_WIDGET(w->output_entry), TRUE);
		grid_row(GTK_GRID(gg), r++, "Output:", GTK_WIDGET(w->output_entry));
		connect_mark_unsaved(GTK_WIDGET(w->output_entry), w);
	}

	/* ---- Widgets ---- */
	gtk_box_append(GTK_BOX(content),
		section_label(
			"<b>Widgets</b>  <small>(use ▲▼ to change bar order)</small>"));

	/* We build the three widget rows in the order stored in cfg.widget_order.
	 * Each row has a header with ▲/▼ buttons that swap adjacent entries in
	 * widget_order and refresh the whole section.
	 *
	 * The three rows are kept in a single GtkBox (w->widgets_box) so they
	 * can be fully rebuilt by refresh_widgets_list(). */
	w->widgets_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	gtk_box_append(GTK_BOX(content), GTK_WIDGET(w->widgets_box));
	refresh_widgets_list(w);

	/* ---- Applications ---- */
	gtk_box_append(GTK_BOX(content),
		section_label(
			"<b>Applications</b>  <small>(use ▲▼ to reorder)</small>"));

	w->apps_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	gtk_box_append(GTK_BOX(content), GTK_WIDGET(w->apps_box));
	refresh_apps_list(w);

	/* ---- Bottom bar ---- */
	gtk_box_append(GTK_BOX(outer),
		gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

	GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_margin_start(bottom, 12);
	gtk_widget_set_margin_end(bottom, 12);
	gtk_widget_set_margin_top(bottom, 6);
	gtk_widget_set_margin_bottom(bottom, 6);
	gtk_box_append(GTK_BOX(outer), bottom);

	w->status_label = GTK_LABEL(gtk_label_new(""));
	gtk_label_set_xalign(w->status_label, 0.0);
	gtk_widget_set_hexpand(GTK_WIDGET(w->status_label), TRUE);
	gtk_box_append(GTK_BOX(bottom), GTK_WIDGET(w->status_label));

	GtkWidget *add_btn = gtk_button_new_with_label("➕  Add App");
	gtk_box_append(GTK_BOX(bottom), add_btn);
	g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_app_clicked), w);

	GtkWidget *save_btn = gtk_button_new_with_label("💾  Save");
	gtk_widget_add_css_class(save_btn, "suggested-action");
	gtk_box_append(GTK_BOX(bottom), save_btn);
	g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_clicked), w);

	GtkWidget *close_btn = gtk_button_new_with_label("✕  Close");
	gtk_box_append(GTK_BOX(bottom), close_btn);
	g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), w);

	gtk_window_present(w->window);
}

/* =========================================================================
 * Public entry point
 * ======================================================================= */
int
config_window_run(void)
{
	CfgWin *w = calloc(1, sizeof(CfgWin));
	if (!w)
		return 1;

	w->cfg = load_config();

	GtkApplication *gapp =
		gtk_application_new("org.labar.config", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), w);
	int status = g_application_run(G_APPLICATION(gapp), 0, NULL);

	g_object_unref(gapp);
	/* Unref color_dialog here — after g_application_run returns all widgets
	 * are fully destroyed, so it is safe to drop our last reference. */
	if (w->color_dialog) {
		g_object_unref(w->color_dialog);
		w->color_dialog = NULL;
	}
	free_config(&w->cfg);
	free(w);
	return status == 0 ? 0 : 1;
}
