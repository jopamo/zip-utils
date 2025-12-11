<p align="center">
  <img src="zip-utils.svg" alt="InfoZIP Utils logo" width="220">
</p>

# ZIP Utils

Modern rewrite of Info-ZIP's `zip`/`unzip` with a reentrant core (`libziputils`) and thin CLI front-ends aimed at drop-in compatibility.

---

## Scope and status

- POSIX-first rewrite that keeps the Info-ZIP UX but aims for cleaner internals; unsupported options fail fast instead of silently diverging.
- `zip`, `unzip`, and `zipinfo` are the primary entry points; see `MISSING_FEATURES.md` and `OPTION_MATRIX.md` for parity gaps.
- Library-first design: `libziputils` exposes the core behaviors, while the CLIs stay thin.

## Quick start

```bash
meson setup build
ninja -C build
meson test -C build
```

Targets live in `build/`: `zip`, `unzip`, the `zipinfo` alias, and `libziputils.a`. Use a separate dir (e.g., `build-sanitize`) for sanitizer builds.

## Documentation

- `HACKING.md`: development workflow, coding style, and test expectations.
- `MISSING_FEATURES.md`: high-level parity gaps and backlog priorities.
- `OPTION_MATRIX.md`: per-flag status against Info-ZIP.
- `src/README.md`: library layout and ownership notes.

## Highlights (current state)

- **Reentrant core**: `libziputils` powers `zip`, `unzip`, and `zipinfo` behavior with minimal global state.
- **Archive handling**: deflate/store/bzip2, recursion/junk-path toggles, include/exclude globs, move/delete sources with `-m`, update/freshen/file-sync flows, copy-on-write output (`-O`), and stdin/stdout streaming when the path is `-`.
- **Zip64 + recovery**: writes and reads Zip64 EOCD/locators, handles >4 GiB entries, optional split volumes (`-s`/`-sp`), and recovery helpers (`-F`/`-FF`) to rebuild damaged archives.
- **Integrity and metadata**: `zip -T`/`unzip -t`, preserves POSIX mode bits and mtimes on extract, time filters (`-t`/`-tt`), overwrite guards (`-n`/`-o`), and quiet/verbose logging including `-lf`/`-la`/`-li`.
- **Crypto**: Legacy ZipCrypto encryption/decryption (`zip -e/-P`, `unzip -P`) with password prompts and clear errors for bad keys.
- **Listings**: Zipinfo-compatible listings via `unzip -Z` or the `zipinfo` alias: short/medium/long/verbose formats, names-only output, totals-only mode, decimal timestamps, and archive comment display.

---

## Usage examples

```bash
# Create/update an archive from files/dirs
build/zip archive.zip src/ README.md

# Extract with overwrite prompts and quiet logging
build/unzip archive.zip

# Zipinfo-style listing
build/zipinfo -l archive.zip
```

See `tests/` for more CLI parity scenarios.

---

## Build

### Requirements

* Meson >= 1.2
* Ninja
* zlib and bzip2 development headers
* Python 3 (for tests)

### Steps

Default build (same commands as the quick start):

```bash
meson setup build
ninja -C build
```

Outputs: `build/zip`, `build/unzip`, and `libziputils.a`. `meson install -C build` installs the binaries (the static library stays local).

For an AddressSanitizer/UBSan build, keep a separate directory:

```bash
meson setup build-sanitize -Ddebug_sanitize=true
ninja -C build-sanitize
```

---

## Testing

Run C unit tests plus Python integration tests:

```bash
meson test -C build
```

Several Python tests compare against the system `zip`/`unzip` (Info-ZIP 6.x) on your `PATH`. Long Zip64 coverage lives in the `long` suite; enable the large-file run with:

```bash
ZU_RUN_LARGE_TESTS=1 ZU_LARGE_SIZE_GB=5 meson test -C build --suite long
```

Target a single test with `meson test -C build integration-create -v`.

List suites with `meson test -C build --list`; the `integration` suite exercises CLI parity paths.

---

## Contributing

See `HACKING.md` for development/testing expectations. Behavioral parity tracking lives in `MISSING_FEATURES.md` and `OPTION_MATRIX.md`; keep those documents current when closing gaps.

---

## Current differences vs Info-ZIP 6.0

- Focused on POSIX hosts; platform-specific behaviors (VMS, OS/2, Windows ACLs/EAs) are not implemented.
- Supported methods are deflate/store/bzip2 with legacy ZipCrypto only; AES and newer compression methods are not yet handled.
- `zipnote`, `zipcloak`, and `zipsplit` are recognized aliases but remain minimal (entry comments not edited, no re-encryption; split read/write is supported but disk metadata is simplified).
- Archive comments supported via `zip -z` (including comment-only edits) and preserved on rewrite; entry comments are preserved but not editable yet.
- Option surface is intentionally smaller for now; unsupported flags fail fast with usage until parity work lands.

---

## Repository Layout

- `src/cli/`: CLI entry points for `zip`, `unzip`, and `zipinfo` alias handling.
- `src/common/`: context object, logging, string lists, and file I/O helpers.
- `src/compression/`: deflate/bzip2 shims, CRC32, and ZipCrypto.
- `src/format/`: zip reader/writer implementations (central directory, Zip64, split volumes).
- `src/include/`: public headers for `libziputils`.
- `tests/`: C unit tests and Python integration tests (see `HACKING.md`).
- `zip.txt`, `unzip.txt`: upstream Info-ZIP man pages for behavior reference.
- `MISSING_FEATURES.md`: status of larger compatibility gaps.

---

## License

Distributed under the permissive Info-ZIP license used by the original utilities (see the license block in `zip.txt`/`unzip.txt`). A dedicated LICENSE file will be added as the rewrite stabilizes.

---

## Acknowledgments

Thanks to the original Info-ZIP contributors and the broader community keeping classic ZIP tooling alive on modern systems.
