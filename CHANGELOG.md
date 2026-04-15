# Changelog

All notable changes to Ants Terminal are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Sections use the standard categories — **Added** for new features, **Changed**
for changes in existing behavior, **Deprecated** for soon-to-be-removed features,
**Removed** for now-removed features, **Fixed** for bug fixes, and **Security**
for security-relevant changes.

## [Unreleased]

Nothing yet — items queued for 0.7 live in [ROADMAP.md](ROADMAP.md).

## [0.6.22] — 2026-04-15

**Theme:** root-cause fix for the main-screen TUI "scrollback doubling"
regression, plus packaging catch-up (four recipes bumped to 0.6.22, three
missing AppStream `<release>` entries, openSUSE `%post`/`%postun`
scriptlets, Debian `postinst`/`postrm`). The scrollback fix is the
headline — Claude Code v2.1+ repaints its entire TUI on *every* state
update (not just `/compact`), and each repaint was pushing a full-screen
worth of duplicate content into scrollback. 0.6.21's scrollback-insert
pause only triggered while the user was scrolled up; this release adds
a sliding time window anchored to main-screen `CSI 2J`, so the
suppression works at the bottom too.

### Fixed

- **Visual artifacts from leaked SGR decorations.** User-reported: TUI
  apps (Claude Code v2.1+ is the motivating case) leave strikethrough /
  plain-single underline active across rows, producing horizontal
  dashed lines on otherwise-blank rows and leaked decorations on
  pending task-list items. Two-layer fix: (a) render-side filter skips
  strikethrough and plain single-color underline draws on empty-glyph
  cells (space / null codepoint) — the attribute is still recorded,
  just not painted where it has no glyph to decorate, so "underlined
  text with a space in the middle" still draws correctly per-glyph;
  (b) SGR 8 / 28 (conceal / reveal) and SGR 53 / 55 (overline /
  no-overline) now have explicit handlers instead of falling through
  to the default branch silently. Hovered-URL underline hint always
  draws regardless of cell content. See
  `tests/features/sgr_attribute_reset/spec.md` for the invariant
  asserted by CTest.
- **Scrollback no longer accumulates duplicates during main-screen TUI
  repaints.** On main-screen full-display erase (`CSI 2J`,
  `eraseInDisplay(2)`), open a 250 ms sliding window during which
  `scrollUp()` drops the push-to-scrollback step. Each scrollUp during
  the window extends it, so the window survives the entire repaint
  burst regardless of length, but closes promptly when the app goes
  quiet. Alt-screen bypasses scrollback already and is unaffected;
  mode-3 erase (which clears scrollback by request) is also unaffected.
  Defensively cleared on alt-screen entry. The 0.6.21 `m_scrollOffset
  > 0` pause (for users actively reading history) is retained — the
  two triggers compose (either one suppresses). File: `terminalgrid.cpp`
  `eraseInDisplay()` + `scrollUp()`; header additions in
  `terminalgrid.h` (`m_csiClearRedrawActive`, `m_csiClearRedrawTimer`,
  `kCsiClearRedrawWindowMs`).

### Added — Audit tooling

- **Trivy filesystem-scan lane in Project Audit.** New "Trivy
  Filesystem Scan" check runs `trivy fs` with the `vuln`, `secret`,
  and `misconfig` scanners in one invocation, severity-floor
  MEDIUM. Output is piped through `jq` into the standard
  `path:line: severity: scanner/rule-id: title` shape so
  `parseFindings()` consumes it directly. Self-disables when
  either `trivy` or `jq` is missing (the description text
  explains how to install). Auto-selected when both are present.
- **Three new audit rules.** `cmake_no_version_floor` (find_package
  REQUIRED with no version constraint), `bash_c_non_literal`
  (`bash -c <ident>` injection-sink heuristic, restricted to lines
  containing `bash` / `sh` so it doesn't flag `grep -c`), and
  `packaging_version_drift` (cross-file consistency: extracts
  CMakeLists.txt project(VERSION X.Y.Z) and diffs it against
  every packaging recipe). Both regex-based rules ship with
  bad/good fixture pairs under `tests/audit_fixtures/`; the
  drift check is exercised by the audit dialog itself.

### Added — Test infrastructure

- **Feature-conformance test harness (`tests/features/`).** New test
  lane alongside the existing `audit_rule_fixtures`. Each feature
  subdirectory ships a `spec.md` (human contract — reviewed like
  code) and a C++ test executable that exercises the feature through
  its public API and asserts the contract holds. GUI-free, fast,
  labelled `features` so CI and local `ctest -L features` pick them
  up. First test landed: `scrollback_redraw` — the regression guard
  for this release's headline fix. Verified that it FAILS against
  0.6.21 code (growth=77 vs. threshold=34, reproducing the doubling)
  and PASSES against 0.6.22 — i.e. it *would have caught the
  incomplete 0.6.21 fix at commit time*. See `tests/features/README.md`
  for the authoring guide and rationale. CLAUDE.md updated.

### Changed — Packaging

- **openSUSE spec, Arch PKGBUILD, Debian changelog, AppStream metainfo
  all bumped to 0.6.22.** Prior releases had drifted: spec and PKGBUILD
  still showed 0.6.20, metainfo's most recent `<release>` was 0.6.17
  (so 0.6.18/.19/.20/.21 were invisible to GNOME Software / KDE
  Discover users reading the metainfo catalogue).
- **openSUSE spec gets `%post`/`%postun` scriptlets** (`update-desktop-
  database`, `gtk-update-icon-cache`). Without these, minimal
  Tumbleweed images won't see the launcher in the application menu
  until next session restart. Matches Fedora/openSUSE packaging
  guidelines for any package shipping a `.desktop` entry + hicolor
  icons.

## [0.6.21] — 2026-04-15

**Theme:** audit-tool noise reduction, dialog theming, and a long-
standing main-screen-TUI scrollback corruption fix. Self-audit triage
turned up a GNU grep 3.12 argument-ordering quirk that silently
disabled the `--include` file-type filter, producing the bulk of the
CRITICAL/MAJOR false positives. Same release extends the QSS cascade
so every popup dialog inherits the terminal's active theme, and adds
a scrollback-insert pause that stops TUI redraws from interleaving
intermediate frames into the history the user is trying to read.

### Changed

- **Scroll-lock on scrollback read (main-screen TUI fix).** When a
  TUI program runs on the main screen (Claude Code v2.1+ is the
  flagship example — it renders its approval prompts and context bar
  via cursor movement + overwrite, not alt-screen) and the user has
  scrolled up to read history, every cursor-up-and-rewrite cycle that
  extends past the scroll region used to push the overwritten top
  line into scrollback. With a redraw-heavy TUI that fires many
  frames per second (spinner, context growth, expanding tool blocks),
  this interleaved duplicate frames into the scrollback the user was
  reading — the user's perceived position stayed stable (viewport
  anchor shifts with `scrollbackPushed`), but the history below their
  read point filled with duplicated content and the "scrolled" view
  drifted. Fix: `TerminalGrid::scrollUp()` now honours a new
  `scrollbackInsertPaused` flag, set by `TerminalWidget::onPtyData()`
  while `m_scrollOffset > 0`. Lines that would have been pushed into
  scrollback during a paused window are simply dropped (they were
  about to be overwritten in-place anyway). Flag auto-clears on the
  next PTY packet once the user returns to the bottom.
- **Dialog theming cascade.** `MainWindow::applyTheme()` now ships
  a comprehensive QSS covering `QDialog`, `QLabel`, `QPushButton`,
  `QLineEdit`, `QTextEdit` / `QPlainTextEdit` / `QTextBrowser`,
  `QCheckBox`, `QRadioButton`, `QComboBox`, `QSpinBox` /
  `QDoubleSpinBox`, `QGroupBox`, `QListWidget` / `QTreeWidget` /
  `QTableWidget`, `QHeaderView`, `QScrollBar` (v+h), `QToolTip`,
  `QProgressBar`, and `QDialogButtonBox`. Every pop-up created with
  the main window as parent (Settings, Audit, AI, SSH, Claude
  Projects/Transcript/Allowlist, homograph-link warning, permission
  prompts, `QMessageBox`, `QInputDialog`, `QFileDialog`, `QColorDialog`,
  …) now paints with the terminal's active theme colors — no more
  light-gray system defaults leaking through. Cached dialogs are
  re-polished on theme switch so live widgets pick up new colors
  without re-instantiation.

### Fixed

- **Audit: grep argument-ordering bug (major noise source).** GNU grep
  3.12 silently drops the `--include=<glob>` filter whenever a file-level
  `--exclude=<name>` appears earlier on the command line — every file
  under the search root is scanned regardless of extension. Our
  `addGrepCheck()` builder put `kGrepExclSec` (which carried
  `--exclude=auditdialog.cpp --exclude=auditdialog.h`) before
  `kGrepIncludeSource`, so security scans that should have been scoped
  to source files were also matching `.md`, `.xml`, `CMakeLists.txt`,
  and the audit tool's own pattern-definition strings. Split the
  exclude-dir and file-exclude portions, append file-excludes AFTER all
  `--include` flags (including caller-supplied extras). Cuts the
  CRITICAL `cmd_injection` false-positive count from 8 → 4 (remaining
  hits are either real — `execlp` in the forkpty child, by design — or
  intentional test fixtures).
- **Audit: `lineIsCode()` over-counted string-only lines as code.**
  The comment/string-aware filter flagged continuation lines like
  `"system(), popen()…", "Security",` as real code because the
  string-delimiter character itself was treated as a code token.
  Tightened to require an identifier- or operator-class character
  outside strings/comments before a line counts as code. Drops the
  remaining self-referential matches in `auditdialog.cpp`'s own
  pattern-definition arguments without affecting legitimate mixed
  code+string lines like `int n = f("foo");`.
- **Audit: cppcheck `invalidSuppression` noise.** cppcheck's own
  `--inline-suppr` parser misreads documentation comments that mention
  the literal `cppcheck-suppress` token (e.g. the passthrough-marker
  docs in `auditdialog.cpp`) as actual suppression directives and
  emits `invalidSuppression` errors. Added `--suppress=invalidSuppression`
  to both cppcheck invocations — the category catches only meta-parsing
  failures, never real code bugs, so losing it has no downside. Also
  restructured the offending docs (`auditdialog.{h,cpp}`) so the token
  no longer appears as the first word of a comment body.

## [0.6.20] — 2026-04-15

**Theme:** ROADMAP §H5 (Distribution-readiness bundle 5) — ready-to-submit
packaging recipes for the three mainstream non-Flatpak distros. Each
recipe drives the existing CMake install rules (no build-system forks,
no per-distro patches) and runs the audit-rule regression suite in its
`%check` / `check()` / `dh_auto_test` step. Fully additive, no runtime
code changes.

### Added

