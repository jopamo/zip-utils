# Missing Features and Parity Gaps

Tracking gaps against Info-ZIP 6.0 behavior as described in `zip.txt` / `unzip.txt`. Items unchecked are not implemented; items called out as "parsed only" are accepted but lack the documented effect.
See `OPTION_MATRIX.md` for a per-option breakdown.

## How to read this list

- `[ ]` = not implemented; `[x]` = implemented. If an item is "parsed only," the option is accepted but diverges from the man page.
- Scope is POSIX hosts unless noted; platform-specific flags (VMS/OS2/Windows) are listed for completeness.
- Entries are grouped by CLI first, then by archive-format behaviors.
- Use `OPTION_MATRIX.md` to find per-flag status; this document calls out larger behavioral deltas.

## Updating this doc

- Check off items only when behavior matches Info-ZIP and has test coverage (integration preferred).
- When closing a gap, also update the relevant row in `OPTION_MATRIX.md` and cite any intentional divergence there.
- Keep notes terse and user-facing so regressions are obvious to spot during reviews.

## Planning work

- Pick a gap, add coverage, then update both this file and `OPTION_MATRIX.md`.
- Group related gaps into focused PRs (e.g., option parsing parity, split archive support) to keep reviews scoped.
- Note follow-up items in the backlog section if fixes land incrementally.

## CLI Option Parity

### zip
- [ ] Remaining man-page flags not parsed (examples: `-A`, `-@`, `-b`, `-B`, `-D`/`-DB`, `-df`/`-du`, `-g`, `-k`/`-L`/`-LL`, `-MM`, `-nw`/`-ws`, `-R`, `-S`, `-TT`, `-U*`, `-V`, `-X`, `-y`, `-!`, long-option aliases, response files).
- [x] Streaming input parity: treating `-` as stdin with data descriptors like Info-ZIP (`compress_to_temp`/CRC pre-pass currently requires seekable input).
- [x] Entry comments preserved on rewrite; archive comments (including comment-only updates) are written with `-z` (entry comment editing not exposed).
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
- [x] Comment handling: archive comments written and preserved; entry comments preserved on rewrite (no CLI edit path yet).
- [x] Split-archive support: writer emits splits; reader can open `.z01`/`.zip` by concatenating parts (disk metadata still simplified).
- [ ] AES/WinZip encryption and newer compression methods (LZMA, PPMd, etc.) are not supported; only deflate/store/bzip2 + ZipCrypto exist.
- [ ] Symlink/FIFO handling is blocked by default and lacks user-facing toggles; symbolic links are not stored as links.
- [ ] Extended metadata (ACLs, EAs, UID/GID, NTFS timestamps, DOS attributes, UTF-8/codepage flags) is not preserved or configurable.
- [ ] Self-extracting stubs and offset adjustment (`-A`) are not supported.
- [ ] Partial platform behaviors from Info-ZIP (VMS, OS/2, Windows attribute handling, Amiga/Mac metadata) are out of scope today.
- [x] Streaming output/input without known sizes (data descriptors) for stdin/stdout writes.

## Priority Backlog
1. Surface entry comment editing (zipnote-style flows) and align with Info-ZIP prompts.
2. Broaden platform/attribute handling (text/binary toggles, Unicode/codepage flags).
3. Improve zipinfo formatting parity (header/footer suppression combos, pager/header spacing).
