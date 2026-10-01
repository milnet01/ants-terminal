# Feature test — OSC 133 signing without the key on a command line (ANTS-5428)

The shell integration used to sign each prompt marker with
`openssl dgst -sha256 -hmac "$ANTS_OSC133_KEY"`. That puts the key in
openssl's argv, which `ps` shows to every local user while it runs.

`ants-osc133-sign` reads the key from standard input instead. The
scripts feed it with `printf`, a shell builtin, so the key reaches no
process's argv.

## Invariants

- **INV-1** — `printf '%s' KEY | ants-osc133-sign MESSAGE` prints the hex
  HMAC-SHA256 of MESSAGE under KEY's raw bytes, the value
  `terminalgrid.cpp`'s verifier computes, and exits 0. One trailing
  newline on the key is dropped, so `echo KEY |` signs the same.
- **INV-2** — The helper refuses with a non-zero exit and prints nothing
  on stdout when it gets no MESSAGE argument, an empty key, or a key over
  4 KiB.
- **INV-3** — `ants-osc133.bash` and `ants-osc133.zsh` sign through the
  helper when it is on `PATH`, and never call openssl then. Their
  signature equals openssl's for the same key and message.
- **INV-4** — Where the helper is not on `PATH` (the Flatpak and the
  AppImage), the scripts fall back to openssl and still sign.
  `packaging/shell-integration/README.md` says so.
- **INV-5** — `cmake --install` installs the helper beside
  `ants-terminal`, and the openSUSE spec lists it.

INV-1 to INV-4 are checked by `test_osc133_sign.sh`; zsh is checked
where it is installed. INV-5 is a packaging line, read in review.
