project(
  'zip-utils',
  'c',
  version: '6.0-modern',
  default_options: [
    'c_std=c99',
    'warning_level=2',
    'optimization=2'
  ]
)

debug_sanitize = get_option('debug_sanitize')

extra_c_args = []
extra_link_args = ['-Wl,--as-needed']

if debug_sanitize
  extra_c_args += [
    '-fsanitize=address',
    '-fsanitize=undefined',
    '-fno-omit-frame-pointer',
    '-g',
    '-O1'
  ]

  extra_link_args = [
    '-Wl,--as-needed',
    '-fsanitize=address',
    '-fsanitize=undefined'
  ]
endif

inc_src = include_directories('src/include', 'src/common', 'src/compression', 'src/format')

core_sources = files(
  'src/common/status.c',
  'src/common/strlist.c',
  'src/common/ctx.c',
  'src/common/fileio.c',
  'src/compression/zlib_shim.c',
  'src/compression/bzip2_shim.c',
  'src/compression/crc32.c',
  'src/compression/zipcrypto.c',
  'src/format/reader.c',
  'src/format/writer.c'
)

zlib_dep = dependency('zlib')
cc = meson.get_compiler('c')
bz2_dep = cc.find_library('bz2', required: true)

libziputils = static_library(
  'ziputils',
  core_sources,
  include_directories: [inc_src],
  dependencies: [zlib_dep, bz2_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)

zip_exe = executable(
  'zip',
  files('src/cli/main_zip.c', 'src/cli/ops.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep, bz2_dep],
  c_args: extra_c_args,
  link_args: extra_link_args,
  install: true
)

unzip_exe = executable(
  'unzip',
  files('src/cli/main_unzip.c', 'src/cli/ops.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep, bz2_dep],
  c_args: extra_c_args,
  link_args: extra_link_args,
  install: true
)

#meson.add_install_script('build-aux/install-symlinks.sh')

# --- Tests ---

test_compression_exe = executable(
  'test_compression',
  files('tests/test_compression.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
test('unit-compression', test_compression_exe)

test_list_exe = executable(
  'test_list',
  files('tests/test_list.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
# Manual C harness is still present but unused in automated tests.

test_fileio_exe = executable(
  'test_fileio',
  files('tests/test_fileio.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
test('unit-fileio', test_fileio_exe)

python3 = import('python').find_installation('python3')
test('integration-list', python3,
  args: [meson.project_source_root() / 'tests/test_list_integration.py'],
  env: {
    'ZIP_BIN': 'zip',
    'UNZIP_BIN': 'unzip',
    'READER_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-zip64', python3,
  args: [meson.project_source_root() / 'tests/test_zip64_integration.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-zip64-large', python3,
  args: [meson.project_source_root() / 'tests/test_zip64_large.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 400,
  suite: ['long']
)

test('integration-create', python3,
  args: [meson.project_source_root() / 'tests/test_create_integration.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_BIN': 'unzip'
  },
  timeout: 30
)

test('integration-streaming', python3,
  args: [meson.project_source_root() / 'tests/test_streaming.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-logging', python3,
  args: [meson.project_source_root() / 'tests/test_logging.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-time-filtering', python3,
  args: [meson.project_source_root() / 'tests/test_time_filtering.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-output-copy', python3,
  args: [meson.project_source_root() / 'tests/test_output_copy.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-move', python3,
  args: [meson.project_source_root() / 'tests/test_move_option.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-comments', python3,
  args: [meson.project_source_root() / 'tests/test_comments.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-entry-comments', python3,
  args: [meson.project_source_root() / 'tests/test_entry_comments.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-unzip', python3,
  args: [meson.project_source_root() / 'tests/test_unzip_integration.py'],
  env: {
    'ZIP_BIN': 'zip',
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-unzip-filter', python3,
  args: [meson.project_source_root() / 'tests/test_unzip_filter.py'],
  env: {
    'ZIP_BIN': 'zip',
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-zipinfo', python3,
  args: [meson.project_source_root() / 'tests/test_zipinfo_integration.py'],
  env: {
    'ZIP_BIN': 'zip',
    'ZIPINFO_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-crypto', python3,
  args: [meson.project_source_root() / 'tests/test_crypto_integration.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 30
)
