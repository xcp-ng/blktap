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

uuid_dep = dependency('uuid')

subdir('cbt')
subdir('control')
subdir('cpumond')
subdir('lvm')
subdir('vhd')
subdir('qcow2')
subdir('drivers')
subdir('tapback')
subdir('mockatests')

#doc = README README.md LICENSE CONTRIB MAINTAINERS

#TODO: uuid dep in drivers, debug, tests, install

# install_headers(
#   'include/vhd.h',
#   'include/libvhd.h',
#   'include/libvhd-index.h',
#   'include/libvhd-journal.h',
#   'include/vhd-util.h',
#   'include/list.h',
#   subdir : 'vhd',
# )
#
# install_headers(
#   'include/blktap2.h',
#   'include/blktaplib.h',
#   'include/blktap3.h',
#   'include/xen_blkif.h',
#   'include/tapdisk-message.h',
#   'include/tap-ctl.h',
#   'include/debug.h',
#   'include/util.h',
#   'drivers/tapdisk-metrics-stats.h',
#   subdir : 'blktap',
# )
