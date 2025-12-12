# Repository Guidelines

## Project Structure & Module Organization
Core code lives in `src/`: `src/include/` exposes public headers, `src/common/` holds context/logging/file I/O helpers, `src/compression/` wraps deflate/bzip2/CRC/ZipCrypto, `src/format/` owns archive read/write (Zip64 and streaming), and `src/cli/` stays thin by wiring args into `ZContext`. Tests sit in `tests/` with C unit tests (`test_list.c`, `test_fileio.c`, `test_compression.c`) and Python integration scripts (`test_create_integration.py`, `test_zipinfo_integration.py`, etc.) that invoke the built binaries. Parity tracking docs live in `CHECKLIST.md`; protocol and CLI behavior details are captured in `zip-spec.md` and `unzip-spec.md`—consult them when changing flags, prompts, or archive semantics. For navigation, `.llm/meta.jsonl` indexes symbols and doc sections.

## Build, Test, and Development Commands
Default flow (keep a clean dir per config):  
```bash
meson setup build
ninja -C build
meson test -C build
```
Artifacts land in `build/` (`zip`, `unzip`, `zipinfo`, `libziputils.a`). For ASan/UBSan builds, use a separate dir such as `build-sanitize`: `meson setup build-sanitize -Ddebug_sanitize=true && ninja -C build-sanitize`. List tests with `meson test -C build --list`; target one with `meson test -C build integration-create -v`.

## Coding Style & Naming Conventions
Format C with `.clang-format` (Chromium style, 4-space indents, no tabs, 200-column limit; do not sort includes). Preserve reentrancy by threading state through `ZContext`; avoid globals/singletons. Keep CLIs thin and push new behavior into `libziputils` helpers, reusing existing common/file I/O utilities. Match Info-ZIP user-facing text and exit codes unless intentionally diverging; document any divergence in tests/docs.

## Testing Guidelines
Run `meson test -C build` before sending changes; it covers both C unit tests and Python black-box integration tests (which call `build/zip` and `build/unzip` with the system Info-ZIP binaries on `PATH` for parity). Add Python integration coverage for every user-visible feature or bug fix; isolate each feature per file and use `tempfile.TemporaryDirectory` for scratch space. Use `zip-spec.md` and `unzip-spec.md` to confirm expected prompts, exit codes, and listing formats. Enable large Zip64 coverage with `ZU_RUN_LARGE_TESTS=1 ZU_LARGE_SIZE_GB=5 meson test -C build --suite long`. If you must skip long suites, note it in review.

## Commit & Pull Request Guidelines
Favor behavior-focused commits with the existing style (`feat(core): ...`, `fix(zip): ...`, etc.). Describe user-visible changes clearly and keep PRs small and scoped. Update `README.md` and `CHECKLIST.md`, and always double check specs (`zip-spec.md`, `unzip-spec.md`) when closing parity gaps or altering flags. Include the commands you ran (e.g., `meson test -C build`) and call out any skipped suites. Link issues where relevant and summarize behavioral differences from Info-ZIP.
