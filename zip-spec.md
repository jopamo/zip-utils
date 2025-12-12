# zip-utils zip specification

## 1. Scope

This specification defines the Unix/Linux behavior for `zip` in `zip-utils`

* Linux-only semantics
* No split archive support
* Supported archive format: standard ZIP with Store and Deflate methods
* Observable behavior goal: match Info-ZIP for the supported option set (exit codes, stdout/stderr shape, side effects)

## 2. Invocation

```sh
zip [options] zipfile [inputs...] [-xi list]
```

### 2.1 Special operands

* `zipfile == "-"` writes the archive to stdout
* If any input is `"-"`, read file data from stdin and create a single streamed entry
* If both `zipfile` and `inputs` are omitted, read stdin and write the archive to stdout (equivalent to `zip - -`)
* `--` ends option parsing; remaining arguments are treated as literal paths

### 2.2 stdin consumption

At most one stdin consumer is allowed:

* path list mode `-@`
* streamed entry input via input `"-"`
* implicit stdin-to-stdout mode (`zip` with no args)

If more than one would apply, fail with a usage error

## 3. Parsing rules

### 3.1 Short options

* Short options may be grouped (`-rq` equivalent to `-r -q`)
* Short options with values accept:

  * `-ovalue`
  * `-o value`
  * `-o=value`

### 3.2 Long options

* Long options with values accept:

  * `--opt=value`
  * `--opt value`

### 3.3 Pattern list termination

Lists for `-x`, `-i`, `-d`, and `--copy` end at:

* the next token that begins with `-`, or
* `--`, or
* end of argv

## 4. Operating modes

### 4.1 External modes (select filesystem paths)

Default mode is add/replace

* add (default)

  * add new entries
  * replace existing entries unconditionally

* `-u` update

  * add new entries
  * replace existing entries only if filesystem mtime is newer than the archive entry mtime

* `-f` freshen

  * replace existing entries only
  * do not add new entries

* `-FS` filesync

  * add new entries
  * replace entries if either filesystem mtime differs from stored entry mtime, or filesystem size differs from stored uncompressed size
  * delete archive entries that have no matching filesystem path in the effective input set

### 4.2 Internal modes (select archive entries)

* `-d pattern...` delete

  * remove entries from the archive whose stored paths match patterns
  * `-t` and `-tt` may further restrict which archive entries are eligible for deletion
  * `-i` and `-x` do not participate in selecting archive entries for delete mode

* `-U` copy mode (also `--copy`)

  * selects entries in the input archive to copy to a new output archive when used with `--out`
  * selection is pattern-based against stored paths
  * if `--out` is specified and no filesystem inputs are provided:

    * without `-U/--copy`: copy all entries
    * with `-U/--copy`: copy only selected entries

## 5. Filesystem traversal and stored paths

### 5.1 Directory recursion

* Without recursion, directory arguments are not traversed and are treated as directory entries only if directory entries are enabled
* `-r` recurses into directory arguments and adds contained files
* `-R` recurses from the current directory and treats remaining path arguments as patterns matched during traversal

### 5.2 Directory entries

* By default, explicit directory entries may be added as needed for empty directories and for directory placeholders
* `-D` suppresses adding explicit directory entries

  * files within directories are still archived normally

### 5.3 Junk paths

* `-j` stores only basenames for filesystem inputs
* directory components are discarded
* if multiple inputs map to the same stored basename, replacement behavior follows the active mode

### 5.4 Symlinks

* Default: follow symlinks and archive the referenced file contents
* `-y`: store symlinks as symlinks (archive the link itself, not its target)

## 6. Include and exclude filtering

### 6.1 Options

* `-i pattern...` include-only filter
* `-x pattern...` exclude filter

### 6.2 Matching target

Patterns match the stored archive path that would be produced for an input, after:

* recursion mapping
* `-j` path junking
* normalization rules in §7.4

### 6.3 Application order

1. candidate path is generated from traversal or explicit inputs
2. if `-i` is present, candidate must match at least one include pattern
3. if `-x` is present, candidate must not match any exclude pattern

## 7. Pattern matching

### 7.1 Supported wildcards

* `*` matches any sequence of characters, including `/`
* `?` matches any single character
* bracket classes `[list]` match a character in the list

  * ranges supported `[a-f]`
  * negation supported `[!bf]`

