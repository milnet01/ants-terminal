# Feature spec: NASM, CMake and YAML in the code-reading verbs (ANTS-6801)

Status: shipped
Kind: fix
Source: DeFragR feedback 2026-11-16

## Problem

`indie_review_partition`'s computed walk, `function_sizes` and
`comment_claims` admit a file only when `CodebaseIndex::isIndexableFile`
does. That predicate follows what `file_outline` can read, and
`file_outline` had no NASM, CMake or YAML mode. On DeFragR the partition left
15 files unassigned (the NASM engine, both `CMakeLists.txt`, the YAML), and
the two readability verbs read none of the NASM files.

## Surface

- `FileOutline::Mode::Asm`, `Mode::Cmake` and `Mode::Yaml`, reached by
  extension, by an explicit `mode`, and for CMake by the name
  `CMakeLists.txt`.
- `CodebaseIndex::isIndexableSuffix` and `isIndexableFile` admit the same
  files, so the partition, `codebase_index`, `function_sizes` and
  `comment_claims` all see them.
- `function_sizes` measures a NASM label and a CMake function.
- `comment_claims` reads `;` comments in a NASM file.

A `Containerfile` stays out: `file_outline` has no mode for it, and the
predicate admits only what the outline reads.

## Invariants

- **INV-1 NASM outline.** A `.asm`, `.nasm` or `.inc` file reports
  `language:"asm"`. A global label (`name:` at column 0) and a `%macro` are
  kind `func`; an `equ` constant is kind `const`. A local label (`.loop:`)
  and an indented instruction are not symbols.
- **INV-2 CMake outline.** `CMakeLists.txt` and a `.cmake` file report
  `language:"cmake"`, not `md`. `function()` and `macro()` are kind `func`,
  in any letter case. `add_executable`, `add_library` and
  `add_custom_target` are kind `type`.
- **INV-3 YAML outline.** A `.yml` or `.yaml` file reports
  `language:"yaml"`. A top-level key is kind `const`; an indented key is not.
- **INV-4 the file filter.** `isIndexableFile` admits `.asm`, `.nasm`,
  `.inc`, `.cmake`, `CMakeLists.txt`, `.yml` and `.yaml`. A `Containerfile`
  and a plain `.txt` file stay out (the control).
- **INV-5 explicit mode.** `parseMode` accepts `asm`, `cmake` and `yaml`.
- **INV-6 function sizes.** A NASM label's body runs to the last non-blank
  line before the next symbol. A CMake function's body runs to its
  `endfunction()` line.
- **INV-7 NASM comments.** `comment_claims` harvests a `;` comment, and a `;`
  inside a quoted string is not one.

## Test

`test_file_outline_asm_cmake_yaml.cpp` in the `test_core` bundle: one case
per invariant, against files written into a `QTemporaryDir`.
