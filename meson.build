project('labar', 'c',
  version: '1.0.1',
  default_options: [
    'prefix=/usr',
    'c_std=c11',
    'warning_level=3',
    'strip=true'
    ]
)

sources = [
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
]

deps = [
  dependency('wayland-client'),
  dependency('wayland-protocols'),
  dependency('cairo'),
  dependency('librsvg-2.0'),
  dependency('alsa'),
]

# GTK4 for the --config graphical configuration window (optional)
gtk4_dep = dependency('gtk4', required: false)
if gtk4_dep.found()
  sources += 'src/config-window.c'
  deps += gtk4_dep
  # For lround(), used in src/config-window.c
  cc = meson.get_compiler('c')
  deps += cc.find_library('m', required: true)
  add_project_arguments('-DHAVE_GTK4=1', language: 'c')
  message('GTK4 found — building --config window')
else
  message('GTK4 not found — --config window disabled')
endif

# Volume icons installed to /usr/share/pixmaps/labar/ by default
pixmap_dir = get_option('prefix') / get_option('datadir') / 'pixmaps' / 'labar'

add_project_arguments(
  '-DVERSION="' + meson.project_name() + ' ' + meson.project_version() + '"',
  '-DVOLUME_PIXMAP_DIR="' + pixmap_dir + '"',
  language: 'c'
)

# Build wayland-protocols code (generated from XML)
subdir('protocols')
deps += wl_protocols

# Documentation (man page)
subdir('doc')

# Uses pixmap_dir
subdir('pixmaps')

# Main executable
executable(
  'labar',
  sources,
  dependencies: deps,
  install: true,
)
