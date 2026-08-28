#
# spec file for package ants-terminal
#
# Copyright (c) 2026 Ants Terminal Contributors
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.
#
# Please submit bugfixes or comments via
#   https://github.com/milnet01/ants-terminal/issues
#

# RPM spec for Ants Terminal. Primary target is openSUSE Tumbleweed; the
# BuildRequires below carry %%if arms so the same file resolves on Fedora and
# Mageia too (ANTS-3727). Every macro used here (%%cmake, %%cmake_build,
# %%cmake_install, %%ctest, %%autosetup, %%{_*dir}) is common to openSUSE and
# Fedora, so the macros were never the portability problem — the package NAMES
# were, and each arm below records which name each distro actually uses.
#
# The openSUSE arm is deliberately byte-identical to what shipped before the
# arms were added, so a Tumbleweed build resolves exactly the set it always did.
#
# Version is kept in lockstep with CMakeLists.txt PROJECT(VERSION). Bump
# both when releasing.

Name:           ants-terminal
Version:        0.7.107
Release:        0
Summary:        Qt6 terminal emulator with Lua plugins and a project-audit dialog
License:        MIT
URL:            https://github.com/milnet01/ants-terminal
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz
# Filters one verified rpmlint false positive; see the file for the analysis.
# Declared here so it travels in the src.rpm — rpmlint itself does not need the
# declaration, it auto-loads *-rpmlintrc out of SOURCES.
Source1:        %{name}-rpmlintrc

