#AM_CFLAGS += $(if $(GCOV),-fprofile-arcs -ftest-coverage)

# -I$(top_srcdir)/include

cpumond_sources = [
    'cpumond.c',
    'cpumond.h',
    ]

compiler = meson.get_compiler('c')
rt_dep = compiler.find_library('rt')

executable('cpumond', cpumond_sources,
  include_directories : incdir,
  dependencies : [
    rt_dep,
  ],
  c_args : [
    '-D_GNU_SOURCE',
  ],
  install : true,
)

install_data('cpumond.service', install_dir : systemd_unit_dir)
