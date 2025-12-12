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

## Using `.llm/meta.jsonl`

`.llm/meta.jsonl` is a JSON Lines index of “things worth jumping to” in this repo. Each line is a single JSON object produced by `ctags` that points at either documentation structure (chapters/sections) or code symbols (functions, structs, macros, etc). The file is meant to help an agent navigate and answer questions quickly without doing a blind full-text search first.

### Format

* The file is **JSONL**: one JSON object per line
* Each object has, at minimum:

  * `id`: unique-ish identifier for the entry
  * `name`: symbol or heading name
  * `kind`: what it is (`file`, `chapter`, `section`, `function`, `struct`, `macro`, `typedef`, `enum`, `member`, etc)
  * `file`: repo-relative path to open
  * `line`: 1-based line number within that file
  * `language`: source language (`Markdown`, `C`, `C++`, `Meson`, `Python`, etc)
  * `extra`: optional hierarchy/context (e.g. `chapter`, `section`, `typeref`, enum owner, access)

These records are **pointers** only. Always open the referenced file and read surrounding context before making claims, edits, or patches.

### What to use it for

Use `.llm/meta.jsonl` as your primary navigation index to:

* Jump directly to relevant docs sections (`README.md::section::Build`, `HACKING.md::section::Testing Guidelines`, etc)
* Find where a symbol is defined (`compress_to_temp`, `ZContext`, `ZU_SIG_END64`, etc)
* Discover public API surface (`src/include/ziputils.h`) versus internal implementation
* Generate breadcrumbs for answers (“Zip Utils → Build (README.md:62)”)

### Recommended workflow

1. **Parse it line-by-line**

   * Treat it as JSONL, not a JSON array
   * Skip and log malformed lines instead of failing hard

2. **Build quick lookup indexes**

   * `by_id[id] -> entry`
   * `by_file[file] -> entries sorted by line`
   * `by_name[name] -> entries`
   * `by_kind[kind] -> entries`
   * optionally `by_chapter[extra.chapter] -> entries`

3. **Resolve a user question to candidate entries**

   * Prefer doc headings for “how do I build/test/use”
   * Prefer code symbols for “what does this function/macro do”
   * Use `extra.typeref` to jump from a typedef to its struct definition

4. **Open the source and verify**

   * Open `entry.file` at `entry.line`
   * Read enough context to capture the full definition or the full section
   * Confirm the heading/definition text matches `entry.name` before using it

5. **Cross-check usage**

   * After reading the definition, search for references (call sites, macro usage, field access)
   * Use usage to confirm semantics, ownership rules, and error conventions

### Disambiguation rules

If multiple entries match the same name:

* Prefer exact `id` matches when available
* Otherwise prefer:

  * the file the user referenced
  * more specific kinds (`subsection` over `section`, `struct` over `file`)
  * public headers over internal files when the question is about API behavior
* If ambiguity remains, present a short list of candidates (file + line + kind) and pick the most likely default

### Notes on common `kind` values

* `section`, `subsection`, `chapter`: documentation anchors, best for onboarding questions
* `function`: jump to implementation, then validate via call sites
* `macro`: could be include guards, constants, or feature toggles
* `typedef` / `struct`: follow `typeref` when present to find the real definition
* `member`: jump into the owning struct, then verify semantics by searching usage

### Editing guidance

When modifying docs or code based on an entry:

* Re-locate the target by opening `file` at `line`
* Confirm you’re in the right section/definition (line numbers can drift)
* For Markdown sections, edit within the section boundary (until the next same-or-higher header)
* For code symbols, ensure you update call sites/tests if you change signatures or behavior

### What not to assume

* Line numbers can drift as files change
* Generated names like `__anon...` are unstable and should not be exposed as user-facing identifiers
* The index does not prove semantics, only location—always read the real source
