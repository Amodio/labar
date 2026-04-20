project('labar', 'c',
  version: '1.4.3',
  default_options: [
    'prefix=/usr',
    'c_std=c11',
    'warning_level=3',
    'b_sanitize=address,undefined',  # ASan/LSan/UBSan -> on by default
    ]
)

cc = meson.get_compiler('c')

add_project_arguments(
  '-DVERSION="' + meson.project_name() + ' ' + meson.project_version() + '"',
  language: 'c'
)

# GTK4 for the --config graphical configuration window (optional)
gtk4_dep = dependency('gtk4', required: false)

# GTK4 is lazily loaded (to save ~80MB in "normal" mode, not using --config)
add_project_arguments(
  '-DLABAR_LIB_DIR="' + (get_option('prefix') / get_option('libdir') / 'labar') + '"',
  language: 'c'
)

# Volume icons installed to /usr/share/pixmaps/labar/ by default
pixmap_dir = get_option('prefix') / get_option('datadir') / 'pixmaps' / 'labar'
add_project_arguments(
  '-DVOLUME_PIXMAP_DIR="' + pixmap_dir + '"',
  language: 'c'
)

if gtk4_dep.found()
  add_project_arguments('-DHAVE_GTK4=1', language: 'c')
  # Export main executable symbols so liblabar-config.so can resolve them
  add_project_link_arguments('-rdynamic', language: 'c')
  # GTK4 headers are not -Wpedantic-clean (trailing semicolons in GLib macros).
  # Suppress the warning project-wide; it only surfaces when GTK4 is present.
  add_project_arguments('-Wno-pedantic', language: 'c')
  message('GTK4 found — building --config window')
else
  message('GTK4 not found — --config window disabled')
endif

# Forbid {Address,Leak,UndefinedBehavior}Sanitizers in release build
if get_option('buildtype') == 'release'
  if get_option('b_sanitize') != 'none'
    error('ASan/LSan/UBSan are not meant to be used in production')
  endif
  # Strip the binary
  add_project_link_arguments('-s', language: 'c')
endif

sources = [
  'src/main.c',
  'src/config.c',
  'src/exec.c',
  'src/seat.c',
  'src/cache.c',
  'src/widget-common.c',
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

if gtk4_dep.found()
  shared_library(
    'labar-config',
    'src/config-window.c',
    dependencies: [gtk4_dep, cc.find_library('m', required: true)],
    link_args : ['-Wl,--allow-shlib-undefined'],
    override_options : ['b_lundef=false'],
    install: true,
    install_dir : get_option('prefix') / get_option('libdir') / 'labar',
  )
  deps += cc.find_library('dl', required: true)
  # config-window.c is no longer in sources[]
endif

# Build wayland-protocols code (generated from XML)
subdir('protocols')
deps += wl_protocols

# Documentation (man page)
subdir('doc')

# Uses pixmap_dir
subdir('pixmaps')

# Main executable
executable(
  meson.project_name(),
  sources,
  dependencies: deps,
  install: true,
)
