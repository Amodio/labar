# Documentation build configuration for labar
scdoc = dependency('scdoc', version: '>=1.0', required: false)

if scdoc.found()
  manpage = custom_target(
    'labar.1',
    input: 'labar.1.scd',
    output: 'labar.1',
    command: [scdoc.get_variable(pkgconfig: 'scdoc')],
    install: true,
    install_dir: join_paths(get_option('mandir'), 'man1'),
    capture: true
  )
else
  warning('scdoc not found - manpage will not be built')
endif
