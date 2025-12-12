# zip-utils zip implementation checklist

## Scope and compatibility

* [ ] Linux-only behavior enforced
* [ ] Split archives not supported and rejected (`-s`, `-sp`, `-sb`, `-sv`)
* [ ] Only Store and Deflate methods supported
* [ ] Exit codes match Info-ZIP for supported surface
* [ ] stdout/stderr shape is stable and testable for supported options

## Invocation and special operands

* [ ] `zip [options] zipfile [inputs...] [-xi list]` accepted
* [ ] `zipfile == "-"` streams archive to stdout
* [ ] input `"-"` reads file data from stdin and creates a single streamed entry
* [ ] no-args mode: if zipfile and inputs omitted, read stdin and write archive to stdout (`zip - -` equivalent)
* [ ] `--` ends option parsing and treats remaining args as literal paths

## stdin consumption rules

* [ ] only one stdin consumer allowed per run
* [ ] `-@` consumes stdin as a path list
* [ ] input `"-"` consumes stdin as entry data
* [ ] no-args mode consumes stdin as entry data and writes archive to stdout
* [ ] conflicting stdin consumers cause a usage error

## Option parsing

* [ ] grouped short options supported (`-rq` == `-r -q`)
* [ ] short option values supported as `-ovalue`, `-o value`, `-o=value`
* [ ] long option values supported as `--opt=value` and `--opt value`
* [ ] pattern lists terminate at next `-...` token, or `--`, or end of argv for `-x`, `-i`, `-d`, `--copy`

## Operating modes

* [ ] default add/replace mode implemented

  * [ ] add new entries
  * [ ] replace existing entries unconditionally
* [ ] `-u` update mode implemented

  * [ ] add new entries
  * [ ] replace only if filesystem mtime newer than archive entry mtime
* [ ] `-f` freshen mode implemented

  * [ ] replace existing entries only
  * [ ] do not add new entries
* [ ] `-FS` filesync mode implemented

  * [ ] add new entries
  * [ ] replace if filesystem mtime differs or filesystem size differs from stored uncompressed size
  * [ ] delete archive entries with no matching filesystem path in effective input set
* [ ] `-d pattern...` delete mode implemented

  * [ ] delete entries whose stored paths match patterns
  * [ ] `-t` and `-tt` can restrict eligible archive entries for deletion
  * [ ] `-i` and `-x` do not participate in selecting entries for delete mode
* [ ] `-U` / `--copy` copy selection mode implemented

  * [ ] pattern selection matches stored paths in input archive
  * [ ] works with `--out` to produce a new archive
  * [ ] with `--out` and no filesystem inputs:

    * [ ] without `-U/--copy` copy all entries
    * [ ] with `-U/--copy` copy only selected entries

## Filesystem traversal and stored paths

* [ ] without recursion, directory args not traversed
* [ ] `-r` recurses into directory arguments and adds contained files
* [ ] `-R` recurses from current directory and treats remaining args as patterns during traversal
* [ ] directory entries behavior implemented

  * [ ] default may add explicit directory entries for empty dirs/placeholders
  * [ ] `-D` suppresses explicit directory entries
  * [ ] `-D` still archives files within directories normally
* [ ] `-j` junk paths implemented

  * [ ] store only basenames
  * [ ] discard directory components
  * [ ] collisions follow active mode behavior
* [ ] symlink behavior implemented

  * [ ] default follows symlinks and archives target contents
  * [ ] `-y` stores symlink as symlink

## Include and exclude filtering

* [ ] `-i pattern...` include-only filter implemented
* [ ] `-x pattern...` exclude filter implemented
* [ ] patterns match stored archive path after recursion mapping, `-j`, and normalization
* [ ] filter application order correct

  * [ ] generate candidate stored path
  * [ ] apply `-i` if present, must match at least one
  * [ ] apply `-x` if present, must not match any

## Pattern matching and normalization

* [ ] wildcard `*` matches any sequence including `/`
* [ ] wildcard `?` matches any single character
* [ ] bracket classes supported

  * [ ] ranges like `[a-f]`
  * [ ] negation like `[!bf]`
* [ ] pattern matching is case-sensitive
* [ ] stored paths always use `/`
* [ ] normalization rules implemented

  * [ ] no leading `/` in stored paths
  * [ ] `.` segments removed
  * [ ] `..` preserved only when explicitly present and allowed
  * [ ] filesystem inputs that traverse above working root rejected

## Date filtering

* [ ] `-t date` include only mtimes on or after date
* [ ] `-tt date` include only mtimes strictly before date
* [ ] accepted formats: `mmddyyyy` and `yyyy-mm-dd`
* [ ] `-t` and `-tt` may both be specified to form a range

## Compression

* [ ] `-0` store implemented
* [ ] `-1`..`-9` deflate level implemented
* [ ] default level is `-6`
* [ ] `-Z store` forces store
* [ ] `-Z deflate` forces deflate
* [ ] `-n suffixes` store-only suffix list implemented

  * [ ] colon- or comma-separated list
  * [ ] case-insensitive suffix matching

## Text end-of-line translation

* [ ] `-l` LF to CRLF conversion implemented
* [ ] `-ll` CRLF to LF conversion implemented
* [ ] binary detection on initial buffer skips translation
* [ ] translation happens before compression and CRC

## Output, logging, and temp files

* [ ] `-q` quiet mode implemented and stackable
* [ ] `-v` verbose mode implemented
* [ ] `zip -v` with no zipfile prints version info and exits success
* [ ] `-b dir` temp directory support implemented

  * [ ] seekable temp archive used for update/create when needed
  * [ ] temp files created with safe permissions
  * [ ] temp files cleaned up on failure where possible

## Archive testing

* [ ] `-T` tests completed archive before finalizing update
* [ ] `-T` failure affects exit status
* [ ] `-TT cmd` overrides test command

  * [ ] `{}` replaced with temp archive path when present
  * [ ] otherwise temp archive path appended to cmd
  * [ ] command exit status determines pass/fail

## Output to new archive

* [ ] `--out OUTARCHIVE` writes result to OUTARCHIVE
* [ ] OUTARCHIVE is always overwritten if it exists
* [ ] input archive remains unchanged when using `--out`

## Zipfile timestamp behavior

* [ ] `-o` sets output zipfile mtime to newest entry time in resulting archive

## Move semantics

* [ ] `-m` deletes original filesystem files after successful finalize
* [ ] deletions respect active mode semantics, only for successfully archived inputs
* [ ] no deletions for stdin-sourced entries

## Extra attributes policy

* [ ] `-X` strips extra file attributes per zip-utils policy
* [ ] stripping behavior deterministic
* [ ] stripping behavior documented

## Streaming behavior

* [ ] `zipfile == "-"` produces a valid streamed zip to stdout
* [ ] data descriptors used when required
* [ ] stdin streamed entry works for file output and stdout output unless stdin already consumed

## Unsupported options and rejection behavior

* [ ] reject with clear error and nonzero status:

  * [ ] `-s`, `-sp`, `-sb`, `-sv`
  * [ ] `-AS`, `-AC`, `-RE`
  * [ ] `-ic`
  * [ ] `-UN=...`
  * [ ] `-FI`
  * [ ] `-fz-`
  * [ ] `-db`, `-dc`, `-dd`, `-dg`, `-ds`, `-du`, `-dv`
  * [ ] `-lf`, `-la`, `-li`
  * [ ] `-MM`, `-nw`, `-sc`, `-sd`, `-so`, `-ws`
  * [ ] `-DF`, `--dif`
* [ ] help output does not advertise unsupported options

## Exit codes

* [ ] Match original Infozip in all cases.
