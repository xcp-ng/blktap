cbt_include = include_directories('.')

cbtutil_sources = 'cbt-util.c'

cbtutil_slib = static_library('cbtutil',
  cbtutil_sources,
  include_directories : incdir,
  # Keep fopen()/etc. compiling down to the plain (non-64) glibc symbols,
  # so the mockatests linker --wrap=fopen,... flags still intercept them.
  # No functional difference on 64-bit Linux, where off_t is already 64-bit.
  c_args : ['-U_FILE_OFFSET_BITS'],
)

cbt_util_sources = 'main.c'

executable('cbt-util', cbt_util_sources,
  dependencies : uuid_dep,
  link_with : cbtutil_slib,
  install : true,
  install_dir : get_option('sbindir'),
)
