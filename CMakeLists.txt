cmake_minimum_required(VERSION 3.20)
project(ants-terminal VERSION 0.7.91 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single source of truth for the application version. Consumed by main.cpp
# (setApplicationVersion) and auditdialog.cpp (SARIF driver.version, UI badge).
# Bump in PROJECT() above; every caller picks up the new value on rebuild.
add_compile_definitions(ANTS_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Build metadata for Help → About (ANTS-1222)
# ---------------------------------------------------------------------------
# Configure-time bake: date, build type, short git SHA. Surfaced via
# the generated `build_info.h` (see cmake/build_info.h.in). Only
# `aboutdialogs.cpp` includes this header, so a SHA change recompiles
# one TU rather than every TU (vs add_compile_definitions which would
# invalidate ccache for the whole project on every commit).
#
# Reconfigure to refresh between commits — `cmake build/` re-runs the
# git rev-parse step and rewrites the header. The compiler ID/version
# is detected at COMPILE time inside aboutdialogs.cpp via __GNUC__ /
# __clang_major__, so no CMake plumbing for that.
if(DEFINED ENV{SOURCE_DATE_EPOCH})
    string(TIMESTAMP ANTS_BUILD_DATE "%Y-%m-%d" UTC)
else()
    string(TIMESTAMP ANTS_BUILD_DATE "%Y-%m-%d")
endif()

set(ANTS_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT ANTS_BUILD_TYPE)
    set(ANTS_BUILD_TYPE "Unknown")
endif()

set(ANTS_BUILD_COMMIT "unknown")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_sha_raw
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_sha_result
        ERROR_QUIET)
    if(_git_sha_result EQUAL 0 AND _git_sha_raw)
        set(ANTS_BUILD_COMMIT "${_git_sha_raw}")
    endif()
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build_info.h.in"
    "${CMAKE_BINARY_DIR}/generated/build_info.h"
    @ONLY)

# Default build type if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Qt6 version floor. 6.2 is the first LTS that ships the full set of
# components we need (DBus was stabilised in 6.2; Qt6OpenGLWidgets
# reached feature parity there). Building against older Qt6 releases
# (6.0 / 6.1) compiles but silently loses DBus on some distros and
# trips shader-compile edge cases in the OpenGL renderer — without
# this floor a rolling-distro user with an older qt6-base installed
# alongside a newer one could pick up the wrong one.
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets DBus Test)

# Lua 5.4 (optional — plugin system)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(LUA lua5.4)
endif()
if(NOT LUA_FOUND)
    # Fallback: try FindLua
    find_package(Lua 5.4 QUIET)
    if(LUA_FOUND)
        set(LUA_INCLUDE_DIRS ${LUA_INCLUDE_DIR})
    endif()
endif()

# LayerShellQt (optional — wlr-layer-shell-v1 anchoring for Quake-mode on
# Wayland compositors). Without it, Quake-mode still works via the regular
# Qt toplevel path on Wayland but positioning/always-on-top behaviour is at
# the mercy of the compositor (no wl_shell equivalent of X11's
# _NET_WM_STATE_ABOVE). With it, we anchor to the top edge via
# zwlr_layer_shell_v1 so the dropdown behaves the way X11 users expect.
# Used only in mainwindow.cpp's setupQuakeMode() — guarded behind the
# ANTS_WAYLAND_LAYER_SHELL compile definition.
find_package(LayerShellQt CONFIG QUIET)

# ---------------------------------------------------------------------------
# Build-system infrastructure (ANTS-1217 Phase 1)
# ---------------------------------------------------------------------------
# Three knobs that compound the OOM-cure architecture (one main exe + a
# small number of test bundles) with near-zero risk:
#
#   1. ccache as compiler launcher — re-configures and branch switches no
#      longer recompile already-seen TUs; cc1plus is not invoked at all on
#      a cache hit (zero RSS, zero time). CI runners are stateless so they
#      don't benefit, but the workstation rebuild loop does.
#   2. CMAKE_OPTIMIZE_DEPENDENCIES — when an internal STATIC library's
#      interface (its public headers) didn't change, CMake skips re-linking
#      dependents. Requires CMake ≥ 3.19; project mandates 3.20.
#   3. JOB_POOLS (Ninja only — harmless under Make) — caps concurrent
#      compile + link invocations even if the user passes -j$(nproc).
#      Belt-and-suspenders against future regressions: the architectural
#      fix removes the cause of the OOM, this prevents the consequence.
#      Sized at `max(2, nproc/4)` after a second 2026-05-10 stutter
#      report on a 12-core box: `nproc/2` (= 6 jobs) was still pegging
#      enough cores to starve the desktop session's UI thread. `nproc/4`
#      (= 3 jobs on a 12-core / 4 jobs on a 16-core) leaves 9+ cores free
#      for everything outside the build. Each Qt-bloated cc1plus is
#      200-300 MiB and concurrent links are even heavier; `link_pool=1`
#      serialises the bundle linker steps — link is the heaviest single
#      process and the one most likely to swap.
# ---------------------------------------------------------------------------
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
    set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
    message(STATUS "ccache found at ${CCACHE_PROGRAM} — using as compiler launcher")
endif()

set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

include(ProcessorCount)
ProcessorCount(_ANTS_NPROC)
if(_ANTS_NPROC LESS 4)
    set(_ANTS_COMPILE_CAP ${_ANTS_NPROC})
else()
    math(EXPR _ANTS_COMPILE_CAP "${_ANTS_NPROC} / 4")
    if(_ANTS_COMPILE_CAP LESS 2)
        set(_ANTS_COMPILE_CAP 2)
    endif()
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS
    compile_pool=${_ANTS_COMPILE_CAP}
    link_pool=1)
set(CMAKE_JOB_POOL_COMPILE compile_pool)
set(CMAKE_JOB_POOL_LINK    link_pool)
message(STATUS "Build job pools: compile=${_ANTS_COMPILE_CAP} link=1 (host nproc=${_ANTS_NPROC})")

# ---------------------------------------------------------------------------
# Shared compile-flag interface (ANTS-1217 Phase 1)
# ---------------------------------------------------------------------------
# INTERFACE library carrying the warning + hardening flag set so every
# consumer (libs + main exe + test bundles) compiles with identical
# flags. Single source of truth — flag drift breaks loudly at PCH-validation
# time rather than silently producing different ABIs across libs/exe.
#
# -Wformat=2 upgrades -Wformat to include non-literal format strings, %n usage,
# and missing-argument diagnostics — catches the same class of bug the audit
# `format_string` rule flags at the regex layer.
#
# -Wshadow=local (not full -Wshadow) catches a local shadowing another local
# — the actual bug class. Full -Wshadow also flags parameter-vs-member name
# collisions which are pervasive Qt convention (`void setData(QByteArray data)`
# style) and would drown the signal.
#
# -Wnull-dereference catches paths where a pointer known-null reaches a
# dereference — often from Qt getters (findChild, parent, qobject_cast).
#
# -Wconversion / -Wsign-conversion are noisy by nature; kept OFF for now to
# avoid drowning real findings. Flip on individually when a quiet pass is
# worth it (see ROADMAP: "Tighten narrowing conversions").
#
# -gsplit-dwarf (Debug only) writes per-TU debug info to .dwo files instead
# of embedding into .o. The linker no longer reads/relocates hundreds of MiB
# of debug strings, cutting linker peak RSS 30–50%. Release builds keep the
# standard format so distro debuginfo packaging is unchanged.
add_library(ants_warnings INTERFACE)
target_compile_options(ants_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wformat=2 -Wformat-security
    -Wshadow=local -Wnull-dereference
    # Security hardening
    -fstack-protector-strong
    -fstack-clash-protection
    -fcf-protection
    $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>
    -D_GLIBCXX_ASSERTIONS
    # Smaller debug info on Debug builds → lower linker RSS
    $<$<CONFIG:Debug>:-gsplit-dwarf>
)

