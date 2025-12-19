# System Architecture

## High-Level Architecture
The system is designed as a "library-first" rewrite of Info-ZIP tools. The core logic resides in a reentrant library, `libziputils`, which maintains state in a `ZContext` structure. The CLI executables (`zip`, `unzip`, `zipinfo`) are thin wrappers that parse command-line arguments, populate the context, and invoke library functions.

## Core Components
*   **`src/include/`**: Public headers (`ziputils.h`) defining shared structs and error codes.
*   **`src/common/`**: Internal helpers for context management (`ctx.c`), logging, string list manipulation (`strlist.c`), and file I/O abstraction (`fileio.c`).
*   **`src/compression/`**: Wrappers for compression algorithms (`zlib_shim.c`, `bzip2_shim.c`), CRC32 calculation, and ZipCrypto encryption.
*   **`src/format/`**: Archive handling logic, including the reader (`reader.c`) for parsing central directories and local headers, and the writer (`writer.c`) for generating archives. Handles Zip64 and streaming support.
*   **`src/cli/`**: Entry points (`main_zip.c`, `main_unzip.c`) that strictly handle argument parsing and context initialization before calling into `src/format` or `src/common`.

## Data Flow
1.  **Input**: User invokes a CLI tool (e.g., `zip`).
2.  **Parsing**: CLI parses arguments and flags, populating a `ZContext` object.
3.  **Execution**: CLI invokes a high-level operation (e.g., `ops_create_archive`) in `libziputils`.
4.  **Processing**:
    *   For creation: `writer.c` iterates over input files, using `fileio.c` to read and `compression/` modules to compress data, writing the result to the output stream.
    *   For extraction: `reader.c` parses the central directory, locates entries, and uses `compression/` to inflate data, writing to disk via `fileio.c`.
5.  **Output**: Results are written to the filesystem or stdout; logs are sent to stderr based on verbosity settings in `ZContext`.

## Decision Log
*   **Build System**: Meson + Ninja chosen for speed and modern dependency handling.
*   **Reentrancy**: All state is threaded through `ZContext` to avoid globals/singletons, allowing the library to be safe for concurrent use in future applications.
*   **Thin CLIs**: Minimizing logic in the CLI layer ensures that all features are available to library users and facilitates testing.
*   **Compatibility**: Targets Info-ZIP `zip` 3.0 / `unzip` 6.0 behavior strictly; unsupported flags fail fast to avoid silent divergence.
