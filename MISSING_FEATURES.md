# Missing Features

These involve significant changes to how the zip format is written or read (multi-file or damaged files).

- [x] **Split Archive Support** (`-s`, `-sp`)
    - [x] Implement logic to split archives into multiple volumes (chunks).
    - [x] Handle split naming conventions (`.z01`, `.z02`, `.zip`).
    - [x] Implement "pause" mode (`-sp`) for removable media support.
- [x] **Archive Fixing** (`-F`, `-FF`)
    - [x] Implement `fix` (`-F`): Scan for a valid central directory even if the end is damaged.
    - [x] Implement `fixfix` (`-FF`): Brute-force scan of the file for local file headers to rebuild the central directory.
