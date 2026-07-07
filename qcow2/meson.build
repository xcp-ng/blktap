subdir('lib')

qemu_img_sources = [
  'qemu-img.c',
  'qemu-progress.c',
  ]

executable('qemu-img', qemu_img_sources,
  include_directories : [
    incdir,
    qcow2_include,
  ],
  link_with : qcow2_libs.get_shared_lib(),
  dependencies : [
    glib_dep,
    pthread_dep,
  ],
  install : true,
  override_options : [
    'werror=false',
  ],
)
