# Flathub manifest transformer — conformance spec

**Contract:** `packaging/flatpak/make-flathub-manifest.sh` rewrites the
development Flatpak manifest into a Flathub-ready manifest. Only the
`ants-terminal` module's source block changes; every other byte stays
identical. Regressions break the Flathub submission flow silently — the
PR against `flathub/flathub` looks right but ships a manifest that
either doesn't build reproducibly (no `tag:`) or duplicates the `lua`
module / misses `x-checker-data`.

## Invariants

- **INV-1 — `dir` source removed.** Output must contain neither
  `type: dir` nor `path: ../..`. Presence means flatpak-builder would
  try to checkout a working tree the Flathub CI doesn't have.

- **INV-2 — `git` source with pinned tag.** Output must contain
  `type: git`, `url: https://github.com/milnet01/ants-terminal`, and
  `tag: v<VERSION>` where VERSION matches the argument (or the
  CMakeLists.txt version when no argument is passed).

- **INV-3 — Lua module preserved.** The `- name: lua` module and its
  `x-checker-data` stanza survive the transformation byte-identical.
  Regressions here would ship Flathub a Lua-less manifest.

- **INV-4 — Version pattern auto-detect.** Running the script with no
  argument reads VERSION from `CMakeLists.txt` and emits the matching
  `tag: v<VERSION>` line. The `/bump` recipe already bumps
  `CMakeLists.txt`, so Flathub-manifest regeneration requires no
  second version-bump step.

- **INV-5 — Generated-file header.** Output begins with a
  `# GENERATED FILE` banner pointing at the source manifest and the
  regeneration command so a Flathub reviewer can trace where the file
  came from.

- **INV-6 — Invalid VERSION rejected.** Passing something that is not
  `X.Y.Z` (e.g. `1.2`, `abc`, `1.2.3.4`) exits non-zero with an
  error message on stderr. A silently-accepted bogus tag would ship
  a manifest that references a non-existent git ref.
