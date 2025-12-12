## Using `.llm/meta.jsonl`

`.llm/meta.jsonl` is a JSON Lines index of “things worth jumping to” in this repo. Each line is a single JSON object produced by `ctags` that points at either documentation structure (chapters/sections) or code symbols (functions, structs, macros, etc). The file is meant to help an agent navigate and answer questions quickly without doing a blind full-text search first.

### Format

* The file is **JSONL**: one JSON object per line
* Each object has, at minimum:

  * `id`: unique-ish identifier for the entry
  * `name`: symbol or heading name
  * `kind`: what it is (`file`, `chapter`, `section`, `function`, `struct`, `macro`, `typedef`, `enum`, `member`, etc)
  * `file`: repo-relative path to open
  * `line`: 1-based line number within that file
  * `language`: source language (`Markdown`, `C`, `C++`, `Meson`, `Python`, etc)
  * `extra`: optional hierarchy/context (e.g. `chapter`, `section`, `typeref`, enum owner, access)

These records are **pointers** only. Always open the referenced file and read surrounding context before making claims, edits, or patches.

### What to use it for

Use `.llm/meta.jsonl` as your primary navigation index to:

* Jump directly to relevant docs sections (`README.md::section::Build`, `HACKING.md::section::Testing Guidelines`, etc)
* Find where a symbol is defined (`compress_to_temp`, `ZContext`, `ZU_SIG_END64`, etc)
* Discover public API surface (`src/include/ziputils.h`) versus internal implementation
* Generate breadcrumbs for answers (“Zip Utils → Build (README.md:62)”)

### Recommended workflow

1. **Parse it line-by-line**

   * Treat it as JSONL, not a JSON array
   * Skip and log malformed lines instead of failing hard

2. **Build quick lookup indexes**

   * `by_id[id] -> entry`
   * `by_file[file] -> entries sorted by line`
   * `by_name[name] -> entries`
   * `by_kind[kind] -> entries`
   * optionally `by_chapter[extra.chapter] -> entries`

3. **Resolve a user question to candidate entries**

   * Prefer doc headings for “how do I build/test/use”
   * Prefer code symbols for “what does this function/macro do”
   * Use `extra.typeref` to jump from a typedef to its struct definition

4. **Open the source and verify**

   * Open `entry.file` at `entry.line`
   * Read enough context to capture the full definition or the full section
   * Confirm the heading/definition text matches `entry.name` before using it

5. **Cross-check usage**

   * After reading the definition, search for references (call sites, macro usage, field access)
   * Use usage to confirm semantics, ownership rules, and error conventions

### Disambiguation rules

If multiple entries match the same name:

* Prefer exact `id` matches when available
* Otherwise prefer:

  * the file the user referenced
  * more specific kinds (`subsection` over `section`, `struct` over `file`)
  * public headers over internal files when the question is about API behavior
* If ambiguity remains, present a short list of candidates (file + line + kind) and pick the most likely default

### Notes on common `kind` values

* `section`, `subsection`, `chapter`: documentation anchors, best for onboarding questions
* `function`: jump to implementation, then validate via call sites
* `macro`: could be include guards, constants, or feature toggles
* `typedef` / `struct`: follow `typeref` when present to find the real definition
* `member`: jump into the owning struct, then verify semantics by searching usage

### Editing guidance

When modifying docs or code based on an entry:

* Re-locate the target by opening `file` at `line`
* Confirm you’re in the right section/definition (line numbers can drift)
* For Markdown sections, edit within the section boundary (until the next same-or-higher header)
* For code symbols, ensure you update call sites/tests if you change signatures or behavior

### What not to assume

* Line numbers can drift as files change
* Generated names like `__anon...` are unstable and should not be exposed as user-facing identifiers
* The index does not prove semantics, only location—always read the real source
