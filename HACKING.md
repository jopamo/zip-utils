# HACKING

This document outlines important conventions and guidelines for contributing to `zip-utils`.

## Testing Guidelines

New features and bug fixes must be accompanied by appropriate tests. We primarily use two styles of tests:

1.  **C Unit Tests**: Located in `tests/` (e.g., `test_compression.c`, `test_fileio.c`). These are for low-level component validation and are run directly as executables defined in `meson.build`.
2.  **Python Integration Tests**: Located in `tests/` (e.g., `test_create_integration.py`, `test_logging.py`). These tests are Python scripts executed via `meson test`. They should adhere to the following principles:
    *   **Clarity and Isolation**: Each significant feature or logical unit should have its own dedicated Python test file (e.g., `test_logging.py`, `test_time_filtering.py`). This ensures that failures are clearly attributable to a specific feature, simplifying debugging.
    *   **Black-box Testing**: These tests operate by invoking the `zip` and `unzip` binaries (`build/zip`, `build/unzip`) as external processes using `subprocess`.
    *   **Verification**: Test assertions are typically made by examining the output of `unzip -l`, inspecting log files, or by using Python's `zipfile` module to read and verify the contents and metadata of generated archives.
    *   **Temporary Environments**: Tests should use `tempfile.TemporaryDirectory` to create isolated environments for their operations, ensuring no side effects on the project's codebase or the host system.

**All new features must include dedicated Python integration tests** to ensure full coverage and prevent regressions. Existing features should also have their test coverage expanded where lacking.
