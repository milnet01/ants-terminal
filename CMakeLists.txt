cmake_minimum_required(VERSION 3.20)
project(ants-terminal VERSION 0.7.9 LANGUAGES CXX)

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
    src/glrenderer.cpp
    src/sessionmanager.cpp
    src/aidialog.cpp
    src/sshdialog.cpp
    src/settingsdialog.cpp
    src/toggleswitch.cpp
    src/auditdialog.cpp
    src/audithygiene.cpp
    src/featurecoverage.cpp
    src/auditrulequality.cpp
    src/xcbpositiontracker.cpp
    src/claudeallowlist.cpp
    src/claudeintegration.cpp
    src/claudeprojects.cpp
    src/claudetranscript.cpp
    src/globalshortcutsportal.cpp
    src/debuglog.cpp
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

    # menubar_hover_stylesheet — 0.7.4 follow-up to 0.6.43. Pins that the
    # QMenuBar has an opaque background under a translucent-parent window
    # and that every hover pseudo-state is styled, so the hover flash
    # cannot re-emerge from QSS drift. See
    # tests/features/menubar_hover_stylesheet/spec.md.
    add_executable(test_menubar_hover_stylesheet
        tests/features/menubar_hover_stylesheet/test_menubar_hover_stylesheet.cpp
    )
    target_compile_options(test_menubar_hover_stylesheet PRIVATE
        -Wall -Wextra -Wpedantic
    )
    target_compile_definitions(test_menubar_hover_stylesheet PRIVATE
        SRC_MAINWINDOW_PATH="${CMAKE_CURRENT_SOURCE_DIR}/src/mainwindow.cpp"
    )
    add_test(NAME menubar_hover_stylesheet
             COMMAND test_menubar_hover_stylesheet)
    set_tests_properties(menubar_hover_stylesheet PROPERTIES
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
