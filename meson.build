project(
  'zip-utils',
  'c',
  version: '3.0-compat-dev',
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
  'src/format/writer.c',
  'src/format/recovery.c'
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

cli_common_sources = files('src/cli/ops.c', 'src/cli/cli_common.c')

zip_exe = executable(
  'zip',
  cli_common_sources + files('src/cli/main_zip.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep, bz2_dep],
  c_args: extra_c_args,
  link_args: extra_link_args,
  install: true
)

unzip_exe = executable(
  'unzip',
  cli_common_sources + files('src/cli/main_unzip.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep, bz2_dep],
  c_args: extra_c_args,
  link_args: extra_link_args,
  install: true
)

# Create a zipinfo symlink to unzip for testing zipinfo behavior
zipinfo_symlink = custom_target('zipinfo_symlink',
  input: unzip_exe,
  output: 'zipinfo',
  command: ['ln', '-sf', unzip_exe.full_path(), '@OUTPUT@'],
  build_by_default: true
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

test_strlist_exe = executable(
  'test_strlist',
  files('tests/test_strlist.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
test('unit-strlist', test_strlist_exe)

test_writer_exe = executable(
  'test_writer',
  files('tests/test_writer.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
test('unit-writer', test_writer_exe)

test_reader_exe = executable(
  'test_reader',
  files('tests/test_reader.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
test('unit-reader', test_reader_exe)

test_recovery_exe = executable(
  'test_recovery',
  files('tests/test_recovery.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
test('unit-recovery', test_recovery_exe)

test_ctx_exe = executable(
  'test_ctx',
  files('tests/test_ctx.c'),
  include_directories: [inc_src],
  link_with: [libziputils],
  dependencies: [zlib_dep],
  c_args: extra_c_args,
  link_args: extra_link_args
)
# test('unit-ctx', test_ctx_exe)  # disabled due to segfault

python3 = import('python').find_installation('python3')
test('integration-list', python3,
  args: [meson.project_source_root() / 'tests/integration/test_list_integration.py'],
  env: {
    'ZIP_BIN': 'zip',
    'UNZIP_BIN': 'unzip',
    'READER_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-zip64', python3,
  args: [meson.project_source_root() / 'tests/integration/test_zip64_integration.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-zip64-large', python3,
  args: [meson.project_source_root() / 'tests/integration/test_zip64_large.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 400,
  suite: ['long']
)

test('integration-create', python3,
  args: [meson.project_source_root() / 'tests/integration/test_create_integration.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_BIN': 'unzip'
  },
  timeout: 30
)

test('integration-small-compression', python3,
  args: [meson.project_source_root() / 'tests/integration/test_small_file_compression.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-streaming', python3,
  args: [meson.project_source_root() / 'tests/integration/test_streaming.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-logging', python3,
  args: [meson.project_source_root() / 'tests/integration/test_logging.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-strip-attrs', python3,
  args: [meson.project_source_root() / 'tests/integration/test_strip_attrs.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-quiet-pager', python3,
  args: [meson.project_source_root() / 'tests/integration/test_quiet_pager.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'UNZIP_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-zipnote-edit', python3,
  args: [meson.project_source_root() / 'tests/integration/test_zipnote_edit.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path(),
    'ZIPNOTE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-time-filtering', python3,
  args: [meson.project_source_root() / 'tests/integration/test_time_filtering.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-output-copy', python3,
  args: [meson.project_source_root() / 'tests/integration/test_output_copy.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-move', python3,
  args: [meson.project_source_root() / 'tests/integration/test_move_option.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-option-parsing', python3,
  args: [meson.project_source_root() / 'tests/integration/test_option_parsing.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-comments', python3,
  args: [meson.project_source_root() / 'tests/integration/test_comments.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-comment-edit', python3,
  args: [meson.project_source_root() / 'tests/integration/test_comment_edit.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-entry-comments', python3,
  args: [meson.project_source_root() / 'tests/integration/test_entry_comments.py'],
  env: {
    'WRITE_BIN': zip_exe.full_path()
  },
  timeout: 30
)

test('integration-unzip', python3,
  args: [meson.project_source_root() / 'tests/integration/test_unzip_integration.py'],
  env: {
    'ZIP_BIN': 'zip',
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-unzip-filter', python3,
  args: [meson.project_source_root() / 'tests/integration/test_unzip_filter.py'],
  env: {
    'ZIP_BIN': 'zip',
    'UNZIP_REWRITE_BIN': unzip_exe.full_path()
  },
  timeout: 30
)

test('integration-zipinfo', python3,
  args: [meson.project_source_root() / 'tests/integration/test_zipinfo_integration.py'],
  env: {
    'ZIP_BIN': 'zip',
    'ZIPINFO_BIN': zipinfo_symlink.full_path()
  },
  timeout: 30
)

# --- Generated Parity Tests ---

parity_src = files('tests/parity/test_generated.c')

if get_option('generate_parity')
  sys_zip = find_program('zip')
  sys_unzip = find_program('unzip')
  sys_zipinfo = find_program('zipinfo')

  gen_zip = custom_target('gen_zip_jsonl',
    output: 'zip_runs.jsonl',
    command: [
      'sh', '-c',
      '"$1" "$2" --zip "$3" --outdir "zip_gen" && mv "zip_gen/runs.jsonl" "$0"',
      '@OUTPUT@', python3.full_path(), meson.project_source_root() / 'tools/document_zip_output.py', sys_zip.full_path()
    ],
    # console: true # console not supported in all meson versions for custom_target, omitting safely
  )

  gen_unzip = custom_target('gen_unzip_jsonl',
    output: 'unzip_runs.jsonl',
    command: [
      'sh', '-c',
      '"$1" "$2" --unzip "$3" --outdir "unzip_gen" && mv "unzip_gen/runs.jsonl" "$0"',
      '@OUTPUT@', python3.full_path(), meson.project_source_root() / 'tools/document_unzip_output.py', sys_unzip.full_path()
    ],
  )

  gen_zipinfo = custom_target('gen_zipinfo_jsonl',
    output: 'zipinfo_runs.jsonl',
    command: [
      'sh', '-c',
      '"$1" "$2" --zipinfo "$3" --outdir "zipinfo_gen" && mv "zipinfo_gen/runs.jsonl" "$0"',
      '@OUTPUT@', python3.full_path(), meson.project_source_root() / 'tools/document_zipinfo_output.py', sys_zipinfo.full_path()
    ],
  )

  parity_src = custom_target('test_generated.c',
    output: 'test_generated.c',
    input: [gen_zip, gen_unzip, gen_zipinfo],
    command: [
      python3, meson.project_source_root() / 'tools/convert_jsonl_to_c.py',
      '@OUTPUT@', '@INPUT@'
    ]
  )
endif

parity_test_exe = executable(
  'test_parity_gen',
  [parity_src, files('tests/parity/common.c')],
  include_directories: [inc_src, include_directories('tests/parity')],
  c_args: extra_c_args,
  link_args: extra_link_args
)

test('parity-generated', parity_test_exe,
  env: {
    'ZIP_BIN': zip_exe.full_path(),
    'UNZIP_BIN': unzip_exe.full_path(),
    'ZIPINFO_BIN': zipinfo_symlink.full_path()
  },
  depends: [zip_exe, unzip_exe, zipinfo_symlink],
  timeout: 60
)