# ---------------------------------------------------------------------------
# Source libraries (ANTS-1217 Phase 1)
# ---------------------------------------------------------------------------
# Six mandatory + two conditional STATIC libraries. Each src/*.cpp compiles
# ONCE for the whole build (was: 1× for ants-terminal + N× for tests that
# linked it). The libs declare their Qt link surface PUBLIC so consumers
# (the main exe and future test bundles) inherit transitively.
#
# Layering (PUBLIC = transitive, PRIVATE = lib-internal):
#
#   ants_core_lib    ← config / debuglog / IPC / session / PTY foundations
#   ants_vt_lib      ← parser + grid + widget + themes (depends on core)
#   ants_chrome_lib  ← mainwindow + tabbar + palette (depends on vt)
#   ants_claude_lib  ← Claude Code integration (depends on chrome)
#   ants_audit_lib   ← audit dialog + engine + hygiene (depends on core)
#   ants_dialogs_lib ← settings/about/diff/ssh/etc. (depends on chrome)
#   ants_lua_lib     ← only if LUA_FOUND
#   ants_helper_lib  ← only if ANTS_ENABLE_HELPER_CLI
add_library(ants_core_lib STATIC
    src/config.cpp
    src/debuglog.cpp
    src/clipboardguard.cpp
    src/remotecontrol.cpp
    src/fileoutline.cpp
    src/gitwrap.cpp
    src/indiereviewengine.cpp
    src/debtsweepengine.cpp
    src/roadmapfoldin.cpp
    src/subsystemmap.cpp
    src/themedstylesheet.cpp
    src/globalshortcutsportal.cpp
    src/dialogshowtracer.cpp
    src/kwinpositiontracker.cpp
    src/sessionmanager.cpp
    src/ptyhandler.cpp
    src/vtstream.cpp
)
target_link_libraries(ants_core_lib
    PUBLIC  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::DBus util
    PRIVATE ants_warnings)
target_include_directories(ants_core_lib PUBLIC src)
set_target_properties(ants_core_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ants_vt_lib STATIC
    src/vtparser.cpp
    src/terminalgrid.cpp
    src/terminalwidget.cpp
    src/themes.cpp
)
target_link_libraries(ants_vt_lib
    PUBLIC  ants_core_lib Qt6::OpenGL Qt6::OpenGLWidgets
    PRIVATE ants_warnings)
set_target_properties(ants_vt_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ants_chrome_lib STATIC
    src/mainwindow.cpp
    src/titlebar.cpp
    src/dialogchrome.cpp
    src/coloredtabbar.cpp
    src/commandpalette.cpp
)
target_link_libraries(ants_chrome_lib
    PUBLIC  ants_vt_lib
    PRIVATE ants_warnings)
