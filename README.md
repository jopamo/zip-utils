<p align="center">
  <img src="zip-utils.svg" alt="InfoZIP Utils logo" width="220">
</p>

# zip-utils

[![CI with Clang + Sanitizers](https://github.com/jopamo/zip-utils/actions/workflows/ci.yml/badge.svg)](https://github.com/jopamo/zip-utils/actions/workflows/ci.yml)
![Language](https://img.shields.io/badge/language-C-blue.svg)
![Build System](https://img.shields.io/badge/build%20system-Meson-orange.svg)

Modern rewrite of Info-ZIP's `zip`, `unzip`, and `zipinfo` with a reentrant core (`libziputils`) and thin CLIs aimed at drop-in compatibility.

## Scope and Compatibility
- Targets Info-ZIP `zip` 3.0 / `unzip` 6.0 behavior; unsupported flags fail fast instead of silently diverging.
- Library-first design: CLIs only parse args and populate `ZContext`, leaving core logic in `libziputils`.
- Track parity gaps and flag coverage in `CHECKLIST.md`; protocol notes live in `zip-spec.md` and `unzip-spec.md`.
- Split archives are not supported; `-s`/`--split-size` and related options return a not-implemented error.

## Requirements
- Meson ≥ 1.2 and Ninja
- zlib and bzip2 development headers
- Python 3 for integration tests

## Quick Start
```bash
meson setup build          # configure (one build dir per config)
ninja -C build             # compile
meson test -C build        # C unit tests + Python integration tests
```
Artifacts appear in `build/`: `zip`, `unzip`, `zipinfo` (alias), and `libziputils.a`. For sanitizer builds, keep a separate dir such as:
```bash
meson setup build-sanitize -Ddebug_sanitize=true
ninja -C build-sanitize
```

For optimized release binaries, use a dedicated dir with LTO and higher optimization:
```bash
meson setup build-release --buildtype=release -Doptimization=3 -Db_lto=true
ninja -C build-release
```
Optionally add `-Dstrip=true`, `-Db_pie=true`, or `-Dcpp_args=-march=native` when portability is not a concern.

Performance shortcuts:
- `ZU_FAST_WRITE=0` disables the default fast-write heuristics (streaming + small-file fast compression).
- `ZU_FAST_WRITE_THRESHOLD=<bytes>` tunes the small-file fast-write cutoff (default 524288).

## Usage Examples
- Create an archive: `build/zip out.zip file1.txt dir/`
- List contents: `build/unzip -l out.zip`
- Extract with paths: `build/unzip out.zip`
- Stream to stdout: `build/unzip -p out.zip path/in/archive > file`
- Keep verbose logging: `build/zip -lf log.txt -li out.zip file1`

## Repository Layout
- `src/include/`: public headers (`ziputils.h`, shared structs and error codes)
- `src/common/`: context, logging, string lists, file I/O helpers
- `src/compression/`: deflate/bzip2 shims, CRC32, ZipCrypto
- `src/format/`: archive reader/writer (Zip64, streaming)
- `src/cli/`: CLI entry points and option parsing
- `tests/`: C unit tests (`test_fileio.c`, etc.) and Python integration tests (`test_create_integration.py`, `test_zipinfo_integration.py`, ...). Tests invoke built binaries and, when available, system Info-ZIP binaries on `PATH` for parity.
- `tools/`: small helpers for documenting and comparing CLI output

## Testing
- Run `meson test -C build` before sending changes; it drives both C and Python suites.
- Large Zip64 coverage: `ZU_RUN_LARGE_TESTS=1 ZU_LARGE_SIZE_GB=5 meson test -C build --suite long`.
- List suites: `meson test -C build --list`; target one test: `meson test -C build integration-create -v`.

## Contributing
Follow `HACKING.md` for workflow, style (Chromium clang-format with 4-space indents, no tabs, 200 cols), and test expectations. Keep CLIs thin, thread state through `ZContext`, and document any intentional divergences from Info-ZIP in tests and parity docs.