# Wayland-native Quake-mode, via LayerShellQt. Default ON — it resolves on both
# openSUSE (layer-shell-qt6-devel) and Fedora (layer-shell-qt-devel), each at
# 6.7.3. It exists as a knob because CMakeLists.txt treats the dependency as
# optional (find_package CONFIG QUIET, with a Qt-toplevel fallback), so a distro
# that lacks it should lose one feature rather than fail to resolve. Turn it off
# for such a target with --without layershell, or an OBS prjconf line
# `Macros:` / `%%_without_layershell 1` scoped to that repository.
%bcond_without layershell

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
# Only Fedora and RHEL call it ninja-build; openSUSE, Mageia and Debian call it
# ninja. Neither name is a virtual provide of the other — checked on Tumbleweed
# (`ninja` provides ninja + rpm_macro(ninja_*) and nothing else) and against
# Fedora's metadata (`ninja-build` provides only ninja-build) — so this needs a
# real arm rather than one portable spelling.
%if 0%{?fedora} || 0%{?rhel}
BuildRequires:  ninja-build
%else
BuildRequires:  ninja
%endif
# The test suite shells out to real git: verify_changes_build_cache builds a
# throwaway repo via initGitProject() (init/config/add/commit) and
# cut_rc_behaviour exercises packaging/cut-rc.sh against one. A build VM has no
# git unless it is asked for, so those 13 tests fail rather than skip — they
# carry no 'no git in PATH' guard, unlike their siblings in
# mcp_roadmap_branch_drift. Declaring it makes them RUN (real coverage) instead
# of being excluded into silence. git-core is the client alone; the `git`
# metapackage would drag in gitk/git-email for nothing.
BuildRequires:  git-core
# Same shape as git-core above, and found the same way — by a build VM. The
# cited_by verb shells out to ripgrep with no skip path, so all 11 CitedBy
# tests fail with {"code":"rg_failed"} rather than skipping when rg is absent.
# ANTS-4391 already cost five red CI runs for this exact reason and was fixed
# by adding ripgrep to .github/workflows/ci.yml — but only there. The RPM is a
# SECOND environment that runs %%check and never got the same line.
# tests/features/ci_workflow_deps now reads this spec too, so deleting the
# line below reddens the suite. Declaring it makes those tests RUN instead
# of failing the build.
BuildRequires:  ripgrep
# ANTS-4718 — a monospace font. The card-grid case measures laid-out column
# positions, and a chroot with NO fonts installed makes Qt fall back to
# something no user has: `fc-match monospace` resolves to Liberation Mono here,
# and reproducing the empty-font condition locally (FONTCONFIG_FILE pointing at
# a config with no font dirs) reproduced the OBS failure exactly. The layout bug
# that exposed is fixed in the code from 0.7.107; this is the other half — a Qt
# GUI suite should not be measuring text under a degenerate fallback.
#
# Fedora is deliberately NOT given an arm. It already resolves a monospace font
# in its chroot and builds green, and an unverified package name there would
# turn a working target UNRESOLVABLE, which produces no build log to read at
# all. Names below are checked, not guessed: liberation-fonts is installed here,
# and Mageia's fonts-$type-$name convention gives fonts-ttf-liberation.
%if 0%{?mageia}
BuildRequires:  fonts-ttf-liberation
%else
%if !0%{?fedora} && !0%{?rhel}
BuildRequires:  liberation-fonts
%endif
%endif
BuildRequires:  cmake(Qt6Core) >= 6.2
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6OpenGL)
BuildRequires:  cmake(Qt6OpenGLWidgets)
# Qt6Sql is a REQUIRED component of the same find_package (ANTS-3756 — the
# roadmap store, the sole Qt6::Sql consumer, linked only by
# ants_roadmapstore_lib). Debian's qt6-base-dev bundles QtSql, which is why CI
# and the .deb never noticed; openSUSE splits it into qt6-sql-devel, so every
# openSUSE target failed configure with 'Failed to find required Qt component
# "Sql"'. Caught by the OBS validation build of 2026-08-02 — the same way, and
# for the same reason, as Qt6Test below.
BuildRequires:  cmake(Qt6Sql)
# ...and the SQLite DRIVER, at BUILD time, not just runtime. cmake(Qt6Sql) is
# the library and headers; the driver is a separate runtime-loaded plugin, and
# the roadmap-store tests open a real QSqlDatabase during %%check. Without it
# they fail 217 times with 'Driver not loaded' — a red build, long after
# everything has compiled (measured on Leap 16.0 and Tumbleweed, 2026-08-02).
#
# This is the hicolor-icon-theme situation further down, for the same reason:
# a `Requires:` is NOT present in the build chroot, so a package needed by the
# test suite has to be BuildRequired as well as Required. And it is the
# git-core rationale too — declaring it makes those tests RUN rather than fail
# or be excluded into silence.
#
# openSUSE-only, and that is verified rather than assumed: Mageia_10's chroot
# already carries the driver (its build got past the whole suite to RPM
# assembly in the same run), and Fedora keeps it in qt6-qtbase, which
# cmake(Qt6Core) already drags in.
%if 0%{?suse_version}
BuildRequires:  qt6-sql-sqlite
%endif
# Qt6Test is a REQUIRED component of the top-level find_package in
# CMakeLists.txt:92 — unconditionally, not gated on -DANTS_TESTS. Omitting it
# fails configure with 'Failed to find required Qt component "Test"' before a
# single object compiles (caught by the first OBS build, 2026-07-29).
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6Widgets)
# GoogleTest for the test bundles. CMakeLists.txt:806 does
# `find_package(GTest 1.13 QUIET)` and falls back to FetchContent from
# github.com when it fails — which cannot work in a build VM, since OBS gives
# the chroot no network (the first build died on 'could not find git for clone
# of googletest-populate'). Requiring it here keeps the system package on the
# found path so the download never fires.
#
# NOTE the package split: on openSUSE it is *gmock* that provides
# cmake(GTest); the gtest package provides only pkgconfig(gtest) and the
# libraries. Requiring cmake(GTest) is therefore both the correct dependency
# and the one that drags in gtest behind it.
BuildRequires:  cmake(GTest) >= 1.13
# Lua names its pkg-config module per-distro, and the difference is not
# cosmetic. openSUSE ships lua5.4.pc AND an unversioned lua.pc, but FOUR
# packages provide the unversioned pkgconfig(lua) there — lua53-devel,
# lua54-devel, lua55-devel and luajit-devel — so `pkgconfig(lua) >= 5.4` would
# let the resolver satisfy it with Lua 5.5. The versioned spelling pins the
# series exactly, which is why openSUSE keeps it.
#
# Fedora ships a single lua.pc (5.4.8) and no lua5.4.pc at all, so there the
# unversioned name is the only option — and it is unambiguous, because its only
# other provider is compat-lua-devel at 5.1.5, which the floor excludes.
#
# CMakeLists.txt:91 copes with both: pkg_check_modules(LUA lua5.4) misses on
# Fedora, and the find_package(Lua 5.4 QUIET) fallback on the next line finds it
# by header instead. The plugin system stays enabled either way.
%if 0%{?suse_version}
BuildRequires:  pkgconfig(lua5.4)
%else
BuildRequires:  pkgconfig(lua) >= 5.4
%endif
# Wayland-native Quake-mode (0.6.38). When this devel package is present,
# `find_package(LayerShellQt CONFIG QUIET)` in CMakeLists.txt wires layer-shell
# anchoring into setupQuakeMode(). Absent = Wayland Quake falls back to the Qt
# toplevel path; X11 Quake is unaffected. Kept a hard BuildRequires under the
# default-on bcond so distro users get the feature — the package is ~25 KiB and
# its runtime library is one KDE Plasma already pulls in.
#
# cmake(LayerShellQt) is a portable spelling, not an openSUSE one: it is
# generated by rpm's cmake dependency generator, and both layer-shell-qt6-devel
# (openSUSE) and layer-shell-qt-devel (Fedora) provide it at 6.7.3. The bcond
# above is for a target that ships no such package at all.
%if %{with layershell}
BuildRequires:  cmake(LayerShellQt) >= 6.0
%endif
# Packaging artefact validators (H2/H3/H4): invoked nowhere in %%check but
# good hygiene to pull them in so `cmake --install` staging under `osc build`
# surfaces any schema drift against the current appstream / desktop-file
# validators shipped by Tumbleweed.
#
# desktop-file-utils carries the same name everywhere. appstream-glib does not:
# it is libappstream-glib on Fedora and lib64appstream-glib-devel on Mageia,
# three spellings for a tool this spec never actually invokes. Rather than carry
# a three-way name table for an unused validator, it stays openSUSE-only — the
# hygiene it buys is on the platform this package is primarily built for, and
# elsewhere it would be a resolution failure in exchange for nothing.
%if 0%{?suse_version}
BuildRequires:  appstream-glib
%endif
BuildRequires:  desktop-file-utils
# Owns /usr/share/icons/hicolor/*/apps. It is already a runtime Requires below,
# but OBS's check-filelist QA step runs against the BUILD environment, where a
# Requires is absent — so without this the icon directories read as "not owned
# by a package" and the build fails after the RPM is already assembled.
BuildRequires:  hicolor-icon-theme
# Man page is installed pre-formatted (groff source); no runtime dep needed.

