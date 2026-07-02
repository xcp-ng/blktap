blktapctl_sources = [
    'tap-ctl-ipc.c',
    'tap-ctl-list.c',
    'tap-ctl-allocate.c',
    'tap-ctl-free.c',
    'tap-ctl-create.c',
    'tap-ctl-destroy.c',
    'tap-ctl-spawn.c',
    'tap-ctl-attach.c',
    'tap-ctl-detach.c',
    'tap-ctl-open.c',
    'tap-ctl-close.c',
    'tap-ctl-pause.c',
    'tap-ctl-unpause.c',
    'tap-ctl-major.c',
    'tap-ctl-check.c',
    'tap-ctl-stats.c',
    'tap-ctl-xen.c',
    'tap-ctl-info.c',
    'tap-ctl-commit.c',
    ]

libexecdir = get_option('prefix') / get_option('libexecdir')

c_flags = [
  '-D_GNU_SOURCE',
  '-DTAPCTL',
  '-DTAPDISK_EXEC="tapdisk"',
  '-DTAPDISK_EXECDIR="@0@"'.format(libexecdir),
]

blktapctl_libs = both_libraries('blktapctl',
  blktapctl_sources,
  include_directories : incdir,
  c_args : c_flags,
  version : '0.1.1',
  override_options : [
    'debug=true',
  ],
)

tap_ctl_sources = 'tap-ctl.c'

executable('tap-ctl', tap_ctl_sources,
  link_with : blktapctl_libs.get_shared_lib(),
  include_directories : incdir,
  c_args : c_flags,
  override_options : [
    'debug=true',
  ],
)
