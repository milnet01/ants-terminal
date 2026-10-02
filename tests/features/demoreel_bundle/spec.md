# demoreel bundle (ANTS-5617)

Ants Terminal ships the full demoreel recorder at
`<libexecdir>/ants-terminal/demoreel`, pinned to one release tag and its
sha256 in `CMakeLists.txt`.

- **INV-1** — Installing the `demoreel` component puts an executable
  `libexec/ants-terminal/demoreel` under the prefix, and running it with
  `--version` prints exactly `demoreel <ANTS_DEMOREEL_VERSION>`.
  *Breaks when:* the install rule is dropped, installs without the execute
  bit, or the vendored file and the pinned version disagree.
- **INV-2** — A vendored file whose sha256 differs from the pin fails the
  CMake configure. Checked by `file(SHA256)` in `CMakeLists.txt`, not by
  this test.

The test skips (exit 77) when `python3` is absent, since the script cannot
run without it.

Test: `test_demoreel_bundle.sh`, ctest name `demoreel_bundle`.