- **`packaging/opensuse/ants-terminal.spec`** — openSUSE RPM spec
  targeting Tumbleweed. Uses the core [openSUSE CMake macros][suse-cmake]
  (`%cmake`, `%cmake_build`, `%cmake_install`, `%ctest`, `%autosetup`)
  so the file is close to portable to Fedora. BuildRequires declared
  via `cmake(Qt6*)` pkgconfig-style entries so OBS resolves them
  against whichever Qt6 stack the target project carries. `%files`
  enumerates all fifteen install paths explicitly so a missing or
  relocated artefact fails the OBS build instead of producing a
  silently-incomplete package.
- **`packaging/archlinux/PKGBUILD`** — [AUR][aur] recipe on the release
  track (package name `ants-terminal`). `check()` runs ctest. `sha256sums`
  is `SKIP` in the in-tree recipe with a comment pointing packagers at
  `updpkgsums` since the upstream tarball doesn't exist until the tag
  is pushed. A rolling `-git` variant is documented in
  `packaging/README.md` (three-line diff: `pkgname`, `source`,
  `pkgver()`).
- **`packaging/debian/`** — Debian source tree: `control`, `rules`,
  `changelog`, `copyright`, `source/format`. `debhelper-compat 13`
  drives `dh $@ --buildsystem=cmake` with Ninja as the backend; DEP-5
  `copyright` carries the full MIT license text.
  `DEB_BUILD_MAINT_OPTIONS = hardening=+all` stacks dpkg-buildflags'
  hardening wrappers on top of our CMake hardening flags. Suitable
  for `debuild -uc -us`, a Launchpad PPA, or an eventual Debian ITP.
- **`packaging/README.md`** — one-page build / submission guide for
  all three recipes: local-build recipe, OBS / AUR / PPA submission
  flow, `dch` / `osc vc` / `updpkgsums` version-bump recipes, and
  the `-git` variant diff for Arch.

### Changed

- `CMakeLists.txt` `project(... VERSION 0.6.19)` → `0.6.20`. The
  single-source-of-truth macro `ANTS_VERSION` propagates to
  `setApplicationVersion`, the SARIF driver, the dialog title badge,
  and the HTML-export metadata on rebuild.
- `packaging/linux/ants-terminal.1` `.TH` line now reads
  `ants-terminal 0.6.20` (was stuck at `0.6.18` — missed the 0.6.19
  bump). groff ships the version string in its header/footer, so
  `man ants-terminal` now matches `ants-terminal --version` again.
- README current-version line bumped to 0.6.20. The **Install
  footprint** table didn't need to change — H5 is pure packaging
  metadata, not new installed files.
- ROADMAP §H5 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.8.0 📦 narrative section, with a link
  back to this CHANGELOG entry. H1–H5 distribution slice now five
  bundles deep; remaining 0.8.0 packaging work is H6 (Flatpak) and
  H7 (docs site).

[suse-cmake]: https://en.opensuse.org/openSUSE:Packaging_for_Leap
[aur]: https://wiki.archlinux.org/title/Arch_User_Repository

## [0.6.19] — 2026-04-15

**Theme:** ROADMAP §H4 (Distribution-readiness bundle 4 of 4) — shell
completions for the current `QCommandLineParser` surface, installed to
the conventional vendor locations for bash, zsh, and fish so distro
packages don't have to relocate them. Closes the H1–H4 distribution
slice; remaining packaging work (H5 spec/PKGBUILD/debian, H6 Flatpak,
H7 docs site) lives in 0.8.0+. Fully additive, no runtime code changes.

### Added

- **`packaging/completions/ants-terminal.bash`** — [bash-completion][bash-completion]
  function `_ants_terminal` registered via `complete -F`. Suggests the
  full long/short option set; suppresses suggestions after `--new-plugin`
  (the next token is a freeform plugin name with no useful enumeration).
- **`packaging/completions/_ants-terminal`** — [zsh `#compdef`][zsh-compsys]
  function using `_arguments`, with mutually-exclusive groups so `--quake`
  and `--dropdown` only offer the unspecified alias, and `-h` / `-v`
  short-circuit further completion (`(- *)` exclusion).
- **`packaging/completions/ants-terminal.fish`** — [fish `complete`][fish-complete]
  declarations, one per flag, with descriptions that mirror the manpage
  one-liners. `--new-plugin` declared `-r -x` so fish knows it consumes
  the next token but doesn't try to file-complete it.
- **CMake install rules** for all three files at the GNU vendor
  locations: `${datarootdir}/bash-completion/completions/ants-terminal`,
  `${datarootdir}/zsh/site-functions/_ants-terminal`, and
  `${datarootdir}/fish/vendor_completions.d/ants-terminal.fish`. All
  three are auto-discovered without the user sourcing anything by hand.
- **CI lint job** — `.github/workflows/ci.yml` runs `bash -n` on the
  bash file, `zsh -n` on the zsh file, and `fish --no-execute` on the
  fish file so syntax regressions fail the build the same way the H2
  AppStream / desktop-entry validators and H3 groff lint do.

### Changed

- README **Install System-wide** table gains three rows for the
  completion files so the install footprint stays self-documenting.
- ROADMAP §H4 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.7.0 📦 narrative section, with links back
  to this CHANGELOG entry. Closes out the H1–H4 distribution slice.

[bash-completion]: https://github.com/scop/bash-completion
[zsh-compsys]: https://zsh.sourceforge.io/Doc/Release/Completion-System.html
[fish-complete]: https://fishshell.com/docs/current/cmds/complete.html

## [0.6.18] — 2026-04-15

**Theme:** ROADMAP §H3 (Distribution-readiness bundle 3 of 4) — a
spec-compliant `man 1 ants-terminal` page so distro packagers and
users alike get the standard Unix discovery experience (`man
ants-terminal`, `apropos terminal`, `whatis ants-terminal`). Fully
additive, no runtime code changes.

### Added

- **`packaging/linux/ants-terminal.1`** — [groff -man][groff-man]
  source covering `NAME` / `SYNOPSIS` / `DESCRIPTION` / `OPTIONS`
  (`-h`/`--help`, `-v`/`--version`, `--quake`/`--dropdown`, and
  `--new-plugin <name>` with its full validation contract) /
  `ENVIRONMENT` (`SHELL`, `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`,
  `ANTS_PLUGIN_DEV`, `QT_QPA_PLATFORM`) / `FILES` (config /
  themes / plugins / sessions / recordings / logs / `audit_rules.json`
  / `.audit_suppress`) / `EXIT STATUS` (the four `--new-plugin` codes)
  / `BUGS` / `AUTHORS` / `SEE ALSO` (cross-refs to xterm, konsole,
  gnome-terminal, tmux, ssh, forkpty(3), appstreamcli,
  desktop-file-validate). Renders cleanly under both `groff -man` and
  `man -l`.
- **CMake install rule** for the man page — `install(FILES … DESTINATION
  ${CMAKE_INSTALL_MANDIR}/man1)`. DESTDIR-staged install verified to
  drop the page at `…/share/man/man1/ants-terminal.1`, which `man-db`
  picks up automatically once the prefix is on `MANPATH` (true for
  `/usr` and `/usr/local` out of the box).
- **CI lint job** — `.github/workflows/ci.yml` installs `groff` and
  runs `groff -man -Tutf8 -wall packaging/linux/ants-terminal.1
  >/dev/null` so syntax regressions in the man source fail the build
  the same way `appstreamcli` / `desktop-file-validate` do for the
  desktop entry and metainfo XML.

### Changed

- README **Install System-wide** table gains a row for the man page so
  the install footprint stays self-documenting alongside the binary,
  desktop entry, metainfo, and icons.
- ROADMAP §H3 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.7.0 📦 narrative section, with links back
  to this CHANGELOG entry.

[groff-man]: https://man7.org/linux/man-pages/man7/groff_man.7.html

## [0.6.17] — 2026-04-14

**Theme:** ROADMAP §H2 (Distribution-readiness bundle 2 of 4) —
AppStream metainfo and a spec-compliant desktop entry so Ants appears
in GNOME Software / KDE Discover catalogues once a distro packages it.
Fully additive, no runtime code changes.

### Added

- **`packaging/linux/org.ants.Terminal.metainfo.xml`** — [AppStream 1.0][as1]
  descriptor: `id` / `name` / `summary` / multi-paragraph
  `description` / `categories` / `keywords` / `url` (homepage,
  bugtracker, vcs, help, contribute) / `provides/binary` /
  `supports/control` / `content_rating` (OARS 1.1, all-none) /
  `launchable` tying to the `.desktop` id / `releases` with 0.6.14–
  0.6.17 changelog summaries. `metadata_license` CC0-1.0,
  `project_license` MIT. Validated by `appstreamcli validate`
  (pedantic clean aside from the reverse-DNS uppercase hint, which
  matches the convention used by `org.gnome.Terminal` and
  `org.kde.Konsole`).
- **`packaging/linux/org.ants.Terminal.desktop`** — reverse-DNS app
  id so it round-trips with the metainfo `launchable`. Fields:
  `Type=Application`, `Terminal=false`, `Categories=Qt;System;Terminal-
  Emulator;` (exactly one main category — avoids the multi-listing
  menu hint), `Keywords=` tightened, `StartupWMClass=ants-terminal`,
  `TryExec=ants-terminal`. Two `Desktop Action` entries — `NewWindow`
  and `QuakeMode` (wires `--quake`) — for right-click launcher
  integration in most DEs. Validated by `desktop-file-validate` clean.
- **CMake install rules** (`include(GNUInstallDirs)`) for the desktop
  file (`share/applications/`), metainfo XML (`share/metainfo/`), and
  the six hicolor icons renamed to `ants-terminal.png` under
  `share/icons/hicolor/<size>x<size>/apps/`. `DESTDIR=…` staging works
  out of the box for distro build roots.
- **CI validation job** — `.github/workflows/ci.yml` installs
  `appstream` + `desktop-file-utils` and runs `appstreamcli validate
  --explain` + `desktop-file-validate` on every push so schema drift
  in the packaging files fails the build instead of landing in a
  release tarball.

### Changed

- README **Install System-wide** section replaces the bare `make
  install` with `cmake --install build`, adds a table of every path
  the install rule lays down (bin/ desktop/ metainfo/ hicolor icons),
  and notes `DESTDIR=…` support for packagers.
- README **Desktop Entry** section split into two paths: the dev
  workflow (`ants-terminal.desktop.in` + `launch.sh` for running
  uninstalled from the build tree) and the distro workflow (the new
  `packaging/linux/` files installed via CMake). Dev path unchanged;
  packaged path is new.
- README **Project Structure** map mentions `packaging/linux/`.
- ROADMAP §H2 status 📋 → ✅ in both the distribution-adoption
  overview table and the §0.7.0 📦 narrative section, with links to
  this CHANGELOG entry.

[as1]: https://www.freedesktop.org/software/appstream/docs/

## [0.6.16] — 2026-04-14

**Theme:** ROADMAP §H1 (Distribution-readiness bundle 1 of 4) —
community and security policy docs that distro packaging teams look
for before accepting a package. Fully additive, docs-only.

