<p align="center">
  <img src="zip-utils.svg" alt="InfoZIP Utils logo" width="220">
</p>

# ZIP Utils

Modern rewrite of Info-ZIP's `zip`/`unzip` with a reentrant core (`libziputils`) and thin CLI front-ends aimed at drop-in compatibility.

---

## Highlights (current state)

- Reentrant `libziputils` core powers `zip`, `unzip`, and `zipinfo` behavior with minimal global state.
- Archive creation and updates with deflate/store/bzip2, recursion/junk-path toggles, include/exclude globbing, update/freshen/file-sync flows, copy-on-write output (`-O`), and stdin/stdout streaming when the path is `-`.
- Zip64 everywhere: writes and reads Zip64 EOCD/locators, handles >4 GiB entries, optional split volumes (`-s`/`-sp`), and recovery helpers (`-F`/`-FF`) to rebuild damaged archives.
- Integrity and metadata: `zip -T`/`unzip -t`, preserves POSIX mode bits and mtimes on extract, time filters (`-t`/`-tt`), overwrite guards (`-n`/`-o`), and quiet/verbose logging including `-lf`/`-la`/`-li`.
- Legacy ZipCrypto encryption/decryption (`zip -e/-P`, `unzip -P`) with password prompts and clear errors for bad keys.
- Zipinfo-compatible listings via `unzip -Z` or the `zipinfo` alias: short/medium/long/verbose formats, names-only output, totals-only mode, decimal timestamps, and archive comment display.

---

## Build

### Requirements

* Meson ≥ 1.2
* Ninja
* zlib and bzip2 development headers
* Python 3 (for tests)

### Steps

```bash
meson setup build
ninja -C build
```

Outputs: `build/zip`, `build/unzip`, and `libziputils.a`. `meson install -C build` installs the binaries (the static library stays local). For an AddressSanitizer/UBSan build:

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

---

## Differences from Info-ZIP 6.0 (planned)

- Focused on POSIX hosts; platform-specific behaviors (VMS, OS/2, Windows ACLs/EAs) are not implemented.
- Supported methods are deflate/store/bzip2 with legacy ZipCrypto only; AES and newer compression methods are not yet handled.
- `zipnote`, `zipcloak`, and `zipsplit` are recognized aliases but remain minimal (no comment editing, no re-encryption, split handling is new).
- Archive/entry comments are read for display but not yet written or preserved when rewriting.
- Option surface is intentionally smaller for now; unsupported flags fail fast with usage until parity work lands.

---

## Repository Layout

- `src/cli/` — CLI entry points for `zip`, `unzip`, and `zipinfo` alias handling.
- `src/common/` — context object, logging, string lists, and file I/O helpers.
- `src/compression/` — deflate/bzip2 shims, CRC32, and ZipCrypto.
- `src/format/` — zip reader/writer implementations (central directory, Zip64, split volumes).
- `src/include/` — public headers for `libziputils`.
- `tests/` — C unit tests and Python integration tests (see `HACKING.md`).
- `zip.txt`, `unzip.txt` — upstream Info-ZIP man pages for behavior reference.
- `MISSING_FEATURES.md` — status of larger compatibility gaps.

---

## License

Distributed under the permissive Info-ZIP license used by the original utilities (see the license block in `zip.txt`/`unzip.txt`). A dedicated LICENSE file will be added as the rewrite stabilizes.

---

## Acknowledgments

Thanks to the original Info-ZIP contributors and the broader community keeping classic ZIP tooling alive on modern systems.
