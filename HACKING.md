# HACKING

This document outlines important conventions and guidelines for contributing to `zip-utils`.

## Development workflow

- Build with Meson/Ninja (`meson setup build && ninja -C build`) and keep a clean build directory per configuration (e.g., `build`, `build-sanitize`).
- Run the full test suite via `meson test -C build` before sending changes; this runs both C unit tests and Python integration tests.
- Keep commits and merge requests behavior-focused; document user-visible changes in `README.md` and parity tracking docs (`MISSING_FEATURES.md`, `OPTION_MATRIX.md`) as you close gaps.

## Coding style & design

- Preserve reentrancy: thread state through `ZContext` instead of adding globals or singletons.
- Keep the CLIs thin; build new behavior into `libziputils` helpers and reuse existing common/file I/O utilities.
- Match Info-ZIP user-visible text and exit codes unless intentionally diverging; note any divergence in docs/tests.
- Prefer small, focused helpers with clear ownership (parsing vs. core behavior vs. I/O).

## Testing Guidelines

New features and bug fixes must be accompanied by appropriate tests. We primarily use two styles of tests:

1.  **C Unit Tests**: Located in `tests/` (e.g., `test_compression.c`, `test_fileio.c`). These are for low-level component validation and are run directly as executables defined in `meson.build`.
2.  **Python Integration Tests**: Located in `tests/` (e.g., `test_create_integration.py`, `test_logging.py`). These tests are Python scripts executed via `meson test`. They should adhere to the following principles:
    *   **Clarity and Isolation**: Each significant feature or logical unit should have its own dedicated Python test file (e.g., `test_logging.py`, `test_time_filtering.py`). This ensures that failures are clearly attributable to a specific feature, simplifying debugging.
    *   **Black-box Testing**: These tests operate by invoking the `zip` and `unzip` binaries (`build/zip`, `build/unzip`) as external processes using `subprocess`.
    *   **Reference binaries**: Tests expect Info-ZIP `zip`/`unzip` (6.x) on `PATH`; keep that available or skip parity-dependent cases explicitly.
    *   **Verification**: Test assertions are typically made by examining the output of `unzip -l`, inspecting log files, or by using Python's `zipfile` module to read and verify the contents and metadata of generated archives.
    *   **Temporary Environments**: Tests should use `tempfile.TemporaryDirectory` to create isolated environments for their operations, ensuring no side effects on the project's codebase or the host system.
    *   **Long/large runs**: Zip64 and split-volume coverage lives in the `long` suite; enable with `ZU_RUN_LARGE_TESTS=1 ZU_LARGE_SIZE_GB=5 meson test -C build --suite long` when touching those paths.

**All new features must include dedicated Python integration tests** to ensure full coverage and prevent regressions. Existing features should also have their test coverage expanded where lacking.

## Documentation hygiene

- When enabling or adjusting CLI flags, update `OPTION_MATRIX.md` and `MISSING_FEATURES.md` to keep parity tracking reliable.
- If behavior or workflows change, add a short note to `README.md` so new contributors have an accurate quickstart.
- Keep examples and commands copy-pasteable; prefer `tempfile` over ad-hoc paths in docs and tests.
- Stick to ASCII punctuation and terse headings; add language hints to fenced code blocks.
- Record doc structure updates by refreshing the `Documentation` section in `README.md` when new guides appear.

## Before sending changes

- Run `meson test -C build` (plus sanitizer or long suites when relevant).
- Ensure new flags/behaviors are documented in `README.md`, `OPTION_MATRIX.md`, and `MISSING_FEATURES.md` as appropriate.
- Add or update integration tests for user-visible changes; avoid regressions in existing Info-ZIP parity coverage.
- Call out any skipped long/large suites in your review notes if they are relevant to the change.
