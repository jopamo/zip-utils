# Option Parity Matrix (vs Info-ZIP 6.0)

Derived from `zip.txt` and `unzip.txt`. Status values:
- **working**: parsed and implemented with behavior covered by tests.
- **partial**: parsed but missing man-page semantics or edge cases.
- **missing**: not parsed; must be implemented.

## Using this matrix

- Status is measured against Info-ZIP 6.0 CLI semantics; add a short note when behavior intentionally differs.
- When wiring a new flag, update the table and land a covering test in `tests/` (integration preferred for CLI flow).
- Keep notes concise and actionable so contributors know what to validate before marking an option as working.
- Keep the row order roughly aligned to the man pages; update `MISSING_FEATURES.md` when statuses change so both docs stay in sync.
- If coverage is non-obvious, point to the relevant test file or suite in the Notes column.
- Do not mark an option as working without automated coverage; parity assertions should live in integration tests where possible.
- If a behavior differs intentionally, link to the rationale (commit/issue) in the Notes column for future reviewers.

## zip

| Option(s) | Status | Notes |
| --- | --- | --- |
| `-0`..`-9`, `-Z` (deflate/store/bzip2) | working | Other legacy methods (shrink, reduce, etc.) missing. |
| `-r` | working | Recursive add. |
| `-j` | working | Junk paths. |
| `-d` | working | Delete entries (basic globbing). |
| `-f`, `-u`, `-FS` | working | Freshen/update/file sync implemented. |
| `-m` | working | Moves inputs by deleting source files after successful write. |
| `-T` | partial | Tests after write; exit-code parity not fully validated. |
| `-q`, `-qq`, `-v` | working | Quiet levels (0/1/2) suppress listings; progress dots differ from man page. |
| `-x`, `-i` | working | Include/exclude globs; must-match precedence (`-MM`) not implemented. |
| `-e`, `-P` | working | ZipCrypto only. |
| `-O` | working | Copy-on-write output path. |
| `-s`, `-sp` | working | Split write and pause; reader opens `.z01` + `.zip` concatenated splits (disk metadata simplified). |
| `-t`, `-tt` | working | Time filters. |
| `-F`, `-FF` | partial | Fix/fixfix implemented; recovery semantics need validation. |
| `zipnote` alias | working | Lists comments; `-w` applies entry/archive comment edits (zipnote-style format). |
| `zipcloak` alias | partial | Enables encryption on new writes; no re-encrypt of existing entries. |
| `zipsplit` alias | missing | Alias only. |
| Logging `-la/-lf/-li` | working | Project-specific (not in man page). |
| `-@` | working | Names from stdin. |
| `-a`, `-A`, `-AC`, `-AS` | missing | ASCII translate; adjust SFX offsets; archive-bit handling. |
| `-B`, `-Bn` | missing | Binary/Enscribe/Tandem flags. |
| `-b` | working | Temp-path selection for staging. |
| `-c` | missing | Per-entry comment prompts. |
| `-C`, `-C2`, `-C5` | missing | VMS case-preservation options. |
| `-db/-dc/-dd/-dg/-ds/-du/-dv` | missing | Progress meters and byte/volume displays. |
| `-df`, `-du` | missing | Mac data-fork only, uncompressed-size display. |
| `-D` | working | Suppress directory entries (files still recurse). |
| `-DF` | missing | Differential archive based on existing zip. |
| `-E` | missing | OS/2 longnames. |
| `-FI` | missing | Platform include flag. |
| `-g` | missing | Grow/append to existing archive. |
| `-h2` | missing | Extended help. |
| `-I`, `-ic` | missing | Archive name ignore-case toggles. |
| `-jj` | missing | Alternate junk-path behavior. |
| `-J` | missing | Junk SFX when reading. |
| `-k`, `-L`, `-LL` | missing | DOS/OS2/Unicode path handling toggles. |
| `-l`, `-ll` (text) | working | LF→CRLF / CRLF→LF translation; stored entries are uncompressed; metadata differs from Info-ZIP extras. |
| `-MM` | missing | Must-match (fail if no files processed). |
| `-n` | working | Case-insensitive suffix list, colon-delimited. |
| `-nw`, `-ws` | missing | Wildcard stop-at-dir / warning tweaks. |
| `-N` | missing | NTFS extra field storage. |
| `-o` | working | Archive mtime set to newest entry in output. |
| `-p` | missing | Explicit "store paths" toggle (default behavior). |
| `-Qn` | missing | QDOS headers. |
| `-R`, `-RE` | missing | Recurse patterns / regex include. |
| `-sb/-sc/-sf/-so/-su/-sU/-sv` | missing | Split tuning flags. |
| `-S` | missing | Include system/hidden files. |
| `-TT` | missing | Command hook after archive create. |
| `-U`, `-UN` | missing | Unicode/UTF-8 handling. |
| `-V`, `-VV`, `-w`, `-ww` | missing | VMS attribute/version handling. |
| `-X` | partial | Clears external attrs and drops Unix/timestamp extras when rewriting entries; other extras are kept. |
| `-y` | working | Store symlinks as links (uses link target data). |
| `-z` | working | Archive comments can be added/edited via `-z` even without other inputs; entry comments are preserved on rewrite (editing entry comments still pending). |
| `-!` | missing | Use privileges (Amiga). |
| `-fz-` | missing | Force Zip64 off. |
| Long-option aliases/negations | partial | Only wired for implemented short options. |

