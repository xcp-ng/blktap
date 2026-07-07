
test_control_sources = [
  'test-control.c',
  'test-tap-ctl-free.c',
  'test-tap-ctl-allocate.c',
  'test-tap-ctl-close.c',
  'test-tap-ctl-list.c',
  'control-wrappers.c',
  'util.c',
  ]

test_control = executable('test-control', test_control_sources,
  include_directories : [
    incdir,
    mockatests_include,
  ],
  c_args : [
    '-fno-inline',
  ] + c_args_tests,
  dependencies : [
    cmocka_dep,
    gcov_dep,
  ],
  link_with : [
    blktapctl_libs.get_static_lib(),
    wrappers_slib,
  ],
  link_args : [
    #'-static-libtool-libs',
    # Would be good to use the cmocka malloc wraps but looks like maybe strdup doesn't call malloc
    #'-Wl,--wrap=malloc,--wrap=free',
    '-Wl,--wrap=socket,--wrap=connect,--wrap=read,--wrap=select,--wrap=write,--wrap=fdopen',
    '-Wl,--wrap=open,--wrap=ioctl,--wrap=close,--wrap=access,--wrap=mkdir,--wrap=flock,--wrap=unlink,--wrap=mknod,--wrap=__xmknod',
    #'-Wl,--wrap=execl,--wrap=waitpid',
    '-Wl,--wrap=glob,--wrap=globfree',
    '-Wl,--wrap=fopen,--wrap=fclose,--wrap=fseek,--wrap=fwrite',
  ],
  override_options : mocka_override_options,
)
test('test_control', test_control)
