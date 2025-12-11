<p align="center">
  <img src="zip-utils.svg" alt="InfoZIP Utils logo" width="220">
</p>

# ZIP Utils

Parallel rewrite of Info-ZIP’s classic `zip` and `unzip` utilities. Legacy sources are preserved under `legacy/` for reference only; the new `src/` tree is a clean, reentrant scaffold that currently parses CLI options but does not yet implement archive operations.

---

## Highlights (current state)

* Strangler Fig approach: legacy sources parked in `legacy/`, clean rewrite lives in `src/`
* New `ZContext` replaces globals; CLI stubs parse legacy flags into the context
* Meson builds only the new stubs (not installed) to unblock incremental development
* Legacy docs/artifacts kept in root for reference (`History.600`, `zip.txt`, `unzip.txt`)

---

## Build

### Requirements

* Meson ≥ 1.2
* Ninja

### Steps

```bash
meson setup build
ninja -C build
```

Outputs: `build/zip` and `build/unzip` stubs linked against the new `libziputils` scaffold. Install is disabled until functionality is in place.

---

## Testing

The new pipeline does not yet include tests. The legacy harness remains at `legacy/test.sh` for historical reference; new unit/integration tests will land alongside the rewritten modules in `src/`.

---

## Differences from Info-ZIP 6.0 (planned)

* Linux/Unix-only targets; no legacy per-OS glue
* Reentrant API centered on `ZContext` instead of globals
* Thin CLI wrappers over a library-first core
* Feature parity and compression backends are being rebuilt incrementally

---

## Repository Layout

```
.
├── legacy/        # original sources + scripts (reference only, not built)
├── src/
│   ├── include/   # public headers (ziputils.h)
│   ├── common/    # shared context, status, file I/O, util
│   ├── compression/  # zlib/CRC shims (planned)
│   ├── format/    # ZIP container parsing/writing (planned)
│   └── cli/       # thin zip/unzip entrypoints + stub ops
├── meson.build    # builds new scaffold
└── CHECKLIST.md   # rewrite roadmap
```

Legacy reference material from the original project (e.g. `History.600`, `zip.txt`, `unzip.txt`) is preserved for context.

---

## License

InfoZIP Utils continues to use the permissive Info-ZIP license. See [`LICENSE`](LICENSE) for the current text and `COPYING.OLD` for the historical notice.

---

## Acknowledgments

Thanks to the original Info-ZIP contributors and the broader community keeping classic ZIP tooling alive on modern systems.
