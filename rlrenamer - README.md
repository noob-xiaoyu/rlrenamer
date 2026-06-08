# raylib_lex_renamer_tool

A small **C++17** utility to prefix/rename raylib identifiers using a lexer (token-based rewrite).

Core behavior:
- Rewrites **identifiers only** (e.g., `CloseWindow` → `RL_CloseWindow`)
- Does **not** touch occurrences inside **comments**, **string literals**, **char literals**, or **C++ raw strings**
- Can build symbol lists by **lexically parsing raylib headers** (no regex)

This is useful when you want to avoid name collisions (e.g. Windows SDK / other libraries) without patching system headers.

## Build

```bash
cmake -S . -B build
cmake --build build --config Release
```

Output: `build/raylib_renamer`

## Usage

### 1) Extract from raylib + rewrite a project (copy-to-output)

```bash
./build/raylib_renamer \
  --root path/to/your/project \
  --out  path/to/rewritten_project \
  --raylib-src path/to/raylib/repo_or_src \
  --using-expand-headers \
  --api-define RLAPI,RMAPI \
  --prefix RL_
```

### 2) In-place rewrite

```bash
./build/raylib_renamer \
  --root path/to/your/project \
  --in-place \
  --raylib-src path/to/raylib/repo_or_src \
  --using-expand-headers \
  --api-define RLAPI,RMAPI \
  --prefix RL_
```

### 3) Export symbol tables

Name-only list:

```bash
./build/raylib_renamer \
  --root path/to/your/project \
  --out  rewritten \
  --raylib-src path/to/raylib/repo_or_src \
  --using-expand-headers \
  --dump-symbols symbols.txt
```

Richer **symbol DB** (TSV: includes kind + arity hints for functions):

```bash
./build/raylib_renamer \
  --root path/to/your/project \
  --out  rewritten \
  --raylib-src path/to/raylib/repo_or_src \
  --using-expand-headers \
  --dump-symbols-db symbols.db.tsv

If you want to keep the symbol baseline strictly limited to the public header only, use
`--raylib-src ... --strict-public-api` together with `--raylib-h`.
```

You can reuse the DB later:

```bash
./build/raylib_renamer \
  --root path/to/your/project \
  --out  rewritten \
  --symbols-db symbols.db.tsv
```

### 4) Consumer mode (recommended for JackalClient-like projects)

Consumer mode adds safety heuristics:
- Skips identifiers inside preprocessor directives (unless enabled)
- Skips `obj.Member`, `obj->Member`, `Type::Member` qualified occurrences
- Heuristically detects **shadowed** (locally-defined) names per file and skips them
- For functions, replaces only when it looks like a call `Name(...)`, and (when available) checks argument count against the raylib signature hint
- If an occurrence looks suspicious, the default is **do not replace**

```bash
./build/raylib_renamer \
  --root path/to/JackalClient \
  --out  JackalClient_renamed \
  --symbols-db symbols.db.tsv \
  --consumer-mode \
  --export-consumer-context jc_context.json
```

If you really want to replace even suspicious occurrences:

```bash
./build/raylib_renamer ... --consumer-mode --consumer-force
```

### 5) Excluding directories

```bash
./build/raylib_renamer \
  --root path/to/your/project \
  --out rewritten \
  --symbols-db symbols.db.tsv \
  --exclude-dir external,build,.git
```

## Notes

- Windows `cmd.exe` quoting pitfall: avoid ending a quoted path with `\`.
  Use `"C:\\path\\to\\dir"` (no trailing backslash), or `"...\\dir\\."`.
- Preprocessor operator `defined(...)` is never rewritten.
- `--with-macros` / `--with-enum-values` are intentionally OFF by default because renaming those can break build flags or code assumptions.
