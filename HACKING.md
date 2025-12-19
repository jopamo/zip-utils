# Developer Guide

## Development Setup
### Requirements
- Meson ≥ 1.2 and Ninja
- zlib and bzip2 development headers
- Python 3 (for integration tests)

## Build and Test

### Basic Build
Keep a clean directory per configuration.
```bash
meson setup build
ninja -C build
```
Artifacts are generated in `build/`: `zip`, `unzip`, `zipinfo`, and `libziputils.a`.

### Sanitizer Build (ASan/UBSan)
```bash
meson setup build-sanitize -Ddebug_sanitize=true
ninja -C build-sanitize
```

### Release Build
For optimized binaries with LTO:
```bash
meson setup build-release --buildtype=release -Doptimization=3 -Db_lto=true
ninja -C build-release
```

### Running Tests
Run the full suite (C unit tests + Python integration tests):
```bash
meson test -C build
```

Specific test targets:
*   List suites: `meson test -C build --list`
*   Run specific test: `meson test -C build integration-create -v`
*   Large Zip64 coverage: `ZU_RUN_LARGE_TESTS=1 ZU_LARGE_SIZE_GB=5 meson test -C build --suite long`

### Parity Verification
Capture and diff output against system Info-ZIP binaries to ensure compatibility.

1.  **Zip**:
    *   Baseline: `python3 tools/document_zip_output.py --zip zip --outdir zip-doc-system`
    *   Build: `python3 tools/document_zip_output.py --zip $(pwd)/build/zip --outdir zip-doc-build`
2.  **Unzip**:
    *   Baseline: `python3 tools/document_unzip_output.py --unzip unzip --outdir unzip-doc-system`
    *   Build: `python3 tools/document_unzip_output.py --unzip $(pwd)/build/unzip --outdir unzip-doc-build`
3.  **Zipinfo**:
    *   Baseline: `python3 tools/document_zipinfo_output.py --zipinfo zipinfo --outdir zipinfo-doc-system`
    *   Build: `python3 tools/document_zipinfo_output.py --zipinfo $(pwd)/build/unzip --zipinfo-arg=-Z --outdir zipinfo-doc-build`

Use `tools/run_comparison_docs.sh` to run all captures if configured.

## Coding Standards
*   **Formatting**: Use `.clang-format` (Chromium style, 4-space indents, no tabs, 200-column limit).
*   **Reentrancy**: Thread all state through `ZContext`. Avoid globals and singletons.
*   **Architecture**: Keep CLIs thin. Push logic into `libziputils`.
*   **Compatibility**: Match Info-ZIP user-facing text and exit codes strictly. Document divergences.
*   **Tests**: Add Python integration coverage for every user-visible feature.

## Contribution Flow
1.  **Check Context**: Review `DESIGN.md` for architecture and the `-spec.md` files for supported options.
2.  **Branch**: Create a feature branch.
3.  **Implement**: Write code and tests. Run `meson test -C build` frequently.
4.  **Verify**: Run parity checks if modifying CLI output.
5.  **Review**: Open a Pull Request. Ensure CI passes.