### Added

- **`CODE_OF_CONDUCT.md`** — [Contributor Covenant 2.1][cc21] verbatim
  with two reporting channels: a dedicated maintainer email and the
  private GitHub Security Advisory (tagged `[conduct]` for triage) for
  reporters who prefer GitHub's private-report flow. Email is listed
  first per CoC convention — no GitHub account required. Ships
  ROADMAP §H1.
- **`SECURITY.md`** — coordinated-disclosure policy: supported-versions
  table, reporting channels (GitHub Security Advisory preferred;
  signed encrypted email for reporters who need it), 48h / 7d / 30d /
  90d disclosure timeline, severity rubric (Critical / High / Medium /
  Low), in-scope / out-of-scope lists, and an acknowledgement of the
  hardening already in the tree (image-bomb budgets, URI scheme
  allowlist, plugin sandbox, OSC 52 quotas, multi-line paste
  confirmation, compile-time hardening flags, ASan + UBSan CI).
  Completes ROADMAP §H1. (The file itself landed in commit
  `4599813`; this release changelogs it alongside the CoC.)
- **Distribution-adoption plan in `ROADMAP.md`** — new 📦 theme plus a
  rollup section covering bundles H1–H16 (security/CoC, AppStream +
  desktop, man page, shell completions, distro packaging for
  openSUSE / Arch / Debian, Flatpak, docs site, macOS, accessibility,
  i18n, reproducible builds, SBOM, distro submissions, deb/rpm
  signing). Nothing ships from this plan yet except H1; it exists so
  later releases have a single source of truth for packaging work.

### Changed

- ROADMAP transition: §H1 status 📋 → ✅ in both the
  distribution-adoption overview table and the §0.7.0 📦 narrative
  section, with links to this CHANGELOG entry.
- README's **Contributing** section now links `CONTRIBUTING.md`,
  `CODE_OF_CONDUCT.md`, and `SECURITY.md` before the step-by-step
  fork/PR instructions, so first-time contributors see the policy
  docs before they see the mechanics.
- `CONTRIBUTING.md` intro references the CoC (participation
  agreement) and `SECURITY.md` (private channel for sensitive
  reports) so the rules aren't buried.

[cc21]: https://www.contributor-covenant.org/version/2/1/code_of_conduct/

## [0.6.15] — 2026-04-14

**Theme:** the **incremental reflow** optimization from ROADMAP §0.7.0
Performance. Resizing a terminal with a long scrollback used to re-join
every line into a logical-line buffer and re-wrap every logical line
back into TermLines, allocating scratch vectors and copying cells
cell-by-cell — O(scrollback) allocation churn on every width change.
This release adds a fast path for the common case: standalone lines
(not part of a soft-wrap sequence) whose content fits the new width
get resized in place, skipping join/rewrap entirely.

### Changed

