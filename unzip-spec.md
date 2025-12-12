# zip-utils unzip specification

## 1. Scope

This specification defines the Unix/Linux behavior for `unzip` in `zip-utils`

* Unix/Linux-only semantics
* No split archive support
* Supported archive format: standard ZIP with Store and Deflate methods
* Observable behavior goal: match Info-ZIP **for the supported option set** (exit codes, stdout/stderr shape, side effects) ([Linux Documentation][1])

## 2. Invocation

```sh
unzip [options] archive[.zip] [list...] [-x xlist...] [-d exdir]
```

### 2.1 Special operands

* `archive == "-"` reads the archive from stdin (streaming read)
* `--` ends option parsing; remaining arguments are treated as literal patterns

### 2.2 stdin consumption

At most one stdin consumer is allowed:

* archive input via `archive == "-"`

If more than one would apply, fail with a usage error

## 3. Parsing rules

### 3.1 Short options

* Short options may be grouped (`-qn` equivalent to `-q -n`)
* Short options with values accept:

  * `-dDIR`
  * `-d DIR`
  * `-d=DIR`

### 3.2 Long options

Long options are not supported unless explicitly listed by the project; unknown `--opt` must be rejected with a clear error

### 3.3 Pattern list termination

Lists for `list...` and `-x xlist...` end at:

* the next token that begins with `-`, or
* `--`, or
* end of argv

## 4. Operating modes

Exactly one primary mode is active:

### 4.1 Extract (default)

* Extracts entries matching `list...` (or all entries if no list is provided)
* Skips entries matching any `-x` pattern
* By default, creates directories as needed (unless `-j` is used)

### 4.2 List: `-l`

* Lists entries in short format
* No extraction side effects

### 4.3 Verbose list / version: `-v`

* With only `-v` and no archive operand: print version info and exit success ([Linux Documentation][1])
* Otherwise: list entries in verbose format

### 4.4 Test: `-t`

* Tests compressed data for selected entries (as if extracting, but without writing files) ([Linux Documentation][1])

### 4.5 Comment: `-z`

* Displays archive comment only ([Linux Documentation][1])

### 4.6 Pipe extract: `-p`

* Writes file data for selected entries to stdout, concatenated, with no per-file headers ([Linux Documentation][1])
* No filesystem writes occur (but errors and selection behavior still apply)

## 5. Selection, include/exclude, and matching

### 5.1 Matching target

Patterns match **stored archive paths** (not filesystem paths), using the normalization rules in §6.

### 5.2 Application order

1. Candidate entry set is determined by `list...` (or “all entries” if omitted)
2. Candidates are removed if they match any `-x` pattern
3. The remaining entries are processed by the active mode (extract/list/test/pipe/comment)

### 5.3 Wildcards (supported)

* `*` matches any sequence of characters, including `/`
* `?` matches any single character
* bracket classes `[list]` match a character in the list

  * ranges supported `[a-f]`
  * negation supported `[!bf]`

### 5.4 Case sensitivity

* Pattern matching is case-sensitive

## 6. Path handling and security policy

### 6.1 Stored path separator

* Stored paths use `/` as the separator

### 6.2 Normalization (for extraction)

When extracting an entry, its stored path is normalized as follows:

* Leading `/` is stripped (entries are always treated as relative)
* `.` path segments are removed
* Any path which would escape the extraction root via `..` is **rejected as an error**

  * This includes `../x`, `a/../../x`, etc.
* NUL bytes and other invalid path bytes are rejected

### 6.3 Extraction root

* Default extraction root is the current working directory
* `-d exdir` sets the extraction root to `exdir` (created if necessary)

### 6.4 Symlinks and special files

* If an entry is a symlink (per Unix mode metadata), create a symlink

  * The symlink target is taken from the entry data
  * If the link target would be unsafe (e.g., absolute or escaping the root when later resolved), it is still created as a literal link string; **no dereference occurs during extraction**
* Device nodes, FIFOs, and other special files are rejected (clear error, nonzero exit)

## 7. Filesystem write policy

### 7.1 Directory creation

* By default, create parent directories as needed
* `-j` junk paths: discard directory components and extract into the extraction root only

  * If multiple entries map to the same basename, later entries follow overwrite policy (§7.2)

### 7.2 Overwrite policy

Default behavior: if a target path exists, prompt on the controlling tty (or fail in non-interactive contexts)

Modifiers:

* `-n` never overwrite existing files (silently skip that entry) ([Linux Documentation][1])
* `-o` overwrite existing files without prompting ([Linux Documentation][1])

### 7.3 Timestamps and permissions

* Restores mtime for extracted regular files by default
* Restores basic Unix mode bits for regular files by default (respecting umask for newly created files)
* No UID/GID restoration support in this fork (attempts must be rejected if options are provided)

## 8. Logging and output shape

* `-q` quiet mode, stackable (`-qq` is quieter)

  * Suppresses most non-error messages; errors still go to stderr
* In extract mode, successful extraction prints Info-ZIP-like “inflating:” / “extracting:” style lines unless `-q` suppresses them (exact wording is implementation-defined but must be stable)

## 9. Unsupported options and features

The following must be rejected with a clear error and nonzero exit status:

* Split archive reading
* Case-insensitive matching (`-C`)
* Lowercasing controls (`-L`, `-LL`)
* UID/GID / ACL restore knobs (`-X`, `-XX`)
* “Allow outside root” traversal knobs (Info-ZIP `-:`-like behavior)
* Any option not explicitly listed in this spec

## 10. Exit codes

Exit codes follow Info-ZIP conventions (for the supported surface): ([Oracle Documentation][2])

* `0` success
* `1` warnings encountered (e.g., some files skipped) but overall processing completed ([Oracle Documentation][2])
* `2` error in archive format ([Oracle Documentation][2])
* `3` severe error in archive format ([Oracle Documentation][2])
* `4` memory allocation failed during initialization ([Oracle Documentation][2])
* `5` memory allocation / terminal I/O failed during password processing

If the upstream Info-ZIP surface returns additional specific codes for conditions (e.g., “unsupported method”), this fork may either:

* map them into the nearest Info-ZIP category above (1/2/3), **or**
* preserve the numeric code **if** the project documents the mapping deterministically

## 11. Quick reference (supported option set)

* Modes: `-l`, `-v`, `-t`, `-z`, `-p`
* Extraction target: `-d exdir`
* Filtering: `list...`, `-x xlist...`
* Modifiers: `-q` (stackable), `-n`, `-o`, `-j`
