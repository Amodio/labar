wayland_protocols = dependency('wayland-protocols')
wp_dir = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
  wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'),
  native: true,
)

protocols = [
  [ wp_dir, 'stable/xdg-shell/xdg-shell.xml' ],
  [ wp_dir, 'stable/viewporter/viewporter.xml' ],
  [ wp_dir, 'staging/fractional-scale/fractional-scale-v1.xml' ],
  [ 'wlr-layer-shell-unstable-v1.xml' ],
]

wl_protocols_src = []
wl_protocols_headers = []

foreach p : protocols
  xml = join_paths(p)

  wl_protocols_src += custom_target(
    xml.underscorify() + '_c',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [ wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@' ],
  )

  wl_protocols_headers += custom_target(
    xml.underscorify() + '_h',
    input: xml,
    output: '@BASENAME@-protocol.h',
    command: [ wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@' ],
  )
endforeach

wl_protocols_lib = static_library(
  'labar_protocols',
  wl_protocols_src + wl_protocols_headers,
  dependencies: [ wayland_client ],
)

wl_protocols = declare_dependency(
  link_with: wl_protocols_lib,
  sources: wl_protocols_headers,
)
