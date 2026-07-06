
tapback_sources = [
  'xenstore.c',
  'frontend.c',
  'backend.c',
  'tapback.h',
  'tapback.c',
  ]

xenstore_dep = dependency('xenstore')

executable('tapback', tapback_sources,
  include_directories : incdir,
  dependencies : xenstore_dep,
  link_with : [
    blktapctl_libs.get_shared_lib(),
  ],
  c_args : [
    '-D_GNU_SOURCE', # required by vasprintf
  ],
  install : true,
  override_options : 'warning_level=2',
)

install_data('tapback.service', install_dir : systemd_unit_dir)
