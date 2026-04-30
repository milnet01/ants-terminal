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
Version:        0.7.56
Release:        0
Summary:        Qt6 terminal emulator with Lua plugins and a project-audit dialog
License:        MIT
URL:            https://github.com/milnet01/ants-terminal
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  cmake(Qt6Core) >= 6.2
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6OpenGL)
BuildRequires:  cmake(Qt6OpenGLWidgets)
BuildRequires:  cmake(Qt6Widgets)
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
# Audit-rule regression suite. Pure shell + fixture tree, no GUI needed;
# safe under `osc build` / OBS chroot.
%ctest

%files
%license LICENSE
%doc README.md CHANGELOG.md ROADMAP.md
%{_bindir}/%{name}
%{_datadir}/applications/org.ants.Terminal.desktop
%{_datadir}/metainfo/org.ants.Terminal.metainfo.xml
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

%changelog
# openSUSE convention keeps the changelog in a separate .changes file
# rather than inline in the spec. See packaging/README.md for the OBS
# workflow (osc service run, .changes update via `osc vc`).
