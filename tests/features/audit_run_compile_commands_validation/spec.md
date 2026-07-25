# audit_run compile_commands.json include-path validation (ANTS-1446)

`audit_run`'s clazy / clang-tidy invocations consume the project's
`compile_commands.json` via the `-p` flag. Every `-I` / `-isystem` /
`-iquote` / `-include` argument in the JSON is followed verbatim by
the underlying tool, so a hostile or misconfigured file with
`-include /home/user/.ssh/id_rsa` would load the named file into
every TU and surface its bytes through audit samples.

`AuditRunner::internal::validateCompileCommands` walks every entry's
arguments before the first child process spawns and refuses the run
with `code:"compile_commands_escape"` if any include-style path
escapes the project root AND isn't under a hardcoded system-include
prefix (`/usr/include`, `/usr/lib`, `/opt`, …).

## INVs

- INV-1: `extractIncludeArgs` returns the path argument for the
  `-I` / `-isystem` / `-iquote` / `-include` flags in both their
  split form (`-I /abs/path`) and their glued form (`-I/abs/path`).
- INV-2: `extractIncludeArgs` ignores other flags (e.g. `-O3`,
  `-std=c++20`, `-DFOO=bar`).
- INV-3: `splitCommandString` honours plain whitespace, double-
  quoted runs, single-quoted runs, and `\<char>` escapes.
- INV-4: `isIncludePathAllowed` accepts paths under the project
  root.
- INV-5: `isIncludePathAllowed` accepts paths under the system-
  include prefix allowlist (`/usr/include`, `/usr/lib`, `/opt`,
  `/lib`, etc.).
- INV-6: `isIncludePathAllowed` rejects paths outside the project
  root AND outside the system-include allowlist (e.g.
  `/home/user/.ssh`, `/tmp/secrets`).
- INV-7: `isIncludePathAllowed` rejects paths containing control
  characters or backslashes (defence-in-depth against argv-
  injection lookalikes).
- INV-8: `isIncludePathAllowed` resolves a relative include path
  against the entry's `directory` field, falling back to project
  root when `directory` is empty.
- INV-9: `validateCompileCommands` returns true (no error) when
  `compile_commands.json` is absent — the downstream tool
  surfaces its own `not_runnable` diagnostic.
- INV-10: `validateCompileCommands` returns true when the JSON
  contains only project-internal and system-prefix include paths.
- INV-11: `validateCompileCommands` returns false with an
  `errReason` mentioning the offending path when any entry has an
  include path that escapes both project and system prefixes.
- INV-12: `validateCompileCommands` handles both the
  `arguments[]` array form and the `command` string form.
- INV-13: `runAudit` calls `validateCompileCommandsImpl` before
  spawning processes, and returns
  `{ok:false, code:"compile_commands_escape", error:…}` on failure.
  (Source-grep verification.)
- INV-14: `validateCompileCommands` short-circuits with a refusal
  envelope when the JSON exceeds the 32 MiB byte cap or 50000
  entry cap (defence-in-depth against DoS via giant files).
- INV-15 (ANTS-3624): `validateCompileCommands` locates the DB
  through `AuditEngine::resolveCompileCommands` — the **same**
  resolver that builds the tools' `-p` argument — so every build-dir
  variant it walks (`build`, `build-fast`, `build-asan`,
  `build-workstation`, `build-release`, `build-debug`, `build-test`)
  is validated, plus a root-level `compile_commands.json` as the one
  location the shared probe does not cover.

  This closes a **silent bypass**, not a cosmetic mismatch. The
  validator previously hand-rolled a two-entry list (`build/` +
  project root) while clazy and clang-tidy resolved their DB through
  the seven-name probe. On a tree whose DB lived anywhere else the
  validator found no file, concluded "nothing to validate", returned
  true — and the tools then consumed that DB with its include paths
  never checked for escapes. It failed open exactly where a
  non-default build tree was in use, which on this project is the
  documented iteration workflow (`build-fast/`). ANTS-3367
  centralised the candidate list so precisely this drift could not
  happen; this call site predated it and was missed.

  INV-15b pins the other direction: a project with no DB anywhere
  still returns true, so the swap cannot turn "nothing to validate"
  into a refusal for every non-C/C++ project.
