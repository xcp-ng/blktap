
#CFLAGS:=$(filter-out -fsanitize=leak,$(CFLAGS))
#LDFLAGS:=$(filter-out -static-liblsan,$(LDFLAGS))

mockatests_include = include_directories('include')

cmocka_dep = dependency('cmocka')

# Debug symbols and light optimization make the wrapped test binaries
# easier to step through while still catching -O-dependent bugs.
mocka_override_options = ['debug=true', 'optimization=g']

subdir('wrappers')
if enable_tests == true
  gcov_dep = compiler.find_library('gcov')

  subdir('drivers')
  subdir('cbt')
  subdir('control')
  subdir('vhd')
endif
