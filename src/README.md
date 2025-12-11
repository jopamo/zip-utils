# Zip Utils Core

`libziputils` is the reentrant core powering the `zip`, `unzip`, and `zipinfo` front-ends. The CLIs are thin shells over the library; there is no legacy code tree here.

## Layout

- `src/include/`: public headers (`ziputils.h` error codes plus shared structs).
- `src/common/`: context, logging, string lists, and file I/O helpers.
- `src/compression/`: deflate/bzip2 shims, CRC32, and ZipCrypto.
- `src/format/`: archive reader/writer implementations (central directory handling, Zip64, split archives, streaming).
- `src/cli/`: option parsing and entry points that populate `ZContext` and call into the core.

## Build & Test

```bash
meson setup build
ninja -C build
meson test -C build
```

Use separate build dirs for variants (e.g., `build`, `build-sanitize`). `meson test` runs both the C unit tests and Python integration tests under `tests/`.

## Behavior Notes

- `ZContext` carries all configuration for an operation; CLI parsers fill it and then invoke writer/reader helpers.
- Writer (`src/format/writer.c`) owns create/update flows, copy-on-write output (`-O`), split volumes, ZipCrypto, and streaming stdin/stdout via data descriptors when the path is `-`.
- Reader (`src/format/reader.c`) owns listing, testing, extraction, and `zipinfo` formatting (including decimal-time and pager-aware modes).
- Integration tests exercise the binaries against the system `zip`/`unzip` for parity coverage; see `HACKING.md` when adding new tests or changing behavior.
