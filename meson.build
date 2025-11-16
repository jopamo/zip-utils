project(
  'zip-unzip',
  'c',
  version: '6.0-custom',
  default_options: [
    'c_std=c99',
    'warning_level=2',
    'optimization=2'
  ]
)

debug_sanitize = get_option('debug_sanitize')

# select compiler
if debug_sanitize
  # force clang when sanitizer mode is on
  cc = meson.get_compiler('c', native: true)
  if cc.get_id() != 'clang'
    cc = meson.get_compiler('c', native: true, default_options : ['cc=clang'])
  endif
  # note: if builddir was first configured with gcc, Meson will complain
  # the right workflow is:
  #   CC=clang meson setup builddir -Ddebug_sanitize=true
  # from a clean builddir
else
  cc = meson.get_compiler('c')
endif

# try to find bzip2 in a portable way
bz2_dep = dependency('bzip2', required: false)

if not bz2_dep.found()
  # pkg-config / cmake didn't work, try bare -lbz2
  bz2_lib = cc.find_library('bz2', required: false)
  if bz2_lib.found()
    bz2_dep = bz2_lib
  endif
endif

if not bz2_dep.found()
  error('fatal: libbz2 (bzip2) dev headers/libs not found. Install libbz2-dev / bzip2-devel')
endif

# common defines for both targets
common_defs = [
  '-DUNIX',
  '-DUSE_BZIP2',          # we only define this because we just verified bz2_dep exists
  '-D_XOPEN_SOURCE=700',
  '-D_DEFAULT_SOURCE'
]

# unzip flags
unzip_defs = common_defs + [
  '-DUNZIP',
  '-DCRYPT',
  '-DUNICODE_SUPPORT',
  '-DNO_ZIPINFO',
  '-DNO_ZIPGREP',
  '-DNO_LCHOWN',
  '-DNO_LCHMOD',
  '-DDYNALLOC_CRCTAB',
  '-include', 'utime.h'
]

# zip flags
# do not pass -DZIP if zip/zip.h already defines it
zip_defs = common_defs + [
  '-DCRYPT',
  '-DUNICODE_SUPPORT'
]

# sanitizer / link flags
extra_c_args   = []
extra_link_args = ['-Wl,--as-needed', '-Wl,--no-undefined']

if debug_sanitize
  # AddressSanitizer + UBSan + debug-ish
  extra_c_args += [
    '-fsanitize=address',
    '-fsanitize=undefined',
    '-fno-omit-frame-pointer',
    '-g',
    '-O1'
  ]

  extra_link_args = [
    '-Wl,--as-needed',
    # drop --no-undefined because ASan runtime is pulled in later and this can fail
    '-fsanitize=address',
    '-fsanitize=undefined'
  ]
endif

inc_unzip   = include_directories('unzip')
inc_zip     = include_directories('zip')
inc_common  = include_directories('common')

unzip_sources = files(
  'unzip/unzip.c',
  'unzip/extract.c',
  'unzip/process.c',
  'unzip/fileio.c',
  'unzip/list.c',
  'unzip/match.c',
  'unzip/inflate.c',
  'unzip/explode.c',
  'unzip/unreduce.c',
  'unzip/unshrink.c',
  'common/crypt.c',
  'common/crc32.c',
  'unzip/envargs.c',
  'unzip/globals.c',
  'unzip/timezone.c',
  'unzip/unix.c',
  'unzip/ubz2err.c',
  'common/ttyio.c'
)

zip_sources = files(
  'zip/zip.c',
  'zip/zipfile.c',
  'zip/zipup.c',
  'zip/fileio.c',
  'zip/util.c',
  'zip/globals.c',
  'common/crypt.c',
  'common/crc32.c',
  'zip/zbz2err.c',
  'zip/unix.c',
  'zip/deflate.c',
  'zip/trees.c',
  'common/ttyio.c'
)

unzip_exe = executable(
  'unzip',
  unzip_sources,
  include_directories: [inc_unzip, inc_common],
  dependencies: [bz2_dep],
  c_args: unzip_defs + extra_c_args,
  link_args: extra_link_args,
  install: true
)

zip_exe = executable(
  'zip',
  zip_sources,
  include_directories: [inc_zip, inc_common],
  dependencies: [bz2_dep],
  c_args: zip_defs + extra_c_args,
  link_args: extra_link_args,
  install: true
)

install_data('man/unzip.1', install_dir: get_option('mandir') / 'man1')
install_data('man/zip.1',   install_dir: get_option('mandir') / 'man1')

test_script = find_program('test.sh', dirs: meson.project_source_root())

test_env = environment()
test_env.set('ZIP_BIN', zip_exe.full_path())
test_env.set('UNZIP_BIN', unzip_exe.full_path())

test(
  'integration-test-script',
  test_script,
  env: test_env,
  workdir: meson.project_build_root()
)
