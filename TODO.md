# Work Tracking

## Planned Features
- [x] Implement Extra Fields support for metadata preservation
      - [x] Generate `UT` (0x5455, extended timestamp) and `Ux` (0x7875, UID/GID) extra fields in `src/format/writer.c`
      - [x] Parse and apply `UT` and `Ux` extra fields in `src/format/reader.c`
      - [x] Verify using `tests/integration/test_strip_attrs.py` (currently skipped)

## Bugs
- [ ] (No known bugs)

## Refactoring
- [ ] (No pending refactors)

## Documentation
- [ ] Expand `DESIGN.md` with data flow diagrams.