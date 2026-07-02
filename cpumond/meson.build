#AM_CFLAGS += $(if $(GCOV),-fprofile-arcs -ftest-coverage)

# -I$(top_srcdir)/include

cpumond_sources = [
    'cpumond.c',
    'cpumond.h',
    ]

# 'cpumond.service',
#SYSTEMD_SERVICE_DIR = /usr/lib/systemd/system

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
)