### 7.2 Case sensitivity

* Pattern matching is case-sensitive

### 7.3 Path separator

* Stored paths use `/` as the separator

### 7.4 Normalization

* Input paths are stored without a leading `/`
* `.` path segments are removed
* `..` segments are preserved only when explicitly present in inputs and allowed by the tool’s path policy

  * for filesystem inputs, path traversal above the working root is rejected as an error

## 8. Date filtering

* `-t date` exclude before

  * include only filesystem inputs (or eligible archive entries in delete mode) with mtime on or after `date`

* `-tt date` include before

  * include only filesystem inputs (or eligible archive entries in delete mode) with mtime strictly before `date`

Accepted formats:

* `mmddyyyy`
* `yyyy-mm-dd`

Both may be specified together to form a range

## 9. Compression

### 9.1 Levels

* `-0` store (no compression)
* `-1` through `-9` deflate level, fastest to best
* default level is `-6` unless overridden

### 9.2 Method selection

* `-Z store` forces store
* `-Z deflate` forces deflate

### 9.3 Store-only suffixes

* `-n suffixes` marks suffixes that must be stored uncompressed
* suffix list is colon- or comma-separated
* suffix matching is case-insensitive

## 10. Text end-of-line translation

Text translation applies only when explicitly requested and only for regular file data

* `-l` convert LF to CRLF
* `-ll` convert CRLF to LF
* if the initial read buffer is detected as binary, translation is skipped for that file

Translation is applied before compression and CRC calculation

## 11. Output, logging, and temporary files

* `-q` quiet mode, stackable

* `-v` verbose mode

  * `zip -v` with no zipfile prints version information and exits successfully

* `-b dir` temporary directory

  * used for a seekable temporary archive during update/create operations
  * temporary files must be created with safe permissions and cleaned up on failure where possible

## 12. Archive testing

* `-T` test the completed archive before finalizing an update

  * failure must be reflected in exit status

* `-TT cmd` sets the test command used by `-T`

  * `{}` in cmd is replaced with the temporary archive path
  * if `{}` is not present, the temporary archive path is appended as the final argument
  * the command’s exit status determines pass/fail

## 13. Output to a new archive

* `--out OUTARCHIVE` writes the result to `OUTARCHIVE` instead of updating the input archive in place
* `OUTARCHIVE` is always overwritten if it exists
* when `--out` is used, the input archive remains unchanged

## 14. File timestamp behavior

* `-o` sets the resulting zipfile mtime to the newest entry time in the resulting archive

## 15. Move semantics

* `-m` deletes original filesystem files after they have been successfully archived according to the active mode
* deletions occur only after the archive output has been successfully finalized
* deletions are not performed for entries sourced from stdin

## 16. Extra attributes policy

* `-X` excludes extra file attributes as implemented by `zip-utils`
* the exact set of stripped metadata must be deterministic and documented by the project

## 17. Streaming behavior

* If `zipfile == "-"`, output is streamed to stdout
* When streaming, the implementation may use data descriptors as required
* stdin streamed entry (`"-"` input) must work with both file output and stdout output unless stdin is already consumed by another mode

## 18. Unsupported options and features

The following are not supported in this Unix/Linux-only fork and must be rejected with a clear error and nonzero exit status:

* Split archives: `-s`, `-sp`, `-sb`, `-sv`
* DOS/Windows-only semantics: `-AS`, `-AC`, `-RE`
* Case-insensitive archive matching: `-ic`
* Unicode policy knobs: `-UN=...`
* FIFO enabling knob: `-FI`
* Zip64 policy toggle mentioned by upstream help: `-fz-`
* Dot/count UI knobs: `-db`, `-dc`, `-dd`, `-dg`, `-ds`, `-du`, `-dv`
* Logging framework: `-lf`, `-la`, `-li`
* Diagnostics toggles: `-MM`, `-nw`, `-sc`, `-sd`, `-so`, `-ws`
* Difference mode: `-DF`, `--dif`

If an option is displayed in fork help text but is unsupported, the fork’s help output must be corrected to avoid advertising it

## 19. Exit codes

Exit codes follow Info-ZIP conventions for the supported surface:
