# zip-utils: ZIP Utility Specification

## 1. Scope

`zip-utils` is a Linux-only implementation of `zip` with an Info-ZIP–style CLI.

This spec is **normative** for supported flags and observable behavior:

* stdout/stderr shape (high-level compatibility)
* exit codes
* selection rules and side effects
* archive correctness/compatibility (ZIP readers must accept output)

Out-of-scope features are explicitly listed in §14.

---

## 2. Invocation

```sh
zip [options] archive.zip [inputs...]
```

### 2.1 Special operands

* If `archive.zip` is `-`, write the resulting archive to stdout
* If any input path is `-`, read file data from stdin and add a streamed entry
* `--` ends option parsing; remaining args are literal paths
* `-@` reads one input path per line from stdin

Only **one stdin consumer** may be active (e.g., `-@` and `-` file data cannot both use stdin in the same run). If ambiguous, error.

---

## 3. Option Parsing

### 3.1 Short options

* Short options may be grouped where unambiguous (Info-ZIP style)
* For short options that take a value, all forms must be accepted:

  * `-ovalue`
  * `-o value`
  * `-o=value`

To avoid ambiguity with 2-character options, parsing must match Info-ZIP conventions where practical.

### 3.2 Long options

* Long options use:

  * `--opt=value` or `--opt value`

### 3.3 Terminators

* `--` terminates options
* Pattern lists for `-i`, `-x`, `-d`, and `--copy` end at the next token that begins with `-`, or `--`, or end-of-argv

---

## 4. Operating Modes

### 4.1 External modes (select filesystem inputs)

Default is **add**:

* **add (default)**
  Add new files; replace existing entries unconditionally

* `-u` **update**
  Add new files; replace only if filesystem mtime is newer than the archive entry mtime

* `-f` **freshen**
  Replace existing only; do not add new entries

* `-FS` **filesync**
  Update if entry differs by **mtime or size**; delete entries with no matching filesystem object (see §10)

### 4.2 Internal modes (select archive entries)

* `-d pattern...` **delete**
  Delete archive entries whose stored paths match patterns

* `-U` / `--copy pattern...` **copy selection mode**
  Select entries in the input archive to copy to a new archive when used with `--out` (see §11)

---

## 5. Output Targeting

### 5.1 In-place update (default)

If `archive.zip` exists, it is read and updated according to the selected mode.

### 5.2 Output to new archive: `--out`

* `--out OUTARCHIVE` writes result to OUTARCHIVE instead of updating input in place
* Input archive remains unchanged
* `--out` overwrites any existing OUTARCHIVE

If no filesystem inputs are provided and `--out` is specified, all entries from the input archive are copied to the output archive (unless filtered by `--copy` / patterns).

---

## 6. Selection and Paths

### 6.1 Recursion

* `-r` recurse into directories specified on the command line
* `-R` recurse current directory and treat remaining path arguments as patterns to match within the traversal (Info-ZIP behavior subset)

### 6.2 Junk paths

* `-j` store only basenames; drop directory components

### 6.3 Read list from stdin

* `-@` reads one path per line from stdin (newline stripped)
* Empty lines ignored
* Paths are treated literally (no shell expansion)

---

## 7. Include / Exclude Filters

* `-i pattern...` include-only filter (logical OR across patterns)
* `-x pattern...` exclude filter (logical OR across patterns)

Filtering applies to the **stored archive path** that would be produced (after `-j`, recursion mapping, etc).

Order:

1. candidate is produced from traversal / inputs
2. include filter (if any) must match
3. exclude filter (if any) must not match

---

## 8. Pattern Matching Semantics

* Globbing supports:

  * `*` matches any sequence (including `/`)
  * `?` matches any single character
  * bracket classes `[abc]`, ranges `[a-z]`, negation `[!x]`

Matching is case-sensitive on Linux.

---

## 9. Compression

* `-0..-9` compression level (`-0` == store)
* `-Z store|deflate` compression method
* `-n suffixes` store-only suffix list:

  * suffix list is comma- or colon-separated
  * suffix matching is case-insensitive
  * matching files must be stored uncompressed regardless of level

If both `-Z` and `-0..-9` are specified, the last effective method/level selection wins following Info-ZIP precedence behavior.

---

## 10. Filesync and Deletion Details

### 10.1 `-FS` (filesync)

For each filesystem candidate in the effective file set:

* if entry missing in archive: add
* if entry exists:

  * replace if filesystem mtime differs from entry mtime **or** size differs
  * otherwise retain by copying existing entry data where possible

Then remove from archive any entry that has no corresponding filesystem object in the effective file set.

### 10.2 `-d` (delete)

* Delete matches against **archive stored paths**
* `-d` does not read filesystem inputs (unless also supplied by user for other reasons)

---

## 11. Copy Mode (`-U` / `--copy`) with `--out`

Copy mode selects existing entries from the input archive and writes them to the output archive.

Rules:

* Requires an input archive
* Intended for use with `--out`
* Patterns match stored paths
* May be combined with `-x` to exclude a subset of the copy set

If `--out` is specified and no filesystem inputs are given:

* default: copy all entries
* with `--copy`: copy only selected entries

---

## 12. Unix Semantics

* `-y` store symlinks as symlinks (do not follow)

* Without `-y`, symlinks are followed and the referenced file data is archived

* `-X` strip extra attributes as implemented by this project

  * must be stable and documented in release notes (what is stripped vs preserved)

---

## 13. Output / Logging / Verification

* `-q` quiet mode, stackable

* `-v` verbose mode

  * `zip -v` alone may print version information

* `-T` test archive after writing

  * must affect exit status on failure

* `-b dir` temp directory for a seekable temp archive when needed

---

## 14. File Side Effects

* `-m` move into archive
  After successful archive creation/update, delete original filesystem files that were added/replaced

  * Deletion occurs only if the archive operation succeeded for that entry set
  * For directories, behavior is implementation-defined but must not delete directory trees unless explicitly documented

* `-o` set archive mtime
  After writing, set the archive file mtime to match the newest entry mtime in the resulting archive

---

## 15. Streaming Requirements

* If output is stdout (`archive.zip == "-"`), the archive must be streamable
* Data descriptors may be used when sizes/CRCs are not known up front
* Implementation may use `-b` to spool to a temp file for broader compatibility when configured/required

---

## 16. Exit Codes

Target Info-ZIP compatibility:

* `0` success
* `1` warnings (non-fatal)
* `2` fatal error (archive not produced or operation failed)

If you intentionally differ, you must document it in your test harness output and release notes.

---

## 17. Intentionally Unsupported Options

These are explicitly out of scope for `zip-utils`:

* Comments / SFX: `-c`, `-z`, `-A`, `-J`
* Weak encryption: `-e`, `-P`
* Fix/salvage: `-F`, `-FF`
* Split archives: `-s`, `-sp`, `-sb`, `-sv`
* Dot/count UI knobs: `-db`, `-dc`, `-dd`, `-dg`, `-ds`, `-du`, `-dv`
* Logging framework: `-lf`, `-la`, `-li`
* Diagnostics toggles: `-MM`, `-nw`, `-sc`, `-sd`, `-so`, `-ws`
* Non-Linux semantics: `-AS`, `-AC`, `-RE`, `-V`, `-l`, `-ll`, `-ic`, `-FI`, Unicode policy knobs (`-UN=...`)
* Difference mode: `-DF` / `--dif`

---

## 18. Examples

```sh
zip out.zip file1 file2 dir/file3
zip -r out.zip .
zip -r out.zip . -x "*.o" "*.a" -x "build/*"
zip -r out.zip . -i "*.c" "*.h"
zip -r - . > out.zip
printf "hello\n" | zip out.zip -
find . -type f -print | zip -@ out.zip
zip -u release.zip src/*.c include/*.h
zip -f release.zip src/*.c
zip release.zip -d "build/*" "*.tmp"
zip old.zip --out new.zip
zip old.zip --copy "*.c" --out c-only.zip -x foo.c
zip -m release.zip dist/*   # move into archive
zip -o release.zip src/*    # set archive mtime to newest entry
```
