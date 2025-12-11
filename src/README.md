# Modern Rewrite Scaffold

Goal: rebuild `zip` and `unzip` in a reentrant, global-free style while keeping the legacy tree around for reference (`legacy/`).

## Layout

- `src/include/ziputils.h` — public-facing status codes.
- `src/common/` — shared utilities (`ctx`, `strlist`, status helpers).
- `src/cli/` — thin CLI entry points and stub ops.
- `legacy/` — frozen legacy sources (not built).

The current Meson build only compiles the new `zip`/`unzip` stubs (not installed). They parse CLI flags into `ZContext` but do not yet perform archive work.

## Option capture (current stubs)

- Zip: `-r`, `-j`, `-m`, `-d`, `-f`, `-u`, `-T`, `-q`, `-v`, `-0`..`-9`, `-i pattern`, `-x pattern`, `archive.zip`, inputs. `archive.zip` of `-` toggles `output_to_stdout`.
- Unzip: `-l`, `-t`, `-d DIR`, `-o`, `-n`, `-q`, `-v`, `-C`, `-i pattern`, `-x pattern`, `archive.zip`, patterns.

Unknown or unsupported options are rejected with usage for now. Expand the tables as functionality lands.
