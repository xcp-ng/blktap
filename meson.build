project(
  'tapdisk',
  'c',
  default_options : [
    'c_std=gnu11',
    'warning_level=1',
    'werror=true',
    'buildtype=plain',
  ],
)

c_args_tests = []
gcov = get_option('GCOV')
enable_tests = get_option('tests') or gcov
if gcov == true
  c_args_tests = ['-fprofile-arcs', '-ftest-coverage']
endif

conf_data = configuration_data()
conf_data.set('_BLKTAP', 1)

configure_file(output : 'config.h', configuration : conf_data)

incdir = include_directories('.', 'include')

# Required by vasprintf(). Historically cbt-util was built without it
# (autotools never set it there), but it doesn't use any GNU-specific
# symbol, so applying it project-wide is a no-op for it in practice.
add_project_arguments('-D_GNU_SOURCE', language : 'c')

uuid_dep = dependency('uuid')

compiler = meson.get_compiler('c')
aio_dep = compiler.find_library('aio')
dl_dep = compiler.find_library('dl')
rt_dep = compiler.find_library('rt')

systemd_dep = dependency('systemd', required : false)
if systemd_dep.found()
  systemd_unit_dir = systemd_dep.get_variable(pkgconfig : 'systemdsystemunitdir')
else
  systemd_unit_dir = '/usr/lib/systemd/system'
endif

subdir('cbt')
subdir('control')
subdir('cpumond')
subdir('lvm')
subdir('vhd')
subdir('qcow2')
subdir('drivers')
subdir('tapback')
subdir('mockatests')

#TODO: uuid dep in drivers, debug, tests

install_data(
  ['README', 'README.md', 'LICENSE', 'CONTRIB', 'MAINTAINERS'],
  install_dir : get_option('datadir') / 'doc' / meson.project_name(),
)

install_headers(
  'include/vhd.h',
  'include/libvhd.h',
  'include/libvhd-index.h',
  'include/libvhd-journal.h',
  'include/vhd-util.h',
  'include/list.h',
  subdir : 'vhd',
)

install_headers(
  'include/blktap2.h',
  'include/blktaplib.h',
  'include/blktap3.h',
  'include/xen_blkif.h',
  'include/tapdisk-message.h',
  'include/tap-ctl.h',
  'include/debug.h',
  'include/util.h',
  'drivers/tapdisk-metrics-stats.h',
  subdir : 'blktap',
)
