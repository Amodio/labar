project('labar', 'c',
  version: '0.9.0',
  default_options: ['c_std=c11', 'prefix=/usr']
)

# Wayland dependencies
wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
# Cairo & librsvg for SVG icon rendering
cairo_dep = dependency('cairo')
librsvg_dep = dependency('librsvg-2.0')
# ALSA for volume control
alsa_dep = dependency('alsa')

# GTK4 for the --config graphical configuration window (optional)
gtk4_dep = dependency('gtk4', required: false)
have_gtk4 = gtk4_dep.found()

extra_sources = []
extra_deps    = []
extra_cargs   = []

if have_gtk4
  extra_sources += 'src/config-window.c'
  extra_deps    += gtk4_dep
  extra_cargs   += '-DHAVE_GTK4=1'
  message('GTK4 found — building --config window')
else
  message('GTK4 not found — --config window disabled')
endif

# Build wayland-protocols code (generated from XML)
subdir('protocols')

# Documentation
subdir('doc')

# Volume icons installed to /usr/share/pixmaps/labar/ by default
pixmap_dir = get_option('prefix') / get_option('datadir') / 'pixmaps' / 'labar'
subdir('pixmaps')

# Main executable
executable(
  'labar',
  'src/main.c',
  'src/config.c',
  'src/exec.c',
  'src/seat.c',
  'src/cache.c',
  'src/widget-volume.c',
  'src/widget-date.c',
  'src/calendar-popup.c',
  'src/widget-net.c',
  'src/widget-sysinfo.c',
  extra_sources,
  dependencies: [
    wayland_client,
    wl_protocols,
    cairo_dep,
    librsvg_dep,
    alsa_dep,
    extra_deps,
  ],
  c_args: [
    '-DVERSION="' + meson.project_name() + ' ' + meson.project_version() + '"',
    '-DVOLUME_PIXMAP_DIR="' + pixmap_dir + '"',
    extra_cargs,
  ],
  install: true
)
