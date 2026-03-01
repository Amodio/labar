# Documentation build configuration for labar
scdoc = find_program('scdoc', required: false)
if scdoc.found()
  manpage = custom_target(
    'labar.1',
    input: 'labar.1.scd',
    output: 'labar.1',
    command: ['sh', '-c', scdoc.full_path() + ' < @INPUT@'],
    install: true,
    install_dir: join_paths(get_option('mandir'), 'man1'),
    capture: true,
  )
else
  warning('scdoc not found - manpage will not be built')
endif
