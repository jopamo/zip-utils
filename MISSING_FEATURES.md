# Missing Features and Parity Gaps

Tracking gaps against Info-ZIP 6.0 behavior as described in `zip.txt` / `unzip.txt`. Items unchecked are not implemented; items called out as “parsed only” are accepted but lack the documented effect.
See `OPTION_MATRIX.md` for a per-option breakdown.

## CLI Option Parity

### zip
- [ ] Remaining man-page flags not parsed (examples: `-A`, `-@`, `-b`, `-B`, `-D`/`-DB`, `-df`/`-du`, `-g`, `-k`/`-L`/`-LL`, `-MM`, `-nw`/`-ws`, `-R`, `-S`, `-TT`, `-U*`, `-V`, `-X`, `-y`, `-z`, `-!`, long-option aliases, response files).
- [x] Streaming input parity: treating `-` as stdin with data descriptors like Info-ZIP (`compress_to_temp`/CRC pre-pass currently requires seekable input).
- [ ] `-m` (move/delete sources) is parsed but does not remove input files.
- [ ] Archive and entry comments: `-z`/comment editing unsupported; rewrites drop existing comments.
- [ ] Full pattern/recursion parity: must-match semantics (`-MM`), hidden/system file handling, case-folding choices, and include/exclude precedence are not matched to Info-ZIP.
- [ ] Self-extractor offset adjustments (`-A`) and SFX stub integration are missing.

### unzip / zipinfo
- [ ] Missing/ignored flags from the man page: `-aa`, `-L`/`-LL`, `-j`, `-U`/`-UU`, `-V`, `-X`, `-O` (codepage), `-C` is implemented but no flag to force case-sensitivity back on, pager prompts beyond no-op `-M`, response files, and other long-option aliases.
- [ ] Symlink/FIFO extraction is rejected (no `-y`-style allow flag); VMS/OS2/NTFS attribute preservation is absent.
- [ ] Zipinfo parity gaps: header/footer suppression combinations, ISO/decimal time switches beyond current `-T`, and archive/entry comments are read-only.
- [ ] Multi-disk archive reading is not implemented (writer can emit splits, reader only opens single files).

### zipnote / zipcloak / zipsplit
- [ ] `zipnote` does not edit/write comments; current alias only lists comments.
- [ ] `zipcloak` does not re-encrypt existing entries; alias only enables encryption on new writes.
- [ ] `zipsplit` is unimplemented beyond the option alias.

## Archive Format & Feature Gaps
- [ ] AES/WinZip encryption and newer compression methods (LZMA, PPMd, etc.) are not supported; only deflate/store/bzip2 + ZipCrypto exist.
- [ ] Entry and archive comments are not preserved or emitted when rewriting archives.
- [ ] Reading split archives (`.z01`/`.zip`) is not implemented; writing splits exists.
- [x] Streaming output/input without known sizes (data descriptors) for stdin/stdout writes.
- [ ] Symlink/FIFO handling is blocked by default and lacks user-facing toggles; symbolic links are not stored as links.
- [ ] Extended metadata (ACLs, EAs, UID/GID, NTFS timestamps, DOS attributes, UTF-8/codepage flags) is not preserved or configurable.
- [ ] Self-extracting stubs and offset adjustment (`-A`) are not supported.
- [ ] Partial platform behaviors from Info-ZIP (VMS, OS/2, Windows attribute handling, Amiga/Mac metadata) are out of scope today.
