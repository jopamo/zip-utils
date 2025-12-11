# Missing Features Checklist

This checklist organizes the missing features from `zip-utils` (compared to Info-ZIP `zip` 3.0) into a logical implementation order, starting with foundational utilities and moving to complex archive manipulations.

## Phase 1: Core Utilities & Filtering
These features are relatively self-contained and provide immediate value for scripting and basic usage.

- [ ] **Logging Support** (`-la`, `-lf`, `-li`)
    - [ ] Implement log file opening and management.
    - [ ] Support append (`-la`) vs. overwrite defaults.
    - [ ] Implement informational logging (`-li`) for verbose output to file.
- [ ] **Time-based Filtering** (`-t`, `-tt`)
    - [ ] Implement date parsing (supports `mmddyyyy` and ISO 8601 `yyyy-mm-dd`).
    - [ ] Add file selection logic to include/exclude based on modification time (after/before).
- [ ] **Output File Support** (`-O`, `--output-file`)
    - [ ] Allow creating a new archive from an existing one without in-place modification.
    - [ ] Support "Copy Mode" semantics (reading from one, writing to another).

## Phase 2: Advanced Archive Management
These features require more complex logic regarding file comparison and archive modification.

- [ ] **File Sync Mode** (`-FS`, `--filesync`)
    - [ ] Implement synchronization logic:
        - [ ] Add new files from filesystem.
        - [ ] Update changed files (size/time check).
        - [ ] Delete archive entries that no longer exist on filesystem.
- [ ] **Bzip2 Compression** (`-Z bzip2`)
    - [ ] Integrate `libbz2` or equivalent.
    - [ ] Implement compression method 12 (Bzip2).
    - [ ] Add configuration to select compression method.

## Phase 3: Structural Features & Recovery
These involve significant changes to how the zip format is written or read (multi-file or damaged files).

- [ ] **Split Archive Support** (`-s`, `-sp`)
    - [ ] Implement logic to split archives into multiple volumes (chunks).
    - [ ] Handle split naming conventions (`.z01`, `.z02`, `.zip`).
    - [ ] Implement "pause" mode (`-sp`) for removable media support (optional/legacy).
- [ ] **Archive Fixing** (`-F`, `-FF`)
    - [ ] Implement `fix` (`-F`): Scan for a valid central directory even if the end is damaged.
    - [ ] Implement `fixfix` (`-FF`): Brute-force scan of the file for local file headers to rebuild the central directory.

## Phase 4: Security & Niche Enhancements
Low-priority or highly specific features.

- [ ] **Strong Encryption**
    - [ ] Research and implement AES encryption (WinZip standard) if "strong encryption" is desired beyond ZipCrypto.
- [ ] **Advanced Wildcards**
    - [ ] Implement `-ws` (wildcard stop) to limit wildcards to directory levels.
    - [ ] Implement `-RE` (regex) matching for filenames.
- [ ] **Granular Display Options**
    - [ ] Add `--display-bytes` (running byte count).
    - [ ] Add `--display-counts` (entry count).
- [ ] **Advanced Unicode Handling** (`-UN`)
    - [ ] Implement conflict resolution between standard path fields and Info-ZIP Unicode Path Extra Fields.
- [ ] **OS-Specific Attributes**
    - [ ] Add support for preserving attributes from other OSes (VMS, OS/2, MacOS resource forks) if strictly necessary for portability.