- **Incremental scrollback reflow** in `TerminalGrid::resize()`. The
  new algorithm walks scrollback once:
  - **Fast path** (standalone line, `softWrapped == false`, not mid-
    sequence, content width ≤ new cols): `tl.cells.resize(cols)`
    with default-attr padding when growing, or trim trailing blanks +
    drop combining-char entries at now-removed columns when shrinking.
    No allocation of scratch TermLines. No cell-by-cell copy.
  - **Slow path** (soft-wrap sequence OR standalone that doesn't fit):
    the existing `joinLogical` + `rewrap` round-trip, unchanged. This
    preserves correctness for trailing-space trim, combining-char
    position merging, and wide-character boundary handling.
  Typical scrollback is dominated by standalone lines ≤ the new width,
  so the fast path handles most rows. Multi-line soft-wrap sequences
  (long commands that wrapped at the prompt's edge) still round-trip
  through the full logic so nothing regresses there.
- ROADMAP transition: §0.7.0 → ⚡ Performance → "Incremental reflow on
  resize" 💭→✅ with a link to this CHANGELOG section. See
  [ROADMAP.md](ROADMAP.md#070--shell-integration--triggers-target-2026-06).

### Notes

- **No behavior change on correctness.** The fast path only applies
  when the line's logical-line representation would be a single
  TermLine identical (up to trailing blanks) to the one we produce by
  direct resize. For every case the slow path is still reachable via
  the existing code.
- **Screen-line reflow is unchanged.** Screen rows are already
  small-N (typically ≤ 50) so the O(N) savings aren't worth the
  added branch complexity; the joinLogical/rewrap loop runs on
  `m_screenLines` as before.
- **Alt-screen and scrollback-hyperlink reflow paths unchanged.**
  Alt-screen is explicitly `simple copy` today; scrollback hyperlink
  spans are not re-wrapped at all (pre-existing behavior) — both
  outside the scope of this optimization.
- **No plugin API change, no config schema change, no UI change.**
  `PLUGINS.md` does not need a bump.

## [0.6.14] — 2026-04-14

**Theme:** small security follow-up to 0.6.13 — catch a URI-scheme
injection vector in the new `make_hyperlink` trigger action that
self-review after shipping turned up. No ROADMAP transition; this is
a hardening patch on an already-shipped feature.

### Security

- **URI scheme allowlist in `TerminalGrid::addRowHyperlink()`**. The
  `make_hyperlink` trigger action expands `$0..$9` backrefs from the
  matched PTY output into the URL template — and PTY output is
  attacker-controllable (SSH sessions, remote shells, any inner-tty
  process). Without a scheme check, an innocent-looking template
  like `https://example.com/$1` could be weaponized when the
  pattern matches an attacker-supplied string that expands to
  `javascript:alert(1)` or `data:text/html,...`. The new check
  mirrors the existing OSC 8 allowlist (`http`, `https`, `ftp`,
  `file`, `mailto`); hyperlinks with any other scheme are silently
  dropped, matching OSC 8's drop-on-bad-scheme semantics — text
  still prints, it just isn't clickable.

### Notes

- Same allowlist used by OSC 8 remote hyperlink parsing in
  `handleOsc()`; kept in lockstep so both sources share the same
  threat model.
- No config schema change, no plugin API change, no UI change.
- `QSet` added to `terminalgrid.cpp` includes for the allowlist
  storage.

## [0.6.13] — 2026-04-14

**Theme:** close out the last deferred bullet from the 0.6.9 trigger-system
bundle — the iTerm2 **HighlightLine**, **HighlightText**, and
**MakeHyperlink** actions, which needed grid-cell mutation surgery that
chunk-level matching (checkTriggers) couldn't provide. This release adds
a new line-completion callback on `TerminalGrid` that fires from
`newLine()` with the row the cursor is leaving, and `TerminalWidget`
runs the grid-mutation subset of trigger rules against the finalized
line text there — so matches map to exact col ranges on a real row, and
mutations land before the row scrolls into scrollback.

### Added

- **`highlight_line` trigger action** — when the pattern matches in
  finalized output, recolors the **entire line** the match landed on.
  Action Value format: `"#fg"` (fg only), `"#fg/#bg"` (both), or
  `"/#bg"` (bg only). Empty-string slots mean "leave unchanged" —
  so partial overrides compose cleanly with the line's existing SGR
  attributes.
- **`highlight_text` trigger action** — same color-string format as
  `highlight_line`, but recolors **only the matched substring** using
  the regex's capture-0 span as the col range.
- **`make_hyperlink` trigger action** — turns the matched substring
  into a clickable OSC 8-equivalent hyperlink. Action Value is a URL
  **template** with `$0..$9` backrefs expanded against regex capture
  groups (`$0` = whole match, `$1..$9` = groups). Example: pattern
  `#(\d+)` + template `https://github.com/milnet01/ants-terminal/issues/$1`
  → a clickable issue link on every `#123` reference in output. Matches
  iTerm2's MakeHyperlink contract.
- **`TerminalGrid::setLineCompletionCallback(…)`** — new public API.
  Fired from `newLine()` **before** any cursor/scroll motion with the
  screen row the cursor was leaving. Used by TerminalWidget to run
  grid-mutation trigger rules; any other consumer can hook in for
  per-line post-processing (e.g., future trigger-system actions that
  need line-level context).
- **`TerminalGrid::applyRowAttrs(row, startCol, endCol, fg, bg)`** —
  overrides per-cell fg/bg on an inclusive col range of a screen row.
  Invalid QColor() leaves that channel untouched so callers can write
  fg-only, bg-only, or both. Marks the row dirty for re-paint.
- **`TerminalGrid::addRowHyperlink(row, startCol, endCol, uri)`** —
  pushes a hyperlink span onto the screen row's per-line
  `m_screenHyperlinks` vector with the same shape the OSC 8 parser
  uses. Auto-grows the vector if the row index is past the current
  end. Rows scrolling into scrollback preserve the span via the
  existing scrollUp move-semantics path.
- **`TerminalWidget::onGridLineCompleted(int screenRow)`** — dispatch
  handler installed as the grid's line-completion callback. Iterates
  rules, filters to the three new grid-mutation types, runs
  `globalMatch()` on the finalized line text, and invokes the
  appropriate `applyRowAttrs` / `addRowHyperlink` for each match (so
  multiple hits on one line all apply).

### Changed

- **Settings → Triggers dropdown** now exposes all action types. The
  hardcoded `{"notify", "sound", "command"}` list was hiding the
  `bell` / `inject` / `run_script` types shipped in 0.6.9 from the
  UI; 0.6.13 adds those plus the three new grid-mutation types so
  every trigger rule supported by the backend is reachable from
  Settings. The trigger-tab description was also expanded to document
  the Action Value format for each type.
- `TerminalWidget::checkTriggers()` now **skips** the three new
  grid-mutation types — those dispatch through `onGridLineCompleted`
  instead. Routing them through both paths would double-fire and
  match on raw PTY byte chunks (which don't map to grid cells)
  instead of finalized line text.
- ROADMAP transition: §0.7.0 → 🔌 Plugins trigger system — the
  "HighlightLine / HighlightText / MakeHyperlink actions from the
  iTerm2 reference are deferred" caveat on the 0.6.9 bullet is now
  resolved; the bullet is marked fully complete.

### Notes

- **Grid-mutation triggers fire on line completion only** — i.e.,
  when `newLine()` is called after a `\n`. The `instant` flag is not
  honored for these three types (it wouldn't make sense — the mid-
  line text isn't final, and half-matched mutations would flash
  then get overwritten). Dispatch types (notify/sound/etc.) still
  honor `instant` via the existing chunk-level matcher.
- **No config schema change.** Same `{pattern, action_type,
  action_value, instant, enabled}` rule shape from 0.6.9; the new
  types just teach both `checkTriggers` and `onGridLineCompleted`
  how to interpret `action_type` and `action_value`.
- **Multi-match support** — `highlight_text` and `make_hyperlink`
  both use `globalMatch()` so every occurrence on a line is
  decorated (not just the first).
- **No plugin API surface change.** `PLUGINS.md` does not need a
  bump; trigger rules live in config, not in the `ants.*` API.

## [0.6.12] — 2026-04-14

**Theme:** close out the **semantic-history** ROADMAP item (which was
already implemented but never promoted on the roadmap) and round out
its editor coverage so the line/column jump works for editors beyond
just VS Code and Kate.

### Added

- **Editor jump support for nvim, vim, sublime, JetBrains IDEs,
  helix, and micro** in `TerminalWidget::openFileAtPath()`. Ctrl-click
  on a `path:line:col` capture in scrollback (compiler / linter /
  stack-trace output) now opens the file at the cited location for:
  - **VS Code family**: `code`, `code-insiders`, `codium`, `vscodium`
    via `--goto <path>:<line>:<col>`
  - **Kate**: `-l <line>` + `-c <col>` (col was previously ignored)
  - **Sublime / Helix / Micro**: `<path>:<line>:<col>` argv shape
  - **Vi family**: `nvim`, `vim`, `vi`, `ex` via `+<line> <path>`
  - **Nano**: `+<line>,<col> <path>`
  - **JetBrains IDEs**: `idea`, `pycharm`, `clion`, `goland`,
    `webstorm`, `rider`, `rubymine`, `phpstorm`, `datagrip` via
    `--line <line>` + `--column <col>`
  - **Anything else**: best-effort (open the path, no jump). Users
    can wrap their editor in a small shell script if it isn't on the
    list yet.
  Editor name match uses `QFileInfo(editor).fileName()` so absolute
  paths (`/usr/local/bin/code`) and bare names (`code`) both work.

### Changed

- ROADMAP transition: §0.7.0 → 🎨 Features → "Semantic history"
  💭→✅. The OSC 7-less implementation was already shipped (uses
  `/proc/<pid>/cwd` to resolve relative paths); this release just
  documents the existing behavior and broadens the editor list. See
  [CHANGELOG.md §0.6.12](CHANGELOG.md#0612--2026-04-14).

### Notes

- **No new config keys, no new permissions, no plugin API change.**
  PLUGINS.md does not need a bump.
- The path-detection regex is unchanged from 0.6.x — the existing
  `(?:^|[\s("'])((\.{0,2}/)?[a-zA-Z0-9_\-./]+\.[a-zA-Z0-9_]+(?::(\d+)(?::(\d+))?)?)`
  pattern in `detectUrls()` already captures `path:line:col` and was
  the basis for the original semantic-history routing.
- Line jump is suppressed when the captured line is 0 (unparseable)
  so editors don't see a bogus `+0` / `--line 0` arg.

## [0.6.11] — 2026-04-14

**Theme:** ship the **0.7.0 security pair** — a UI for auditing and
revoking plugin capabilities, plus image-bomb defenses for the inline
graphics decoders. Both items were the smallest, lowest-risk slice of
the remaining 0.7.0 work; bundling them keeps the security theme
closing out before the riskier threading/platform work begins.

### Added

- **Plugin capability audit UI.** Settings → **Plugins** lists every
  discovered plugin with its name, version, author, description, and
  the full set of permissions declared in its `manifest.json`. Each
  permission renders as a checkbox whose initial state reflects the
  current grant in `config.plugin_grants[<name>]`. Unchecking + Apply
  persists the revocation; **takes effect at the next plugin reload**
  (matches the existing first-load permission prompt's grant
  semantics). When no plugins are loaded — either Lua is not
  compiled in or the plugin directory is empty — the tab shows a
  guidance message pointing at `PLUGINS.md`. Permission descriptions
  match the first-load dialog wording so users see the same
  language across both surfaces. Closes
  [ROADMAP.md §0.7.0 → "Plugin capability audit UI"](ROADMAP.md#070--shell-integration--triggers-target-2026-06).
- **Image-bomb defenses.** New `ImageBudget` class (private to
  `TerminalGrid`) tracks total decoded image bytes across every
  inline-display entry (Sixel + Kitty `T`/`p` + iTerm2 OSC 1337) and
  the Kitty graphics cache. Cap = **256 MB per terminal** — when a
  decode would push the total past the cap, the decoder rejects the
  payload up front (Sixel) or post-decode (Kitty PNG / iTerm2
  OSC 1337 — those don't carry pre-decode dimensions in the params
  block) and surfaces an inline error in the warning color
  (`[ants: <kind> WxH (NN MB) exceeds 256 MB image budget]`). Every
  eviction site (inline-image FIFO drain, Kitty cache eviction, Kitty
  delete-by-id, Kitty delete-all) releases bytes back to the budget.
  Alt-screen swap calls `recomputeImageBudget()` so the counter stays
  accurate when the active and saved containers cross paths. Existing
  per-image dimension cap (`MAX_IMAGE_DIM = 4096`) remains in force —
  ROADMAP specified 16384 as the upper bound but our existing 4096 is
  stricter. Closes
  [ROADMAP.md §0.7.0 → "Image-bomb defenses"](ROADMAP.md#070--shell-integration--triggers-target-2026-06).
- **`SettingsDialog::PluginDisplay`** — lightweight POD struct
  decoupling the Settings dialog from `pluginmanager.h` (and from the
  Lua-gated build chain). MainWindow translates each `PluginInfo` into
  this form before handing the list to the dialog via `setPlugins()`,
  so the Plugins tab compiles and runs even without Lua support.
- **`TerminalGrid::writeInlineError(QString)`** — emits a short red
  ASCII diagnostic line at the cursor (CR+LF after) for surfacing
  image-bomb rejections without the disruption of a desktop
  notification. Color matches the command-block status stripe red.
- **`TerminalGrid::recomputeImageBudget()`** — bulk-recompute helper
  for the alt-screen swap path where individual add/release tracking
  would be brittle. O(N) over the inline + cache vectors.

### Changed

- `SettingsDialog` constructor now wires a "Plugins" tab between
  "Profiles" and the OK/Cancel button row. `applySettings()` collects
  checked permissions per plugin and calls
  `Config::setPluginGrants(name, granted)` for every plugin in the
  audit list — including the "all unchecked" case so revocation
  persists as an empty grant array.
- `MainWindow::settingsAction` lambda now gathers the current plugin
  snapshot from `m_pluginManager->plugins()` (or an empty list when
  `ANTS_LUA_PLUGINS` is undefined) and calls `setPlugins()` on the
  dialog every time it opens — so hot-reloads / new installs are
  reflected without recreating the dialog.
- `TerminalGrid` Sixel / Kitty / iTerm2 decode paths and every
  image-eviction site now consult / update the per-terminal
  `ImageBudget`. RIS (`ESC c`) replaces the entire `TerminalGrid`
  via `*this = TerminalGrid(...)`, which naturally resets the budget
  alongside the containers; no special-case code needed.
- ROADMAP transitions: "Plugin capability audit UI" 📋→✅ and
  "Image-bomb defenses" 📋→✅ in §0.7.0 → 🔒 Security, both linked
  back to this CHANGELOG section.

### Notes

- **Plugin permissions storage on disk is unchanged** — same
  `config.plugin_grants[<name>] = [...]` shape from manifest v2
  (0.6.0). The new UI is additive: it reads what the first-load
  permission prompt already wrote and exposes it for editing.
- **Image budget is conservative.** Same `QImage` stored in both
  `m_inlineImages` and `m_kittyImages` is counted twice even though
  Qt's COW means the underlying bitmap is allocated once. We may
  reject earlier than strictly necessary; we never reject too late.
- **No plugin API surface changed** — `PLUGINS.md` does not need a
  bump. The audit UI sits entirely outside the `ants.*` API.
- No new permissions were introduced; the descriptions for
  `clipboard.write`, `settings`, and `net` mirror the first-load
  dialog text in `mainwindow.cpp`.

## [0.6.10] — 2026-04-14

**Theme:** ship the **command-block UI bundle** from 0.7.0 §Features
(shell integration). OSC 133 prompt regions already carried the data;
this release wires the UI that makes blocks first-class citizens —
per-block context menu, pass/fail status stripe, asciicast export of
a single block. Also formalizes asciinema recording (the recorder
itself has shipped — it's now marked ✅ on the roadmap).

### Added

- **Command-block context menu.** Right-click inside an OSC 133
  prompt region surfaces block-scoped actions without requiring a
  selection: **Copy Command** (extracts just the command text via the
  OSC 133 B cursor column so PS1 is stripped), **Copy Output**
  (extracts the range between the C marker and the next region),
  **Re-run Command** (writes the captured command + `\r` to the PTY —
  bypasses paste confirmation since it's your own history, not
  external input), **Fold / Unfold Output** (toggles the existing
  fold triangle for this specific region rather than the cursor
  region), and **Share Block as .cast…** (exports one block to a
  standalone asciicast v2 file). The menu header shows
  `Command Block (exit N)` when the block has finished so users can
  see pass/fail before acting. Actions are gated on block state —
  Re-run requires `commandEndMs > 0`, Copy Output requires
  `hasOutput`, and so on.
- **Per-block exit-code status stripe.** A 2px vertical bar painted
  on the far-left gutter of every finished prompt line — green for
  `exit_code == 0`, red otherwise. Unlike the fold triangle it shows
  on the most recent finished block too (no successor region
  required). Gives at-a-glance scannability of long scrollback for
  pass/fail.
- **`exportBlockAsCast(int, QString)`** — writes a standalone
  asciicast v2 file with a single command block's output. Header =
  current grid dimensions + `commandStartMs` timestamp. Event stream
  = two records: the command echo at t=0.0, and the captured output
  at t=duration (synthesized; we don't have per-byte timing for
  historical blocks). Players replay as a prompt followed by the
  output dump — honest about what we recorded. Shared via the
  **Share Block as .cast…** context menu action; file extension
  `.cast` per the [asciicast v2 spec](https://docs.asciinema.org/manual/asciicast/v2/).
- **Block-text extraction helpers** on `TerminalWidget` —
  `promptRegionIndexAtLine(int)`, `commandTextAt(int)`,
  `outputTextAt(int)`, `rerunCommandAt(int)`, `toggleFoldAt(int)`.
  Public so plugin code and future UI (marketplace, block sharing
  service) can consume the same semantics.

### Changed

- **`PromptRegion` now carries three new fields** — `exitCode`
  (stored in the OSC 133 D handler alongside `m_lastExitCode`; lets
  block UI paint per-region pass/fail instead of relying on the
  grid-global most-recent value), `commandStartCol` (cursor column
  at OSC 133 B fire — lets `commandTextAt` strip PS1 from the
  extracted command), and `outputStartLine` (global line where OSC
  133 C fired, so output extraction doesn't guess based on
  `endLine + 1`). All additive; existing consumers unaffected.
- **ROADMAP 0.7.0 §Features.** "Command blocks as first-class UI"
  and "Asciinema recording (`.cast` v2)" move from 📋 (planned) to
  ✅ (shipped in 0.6.10). Four bullets consolidated under the
  command-block item — navigation (`Ctrl+Shift+Up`/`Down`) was
  already wired in an earlier release, metadata display (duration
  + timestamp + exit stripe) completes here, per-block menu ships
  here, asciinema recorder was already implemented (now marked
  shipped). The "suppress output noise" sub-bullet from the
  original 0.7.0 design was deferred — it's ill-defined without a
  noise heuristic and not worth blocking the bundle on. Semantic
  history (Cmd-click on path:line:col) and OSC 133 HMAC remain as
  future work.

### Notes

- No plugin API changes in this release. `PLUGINS.md` not bumped.
- No new permission gates. The context menu is client-side UI;
  `rerunCommandAt` uses the same `m_pty->write` path the user's own
  keystrokes use.
- Multi-line wrapped commands are captured correctly — `commandTextAt`
  follows the region from `endLine` through `outputStartLine - 1`
  (or the next region's `startLine - 1` when no output was captured),
  joining lines with `\n`. Trailing-space padding is stripped per
  line.

## [0.6.9] — 2026-04-14

**Theme:** ship the **trigger system bundle** from 0.7.0 §Plugins —
four items that share LuaEngine / PluginManager / OSC-dispatch
plumbing and are cleaner to land together than to drip-feed across
four releases. Sets up shell-aware plugin workflows for 0.7.

### Added

- **Trigger system extensions.** `TriggerRule` JSON schema gains an
  `instant` boolean (iTerm2 convention — `true` fires on every PTY
  chunk for in-progress matches like password prompts; `false`
  defaults to firing only on newline-terminated chunks so settled
  output doesn't double-fire mid-line) and three new `action_type`
  values: `bell` (alias for `sound` — explicit per iTerm2's vocabulary),
  `inject` (writes `action_value` directly to the focused PTY — useful
  for "yes\n" auto-confirm rules), and `run_script` (broadcasts to
  plugins as a `palette_action` event with payload
  `"<action_id>\t<matched_substring>"`). Existing `notify` / `sound` /
  `command` actions unchanged.
- **OSC 1337 SetUserVar channel.** Parses
  `ESC ] 1337 ; SetUserVar=NAME=<base64-value> ST` (iTerm2 / WezTerm
  convention) and fires a `user_var_changed` plugin event with payload
  `"NAME=value"`. Disambiguated from inline images by the byte after
  `1337;` (`S` for SetUserVar, `F` for File). NAME capped at 128 chars,
  decoded value capped at 4 KiB — this is a status channel, not a
  transport. Lets a shell hook pipe state (git branch, k8s context,
  virtualenv) into plugins without prompt-text scraping.
- **`ants.palette.register({title, action, hotkey})`.** Lua API for
  plugins to inject Ctrl+Shift+P palette entries. `title` and `action`
  required; `hotkey` optional (when set, registers a global QShortcut
  that fires the same action). Entries appear as `"<plugin>: <title>"`
  to keep the palette scannable across plugins. Triggering an entry
  fires a `palette_action` event with `action` as payload, scoped to
  the registering plugin only — broadcast events use a different
  payload format (`"<id>\t<match>"` from `run_script` triggers).
- **Five new plugin events:**
  - `command_finished` — fires on OSC 133 D; payload
    `"exit_code=N&duration_ms=N"` (URL-form so plugins can split on
    `&` without escaping).
  - `pane_focused` — fires on tab switch; payload = tab title. Today
    fires on tab changes; will cover within-tab pane focus when split-
    pane focus tracking lands.
  - `theme_changed` — fires after `applyTheme()` completes; payload =
    theme name. Lets plugins swap palette/icon assets to match.
  - `window_config_reloaded` — fires after Settings → Apply; payload
    empty (plugins re-read settings via `ants.settings.get` on demand).
  - `user_var_changed` — fires on OSC 1337 SetUserVar (see above).
  - `palette_action` — fires when a registered palette entry is
    triggered, or when a `run_script` trigger matches.
- **`TerminalGrid` callbacks.** New `setCommandFinishedCallback`,
  `setUserVarCallback`. Wired by `TerminalWidget` to corresponding
  Qt signals (`commandFinished`, `userVarChanged`, `triggerRunScript`)
  which `MainWindow` forwards into `PluginManager::fireEvent`.

### Changed

- **Command palette is now dynamic.** `MainWindow::rebuildCommandPalette()`
  collects menu actions and plugin-registered entries on every change
  (plugin load/reload/unload, individual `register` calls). On
  `pluginsReloaded` signal, all plugin entries are torn down so a
  removed plugin's entries don't survive across reloads — init.lua
  re-runs on reload and re-registers anything that should still appear.
- **PLUGINS.md** — documents the six new events, the `palette` API,
  the `instant` flag, the new trigger action types. The 0.7 roadmap
  section in PLUGINS.md is updated to reflect what's now shipped vs
  what remains.
- **ROADMAP.md** — moves all four trigger-system items from 📋 to ✅
  in 0.7.0 §Plugins. Remaining 0.7.0 work is shell-integration UI
  (command blocks, asciinema), performance (SIMD VT-parser scan,
  decoupled threads), platform (Wayland Quake mode), and security
  (plugin capability audit UI, image-bomb defenses).

### Notes

- The trigger system's `HighlightLine` / `HighlightText` /
  `MakeHyperlink` actions from the original ROADMAP item are deferred
  — they require grid-cell mutation (per-cell color overrides + OSC 8
  hyperlink injection from outside the parser) which is a bigger
  surgery than the dispatch-level actions shipped here. The actions
  shipped (`notify`, `sound`/`bell`, `command`, `inject`, `run_script`,
  `PostNotification` via `notify`) cover the iTerm2 trigger doc's
  most-used cases. Grid-mutation actions are tracked as a follow-up.
- `pane_focused` fires on tab switches today; once split-pane focus
  tracking lands, the same event covers within-tab pane changes —
  plugin handlers don't need to change.

## [0.6.8] — 2026-04-14

**Theme:** close the two remaining Qt-specific audit rules from
0.7.0 §Dev experience. With these in place, the entire 0.7.0
"Project Audit tool" lane is shipped — every motivating case
surfaced by the 0.6.5 audit pass now has automated coverage.

### Added

- **Audit rule `unbounded_callback_payloads`.** Same-line regex flags
  PTY / OSC / IPC byte buffers forwarded straight into a user-supplied
  `*Callback(…)` without a length cap (`.left()` / `.truncate()` /
  `.mid()` / `.chopped()` / `.chop()`). Motivating case: pre-0.6.5
  OSC 9 / OSC 777 notification body shovelled the entire escape payload
  (potentially MB) into the desktop notifier, which then crashed the
  notification daemon and/or amplified a malformed-OSC DoS. Fix shipped
  in 0.6.5; the rule prevents the regression. Severity Major.
  `terminalgrid.cpp` is the canonical safe call site (filtered at
  runtime by `.left(kMaxNotifyBody)`); rule produces zero findings on
  our tree.
- **Audit rule `qnetworkreply_no_abort`.** Detects the dangerous shape
  from the pre-0.6.5 AiDialog incident: a 3-arg `connect()` to a
  QNetworkReply signal whose third argument is a bare lambda. With no
  context object, Qt cannot auto-disconnect when the lambda's captured
  `this` is destroyed — owner closed mid-flight → reply fires later →
  use-after-free. Pattern enforces the 4-arg form (sender, signal,
  context, slot), which Qt protects via auto-disconnect on context
  destruction. Severity Major. Covers `readyRead`, `finished`,
  `errorOccurred`, `sslErrors`. Single-line only — multi-line connect
  formatting is a known false-negative (rare in practice). Ants's own
  AiDialog and AuditDialog use the safe 4-arg shape and are not flagged.
- **Fixtures for both new rules.** `bad.cpp` with three `@expect`
  markers each, `good.cpp` with the corresponding safe shapes that must
  not match the regex. Wired through `audit_self_test.sh` so CI catches
  regressions on every push (now 14 rules, 46 total checks pass).

### Changed

- **ROADMAP** — moved the two remaining 0.7.0 §Dev experience
  audit-rule items (`unbounded_callback_payloads`,
  `qnetworkreply_no_abort`) from 📋 to ✅. The Project Audit tool
  lane in 0.7.0 is now fully shipped; remaining 0.7.0 work is
  features (command blocks, asciinema), the trigger system, and the
  performance / platform / security items.

## [0.6.7] — 2026-04-14

**Theme:** close the remaining grep-shaped items from 0.7.0 §Dev experience
in one sweep, plus fix the CI workflow file which — it turns out — had
never parsed (unquoted colon in a step `name` rejected the entire YAML).

### Added

- **Audit rule `silent_catch`.** Flags empty same-line `catch (...) {}`
  handlers that swallow exceptions without logging or rethrow. Conservative
  first cut (same-line empty body only); extending to multi-line / single-
  statement trivial bodies needs `grep -Pzo` plumbing and is deferred.
- **Audit rule `missing_build_flags`.** Nudges projects toward better
  compile-time coverage by flagging absence of `-Wformat=2`, `-Wshadow`,
  `-Wnull-dereference`, and `-Wconversion` in the top-level `CMakeLists.txt`.
  Strips comment-only lines before matching so a CMakeLists that *discusses*
  a flag in a comment (e.g. why it's disabled) doesn't cause a false negative.
  Severity Minor — this is a nudge, not a bug. Correctly reports one finding
  on our own tree: `-Wconversion`, which is intentionally off (noisy in
  combination with Qt), documented in `CMakeLists.txt`.
- **Audit rule `no_ci`.** Detects projects missing CI configuration
  (`.github/workflows/`, `.gitlab-ci.yml`, `.circleci/`, `.travis.yml`,
  `Jenkinsfile`). Severity Major — regressions ship silently without a CI
  safety net.
- **Sanitizer CI job (`build-asan`).** New parallel GitHub Actions job
  builds with `-DANTS_SANITIZERS=ON` (ASan + UBSan), runs ctest under
  sanitizers, and smoke-tests the binary (`--version`, `--help`) with
  `QT_QPA_PLATFORM=offscreen` so initialization-path bugs surface on every
  push. `detect_leaks=0` for now — Qt global singletons on fast-exit paths
  look like leaks to LSan; re-enable once we have a real unit-test suite
  exercising full app lifecycle.
- **`CONTRIBUTING.md`.** Short actionable guide derived from `STANDARDS.md`:
  build modes, where tests live, step-by-step for adding an audit rule,
  version-bump checklist, commit conventions, what-not-to-send.
- **Fixtures for `silent_catch`** — `bad.cpp` with three `@expect` markers,
  `good.cpp` with logging/rethrow/return-with-value catch bodies that must
  not match.

### Fixed

- **CI workflow was never parsing.** `name: Run cppcheck (audit rule:
  Qt-aware static analysis)` had an unquoted `:` inside the scalar, which
  GitHub Actions' YAML parser rejected as "mapping values are not allowed
  here" — the workflow file was dropped entirely and no jobs ran for 0.6.5
  or 0.6.6. Fixed by double-quoting the step name. The parse error was
  silent on GitHub's side (workflow-file runs showed 0 jobs with a
  one-line "workflow file issue" notice); caught here by running PyYAML's
  strict parser over the file locally.

### Changed

- **ROADMAP** — moved four items from 0.7.0 §Dev experience to 0.6.7
  shipped (silent-catch rule, build-flag recommender, no-CI check,
  sanitizer-in-ctest hookup, CONTRIBUTING.md).

## [0.6.6] — 2026-04-14

**Theme:** close the first Dev-experience item queued in 0.7 after the 0.6.5
audit. One new audit rule + matching CI enforcement, backfill the two
fixtures it immediately surfaced.

### Added

- **Audit self-check: fixture-per-`addGrepCheck`.** New `audit_fixture_coverage`
  rule in the Project Audit dialog enumerates every `addGrepCheck("id", …)`
  call in `src/auditdialog.cpp`, dedups by id, and reports any id without a
  matching `tests/audit_fixtures/<id>/` directory. Catches the exact gap we
  hit in 0.6.5 — four rules shipped a cycle without regression fixtures.
  Silent no-op on projects that don't follow this convention (grep yields no
  ids). Output uses the parseFindings-friendly `file:line: message` shape so
  findings link directly to the registration site.
- **CI-enforced fixture-coverage cross-check** in `tests/audit_self_test.sh`.
  Belt-and-suspenders: the runtime check surfaces findings to the dev on next
  audit run; the test-harness assertion blocks merges in CI when a new rule
  id lacks either a fixture dir OR a `run_rule` line in the script. Mirrors
  the runtime check exactly. Added to ctest output as
  `PASS/FAIL: fixture-coverage <id>`.
- **Fixture coverage for `memory_patterns` and `qt_openurl_unchecked`** —
  the two real gaps the new rule just surfaced. Each gets `bad.cpp` with
  `@expect` markers and a `good.cpp` that avoids matching tokens (including
  in comments — the pattern is case-sensitive and line-based, so comment
  wording had to use UPPER-CASE or non-literal phrasing).

### Changed

- **ROADMAP** — moved the "Self-consistency: fixture-per-`addGrepCheck`" item
  from 0.7.0 §Dev experience to 0.6.6 shipped.

## [0.6.5] — 2026-04-14

**Theme:** second audit of the day. Ran the 5-phase prompt
(`/mnt/Storage/Scripts/audit_prompt.md`) against the post-0.6.4 tree. Five
subagent reports converged on three real High findings, three Medium, and a
batch of noise. Five subagent claims were rejected on Phase 4 verification
(documented in the commit body) — keeping the signal honest matters more than
the count.

### Security

- **Cap OSC 9 / OSC 777 desktop-notification payloads at 1024 bytes (title
  at 256).** The VT parser caps an OSC payload at 10 MB so inline-image
  escapes round-trip cleanly; `handleOsc()` was forwarding that whole
  payload to the notification callback as a QString. A program inside the
  terminal could spam the freedesktop notification daemon with multi-MB
  titles. `src/terminalgrid.cpp:761-790` now `.left(N)`-clamps before the
  callback, matching the OSC 52 clipboard-cap pattern.
- **Cap accumulated SSE response buffer in AiDialog at 10 MB.**
  `m_sseLineBuffer` already had this cap on the pipe side (`src/aidialog.cpp:179`);
  `m_streamBuffer` — which holds *parsed* content — did not. A misbehaving
  or hostile endpoint could stream past `max_tokens` indefinitely. Once
  capped, we append a single `[response truncated]` marker and drop
  subsequent chunks.
- **AiDialog now has a destructor that aborts any in-flight reply.** Qt
  auto-disconnects signals from a destroyed receiver, but there's a narrow
  window during member destruction where a reply can still fire
  `readyRead` / `finished` on a partially-destructed AiDialog. Explicit
  `abort() + deleteLater()` closes the window.

### Changed

- **Stricter compile flags.** Added `-Wformat=2`, `-Wshadow=local`, and
  `-Wnull-dereference` to the default warning set. `-Wshadow=local` was
  picked over the full `-Wshadow` deliberately: the project uses
  Qt-idiomatic `void setData(QByteArray data)` patterns where parameter
  names legitimately match member accessors; flagging those would drown
  the real `int x = …; if (cond) { int x = …; }` shadowing that the flag
  exists to catch. Surfaced two legitimate local-vs-local shadows
  (`mainwindow.cpp:1170` and `auditdialog.cpp:3062`), both renamed.
- **Silent `catch (…)` blocks in OSC handlers now route to the existing
  `DBGLOG` channel.** Three sites in `src/terminalgrid.cpp` (OSC 133 D,
  OSC 9;4, OSC 1337 param parse) write a diagnostic line when their
  `std::stoi` throws, guarded by `m_debugLog` so there's zero cost when
  debug isn't enabled. The two Kitty-graphics `safeStoi`/`safeStoul`
  helpers stay intentionally silent — documented with a comment — because
  they fire per-chunk in the APC parameter loop and logging would flood.

### Added

- **`.github/workflows/ci.yml`.** Runs `ctest` plus the `cppcheck` Qt-aware
  pass on every push to `main` and every PR. `actions/checkout` pinned by
  40-char SHA (not tag) per STANDARDS.md §Supply chain. Workflow token
  scoped to `contents: read`.
- **Four new audit-rule regression fixtures** under
  `tests/audit_fixtures/`: `todo_scan`, `format_string`, `hardcoded_ips`,
  `weak_crypto`. These four rules ship in `auditdialog.cpp` but had no
  `bad/good` fixture pair, so regex drift would have shipped silently.
  `tests/audit_self_test.sh` now covers 9 of the 9 unconditional
  `addGrepCheck()` rules; `ctest` exercises 27 assertions (up from 23).
- **`launch.sh` now starts with `set -eu`.** Three-line wrapper, but free
  hardening against silent failures if a typo lands in a future edit.
- **`-Wl` cert-err33-c fix.** `src/ptyhandler.cpp:112`'s argv0
  `::snprintf` return value was being ignored; now `(void)`-cast with a
  comment explaining the truncation is a known-acceptable loss (the
  written buffer is a login-marker, not a lookup key).

## [0.6.4] — 2026-04-14

**Theme:** Project Audit signal-to-noise. Addresses a reviewer-provided brief
after a 2026-04-14 audit run surfaced 26 BLOCKER findings that were all false
positives from ASCII section underlines in a vendored single-header library,
and 30 MAJOR world-writable-file findings that were a FUSE-mount artefact.

### Fixed

- **Selection auto-scroll direction was inverted.** Dragging a selection
  past the bottom edge of the viewport scrolled *up* into older scrollback
  instead of revealing newer lines below (and symmetrically at the top
  edge). The auto-scroll timer in `TerminalWidget` applied
  `m_scrollOffset - delta` with a signed direction, which inverted the
  intent; flipped to `+ delta` so the direction convention now matches
  `wheelEvent` / `smoothScrollStep` — `+offset` reveals older content,
  `-offset` reveals newer.

- **Conflict-marker regex no longer matches ASCII underlines.** The
  `conflict_markers` rule required exactly 7 sigil chars at start-of-line
  but had no trailing anchor, so `===========` (and longer banners in
  vendored headers) still matched. Tightened to
  `^(<{7}|\|{7}|={7}|>{7})(\s|$)`. Also added `|{7}` for the diff3-style
  merge-base marker that the old pattern missed entirely. A
  `conflict_markers` self-test fixture now carries eight ASCII-underline
  canaries in `good.cpp` so regressions are caught by the existing CTest
  driver.

- **World-writable check on non-POSIX mounts.** On filesystems that don't
  enforce POSIX permission bits (FUSE, NTFS, vfat/exfat, CIFS, 9p) every
  file reports as world-writable because the mount maps all Unix perms
  to 0777. `AuditDialog` now probes `stat -f -c %T <project>` at
  construction; when the result is in a known non-POSIX list the
  `file_perms` check is replaced with an INFO note explaining the skip,
  instead of producing ~every file in the tree as a finding.

### Changed

- **Default exclude list broadened.** `kFindExcl` and `kGrepExcl` now
  exclude `external/` and `third_party/` in addition to the existing
  `vendor/`, `build/`, `node_modules/`, `.cache/`, `dist/`, `.git/`,
  `.audit_cache/`. Vendored code is not project-maintained, so findings
  there aren't actionable and used to drown out real signal.

- **Category headers surface multi-tool corroboration.** The HTML and
  plain-text renders now append `(N corroborated)` to each category
  header when one or more of its findings are flagged by ≥2 distinct
  tools on the same file:line — the same signal that drives the ★ badge
  and boosts the confidence score. Lets the downstream consumer
  prioritise triage at a glance instead of having to scan every finding's
  inline tags.

- **Confidence legend in the plain-text header.** The `conf N` and ★
  tags were opaque without documentation. A four-line legend now sits
  in the handoff header explaining what the score means and how
  corroboration feeds into it.

### Added

- **5-phase workflow scaffold in the Claude Review handoff.** The
  `/tmp/ants-audit-*.txt` export used by the "Review with Claude"
  button now carries a BASELINE → VERIFY → CITATIONS → APPROVAL →
  IMPLEMENT+TEST prompt between the project-docs block and the
  findings list. Stops the downstream session from plunging straight
  into fixes without verifying findings are real, cross-checking CVE
  version ranges, or committing a regression test per fix. Aligns with
  the project's existing no-workarounds rule (documented in
  `CLAUDE.md`).

## [0.6.3] — 2026-04-14

**Theme:** Claude Code status responsiveness. Replaces the 2-second transcript
poll with an inotify-driven event pipeline; state transitions now surface in
~50ms instead of up to 2s, with lower steady-state CPU.

### Changed

- **Transcript state updates are event-driven.** `ClaudeIntegration` already
  had a `QFileSystemWatcher` on the active transcript but bypassed it with
  an unconditional per-poll parse ("QFileSystemWatcher can miss rapid
  changes on Linux"). That was over-defensive — QFSW's only real miss
  modes are atomic rename-over (already handled by the re-add in
  `parseTranscriptForState`) and full file replacement (now handled by a
  slow backstop). The watcher's `fileChanged` signal now drives a 50ms
  single-shot debounce timer that coalesces streaming-output bursts
  (during an assistant turn Claude writes dozens of JSONL lines per
  second) into at most ~20 parses/sec. Net effect: "Claude:
  thinking…/compacting…/idle" transitions flip in ~50ms instead of up to
  2000ms, and the CPU no longer re-parses 32KB of JSON every 2 seconds
  when nothing has changed.

- **Process-presence poll kept at 2s, with a 20s transcript backstop.**
  Polling `/proc` for claude-code starting/stopping under our shell has
  no clean event equivalent (we're not the direct parent, and netlink
  proc-connector needs elevated caps), but it's cheap — ~40 bytes read
  per cycle. The poll now also re-parses the transcript once every 10
  cycles (~20s) as a defensive net for the rare file-replaced case where
  QFSW loses its watch; the re-parse also re-arms the watch via the
  existing `addPath`-if-missing check.

## [0.6.2] — 2026-04-14

**Theme:** Claude Code status readability. Adds a dedicated "compacting" state
so the status bar doesn't flatten a multi-second `/compact` into generic
"thinking…".

### Added

- **"Claude: compacting…" status.** New `ClaudeState::Compacting` surfaces
  when a `/compact` is in flight, rendered in magenta so it's visually
  distinct from idle (green), thinking (blue), and tool use (yellow).
  Detection is transcript-driven: the parser walks the existing 32KB tail
  window looking for a `user` event whose string content contains
  `<command-name>/compact</command-name>`, and only fires while no
  subsequent `isCompactSummary:true` user event (the condensed-history
  marker Claude Code writes when compaction finishes) has appeared. The
  `PreCompact` hook path now routes through the same state, so terminals
  that have the hook server wired up get the same indicator without the
  2-second poll latency.

## [0.6.1] — 2026-04-14

**Theme:** scroll-anchor correctness. One-liner fix for a long-standing drift
bug that only surfaced with a full scrollback buffer.

### Fixed

- **Scroll anchor drifts when scrollback is at its cap.** While scrolled up
  reading history, bursts of new output (e.g. Claude Code redrawing its
  prompt / spinner) appeared to "redraw text at the current history
  position" — actually the pinned viewport was sliding by one content-line
  per pushed line. Root cause: the anchor math in `onPtyData` diffed
  `scrollbackSize()` before/after parsing, but once the buffer hits
  `scrollback_lines` (default 50k) each push also pops a stale line from
  the front, so the size delta is always zero and the anchor thinks
  nothing happened. `TerminalGrid` now exposes a monotonic
  `scrollbackPushed()` counter that keeps incrementing through the cap;
  `TerminalWidget` diffs that instead. Below-cap behavior is unchanged.

## [0.6.0] — 2026-04-14

**Theme:** make the two features that already make Ants distinctive
(plugins + audit) production-grade. Ships the full context polish + plugin v2
arc from the 0.6 roadmap — scrollback regex search, OSC 9;4 progress,
multi-line paste confirmation, OSC 8 homograph warning, LRU glyph-atlas
eviction, cell-level dirty tracking, per-plugin Lua VMs, manifest v2 with
declarative permissions + capability-gated APIs, plugin keybindings, and
hot reload.

### Added

#### 🎨 Features

- **Scrollback regex search**. Ctrl+Shift+F search bar gains three toggle
  buttons: `.*` (regex mode via `QRegularExpression`), `Aa` (match case),
  and `Ab` (whole word, wraps the pattern in `\b…\b`). Alt+R / Alt+C /
  Alt+W flip toggles without leaving the input. Invalid regex patterns
  render the input with a red border and show the error in the match-
  counter tooltip (`!/!`). Zero-width matches (`\b`, `^`) are detected
  and skipped to avoid infinite loops. Matches Kitty / iTerm2 / WezTerm
  behavior; closes the Ghostty 1.3 gap.
- **OSC 9;4 progress reporting**. Parses ConEmu / Microsoft Terminal
  `ESC]9;4;state;percent ST` sequences. State 0 clears, 1 shows a normal
  blue progress bar, 2 shows a red error bar, 3 shows an indeterminate
  (full-width) lavender bar, 4 shows a yellow warning bar. Renders as a
  3-pixel strip along the bottom edge of the terminal (above the
  scrollbar) and as a small colored dot next to the tab title. Coexists
  with the existing OSC 9 desktop-notification handler via payload
  disambiguation (`9;4;…` vs. `9;<body>`). Spec:
  [MS Terminal progress sequences](https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences).
- **Click-to-move-cursor on shell prompts** (previously in code, now
  documented). When the cursor is in an OSC 133 prompt region with no
  output yet, clicking on the same line sends the appropriate run of
  `ESC [ C` / `ESC [ D` arrow sequences to reposition the cursor. Capped
  at 200 arrows to guard against runaway sends.

#### ⚡ Performance

- **LRU glyph-atlas eviction**. Replaces the old "clear everything on
  overflow" behavior with a warm-half retention policy: entries touched
  within the last 300 frames are kept, colder ones are re-rasterized
  into a fresh atlas. Median-based fallback handles cold-boot overflow.
  Eliminates the paint stall on long ligature-heavy sessions. See
  `glrenderer.cpp:compactAtlas`.
- **Per-line dirty tracking**. `TermLine` gains a `dirty` flag set by
  every grid mutation (print, erase, scroll, alt-screen swap, resize).
  `TerminalWidget::invalidateSpanCaches()` now does targeted eviction —
  only URL / highlight caches for dirty lines get dropped, not the whole
  map. Big win on high-throughput output that only touches a few lines.
  (Full cell-level render-path partial-update is deferred; the dirty
  primitive is in place for that work.)

#### 🔒 Security

- **Multi-line paste confirmation**. Dialog appears when the pasted
  payload contains a newline, the string `sudo `, a curl/wget/fetch
  piped into `sh`/`bash`/`python`/etc., or any non-TAB/LF/CR control
  character. Policy is independent of bracketed paste — iTerm2 /
  Microsoft Terminal got bitten by conflating the two
  ([MS Terminal #13014](https://github.com/microsoft/terminal/issues/13014)).
  Config key: `confirm_multiline_paste` (default `true`). UI:
  Settings → Behavior → "Confirm multi-line / dangerous pastes".
- **Per-terminal OSC 52 write quota**. 32 writes/min + 1 MB/min per
  grid, independent of the existing 1 MB per-string cap. Protects
  against drip-feed exfiltration.
- **OSC 8 homograph warning**. When an OSC 8 hyperlink's visible label
  encodes a hostname-looking token (`github.com`) that doesn't match
  the actual URL host (`github.evil.example.com`), a confirm dialog
  shows both and requires explicit opt-in. Strips leading `www.` for
  fair comparison.

#### 🔌 Plugins — manifest v2 + capability model

- **Per-plugin Lua VMs**. Each plugin gets its own `lua_State`, heap
  budget (10 MB), and instruction hook (10 M ops). One VM stalling or
  leaking globals can no longer affect others. `PluginManager` owns
  a map of `(name → LuaEngine *)` and fans out events to each in turn.
- **Declarative permissions in `manifest.json`**. Plugins declare a
  `"permissions": ["clipboard.write", "settings"]` array. On first
  load, a permission dialog lists each requested capability and lets
  the user accept / un-check individual grants. Grants persist in
  `config.json` under `plugin_grants`; subsequent loads don't re-prompt
  unless the manifest requests a new permission. Un-granted permissions
  surface as *missing API functions* (not `nil` stubs) so plugins can
  feature-detect with `if ants.clipboard then … end`.
- **`ants.clipboard.write(text)`** — gated by `"clipboard.write"`
  permission. Writes to the system clipboard via QApplication.
- **`ants.settings.get(key)` / `ants.settings.set(key, value)`** — gated
  by `"settings"` permission. Per-plugin key/value store, persisted
  under `plugin_settings.<plugin_name>.<key>` in `config.json`. Manifest
  can declare a `"settings_schema"` JSON-Schema subset (plumbing in
  place; schema-driven Settings UI is a follow-up item).
- **Plugin keybinding registration** via manifest `"keybindings"` block:
  `{"my-action": "Ctrl+Shift+K"}`. Firing the shortcut dispatches a
  `keybinding` event to the owning plugin with the action id as the
  payload. `ants.on("keybinding", function(id) ... end)` listens. The
  shortcut lives on the MainWindow; invalid sequences log a warning.
- **Plugin hot reload** via `QFileSystemWatcher`. Watches `init.lua`,
  `manifest.json`, and the plugin directory itself; on change, debounced
  150 ms, the plugin VM is torn down (fires `unload` event) and
  re-initialized (fires `load` event). Enabled only when
  `ANTS_PLUGIN_DEV=1` — idle cost is zero otherwise.
- **`ants._version`** exposes `ANTS_VERSION` string so plugins can
  feature-detect without hardcoding.
- **`ants._plugin_name`** exposes the plugin's declared manifest name.
- **New events**: `load`, `unload` (lifecycle), `keybinding` (manifest
  shortcuts). Added to `eventFromString()` in `luaengine.cpp`.

#### 🧰 Dev experience

- **`ants-terminal --new-plugin <name>` CLI**. Creates a plugin skeleton
  at `~/.config/ants-terminal/plugins/<name>/` with `init.lua`,
  `manifest.json` (empty `permissions` array), and `README.md` templates.
  Validates the name against `[A-Za-z][A-Za-z0-9_-]{0,63}`.
- **`ANTS_PLUGIN_DEV=1`** env var enables verbose plugin logging on
  scan/load, plus hot reload. Cached per-process via a static.
- **`--help` / `--version`** flags wired through `QCommandLineParser`
  (previously `--version` set via `setApplicationVersion` but no CLI
  handler existed).

### Changed

- `README.md` — navbar now links to `PLUGINS.md`, `ROADMAP.md`,
  and `CHANGELOG.md`. Plugins section points to PLUGINS.md as the
  source of truth; keeps a quick-start summary table inline.
- `CLAUDE.md`, `STANDARDS.md`, `RULES.md` — cross-linked to the new
  docs; plugin-system sections defer to PLUGINS.md for the author
  contract and document internal invariants separately.
- `PluginManager` no longer exposes a single `engine()` accessor;
  callers use `engineFor(name)` to get the per-plugin VM. Event
  delivery fans out over all loaded engines.
- Paste path now runs through `TerminalWidget::confirmDangerousPaste`
  before the bracketed-paste wrap — confirmation policy is
  independent of PTY-side behavior.

## [0.5.0] — 2026-04-13

### Added

**Project Audit — context-aware upgrade** (this release's headline feature).
The audit pipeline now collects and applies context at every stage, closing
the gap with commercial tools (CodeQL, Semgrep, SonarQube, DeepSource).

- **Inline suppression directives.** Findings can be silenced directly in
  source via `// ants-audit: disable[=rule-id]` (same line),
  `disable-next-line`, and `disable-file` (first 20 lines). Foreign markers
  are also honored for cross-tool compatibility: clang-tidy `NOLINT` /
  `NOLINTNEXTLINE`, cppcheck `cppcheck-suppress`, flake8 `noqa`, bandit
  `nosec`, semgrep `nosemgrep`, gitleaks `#gitleaks:allow`, eslint
  `eslint-disable-*`, pylint `pylint: disable`. Rule-list parsing and
  glob matching (`google-*`) are supported.
- **Generated-file auto-skip.** Findings in `moc_*`, `ui_*`, `qrc_*`,
  `*.pb.cc`/`*.pb.h`, `*.grpc.pb.*`, flex/bison outputs, `/generated/`
  paths, and `*_generated.*` files are dropped before rendering.
- **Path-based rules** in `audit_rules.json`. A new `path_rules[]` block
  takes entries of `{glob, skip, severity_shift, skip_rules[]}` to
  configure per-directory severity shifts and skips (e.g. `tests/**`
  shifts everything down one severity band; `third_party/**` skips
  entirely). Glob syntax supports `**`, `*`, `?`.
- **Code snippets** (±3 lines) attached to every `file:line` finding,
  cached per-file, rendered with the finding line highlighted. Exposed
  via SARIF `physicalLocation.contextRegion`, embedded in the HTML
  report's expandable `[details]` panel, and included in the Claude
  review handoff.
- **Git blame enrichment.** Author, author date, and short SHA shown
  next to every finding. Cached per `(file, line)` with a 2 s timeout;
  auto-disabled for non-git projects. Exported via SARIF
  `properties.blame` bag (sarif-tools de-facto convention).
- **Confidence score 0-100.** Replaces the binary `highConfidence` flag
  with a weighted sum: severity × 15 + 20 cross-tool agreement + 10
  external AST tool − 20 test-path, clamped, adjusted by explicit AI
  verdicts. Displayed as a coloured pip in the UI; exported in SARIF
  (`properties.confidence`) and HTML.
- **Severity pill filter + live text filter** in the dialog, matching the
  HTML export's UX. Filters across file name, message, rule id, and
  author (via blame). Re-renders instantly.
- **Sort by confidence** toggle reorders every check's findings by
  confidence descending — the "which should I look at first" view.
- **Collapsible `[details]`** per finding. Click to reveal snippet
  context and an inline "🧠 Triage with AI" action. Expanded state
  persists across re-renders.
- **Semgrep** integration (optional). When `semgrep` is available, a
  Security-category check runs `p/security-audit` plus language packs
  (`p/c`, `p/cpp`, `p/python`, `p/javascript`, `p/typescript`). Picks
  up project-local `.semgrep.yml` automatically. Complements clazy's
  Qt-aware AST lane.
- **AI triage per finding.** Click "🧠 Triage with AI" inside an
  expanded finding to POST rule + message + snippet + blame to the
  project's configured OpenAI-compatible endpoint. Response parsed
  as `{verdict, confidence, reasoning}`; verdict badge rendered
  inline, confidence score adjusted accordingly. Opt-in per click,
  respects `Config::aiEnabled`.
- **Version stamp on all audit outputs.** Dialog title, dialog types
  label, SARIF `driver.version`, HTML report meta line, and plain-text
  Claude handoff now all carry `ants-audit v<PROJECT_VERSION>` pulled
  from a single CMake `add_compile_definitions(ANTS_VERSION=…)` source
  of truth.
- **Single-source-of-truth versioning infrastructure.**
  `CMakeLists.txt` `project(... VERSION 0.5.0)` propagates via
  `add_compile_definitions(ANTS_VERSION=…)` to every caller. Four
  previously-hardcoded `"0.4.0"` literals retired: `main.cpp`
  (`setApplicationVersion`), `auditdialog.cpp` (SARIF driver),
  `ptyhandler.cpp` (`TERM_PROGRAM_VERSION` env), and
  `claudeintegration.cpp` (MCP `serverInfo.version`).

### Changed

- **`audit_rules.json` schema** extended with `path_rules[]`. Existing
  `rules[]` entries load unchanged; loader documents both blocks.
- **SARIF export** now emits `physicalLocation.contextRegion` with the
  full ±3 snippet for every finding that has a `file:line` location,
  plus `properties.blame` and `properties.confidence`. `properties`
  also carries `aiTriage` when the user triaged that finding.
- **HTML report** template upgraded with confidence pips, blame tags,
  verdict badges, and per-finding expandable snippet panels. Added
  CSS classes (`.conf-pip`, `.blame`, `.verdict`, `.snippet`, `.hit`).
  `DATA.version` now propagated through the inline payload.
- **Plain-text Claude handoff** body lines carry `[conf N · blame · AI]`
  tags and a 3-line indented snippet per finding. Header gains a
  `Generator:` line.
- **`dropFindingsInCommentsOrStrings`** still opt-in per check via the
  existing `kSourceScannedChecks` set, but is now augmented by the
  inline-suppression scan (applied to *every* finding with a
  `file:line`). Pipeline order documented in `STANDARDS.md`.

### Security

- Inline suppression scan reads files through a bounded per-file cache
  (4 MB cap) — runaway check on a huge file cannot stall the dialog.
- Git blame shells out with a 2 s timeout and is auto-disabled when
  not in a git repository.
- AI triage respects the project's existing `ai_enabled` config and
  refuses non-http/https endpoints.

### Tests / Tooling

- `tests/audit_self_test.sh` extended with 18 suppression-token
  assertions covering every honored marker (ants-native,
  NOLINT/NOLINTNEXTLINE, cppcheck-suppress, noqa, nosec, nosemgrep,
  gitleaks-allow, eslint-disable-*, pylint: disable) plus negative
  cases. Total: 23 rule tests passing.
- `ANTS_VERSION` compile definition wired via CMake
  `add_compile_definitions`. `main.cpp` and `auditdialog.cpp` no
  longer carry hardcoded version strings.

### Docs

- `README.md` — expanded "Project Audit Dialog" section with examples
  of inline suppression directives and `audit_rules.json` schema
  including `path_rules`; semgrep added to optional dependencies.
- `CLAUDE.md` — pipeline order, confidence formula, generated-file
  patterns, new design decisions around context-awareness.
- `STANDARDS.md` — context-awareness pipeline order invariants, blame
  cache rules, AI triage config sourcing.
- `RULES.md` — three new audit rules covering inline-suppression
  preference, `path_rules` vs. hardcoded paths, and "confidence is
  display-only, not a gate".

## [0.4.0] — 2026-04-13

### Added

**Project Audit dialog — full feature arc.** Over the course of eight
incremental commits, the audit panel evolved from a regex-heavy scanner
into a structured, AST-aware, cross-validated tool.

- **SonarQube-style taxonomy** — every finding carries a type
  (Info / CodeSmell / Bug / Hotspot / Vulnerability) and a 5-level
  severity (Info / Minor / Major / Critical / Blocker).
- **Per-finding dedup keys** (SHA-256 of `file:line:checkId:title`).
- **Baseline diff** — save current findings to
  `.audit_cache/baseline.json`; later runs highlight only new findings.
- **Trend tracking** — severity counts persisted at
  `.audit_cache/trend.json` (last 50 runs) and rendered as a delta
  banner against the previous run.
- **Recent-changes scope** — restrict findings to files touched in
  the last N commits, or (stricter) to exact diff hunk line ranges
  via `git diff --unified=0 HEAD~N`.
- **Multi-tool correlation** — ★ badge on findings flagged at the
  same `file:line` by ≥2 distinct tools.
- **clazy integration** — Qt-aware AST checks (`connect-3arg-lambda`,
  `container-inside-loop`, `old-style-connect`, `qt-keywords`, etc.)
  via `clazy-standalone`, reading the project's
  `compile_commands.json`. Retires three FP-prone regex checks.
- **Comment/string-aware filtering** — per-check opt-in state-machine
  scan that drops matches inside `//`, `/* */`, or `"strings"`.
- **SARIF v2.1.0 export** — OASIS-standard JSON consumed by GitHub
  Code Scanning, VSCode SARIF Viewer, SonarQube, CodeQL. Includes
  per-rule catalogue, `partialFingerprints`, and per-finding
  properties bag.
- **Single-file HTML report** — no external assets, embedded JSON
  payload + vanilla JS. Severity pills, text filter, collapsible
  check cards.
- **Interactive suppression** — click any dedup hash to prompt for
  a reason and append to `.audit_suppress` (JSONL v2: `{key, rule,
  reason, timestamp}` per line).
- **User-defined rules** via `<project>/audit_rules.json`. Flat
  schema mirrors the internal `AuditCheck` struct. User rules run
  through the full filter / parse / dedup / suppress pipeline.
- **CTest harness** — `tests/audit_self_test.sh` with per-rule
  `bad.*`/`good.*` fixtures using `// @expect <rule-id>` markers;
  count-based assertion.
- **Review with Claude** handoff — emits a plain-text report with
  `CLAUDE.md`, `STANDARDS.md`, `RULES.md`, and `CONTRIBUTING.md`
  prepended so Claude can weigh findings against documented rules.

### Changed

- `.audit_suppress` upgraded from v1 plain-key-per-line to v2 JSONL;
  v1 still loads, first write converts in place with a migration
  marker.
- Audit rule pack format: JSON (`audit_rules.json`), not YAML —
  `QJsonDocument` is built-in; flat schema's readability gap with
  YAML is small enough that a parser dependency isn't worth it.

### Security

- `audit_rules.json` is a trust boundary — its `command` field runs
  through `/bin/bash` unconditionally. Documented analogous to
  `.git/hooks`: your repo, your commands, no sandbox.

## [0.3.0]

### Added

- Claude Code deep integration: live status bar, project/session
  browser (Ctrl+Shift+J), permission allowlist editor (Ctrl+Shift+L),
  transcript viewer, slash-command shortcuts.
- 12 professional UX features: hot-reload config, column selection,
  sticky headers, tab renaming/coloring, snippets, auto-profile
  switching, badge watermarks, dark/light auto-switching, Nerd Font
  fallback, scrollbar, Ctrl+arrow word movement, scroll anchoring.
- Session persistence: save/restore scrollback via `QDataStream` +
  `qCompress` binary serialization.
- AI assistant dialog: OpenAI-compatible chat completions with SSE
  streaming, configurable endpoint/model.
- SSH manager: bookmark editor, PTY-based `ssh` connection.
- Lua 5.4 plugin system: sandboxed `ants` API, instruction-count
  timeout, event-driven handlers.
- GPU rendering path: QOpenGLWidget with glyph atlas, GLSL 3.3
  shaders.

### Fixed

- Six rounds of comprehensive security + correctness audits covering
  PTY FD leaks, bracketed-paste injection, SIGPIPE, Lua coroutine
  escape, hardcoded colours, atomic config writes, and more.

## [0.2.0]

### Added

- Full C++ terminal emulator rewrite: custom VT100/xterm state-machine
  parser, `TerminalGrid` with scrollback + alt screen, PTY via
  `forkpty`, QPainter rendering with `QTextLayout` ligature shaping.
- Mouse reporting (SGR format), focus reporting (mode 1004),
  synchronized output (mode 2026).
- OSC 8 hyperlinks, OSC 52 clipboard write, OSC 133 shell-integration
  markers, OSC 9 / 777 desktop notifications.
- Sixel graphics (DCS), Kitty graphics protocol (APC), iTerm2 images
  (OSC 1337).
- Kitty keyboard protocol with progressive enhancement (push / pop /
  query / set).
- Combining characters via per-line side table.
- Command palette (Ctrl+Shift+P), tab management with custom frameless
  titlebar, split panes via nested `QSplitter`s, 11 built-in themes.

## [0.1.0]

### Added

- Initial release: basic terminal emulator prototype.
