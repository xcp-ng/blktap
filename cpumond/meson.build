#AM_CFLAGS += $(if $(GCOV),-fprofile-arcs -ftest-coverage)

# -I$(top_srcdir)/include

cpumond_sources = [
    'cpumond.c',
    'cpumond.h',
    ]

executable('cpumond', cpumond_sources,
  include_directories : incdir,
  dependencies : [
    rt_dep,
  ],
  install : true,
)

install_data('cpumond.service', install_dir : systemd_unit_dir)
