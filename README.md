<p align="center">
  <img src=".github/zip-utils.svg" alt="ziputils logo" width="220">
</p>

# zip-utils

[![CI with Clang + Sanitizers](https://github.com/jopamo/zip-utils/actions/workflows/ci.yml/badge.svg)](https://github.com/jopamo/zip-utils/actions/workflows/ci.yml)
![Language](https://img.shields.io/badge/language-C-blue.svg)
![Build System](https://img.shields.io/badge/build%20system-Meson-orange.svg)

Modern rewrite of Info-ZIP's `zip`, `unzip`, and `zipinfo` with a reentrant core (`libziputils`) and thin CLIs aimed at drop-in compatibility.

## Features
- Targets Info-ZIP `zip` 3.0 / `unzip` 6.0 behavior; unsupported flags fail fast instead of silently diverging.
- Library-first design: CLIs only parse args and populate `ZContext`, leaving core logic in `libziputils`.
- Consult `zip-spec.md` and `unzip-spec.md` for detailed behavior and option coverage.
- No support for split archives (returns not-implemented error).

## Installation / Usage

### Build from Source
This project requires compilation. See [HACKING.md](HACKING.md) for detailed development requirements.

```bash
meson setup build
ninja -C build
```

The binaries `zip`, `unzip`, and `zipinfo` will be available in the `build/` directory.

### Examples
- **Create an archive**: `build/zip out.zip file1.txt dir/`
- **List contents**: `build/unzip -l out.zip`
- **Extract with paths**: `build/unzip out.zip`
- **Stream to stdout**: `build/unzip -p out.zip path/in/archive > file`
- **Keep verbose logging**: `build/zip -lf log.txt -li out.zip file1`

## License
InfoZIP
