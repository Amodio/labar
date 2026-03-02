project('labar', 'c',
  version : '0.3.0',
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
# Build protocols code (generated from XML)
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
  'src/volume.c',
  dependencies: [
    wayland_client,
    wl_protocols,
    cairo_dep,
    librsvg_dep,
    alsa_dep,
  ],
  c_args: [
    '-DVERSION="' + meson.project_name() + ' ' + meson.project_version() + '"',
    '-DVOLUME_PIXMAP_DIR="' + pixmap_dir + '"',
    ],
)
