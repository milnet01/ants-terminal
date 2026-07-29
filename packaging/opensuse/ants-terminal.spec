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

# openSUSE RPM spec for Ants Terminal. Targets Tumbleweed; known to build on
# Leap 15.5+ with the same BuildRequires. Submit through OBS to
# devel:languages:misc (or a dedicated project) — every macro used here
# (%%cmake, %%cmake_build, %%cmake_install, %%ctest, %%autosetup, %%{_*dir})
# is an openSUSE / Fedora core macro so the file is close to portable.
#
# Version is kept in lockstep with CMakeLists.txt PROJECT(VERSION). Bump
# both when releasing.

Name:           ants-terminal
Version:        0.7.102
Release:        0
Summary:        Qt6 terminal emulator with Lua plugins and a project-audit dialog
License:        MIT
URL:            https://github.com/milnet01/ants-terminal
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  ninja
BuildRequires:  pkgconfig
# The test suite shells out to real git: verify_changes_build_cache builds a
# throwaway repo via initGitProject() (init/config/add/commit) and
# cut_rc_behaviour exercises packaging/cut-rc.sh against one. A build VM has no
# git unless it is asked for, so those 13 tests fail rather than skip — they
# carry no 'no git in PATH' guard, unlike their siblings in
# mcp_roadmap_branch_drift. Declaring it makes them RUN (real coverage) instead
# of being excluded into silence. git-core is the client alone; the `git`
# metapackage would drag in gitk/git-email for nothing.
BuildRequires:  git-core
BuildRequires:  cmake(Qt6Core) >= 6.2
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6OpenGL)
BuildRequires:  cmake(Qt6OpenGLWidgets)
# Qt6Test is a REQUIRED component of the top-level find_package in
# CMakeLists.txt:86 — unconditionally, not gated on -DANTS_TESTS. Omitting it
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
BuildRequires:  pkgconfig(lua5.4)
# Optional: Wayland-native Quake-mode (0.6.38). When this devel package
# is present, `find_package(LayerShellQt CONFIG QUIET)` in CMakeLists.txt
# wires layer-shell anchoring into setupQuakeMode(). Absent = Wayland
# Quake falls back to the Qt toplevel path; X11 Quake is unaffected.
# Listed as a hard BuildRequires so distro users get the feature by
# default — the package itself is ~25 KiB, runtime dep is the matching
# layer-shell-qt6 library which KDE Plasma already pulls in.
BuildRequires:  cmake(LayerShellQt) >= 6.0
# Packaging artefact validators (H2/H3/H4): invoked nowhere in %check but
# good hygiene to pull them in so `cmake --install` staging under `osc build`
# surfaces any schema drift against the current appstream / desktop-file
# validators shipped by Tumbleweed.
BuildRequires:  appstream-glib
BuildRequires:  desktop-file-utils
# Man page is installed pre-formatted (groff source); no runtime dep needed.

Requires:       hicolor-icon-theme
# Qt6 shlib deps are picked up automatically by rpm's auto-Requires.

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
# never silently lose the ctest invocation in %check.
%cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DANTS_TESTS=ON
%cmake_build

%install
%cmake_install

# Desktop DB + icon cache refresh. openSUSE's packaging guide recommends
# these %post/%postun scriptlets for any package that ships a .desktop
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
# (%%check / %%post appear singly elsewhere in this file and are harmless only
# because they are section keywords with no macro body to expand.)
%ctest

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
%{_mandir}/man1/%{name}.1%{?ext_man}
%{_datadir}/bash-completion/completions/%{name}
%{_datadir}/zsh/site-functions/_%{name}
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