set_target_properties(ants_chrome_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(LayerShellQt_FOUND)
    target_link_libraries(ants_chrome_lib PRIVATE LayerShellQt::Interface)
    target_compile_definitions(ants_chrome_lib PRIVATE ANTS_WAYLAND_LAYER_SHELL=1)
endif()
# mainwindow.cpp gates Lua-related slots and members on ANTS_LUA_PLUGINS;
# a handful of call sites also reference those symbols unconditionally
# (e.g. rebuildCommandPalette() at line 411). Pre-1217 the define was
# attached to the monolithic ants-terminal target so mainwindow.cpp
# always saw it. Carry the same semantics forward by applying it to
# the chrome lib when LUA_FOUND. mainwindow.cpp cleanup is out of
# Phase 1 scope.
if(LUA_FOUND)
    target_compile_definitions(ants_chrome_lib PUBLIC ANTS_LUA_PLUGINS=1)
endif()

add_library(ants_claude_lib STATIC
    src/claudeintegration.cpp
    src/claudeallowlist.cpp
    src/claudestatuswidgets.cpp
    src/claudetabtracker.cpp
    src/claudetranscript.cpp
    src/claudeprojects.cpp
    src/claudebgtasks.cpp
    src/claudebgtasksdialog.cpp
    src/claudetasklist.cpp
    src/claudetasklistdialog.cpp
)
target_link_libraries(ants_claude_lib
    PUBLIC  ants_chrome_lib
    PRIVATE ants_warnings)
set_target_properties(ants_claude_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ants_audit_lib STATIC
    src/auditdialog.cpp
    src/auditengine.cpp
    src/audithygiene.cpp
    src/featurecoverage.cpp
    src/auditrulequality.cpp
)
target_link_libraries(ants_audit_lib
    PUBLIC  ants_core_lib
    PRIVATE ants_warnings)
set_target_properties(ants_audit_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ants_dialogs_lib STATIC
    src/settingsdialog.cpp
    src/roadmapdialog.cpp
    src/roadmapshortcutsdialog.cpp
    src/diffviewer.cpp
    src/sshdialog.cpp
    src/aidialog.cpp
    src/aboutdialogs.cpp
    src/toggleswitch.cpp
)
target_link_libraries(ants_dialogs_lib
    PUBLIC  ants_chrome_lib
    PRIVATE ants_warnings)
# ANTS-1222: aboutdialogs.cpp includes the configure-time-generated
# build_info.h. Header lives in build/generated/ and is regenerated
# on every `cmake build/` reconfigure (see top-of-file block).
target_include_directories(ants_dialogs_lib
    PRIVATE "${CMAKE_BINARY_DIR}/generated")
set_target_properties(ants_dialogs_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(LUA_FOUND)
    add_library(ants_lua_lib STATIC
        src/luaengine.cpp
        src/pluginmanager.cpp
    )
    target_link_libraries(ants_lua_lib
        PUBLIC  ants_core_lib ${LUA_LIBRARIES}
        PRIVATE ants_warnings)
    target_include_directories(ants_lua_lib PUBLIC ${LUA_INCLUDE_DIRS})
    target_compile_definitions(ants_lua_lib PUBLIC ANTS_LUA_PLUGINS=1)
    set_target_properties(ants_lua_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
    message(STATUS "Lua ${LUA_VERSION} found — plugin system enabled")
else()
    message(STATUS "Lua 5.4 not found — plugin system disabled")
endif()

if(LayerShellQt_FOUND)
    message(STATUS "LayerShellQt found — Wayland-native Quake-mode enabled")
else()
    message(STATUS "LayerShellQt not found — Quake-mode on Wayland falls back to the Qt toplevel path")
endif()

# ---------------------------------------------------------------------------
# Main executable (ANTS-1217 Phase 1)
# ---------------------------------------------------------------------------
# Just main.cpp — every other src/*.cpp is in a STATIC library above and is
# reached transitively through target_link_libraries.
add_executable(ants-terminal src/main.cpp)
target_link_libraries(ants-terminal PRIVATE
    ants_core_lib
    ants_vt_lib
    ants_chrome_lib
    ants_claude_lib
    ants_audit_lib
    ants_dialogs_lib
    $<$<BOOL:${LUA_FOUND}>:ants_lua_lib>
    ants_warnings
)

# Precompile Qt aggregate headers. Qt headers dominate per-TU compile cost
# (~200-300 MiB working set in cc1plus) and are pulled in by nearly every
# .cpp under src/. PCH cuts that 30-50%, which keeps the build under
# RAM+swap on 32 GiB hosts as the source tree grows. Aggregate headers
# (no individual classes) so the PCH stays valid across edits.
target_precompile_headers(ants-terminal PRIVATE
    <QtCore/QtCore>
    <QtGui/QtGui>
    <QtWidgets/QtWidgets>
)

# ANTS-1116 v1 — optional `ants-helper` CLI binary. Default OFF
# until ANTS-1120 measurement validates the token-saving claim.
option(ANTS_ENABLE_HELPER_CLI
    "Build the ants-helper local-subagent CLI binary (ANTS-1116 v1)"
    OFF)
if(ANTS_ENABLE_HELPER_CLI)
    add_library(ants_helper_lib STATIC src/antshelper.cpp)
    target_link_libraries(ants_helper_lib
        PUBLIC  Qt6::Core
        PRIVATE ants_warnings)
    target_include_directories(ants_helper_lib PUBLIC src)
    set_target_properties(ants_helper_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

    add_executable(ants-helper src/antshelpermain.cpp)
    target_link_libraries(ants-helper PRIVATE ants_helper_lib ants_warnings)
endif()

# Sanitizer support (development builds). Applied via the shared
# ants_warnings INTERFACE so every lib + the main exe pick it up
# consistently. The link side still attaches to the executable directly
# because INTERFACE link options on a chain of STATIC libs only reach
# the final consumer.
option(ANTS_SANITIZERS "Enable ASan+UBSan for debugging" OFF)
if(ANTS_SANITIZERS)
    target_compile_options(ants_warnings INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    # Match link option to compile option: every consumer of ants_warnings
    # (main exe + test bundles) needs the sanitizer runtime libs pulled in
    # by the linker, otherwise STATIC-lib .o files instrumented with
    # __asan_*/__ubsan_* calls fail to link with "undefined reference".
    # Pre-1217 ANTS_SANITIZERS predated the STATIC-lib refactor; the
    # global add_compile_options/add_link_options handled this implicitly.
    target_link_options(ants_warnings INTERFACE
        -fsanitize=address,undefined)
endif()
target_link_options(ants-terminal PRIVATE
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
)
set_target_properties(ants-terminal PROPERTIES POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

install(TARGETS ants-terminal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Desktop integration — freedesktop.org spec + AppStream metainfo
# ---------------------------------------------------------------------------
# Reverse-DNS naming (org.ants.Terminal) is what AppStream and .desktop both
# expect — `launchable` in the metainfo ties the two files together by the
# shared stem. Installed under share/ so distro packagers can override with
# DESTDIR for split-staging builds.
install(FILES packaging/linux/org.ants.Terminal.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

install(FILES packaging/linux/org.ants.Terminal.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)

# Hicolor icon theme — six sizes. Install each under the matching
# <size>x<size>/apps/ subdir and RENAME to the generic `ants-terminal.png`
# so the .desktop `Icon=ants-terminal` lookup resolves via the standard
# icon-theme index at runtime.
foreach(_sz IN ITEMS 16 32 48 64 128 256)
    install(FILES assets/ants-terminal-${_sz}.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${_sz}x${_sz}/apps
            RENAME ants-terminal.png)
endforeach()

# Man page (section 1 — user commands). Source-of-truth groff lives under
# packaging/linux/; CMake places it at the GNU-standard mandir. man-db /
# mandoc / less-via-man pick it up automatically once the install root is
# on MANPATH (true for /usr and /usr/local out of the box).
install(FILES packaging/linux/ants-terminal.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

# Shell completions (bash / zsh / fish). Hand-written for the current
# QCommandLineParser surface (-h/-v/--quake/--dropdown/--new-plugin); each
# file is installed to the conventional vendor location for its shell so
# distro packages don't have to relocate them:
#   bash → ${datarootdir}/bash-completion/completions/ants-terminal
#   zsh  → ${datarootdir}/zsh/site-functions/_ants-terminal
#   fish → ${datarootdir}/fish/vendor_completions.d/ants-terminal.fish
# All three are auto-discovered without requiring the user to source
# anything by hand.
install(FILES packaging/completions/ants-terminal.bash
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/bash-completion/completions
        RENAME ants-terminal)
install(FILES packaging/completions/_ants-terminal
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/zsh/site-functions)
install(FILES packaging/completions/ants-terminal.fish
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/fish/vendor_completions.d)

# OSC 133 shell-integration hook scripts (0.7.0). HMAC-signed shell hooks
# the user sources from ~/.bashrc / ~/.zshrc to authenticate OSC 133
# markers against the verifier in TerminalGrid. See
# packaging/shell-integration/README.md for setup + threat model. Sourced
# manually by the user (not auto-loaded) so they can opt in per shell
# per machine — the verifier is silent unless $ANTS_OSC133_KEY is set.
install(FILES packaging/shell-integration/ants-osc133.bash
              packaging/shell-integration/ants-osc133.zsh
              packaging/shell-integration/README.md
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ants-terminal/shell-integration)

# ---------------------------------------------------------------------------
# Tests — audit rule fixtures (ctest)
# ---------------------------------------------------------------------------
# Shell-based harness that runs each audit grep pattern against a small
# fixture tree and asserts the expected match counts. Keeps the rule
# regexes in sync with src/auditdialog.cpp; CI runs `ctest` after the
# build to catch drift.
#
# Default ON since 2026-05-10 (ANTS-1217 Phase 7): the per-test
# add_executable sprawl that originally forced this OFF was drained by
# Phases 2–6 — 141 standalone test binaries → 7–9 GoogleTest bundle
# binaries with `gtest_discover_tests`. Combined with the
# `compile_pool=max(2, nproc/4)` JOB_POOLS cap below, a default
# `cmake -B build && cmake --build build` no longer trips earlyoom on
# 32 GiB hosts. Pass `-DANTS_TESTS=OFF` to opt out (e.g. for a fast
# main-exe-only iteration).
option(ANTS_TESTS "Build tests (includes audit rule regression tests)" ON)
if(ANTS_TESTS)
    enable_testing()

    # ---- ANTS-1217 Phase 2 — GoogleTest plumbing ----
    # System pkg first (Tumbleweed gtest-devel, Ubuntu 24.04 libgtest-dev).
    # Floor 1.13 — DISCOVERY_MODE PRE_TEST semantics.
    # FetchContent fallback only fires on hosts without a usable system pkg.
    find_package(GTest 1.13 QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "System GTest >=1.13 not found — falling back to FetchContent v1.15.2")
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.15.2)
        set(BUILD_GMOCK    OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST  OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    else()
        message(STATUS "GTest ${GTest_VERSION} found — using system package")
    endif()
    include(GoogleTest)

    # GUI bundle helper — bundles share PCH from ants-terminal so the per-TU
    # Qt cost only pays once across the whole tests-on build. The Qt6::Widgets
    # link surface comes through transitively from each ants_*_lib (PUBLIC).
    #
    # The internal ants_*_lib graph has cycles (chrome ↔ claude ↔ dialogs
    # via mainwindow.cpp's calls into all three). GNU ld scans archives
    # left-to-right and won't revisit, so wrap the bundle's static libs in
    # `--start-group/--end-group` so the linker keeps re-scanning until
    # all symbols resolve. Cheap on archive count this small (≤ 7).
    function(ants_add_gui_bundle name)
        cmake_parse_arguments(B "" "" "LIBS;SOURCES" ${ARGN})
        add_executable(${name} tests/bundle_main_gui.cpp ${B_SOURCES})
        target_link_libraries(${name} PRIVATE
            -Wl,--start-group ${B_LIBS} -Wl,--end-group
            ants_warnings GTest::gtest)
        target_precompile_headers(${name} REUSE_FROM ants-terminal)
        gtest_discover_tests(${name}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "features;fast" TIMEOUT 10)
    endfunction()

    # Core-only bundle helper — Qt::Core only, no Widgets. PCH not reused
    # from ants-terminal (different INTERFACE flag set without Widgets).
    function(ants_add_core_bundle name)
        cmake_parse_arguments(B "" "" "LIBS;SOURCES" ${ARGN})
        add_executable(${name} tests/bundle_main_core.cpp ${B_SOURCES})
        target_link_libraries(${name} PRIVATE
            ${B_LIBS} ants_warnings GTest::gtest Qt6::Core)
        gtest_discover_tests(${name}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "features;fast" TIMEOUT 10)
    endfunction()

    add_test(NAME audit_rule_fixtures
             COMMAND bash ${CMAKE_SOURCE_DIR}/tests/audit_self_test.sh)
    set_tests_properties(audit_rule_fixtures PROPERTIES
        LABELS "audit;fast"
        TIMEOUT 30)

    add_test(NAME claude_git_context_script
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/features/claude_git_context_hook/test_script.sh")
    set_tests_properties(claude_git_context_script PROPERTIES
        LABELS "features;fast"
        TIMEOUT 30
        ENVIRONMENT "SRC_SETTINGSDIALOG_CPP=${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp")

    # ANTS-1252 — token-saving hook pack conformance (shell harness).
    add_test(NAME hook_pack
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/features/hook_pack/test_hooks.sh")
    set_tests_properties(hook_pack PROPERTIES
        LABELS "features;fast"
        TIMEOUT 30)

    # bench_vt_throughput — VT parse + grid-apply throughput harness.
    # Drives the 0.8.0 "Terminal throughput slowdowns" investigation
    # (ROADMAP.md § 0.8.0 > ⚡ Performance). Linked against only
    # terminalgrid.cpp + vtparser.cpp — no GUI, no PTY. Builds at -O2
    # so numbers resemble release builds regardless of
    # CMAKE_BUILD_TYPE. Registered under the `perf` label; excluded
    # from the default `fast` suite so day-to-day ctest runs stay
    # under two seconds. Run explicitly with
    # `ctest -L perf --verbose` or the raw binary. Increase corpus
    # size with `ANTS_PERF_MB=64 ./bench_vt_throughput`.
    add_executable(bench_vt_throughput
        tests/perf/bench_vt_throughput.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(bench_vt_throughput PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(bench_vt_throughput PRIVATE src)
    target_compile_options(bench_vt_throughput PRIVATE
        -Wall -Wextra -Wpedantic -O2
    )
    add_test(NAME vt_throughput COMMAND bench_vt_throughput)
    set_tests_properties(vt_throughput PROPERTIES
        LABELS "perf"
        TIMEOUT 60)

    # ----------------------------------------------------------------------
    # ANTS-1217 Phase 2 — test_vt bundle.
    # 27 standalone test executables for vtparser/terminalgrid/themes
    # invariants consolidated into one GoogleTest bundle. Each migrated
    # test_*.cpp registers via TEST(SuiteName, ...) blocks under a
    # directory-derived Suite (see tests/features/README.md §Bundles).
    # gtest_discover_tests turns each TEST() block into a separate ctest
    # entry, so ctest -L features keeps working unchanged.
    # ----------------------------------------------------------------------
    ants_add_gui_bundle(test_vt
        LIBS    ants_vt_lib ants_core_lib
        SOURCES tests/features/scrollback_redraw/test_redraw.cpp
                tests/features/scrollback_redraw/test_viewport_stable.cpp
                tests/features/scrollback_frozen_view/test_frozen_view.cpp
                tests/features/scroll_region_rotate/test_scroll_region_rotate.cpp
                tests/features/scroll_region_leak_after_apps/test_scroll_region_leak.cpp
                tests/features/scroll_region_growth_on_resize/test_scroll_region_growth.cpp
                tests/features/decstr_soft_reset/test_decstr_soft_reset.cpp
                tests/features/osc9_progress_disambiguator/test_osc9_progress.cpp
                tests/features/wide_char_resize/test_wide_char_resize.cpp
                tests/features/sgr_attribute_reset/test_sgr_reset.cpp
                tests/features/vtparser_simd_scan/test_vtparser_simd.cpp
                tests/features/vt_osc_esc_discard/test_vt_osc_esc_discard.cpp
                tests/features/vtparser_print_run_coalesce/test_print_run_coalesce.cpp
                tests/features/combining_on_resize/test_combining.cpp
                tests/features/osc8_insert_delete_lines/test_osc8_linerange.cpp
                tests/features/hyperlink_resize_clamp/test_hyperlink_resize_clamp.cpp
                tests/features/osc133_hmac_verification/test_osc133_hmac.cpp
                tests/features/threaded_parse_equivalence/test_threaded_parse_equivalence.cpp
                tests/features/threaded_response_ordering/test_threaded_response_ordering.cpp
                tests/features/ris_preserves_callbacks/test_ris_preserves_callbacks.cpp
                tests/features/bce_scroll_erase/test_bce_scroll_erase.cpp
                tests/features/wide_char_overwrite_mate/test_wide_char_overwrite.cpp
                tests/features/osc8_apc_memory_caps/test_osc8_apc_memory_caps.cpp
                tests/features/themedstylesheet_extraction/test_themedstylesheet_extraction.cpp
                tests/features/origin_mode_correctness/test_origin_mode.cpp
                tests/features/osc_color_query/test_osc_color_query.cpp
                tests/features/decrqss/test_decrqss.cpp)
    # scrollback_frozen_view test reads terminalwidget.cpp source via grep —
    # the path must be passed as a compile definition same as the standalone
    # had it. Apply target-wide so any future bundle source can reuse.
    target_compile_definitions(test_vt PRIVATE
        SRC_TERMINALWIDGET_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_TERMINALGRID_PATH="${CMAKE_SOURCE_DIR}/src/terminalgrid.cpp"
        SRC_TERMINALGRID_CPP_PATH="${CMAKE_SOURCE_DIR}/src/terminalgrid.cpp"
        SRC_VTPARSER_PATH="${CMAKE_SOURCE_DIR}/src/vtparser.cpp"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_H_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.h"
        SRC_THEMEDSTYLESHEET_CPP_PATH="${CMAKE_SOURCE_DIR}/src/themedstylesheet.cpp"
        SRC_THEMEDSTYLESHEET_H_PATH="${CMAKE_SOURCE_DIR}/src/themedstylesheet.h")

    # ----------------------------------------------------------------------
    # ANTS-1217 Phase 3 — test_chrome bundle.
    # Subject: mainwindow + titlebar + coloredtabbar + commandpalette and
    # everything bolted onto them (status bar, menu, focus-management,
    # quake mode, portal integration, paste pipeline, review-changes UI).
    # ----------------------------------------------------------------------
    # mainwindow.cpp cross-references ClaudeIntegration / SshBookmark /
    # ClaudeStatusBarController / ClaudeProjectsDialog — symbols that live
    # in ants_claude_lib and ants_dialogs_lib. Linking those libs here
    # makes the bundle's mainwindow.cpp resolution match ants-terminal's.
    # ants_lua_lib also needed because mainwindow.cpp gates plugin glue
    # behind ANTS_LUA_PLUGINS but a few unconditional references slip
    # through (PluginManager construction); pulling the lib is harmless
    # since it's STATIC and only adds the symbols actually referenced.
    ants_add_gui_bundle(test_chrome
        LIBS    ants_chrome_lib ants_vt_lib ants_core_lib
                ants_claude_lib ants_dialogs_lib ants_audit_lib
                $<$<BOOL:${LUA_FOUND}>:ants_lua_lib>
        SOURCES tests/features/tab_rename_persist/test_tab_rename_persist.cpp
                tests/features/status_bar_elision/test_status_bar_elision.cpp
                tests/features/review_changes_click/test_review_changes_click.cpp
                tests/features/tab_color/test_tab_color.cpp
                tests/features/shell_command_wiring/test_shell_command_wiring.cpp
                tests/features/wayland_quake_mode/test_wayland_quake_mode.cpp
                tests/features/global_shortcuts_portal/test_global_shortcuts_portal.cpp
                tests/features/focus_redirect_menu_guard/test_focus_redirect_menu_guard.cpp
                tests/features/menubar_hover_stylesheet/test_menubar_hover_stylesheet.cpp
                tests/features/tabbar_statusbar_opaque/test_tabbar_statusbar_opaque.cpp
                tests/features/tab_close_button_visible/test_tab_close_button_visible.cpp
                tests/features/review_changes_branches/test_review_changes_branches.cpp
                tests/features/review_changes_clickable/test_review_changes_clickable.cpp
                tests/features/review_changes_scroll_preserve/test_review_changes_scroll_preserve.cpp
                tests/features/github_status_bar/test_github_status_bar.cpp
                tests/features/roadmap_status_bar_refresh/test_roadmap_status_bar_refresh.cpp
                tests/features/ui_state_persistence/test_ui_state_persistence.cpp
                tests/features/crash_safe_session_persist/test_crash_safe_session_persist.cpp
                tests/features/dialog_close_focus_return/test_dialog_close_focus_return.cpp
                tests/features/dialog_pseudo_modal/test_dialog_pseudo_modal.cpp
                tests/features/clipboard_redaction/test_clipboard_redaction.cpp
                tests/features/update_available_menubar/test_update_available_menubar.cpp
                tests/features/status_bar_branch_chip/test_status_bar_branch_chip.cpp
                tests/features/a11y_chrome_names/test_a11y_chrome_names.cpp
                tests/features/command_palette_ghost_completion/test_command_palette_ghost_completion.cpp
                tests/features/no_anim_style/test_no_anim_style.cpp
                tests/features/paste_dialog_custom/test_paste_dialog_custom.cpp
                tests/features/tab_rename_pin/test_tab_rename_pin.cpp
                tests/features/confirm_close_with_processes/test_confirm_close_with_processes.cpp
                tests/features/help_about_menu/test_help_about_menu.cpp
                tests/features/portal_session_close/test_portal_session_close.cpp
                tests/features/styled_font_kerning_off/test_styled_font_kerning_off.cpp
                tests/features/sigpipe_ignore/test_sigpipe_ignore.cpp
                tests/features/pty_dtor_off_main_thread/test_pty_dtor_off_main_thread.cpp
                tests/features/terminal_partial_update_mode/test_terminal_partial_update_mode.cpp
                tests/features/command_mark_gutter/test_command_mark_gutter.cpp
                tests/features/osc133_last_command/test_osc133_last_command.cpp
                tests/features/scroll_snapshot_intent/test_scroll_snapshot_intent.cpp
                tests/features/sync_output_snapshot/test_sync_output_snapshot.cpp
                tests/features/vtbatch_zero_copy/test_vtbatch_zero_copy.cpp
                tests/features/shift_enter_bracketed_paste/test_shift_enter.cpp
                tests/features/kwin_position_tracker/test_kwin_position_tracker.cpp)
    # command_palette_ghost_completion uses QSignalSpy (Qt6::Test).
    target_link_libraries(test_chrome PRIVATE Qt6::Test)
    target_compile_definitions(test_chrome PRIVATE
        SRC_MAIN_PATH="${CMAKE_SOURCE_DIR}/src/main.cpp"
        SRC_MAINWINDOW_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_CPP="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_H_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.h"
        MAINWINDOW_CPP="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        MAINWINDOW_H="${CMAKE_SOURCE_DIR}/src/mainwindow.h"
        SRC_TERMINALWIDGET_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_TERMINALWIDGET_CPP_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_TERMINALWIDGET_H_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.h"
        SRC_TERMINALWIDGET_HEADER_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.h"
        SRC_TERMINALWIDGET_IMPL_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp"
        TERMINALWIDGET_CPP="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp"
        TERMINALWIDGET_H="${CMAKE_SOURCE_DIR}/src/terminalwidget.h"
        SRC_SESSIONMANAGER_H_PATH="${CMAKE_SOURCE_DIR}/src/sessionmanager.h"
        SRC_PORTAL_HEADER_PATH="${CMAKE_SOURCE_DIR}/src/globalshortcutsportal.h"
        SRC_PORTAL_IMPL_PATH="${CMAKE_SOURCE_DIR}/src/globalshortcutsportal.cpp"
        SRC_PORTAL_H_PATH="${CMAKE_SOURCE_DIR}/src/globalshortcutsportal.h"
        SRC_PORTAL_CPP_PATH="${CMAKE_SOURCE_DIR}/src/globalshortcutsportal.cpp"
        SRC_CONFIG_HEADER_PATH="${CMAKE_SOURCE_DIR}/src/config.h"
        SRC_CONFIG_IMPL_PATH="${CMAKE_SOURCE_DIR}/src/config.cpp"
        SRC_CONFIG_H_PATH="${CMAKE_SOURCE_DIR}/src/config.h"
        SRC_CONFIG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/config.cpp"
        SRC_SETTINGSDIALOG_HEADER_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.h"
        SRC_SETTINGSDIALOG_IMPL_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SETTINGSDIALOG_H_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.h"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_OPAQUEMENUBAR_PATH="${CMAKE_SOURCE_DIR}/src/opaquemenubar.h"
        SRC_OPAQUESTATUSBAR_PATH="${CMAKE_SOURCE_DIR}/src/opaquestatusbar.h"
        SRC_THEMEDSTYLESHEET_PATH="${CMAKE_SOURCE_DIR}/src/themedstylesheet.cpp"
        SRC_THEMEDSTYLESHEET_CPP_PATH="${CMAKE_SOURCE_DIR}/src/themedstylesheet.cpp"
        SRC_COLOREDTABBAR_CPP_PATH="${CMAKE_SOURCE_DIR}/src/coloredtabbar.cpp"
        SRC_COLOREDTABBAR_H_PATH="${CMAKE_SOURCE_DIR}/src/coloredtabbar.h"
        TITLEBAR_CPP_PATH="${CMAKE_SOURCE_DIR}/src/titlebar.cpp"
        COMMANDPALETTE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/commandpalette.cpp"
        DIALOGFOCUS_H="${CMAKE_SOURCE_DIR}/src/dialogfocus.h"
        CLIPBOARDGUARD_H="${CMAKE_SOURCE_DIR}/src/clipboardguard.h"
        KWINPOS_CPP="${CMAKE_SOURCE_DIR}/src/kwinpositiontracker.cpp"
        KWINPOS_H="${CMAKE_SOURCE_DIR}/src/kwinpositiontracker.h"
        SRC_PTYHANDLER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/ptyhandler.cpp"
        SRC_DIFFVIEWER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/diffviewer.cpp"
        SRC_ABOUTDIALOGS_PATH="${CMAKE_SOURCE_DIR}/src/aboutdialogs.cpp"
        SRC_RELEASE_WORKFLOW_PATH="${CMAKE_SOURCE_DIR}/.github/workflows/release.yml"
        DOCS_STATUS_BAR_STANDARD_PATH="${CMAKE_SOURCE_DIR}/docs/standards/status-bar.md"
        CMAKELISTS_TXT="${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        SRC_CMAKELISTS_PATH="${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        SRC_DIR="${CMAKE_SOURCE_DIR}/src"
        VTSTREAM_H="${CMAKE_SOURCE_DIR}/src/vtstream.h"
        VTSTREAM_CPP="${CMAKE_SOURCE_DIR}/src/vtstream.cpp")

    # ANTS-1217 Phase 4: Claude integration + allowlist + AI + SSH +
    # shell-utils tests. Bundle pulls in claude_lib (its primary subject)
    # plus chrome+dialogs+vt+core because (a) several tests grep
    # mainwindow.cpp, (b) sshdialog/aidialog live in dialogs_lib,
    # (c) chrome_lib requires its claude/dialogs/audit cross-refs to
    # link, just like test_chrome.
    ants_add_gui_bundle(test_claude
        LIBS    ants_chrome_lib ants_vt_lib ants_core_lib
                ants_claude_lib ants_dialogs_lib ants_audit_lib
                $<$<BOOL:${LUA_FOUND}>:ants_lua_lib>
        SOURCES tests/features/claude_status_bar/test_claude_status.cpp
                tests/features/claude_status_bar_per_tab/test_claude_status_bar_per_tab.cpp
                tests/features/claude_plan_mode_detection/test_claude_plan_mode.cpp
                tests/features/claude_tab_status_indicator/test_claude_tab_tracker.cpp
                tests/features/claude_transcript_robustness/test_claude_transcript_robustness.cpp
                tests/features/allowlist_add/test_allowlist_add.cpp
                tests/features/allowlist_perms_postcommit/test_allowlist_perms_postcommit.cpp
                tests/features/ssh_dash_host_rejected/test_ssh_dash.cpp
                tests/features/ssh_extra_args_sanitize/test_ssh_extra_args.cpp
                tests/features/shellutils_whitelist/test_shellutils_whitelist.cpp
                tests/features/ai_insert_command_sanitize/test_ai_insert_command.cpp
                tests/features/claude_git_context_hook/test_installer_grep.cpp
                tests/features/ai_context_redaction/test_ai_context_redaction.cpp
                tests/features/ssh_control_master/test_ssh_control_master.cpp
                tests/features/claude_bg_tasks_button/test_claude_bg_tasks_button.cpp
                tests/features/claude_state_dot_palette/test_claude_state_dot_palette.cpp
                tests/features/claude_statusbar_extraction/test_claude_statusbar_extraction.cpp
                tests/features/claude_task_list/test_claude_task_list.cpp
                tests/features/claude_session_freshness/test_claude_session_freshness.cpp
                tests/features/claude_task_list_session_isolation/test_claude_task_list_session_isolation.cpp
                tests/features/claude_pid_replacement/test_claude_pid_replacement.cpp
                tests/features/mcp_extra_tools/test_mcp_extra_tools.cpp
                tests/features/tasks_chip_done_over_total/test_tasks_chip_done_over_total.cpp
                tests/features/mcp_roadmap_status_filter/test_mcp_roadmap_status_filter.cpp
                tests/features/mcp_tools_list_schema/test_mcp_tools_list_schema.cpp
                tests/features/mcp_workspace_search/test_mcp_workspace_search.cpp
                tests/features/mcp_file_outline/test_mcp_file_outline.cpp
                tests/features/mcp_git_state/test_mcp_git_state.cpp
                tests/features/mcp_subsystem/test_mcp_subsystem.cpp
                tests/features/mcp_provider_registry/test_mcp_provider_registry.cpp
                tests/features/mcp_last_audit_summary/test_mcp_last_audit_summary.cpp
                tests/features/mcp_indie_review_tools/test_mcp_indie_review_tools.cpp
                tests/features/mcp_debt_sweep_tools/test_mcp_debt_sweep_tools.cpp)
    # claude_status_bar et al. use QSignalSpy (Qt6::Test); also pulls
    # Qt6::Network for ClaudeIntegration's QLocalSocket/QLocalServer
    # MCP-server hooks.
    target_link_libraries(test_claude PRIVATE Qt6::Test Qt6::Network)
    target_compile_definitions(test_claude PRIVATE
        ANTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        SRC_MAINWINDOW_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_H_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.h"
        MAINWINDOW_CPP="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_CLAUDESTATUSWIDGETS_PATH="${CMAKE_SOURCE_DIR}/src/claudestatuswidgets.cpp"
        SRC_CLAUDESTATUSWIDGETS_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudestatuswidgets.cpp"
        SRC_CLAUDESTATUSWIDGETS_H_PATH="${CMAKE_SOURCE_DIR}/src/claudestatuswidgets.h"
        CLAUDESTATUSWIDGETS_CPP="${CMAKE_SOURCE_DIR}/src/claudestatuswidgets.cpp"
        SRC_CLAUDEALLOWLIST_PATH="${CMAKE_SOURCE_DIR}/src/claudeallowlist.cpp"
        SRC_CLAUDE_INTEGRATION_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeintegration.cpp"
        SRC_CLAUDE_INTEGRATION_H_PATH="${CMAKE_SOURCE_DIR}/src/claudeintegration.h"
        CLAUDETABTRACKER_H="${CMAKE_SOURCE_DIR}/src/claudetabtracker.h"
        COLOREDTABBAR_H="${CMAKE_SOURCE_DIR}/src/coloredtabbar.h"
        COLOREDTABBAR_CPP="${CMAKE_SOURCE_DIR}/src/coloredtabbar.cpp"
        SRC_AI_CPP="${CMAKE_SOURCE_DIR}/src/aidialog.cpp"
        SRC_AIDIALOG_CPP="${CMAKE_SOURCE_DIR}/src/aidialog.cpp"
        SRC_SETTINGSDIALOG_CPP="${CMAKE_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SETTINGSDIALOG_H="${CMAKE_SOURCE_DIR}/src/settingsdialog.h"
        SRC_CONFIGPATHS_H="${CMAKE_SOURCE_DIR}/src/configpaths.h"
        SRC_BGTASKS_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudebgtasks.cpp"
        SRC_BGTASKS_H_PATH="${CMAKE_SOURCE_DIR}/src/claudebgtasks.h"
        SRC_BGDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudebgtasksdialog.cpp"
        SRC_BGDIALOG_H_PATH="${CMAKE_SOURCE_DIR}/src/claudebgtasksdialog.h"
        SRC_RC_HEADER="${CMAKE_SOURCE_DIR}/src/remotecontrol.h"
        SRC_REMOTECONTROL_CPP_PATH="${CMAKE_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_GITWRAP_CPP_PATH="${CMAKE_SOURCE_DIR}/src/gitwrap.cpp"
        SRC_GITWRAP_H_PATH="${CMAKE_SOURCE_DIR}/src/gitwrap.h"
        SRC_SUBSYSTEMMAP_CPP_PATH="${CMAKE_SOURCE_DIR}/src/subsystemmap.cpp"
        SRC_SUBSYSTEMMAP_H_PATH="${CMAKE_SOURCE_DIR}/src/subsystemmap.h"
        SRC_AUDIT_ENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.h"
        SRC_AUDIT_ENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.cpp"
        SRC_REMOTECONTROL_H_PATH="${CMAKE_SOURCE_DIR}/src/remotecontrol.h"
        FIXTURE_MIN_SARIF="${CMAKE_SOURCE_DIR}/tests/features/mcp_last_audit_summary/fixture_min.sarif"
        FIXTURE_EMPTY_SARIF="${CMAKE_SOURCE_DIR}/tests/features/mcp_last_audit_summary/fixture_empty.sarif"
        ANTS_CLAUDE_MD_PATH="${CMAKE_SOURCE_DIR}/CLAUDE.md"
        CMAKELISTS_PATH="${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        SRC_CMAKELISTS_PATH="${CMAKE_SOURCE_DIR}/CMakeLists.txt")

    # ANTS-1217 Phase 5 — test_audit bundle. Audit tests consolidated
    # behind one target. Most are pure source-grep (slurp + regex on
    # auditdialog.cpp / auditengine.cpp) and need only Qt6::Core; a few
    # exercise live AuditEngine / AuditHygiene / RuleQualityTracker /
    # FeatureCoverage / Config APIs and pull in those translation units
    # via ants_audit_lib + ants_core_lib. ants_audit_lib transitively
    # brings auditdialog.cpp (a QDialog), so the GUI bundle helper
    # applies. Phase 6 added feature_coverage (its impl lives in
    # ants_audit_lib so it logically belongs here).
    ants_add_gui_bundle(test_audit
        LIBS    ants_audit_lib ants_core_lib
        SOURCES tests/features/audit_command_rule_trust/test_audit_command_rule_trust.cpp
                tests/features/audit_dedup_96bit/test_audit_dedup_96bit.cpp
                tests/features/audit_engine_extraction/test_audit_engine_extraction.cpp
                tests/features/audit_hygiene/test_audit_hygiene.cpp
                tests/features/audit_incremental_output_drain/test_audit_incremental_output_drain.cpp
                tests/features/audit_path_traversal/test_audit_path_traversal.cpp
                tests/features/audit_per_tool_timeout/test_audit_per_tool_timeout.cpp
                tests/features/audit_regex_dos_watchdog/test_audit_regex_dos_watchdog.cpp
                tests/features/audit_rule_quality/test_audit_rule_quality.cpp
                tests/features/audit_sarif_suppressions/test_audit_sarif_suppressions.cpp
                tests/features/audit_tool_crash_distinct/test_audit_tool_crash_distinct.cpp
                tests/features/feature_coverage/test_feature_coverage.cpp
                tests/features/roadmap_fold_in/test_roadmap_fold_in.cpp
                tests/features/audit_corroboration_shift/test_audit_corroboration_shift.cpp
                tests/features/audit_drop_alias/test_audit_drop_alias.cpp
                tests/features/audit_framework_detect/test_audit_framework_detect.cpp
                tests/features/indie_review_engine/test_indie_review_engine.cpp
                tests/features/debt_sweep_engine/test_debt_sweep_engine.cpp)
    target_compile_definitions(test_audit PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_ENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_ENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.h"
        SRC_AUDIT_CPP="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H="${CMAKE_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_ENGINE_CPP="${CMAKE_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_ENGINE_H="${CMAKE_SOURCE_DIR}/src/auditengine.h"
        SRC_AUDITDIALOG_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp")

    # ANTS-1217 Phase 6 — test_dialogs bundle. Subject: settingsdialog +
    # roadmapdialog + diffviewer (the dialogs hosted by chrome). 11 tests
    # spanning settings_*, roadmap_*, diffviewer_extraction, and
    # remote_control_roadmap_query (which drives RoadmapDialog::parseBullets).
    # Lib set mirrors test_chrome / test_claude (mainwindow.cpp cross-refs
    # require the full chrome+claude+dialogs+audit set, --start-group
    # resolves the cycle via the existing helper).
    ants_add_gui_bundle(test_dialogs
        LIBS    ants_chrome_lib ants_vt_lib ants_core_lib
                ants_claude_lib ants_dialogs_lib ants_audit_lib
                $<$<BOOL:${LUA_FOUND}>:ants_lua_lib>
        SOURCES tests/features/settings_dependency_gating/test_settings_dependency_gating.cpp
                tests/features/settings_profile_cancel_rollback/test_settings_profile_cancel_rollback.cpp
                tests/features/settings_restore_defaults/test_settings_restore_defaults.cpp
                tests/features/settings_parse_failure_mirror/test_settings_parse_failure_mirror.cpp
                tests/features/settings_dialog_config_reload/test_settings_dialog_config_reload.cpp
                tests/features/roadmap_viewer/test_roadmap_viewer.cpp
                tests/features/roadmap_viewer_tabs/test_roadmap_viewer_tabs.cpp
                tests/features/roadmap_viewer_archive/test_roadmap_viewer_archive.cpp
                tests/features/roadmap_kind_facets/test_roadmap_kind_facets.cpp
                tests/features/roadmap_dialog_cards/test_roadmap_dialog_cards.cpp
                tests/features/roadmap_shortcuts_cheatsheet/test_roadmap_shortcuts_cheatsheet.cpp
                tests/features/roadmap_search_keybinds/test_roadmap_search_keybinds.cpp
                tests/features/roadmap_inprogress_age/test_roadmap_inprogress_age.cpp
                tests/features/roadmap_density/test_roadmap_density.cpp
                tests/features/diffviewer_extraction/test_diffviewer_extraction.cpp
                tests/features/remote_control_roadmap_query/test_remote_control_roadmap_query.cpp)
    target_compile_definitions(test_dialogs PRIVATE
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SETTINGSDIALOG_H_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.h"
        SRC_CLAUDEALLOWLIST_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeallowlist.cpp"
        SRC_MAINWINDOW_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_DIFFVIEWER_H_PATH="${CMAKE_SOURCE_DIR}/src/diffviewer.h"
        SRC_DIFFVIEWER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/diffviewer.cpp"
        ROADMAPDIALOG_H="${CMAKE_SOURCE_DIR}/src/roadmapdialog.h"
        ROADMAPDIALOG_CPP="${CMAKE_SOURCE_DIR}/src/roadmapdialog.cpp"
        ROADMAP_FORMAT_MD="${CMAKE_SOURCE_DIR}/docs/standards/roadmap-format.md"
        ROTATE_ROADMAP_SH="${CMAKE_SOURCE_DIR}/packaging/rotate-roadmap.sh"
        SRC_DIR="${CMAKE_SOURCE_DIR}/src"
        MAINWINDOW_H="${CMAKE_SOURCE_DIR}/src/mainwindow.h"
        MAINWINDOW_CPP="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_RC_HEADER="${CMAKE_SOURCE_DIR}/src/remotecontrol.h"
        SRC_RC_CPP="${CMAKE_SOURCE_DIR}/src/remotecontrol.cpp"
        ROADMAPSHORTCUTSDIALOG_CPP="${CMAKE_SOURCE_DIR}/src/roadmapshortcutsdialog.cpp"
        ROADMAPSHORTCUTSDIALOG_H="${CMAKE_SOURCE_DIR}/src/roadmapshortcutsdialog.h")
    # ANTS-1236 — Qt6::Test for QTest::keyClick used by
    # roadmap_shortcuts_cheatsheet. Mirrors the test_chrome / test_claude
    # bundles that also pull Qt6::Test in.
    target_link_libraries(test_dialogs PRIVATE Qt6::Test)

    # ANTS-1217 Phase 6 — test_lua bundle (gated on LUA_FOUND). Subject:
    # luaengine + pluginmanager. lua_sandbox_hardening drives the live
    # LuaEngine API; plugin_manifest_safety is a source-grep test on
    # pluginmanager.cpp. Bundle links ants_lua_lib + ants_core_lib per
    # the Phase 7 architecture.
    if(LUA_FOUND)
        ants_add_gui_bundle(test_lua
            LIBS    ants_lua_lib ants_core_lib
            SOURCES tests/features/lua_sandbox_hardening/test_lua_sandbox_hardening.cpp
                    tests/features/plugin_manifest_safety/test_plugin_manifest_safety.cpp)
        target_compile_definitions(test_lua PRIVATE
            ANTS_LUA_PLUGINS=1
            SRC_LUAENGINE_PATH="${CMAKE_SOURCE_DIR}/src/luaengine.cpp"
            SRC_PLUGINMANAGER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/pluginmanager.cpp")
    endif()

    # ANTS-1217 Phase 6 — test_core bundle. Subject: core infrastructure —
    # PTY, sessionmanager, config, debuglog, remote-control IPC verbs,
    # threading invariants, packaging manifests, helper-CLI. 28 tests, all
    # Qt::Core-only at runtime (the lib's transitive Widgets dep is unused).
    # ants_core_lib supplies Config / DebugLog / SessionManager / RemoteControl
    # symbols for behavioural tests; source-grep tests get their paths via
    # the target_compile_definitions block below. local_subagent_framework
    # links antshelper.cpp directly (gated source) when the helper-CLI is
    # disabled, otherwise via ants_helper_lib so the .cpp compiles once.
    ants_add_core_bundle(test_core
        LIBS    ants_core_lib
                $<$<BOOL:${ANTS_ENABLE_HELPER_CLI}>:ants_helper_lib>
        SOURCES tests/features/session_persistence_default/test_session_persistence_default.cpp
                tests/features/session_sha256_checksum/test_session_sha256_checksum.cpp
                tests/features/session_qcompress_length_guard/test_session_qcompress_length_guard.cpp
                tests/features/session_cell_loop_stream_status/test_session_cell_loop_stream_status.cpp
                tests/features/pty_closefrom/test_pty_closefrom.cpp
                tests/features/pty_write_eagain_queue/test_pty_write_eagain_queue.cpp
                tests/features/image_bomb_png_header_peek/test_image_bomb.cpp
                tests/features/threaded_ptywrite_gating/test_threaded_ptywrite_gating.cpp
                tests/features/threaded_resize_synchronous/test_threaded_resize_synchronous.cpp
                tests/features/remote_control_tab_list/test_remote_control_tab_list.cpp
                tests/features/remote_control_ls/test_remote_control_ls.cpp
                tests/features/remote_control_send_text/test_remote_control_send_text.cpp
                tests/features/remote_control_opt_in/test_remote_control_opt_in.cpp
                tests/features/remote_control_new_tab/test_remote_control_new_tab.cpp
                tests/features/remote_control_select_window/test_remote_control_select_window.cpp
                tests/features/remote_control_set_title/test_remote_control_set_title.cpp
                tests/features/remote_control_get_text/test_remote_control_get_text.cpp
                tests/features/remote_control_launch/test_remote_control_launch.cpp
                tests/features/local_subagent_framework/test_local_subagent_framework.cpp
                tests/features/flatpak_host_shell/test_flatpak_host_shell.cpp
                tests/features/flatpak_lua_module/test_flatpak_lua_module.cpp
                tests/features/flathub_manifest_transform/test_flathub_manifest_transform.cpp
                tests/features/config_parse_failure_guard/test_config_parse_failure.cpp
                tests/features/config_reload_loop_safety/test_config_reload_loop_safety.cpp
                tests/features/debuglog_perms/test_debuglog_perms.cpp
                tests/features/persistence_post_rename_chmod/test_persistence_post_rename_chmod.cpp
                tests/features/concurrent_writer_lock/test_concurrent_writer_lock.cpp
                tests/features/secureio_configbackup_split/test_secureio_configbackup_split.cpp
                $<$<NOT:$<BOOL:${ANTS_ENABLE_HELPER_CLI}>>:src/antshelper.cpp>)
    target_compile_definitions(test_core PRIVATE
        SRC_TERMINALGRID_PATH="${CMAKE_SOURCE_DIR}/src/terminalgrid.cpp"
        SRC_TERMINALGRID_HEADER="${CMAKE_SOURCE_DIR}/src/terminalgrid.h"
        SRC_TERMINALWIDGET_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_VTSTREAM_HEADER_PATH="${CMAKE_SOURCE_DIR}/src/vtstream.h"
        SRC_PTY_PATH="${CMAKE_SOURCE_DIR}/src/ptyhandler.cpp"
        SRC_PTY_CPP_PATH="${CMAKE_SOURCE_DIR}/src/ptyhandler.cpp"
        SRC_PTY_H_PATH="${CMAKE_SOURCE_DIR}/src/ptyhandler.h"
        SRC_PTYHANDLER_PATH="${CMAKE_SOURCE_DIR}/src/ptyhandler.cpp"
        SRC_SESSION_CPP_PATH="${CMAKE_SOURCE_DIR}/src/sessionmanager.cpp"
        SRC_SESSION_H_PATH="${CMAKE_SOURCE_DIR}/src/sessionmanager.h"
        SRC_SESSIONMANAGER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/sessionmanager.cpp"
        SRC_CONFIG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/config.cpp"
        SRC_CLAUDEALLOWLIST_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeallowlist.cpp"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SECUREIO_H_PATH="${CMAKE_SOURCE_DIR}/src/secureio.h"
        SRC_CONFIGBACKUP_H_PATH="${CMAKE_SOURCE_DIR}/src/configbackup.h"
        SRC_DEBUGLOG_PATH="${CMAKE_SOURCE_DIR}/src/debuglog.cpp"
        SRC_RC_HEADER="${CMAKE_SOURCE_DIR}/src/remotecontrol.h"
        SRC_RC_CPP="${CMAKE_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_H="${CMAKE_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAINWINDOW_CPP="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAIN_CPP="${CMAKE_SOURCE_DIR}/src/main.cpp"
        SRC_CMAKELISTS="${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        FLATPAK_MANIFEST_PATH="${CMAKE_SOURCE_DIR}/packaging/flatpak/org.ants.Terminal.yml"
        TRANSFORMER_PATH="${CMAKE_SOURCE_DIR}/packaging/flatpak/make-flathub-manifest.sh")
endif()
