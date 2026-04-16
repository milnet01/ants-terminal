cmake_minimum_required(VERSION 3.20)
project(ants-terminal VERSION 0.6.30 LANGUAGES CXX)

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

add_executable(ants-terminal
    src/main.cpp
    src/mainwindow.cpp
    src/titlebar.cpp
    src/terminalwidget.cpp
    src/terminalgrid.cpp
    src/vtparser.cpp
    src/ptyhandler.cpp
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
    src/xcbpositiontracker.cpp
    src/claudeallowlist.cpp
    src/claudeintegration.cpp
    src/claudeprojects.cpp
    src/claudetranscript.cpp
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
endif()