Requires:       hicolor-icon-theme
# Qt6 shlib deps are picked up automatically by rpm's auto-Requires.
#
# The SQLite driver is the exception, and auto-Requires cannot see it: Qt loads
# it as a runtime PLUGIN, so nothing links it and no shlib dependency is
# generated. Without the plugin present, QSqlDatabase::addDatabase() returns an
# invalid database at RUN time — the roadmap store fails on a user's machine,
# not in the build (CMakeLists.txt:86-91 says the same thing from the other side).
#
# Verified 2026-08-02, per distro, rather than assumed:
#   openSUSE  — split into its own package, so it must be named (zypper: the
#               driver is qt6-sql-sqlite, devel is qt6-sql-devel).
#   Fedora 44 — qt6-qtbase itself owns /usr/lib/qt6/plugins/sqldrivers and
#               there is no qt6-qtbase-sqlite subpackage, so the base package
#               that auto-Requires already drags in covers it. Nothing to add.
#   Mageia    — NOT verified; left unguarded deliberately rather than guessed
#               at. If the store misbehaves there, this is the first suspect.
%if 0%{?suse_version}
Requires:       qt6-sql-sqlite
%endif

%description
Ants Terminal is a terminal emulator built from scratch in C++20 with
Qt6. Features include a VT100/xterm parser with Kitty keyboard and Kitty
graphics, Sixel, OSC 8 hyperlinks, and OSC 133 shell-integration blocks;
an OpenGL glyph-atlas renderer (optional); a Ctrl+Shift+P command
palette; an AI-triage dialog over any OpenAI-compatible endpoint; an
SSH bookmark manager; and a Lua 5.4 plugin system with a sandboxed
ants.* API. The bundled Project Audit dialog runs cppcheck, clazy,
semgrep, and grep rules through a shared pipeline with SHA-256 dedup,
baseline/trend tracking, inline-suppression scanning (clang-tidy
NOLINT, cppcheck-suppress, flake8 noqa, bandit nosec, semgrep
nosemgrep, gitleaks #gitleaks:allow, eslint-disable-*, pylint
disable, ants-native // ants-audit:disable), and SARIF v2.1.0 + HTML
export.

%prep
%autosetup -n %{name}-%{version}

%build
# -DANTS_TESTS=ON is the CMake default; kept explicit so distro rebuilds
# never silently lose the ctest invocation in %%check.
%cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DANTS_TESTS=ON
%cmake_build

%install
%cmake_install

# Desktop DB + icon cache refresh. openSUSE's packaging guide recommends
# these %%post/%%postun scriptlets for any package that ships a .desktop
# entry or hicolor icons — without them, minimal Tumbleweed images won't
# see the launcher until the next session restart.
%post
/usr/bin/update-desktop-database -q %{_datadir}/applications &>/dev/null || :
/usr/bin/gtk-update-icon-cache -q %{_datadir}/icons/hicolor &>/dev/null || :

%postun
/usr/bin/update-desktop-database -q %{_datadir}/applications &>/dev/null || :
/usr/bin/gtk-update-icon-cache -q %{_datadir}/icons/hicolor &>/dev/null || :

%check
# The whole ctest suite, which includes Qt widget tests — not only the
# shell-based audit-rule fixtures. CMakeLists.txt wires QT_QPA_PLATFORM=offscreen
# for the opt-in `e2e` label only (CMakeLists.txt:991), so a build VM with no
# display server needs it exported here or every widget test aborts on platform
# plugin init. Locally the tests pass because a real session is present, which is
# exactly why this only shows up in a chroot build.
export QT_QPA_PLATFORM=offscreen
# Same shape, different prerequisite: the grid's width logic calls the system
# wcwidth() (terminalgrid.cpp:418), and the wide-char / combining tests adopt
# the AMBIENT locale via setlocale(LC_CTYPE, "") — deliberately, so they test
# what a real session does. A build chroot usually has no locale set at all,
# and under C/POSIX glibc cannot classify these characters at all. Measured
# locally, 2026-08-02:
#
#     LC_ALL=C         wcwidth(U+4E00 CJK) = -1   wcwidth(U+0301) = -1
#     LC_ALL=C.UTF-8   wcwidth(U+4E00 CJK) =  2   wcwidth(U+0301) =  0
#
# Not 1 — MINUS ONE, i.e. "unprintable", which is why the failure reads as
# "parser may not be detecting CJK as wide" rather than as an off-by-one. It
# cost three tests on Mageia_10 (WideCharResize x2, CombiningOnResize) once
# that target finally got far enough to run the suite.
#
# C.UTF-8 is built into glibc and needs no locale generation, so this works on
# every target without a locale package. This supplies a prerequisite the tests
# document in their own comments; it does not paper over a failure. The deeper
# question -- that the APP itself inherits the same ambient-locale dependency
# for CJK width -- is ANTS-3792, not something an export here can settle.
export LC_ALL=C.UTF-8
# The whole suite, perf and e2e lanes included. Two things had to be checked
# before trusting that: the perf benchmarks carry an OPTIONAL MB/s regression
# floor that is off by default (0.0 — bench_vt_throughput.cpp:166), so a loaded
# build worker cannot fail them on contention; and the e2e lane already passed
# here, since CMakeLists.txt gives it its own offscreen wiring. An earlier
# attempt to narrow this to -LE '(perf|e2e)' does NOT work regardless: %%ctest
# is declared %%ctest(:-:) and getopt-parses leading dashes, so rpm rejects it
# with "Unknown option L in ctest(:-:)" before ctest ever runs.
#
# Note the doubled percent signs above. rpm expands macros inside # comments —
# they are comments to the shell, not to the macro engine — so a singly-written
# macro reference here expands mid-comment and its trailing (:-:) reaches bash
# as a syntax error, which is exactly how this line first broke the build.
# ANTS-4719 — an earlier version of this note said %%check and %%post were
# "harmless singly, being section keywords with no macro body to expand". That
# is true on openSUSE and FALSE elsewhere, measured 2026-08-26: a BuildRequires
# comment reading "runs %%check" with one %% built fine on Tumbleweed and Leap
# and killed Mageia_10 at `error: Unknown tag`. Macro definitions differ by
# distro, so no macro name is safe unescaped in a comment here -- the openSUSE
# cross-distribution howto puts it as "if a distribution is not listed, the
# macro is not defined", and `rpm --eval %%check` returns %%check unchanged here
# while Mageia expands it. Double every one; rpm's other escape is %%dnl, which
# discards to end of line. obs-submit.sh refuses a bare one before it builds.
# ANTS-4720 — run everything EXCEPT the perf lane. Not a tolerance: the perf
# label is how this project already says "this assertion needs a quiet host",
# and every other gate honours it. tests/features/roadmap_read_seam/spec.md
# puts it plainly -- "A timing assertion on a loaded host is a flake
# generator" -- and both the default presets and tools/hooks/pre-push filter
# -LE 'e2e|perf'. The RPM was the ONLY gate running them, by accident, because
# %%ctest cannot take -LE (see above). Measured 2026-08-26: Inv3Latency's
# wall-clock p95 missed its 50 ms budget by 0.5% on an OBS worker and by 50%
# (75-78 ms) when pinned to two contended cores locally, while passing
# comfortably unloaded. The test already skips itself under ASan for exactly
# this reason -- an unsanitized, unloaded build is a precondition of the
# instrument, not a tolerance to widen.
#
# e2e is deliberately NOT excluded: it passes here, having its own offscreen
# wiring in CMakeLists.txt.
#
# The build directory is found rather than assumed -- %%ctest hardcodes its own
# per distro (plain `build` here, a different one on Fedora), and this file
# builds for four targets.
for _d in build redhat-linux-build %{_target_platform} .; do
    [ -f "$_d/CTestTestfile.cmake" ] && break
done
cd "$_d"
ctest --output-on-failure --force-new-ctest-process -j"${RPM_BUILD_NCPUS:-1}" -LE perf
cd ..

%files
%license LICENSE
%doc README.md CHANGELOG.md ROADMAP.md
%{_bindir}/%{name}
%{_datadir}/applications/za.co.antsprojectshub.AntsTerminal.desktop
%{_datadir}/metainfo/za.co.antsprojectshub.AntsTerminal.metainfo.xml
%{_datadir}/icons/hicolor/16x16/apps/%{name}.png
%{_datadir}/icons/hicolor/32x32/apps/%{name}.png
%{_datadir}/icons/hicolor/48x48/apps/%{name}.png
%{_datadir}/icons/hicolor/64x64/apps/%{name}.png
%{_datadir}/icons/hicolor/128x128/apps/%{name}.png
%{_datadir}/icons/hicolor/256x256/apps/%{name}.png
# Globbed rather than %%{?ext_man}, which is an openSUSE macro (".gz" here) that
# expands to nothing anywhere else. Fedora compresses man pages to .gz all the
# same, so the un-globbed form would name a file that does not exist there and
# fail the build with "Installed (but unpackaged) file(s) found". The glob is
# correct on every distro and stays correct if a compressor ever changes.
%{_mandir}/man1/%{name}.1*
%{_datadir}/bash-completion/completions/%{name}
%{_datadir}/zsh/site-functions/_%{name}
# The fish directories are owned by the fish package, which is not a build
# dependency here — pulling in a whole shell to own two directories is not worth
# it, so this package co-owns them instead (RPM permits shared directory
# ownership). bash-completion and zsh's dirs need no such entry: their owners
# happen to be present in the build root already.
%dir %{_datadir}/fish
%dir %{_datadir}/fish/vendor_completions.d
%{_datadir}/fish/vendor_completions.d/%{name}.fish
# OSC 133 shell-integration hooks (CMakeLists.txt:777). Sourced by the user from
# ~/.bashrc / ~/.zshrc, so they must ship — rpm refuses to leave them unpackaged
# and the build fails with "Installed (but unpackaged) file(s) found".
#
# To re-check this list after changing any install() rule, without waiting on a
# build: DESTDIR=/tmp/d cmake --install build && find /tmp/d -type f -o -type l
# — every entry must be matched by a line in this section.
%dir %{_datadir}/%{name}
%{_datadir}/%{name}/shell-integration

%changelog
# openSUSE convention keeps the changelog in a separate .changes file
# rather than inline in the spec. See packaging/README.md for the OBS
# workflow (osc service run, .changes update via `osc vc`).
