# Parallel Rewrite Checklist (Strangler Fig)

Tracking the clean rewrite that lives beside the legacy code until it can take over.

---

## Setup

* [x] Move existing sources into `legacy/` for reference only
* [x] Create new `src/` layout:
  * `src/include/ziputils.h`
  * `src/common/` (ctx, file I/O, strings)
  * `src/compression/` (zlib shims, crc32)
  * `src/format/` (ZIP container parsing/writing)
  * `src/cli/` (thin zip/unzip wrappers)
* [x] Update `meson.build` to build only `src/` (legacy ignored) and produce library + CLIs

---

## Phase 1 — Core (Context & I/O)

* [x] Define `ZContext` (no globals) in `src/common/ctx.h`
* [x] Implement safe file I/O helpers that record errors in `ZContext`
* [x] Add unit coverage for basic open/read/write paths (gate before legacy removal)

---

## Phase 2 — Engine (Compression)

* [x] Implement zlib adapter in `src/compression/zlib_shim.c`
* [x] Wrap CRC32 in `src/compression/crc32.c`
* [x] Unit test: compress/decompress “hello world” and verify CRC/round-trip

---

## Phase 3 — Container (Reading)

* [x] Define clean ZIP header structs in `src/format/zip_headers.h` (stdint types, packed)
* [x] Implement central directory reader (`zu_list_archive(ctx)`) for `zipinfo`-style listing (Zip64-aware)
* [x] Integration test: list filenames from a sample archive matches `zipfile`/legacy output; quiet/verbose flags covered

---

## Phase 4 — Container (Writing)

* [x] Implement local header writer and data path: headers → zlib → footer
* [x] Support split between stdout and file targets (no Zip64 yet)
* [x] Integration test: create archive from temp dir, validate with `unzip -t`

---

## Phase 5 — Interface (CLIs + API)

* [x] Publish public API in `src/include/ziputils.h` (documented signatures for list/test/extract/write)
* [x] Implement thin `src/cli/main_zip.c` and `src/cli/main_unzip.c` using `ZContext` (ops now list/create/extract/test)
* [x] Mirror legacy CLI options/exit codes (including `zipinfo` flag shape); expand integration coverage for CLI parity
* [x] Provide `zipinfo`-compatible listing mode output backed by the new reader

---

## Notes

* Legacy tree is reference-only; new code must remain reentrant and global-free.
* Favor incremental tests per layer (common → compression → format → CLI).
* What’s left before deleting `legacy/`: finalize CLI/option parity (including `zipinfo` formatting), Zip64 support for writes, and any extra I/O/error coverage as helpers grow.