## unzip / zipinfo

| Option(s) | Status | Notes |
| --- | --- | --- |
| `-l`, `-t`, `-d`, `-o`, `-n`, `-q`, `-p`, `-P` | working | List/test/extract dir/overwrite/never/quiet/pipe/password. |
| `-i`, `-x`, `-C` | working | Include/exclude; case-insensitive matching (no toggle back). |
| `-v` | working | Verbose listing; routes to zipinfo mode. |
| `-Z` | working | Enter zipinfo mode. |
| `-1`, `-2`, `-s`, `-m`, `-h`, `-T`, `-z`, `-M` (zipinfo) | partial | Names-only/short/medium/header/footer/decimal time/comments/pager supported; formatting parity still pending. |
| Multi-disk read | working | Opens `.z01` + `.zip` split archives by concatenating parts. |
| Symlink/FIFO | missing | Rejected; no allow flag. |
| `-A` | missing | DLL API help. |
| `-a`, `-aa` | missing | Text conversion. |
| `-b` (all variants) | missing | Binary handling, Tandem/VMS record controls. |
| `-B` | missing | Backup overwritten files. |
| `-c` | missing | CRT extract with filename banners. |
| `-D` / `-DD` | missing | Suppress timestamp restoration. |
| `-E` | missing | MacOS extra-field display. |
| `-f`, `-u` | missing | Freshen/update extraction modes. |
| `-F` (Acorn/NFS) | missing | Filename translation. |
| `-i` (Mac ignore extra) | missing | Platform-specific flag distinct from include. |
| `-j` | missing | Junk paths on extract. |
| `-J` (BeOS/Mac attr) | missing | Attribute handling toggles. |
| `-K` | missing | Preserve SUID/SGID/Tacky bits. |
| `-L`, `-LL` | missing | Lowercase conversion and related toggles. |
| `-N` | missing | Amiga filenotes. |
| `-O` | missing | Override code page for filenames. |
| `-s` (space->underscore) | missing | OS/2/NT/MS-DOS space conversion. |
| `-S` | missing | VMS Stream_LF conversion. |
| `-T` (set archive mtime) | missing | zip `-go` analog. |
| `-U`, `-UU` | missing | UTF-8 handling tweaks. |
| `-V` | missing | Retain VMS version numbers. |
| `-W` | missing | Wild_stop_at_dir matching. |
| `-X` | missing | Restore owner/protection/ACLs. |
| `-Y` | missing | Treat `.nnn` suffix as VMS version. |
| `-q` / `-qq` | working | Quiet levels (0/1/2) suppress listings; still print errors. |
| `zipinfo` formats (`-l`/`-v`/`-h` combos) | partial | Layouts supported; header/footer suppression needs parity review. |
| Long-option aliases/negations | partial | Only wired for implemented short options. |

## Priority Backlog
1. Broaden platform/attribute handling (text/binary toggles, Unicode/codepage flags).
2. Improve zipinfo formatting parity (header/footer suppression combos, pager/header spacing).
3. Add remaining parsed behaviors and flags in priority order.
