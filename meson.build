project('labar', 'c',
  version : '0.3.0',
  default_options: ['c_std=c11']
)

# Wayland dependencies
wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')

# Cairo & librsvg for SVG icon rendering
cairo_dep = dependency('cairo')
librsvg_dep = dependency('librsvg-2.0')

# Build protocols code (generated from XML)
subdir('protocols')

# Documentation
subdir('doc')

# Main executable
executable(
  'labar',
  'src/main.c',
  'src/config.c',
  'src/exec.c',
  'src/seat.c',
  'src/cache.c',
  dependencies: [
    wayland_client,
    wl_protocols,
    cairo_dep,
    librsvg_dep,
  ],
  c_args: [
    '-DVERSION="' + meson.project_name() + ' ' + meson.project_version() + '"'
    ],
)
