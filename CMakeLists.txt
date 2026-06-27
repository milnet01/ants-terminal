cmake_minimum_required(VERSION 3.20)
project(ants-terminal VERSION 0.7.98 LANGUAGES CXX)

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
# Build metadata for Help → About (ANTS-1222 + ANTS-1394 build-time refresh)
# ---------------------------------------------------------------------------
# Build-time bake: date, time, build type, short git SHA. Surfaced
# via the generated `build_info.h` (see cmake/build_info.h.in). Only
# `aboutdialogs.cpp` + `mainwindow.cpp` include this header, so a
# SHA change recompiles two TUs rather than every TU (vs
# add_compile_definitions which would invalidate ccache for the
# whole project on every commit). The compiler ID/version is
# detected at COMPILE time inside aboutdialogs.cpp via __GNUC__ /
# __clang_major__, so no CMake plumbing for that.
#
# ANTS-1394: a custom target ALL invokes cmake/GenerateBuildInfo.cmake
# on every build (not every configure), so incremental rebuilds
# advance the timestamp. configure_file's copy_if_different keeps
# ccache warm when an intra-minute rebuild produces the same content.
# Default build type if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ANTS-1394: build_info.h refresh on every build, not every configure.
# add_custom_target ALL always re-runs the script; configure_file
# inside the script uses copy_if_different so ccache stays warm
# on intra-minute rebuilds (the file's mtime only advances when
# the embedded HH:MM minute changed OR git SHA / build type changed).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
add_custom_target(ants_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DOUTPUT_FILE=${CMAKE_BINARY_DIR}/generated/build_info.h
        -DTEMPLATE_FILE=${CMAKE_SOURCE_DIR}/cmake/build_info.h.in
        -DANTS_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -P ${CMAKE_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/build_info.h
    COMMENT "Refreshing build_info.h timestamp (ANTS-1394)"
    VERBATIM)

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
# ANTS-1550 — ANTS_CCACHE gates the existing auto-detect. Default ON
# preserves prior behaviour (ccache used when installed); -DANTS_CCACHE=OFF
# disables for users who don't want their PATH ccache picked up implicitly.
option(ANTS_CCACHE "Wire ccache as the C/C++ compiler launcher when installed" ON)
if(ANTS_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
        set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
        message(STATUS "ccache found at ${CCACHE_PROGRAM} — using as compiler launcher")
    endif()
endif()

# ANTS-1550 — opt-in Unity builds. Folds groups of .cpp into single TUs per
# CMake target, dropping cc1plus instance count (each holds ~600 MiB of
# Qt headers resident). Default OFF: unity builds occasionally surface
# latent ODR / anonymous-namespace collisions, and per-file compile
# diagnostics get harder to read. Pair with the JOB_POOLS cap below so
# the combined peak-RAM ceiling drops further.
option(ANTS_UNITY_BUILD "Enable CMake UNITY_BUILD on STATIC libs (opt-in; may surface ODR drift)" OFF)
if(ANTS_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
    message(STATUS "Unity builds enabled (ANTS_UNITY_BUILD=ON)")
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
# ANTS-1558 — link_pool cap is overridable via ANTS_LINK_POOL (default 1).
# The `fast` preset bumps it to 2 so the ~30 test-bundle link tail runs in
# pairs on 32 GiB hosts where two concurrent Qt links (~1.5–2 GiB peak RSS
# each) stay well clear of the OOM ceiling. Other presets keep the safer
# link_pool=1 (each Qt link is the single heaviest process in the build).
set(ANTS_LINK_POOL 1 CACHE STRING "Concurrent link invocations cap (Ninja JOB_POOLS)")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS
    compile_pool=${_ANTS_COMPILE_CAP}
    link_pool=${ANTS_LINK_POOL})
set(CMAKE_JOB_POOL_COMPILE compile_pool)
set(CMAKE_JOB_POOL_LINK    link_pool)
message(STATUS "Build job pools: compile=${_ANTS_COMPILE_CAP} link=${ANTS_LINK_POOL} (host nproc=${_ANTS_NPROC})")

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
#   ants_audit_lib   ← audit engine + runner + hygiene (depends on core)
#   ants_audit_dialog_lib ← AuditDialog QDialog (depends on audit + chrome + dialogs)
#   ants_dialogs_lib ← settings/about/diff/ssh/etc. (depends on chrome)
#   ants_lua_lib     ← only if LUA_FOUND
#   ants_helper_lib  ← only if ANTS_ENABLE_HELPER_CLI
add_library(ants_core_lib STATIC
    src/config.cpp
    src/debuglog.cpp
    src/clipboardguard.cpp
    src/pathvalidation.cpp
    src/remotecontrol.cpp
    src/remotecontrolgate.cpp
    src/fileoutline.cpp
    # ANTS-1993 — Claude permission-prompt structural gate (Qt6::Core-only).
    src/claudepromptdetect.cpp
    # ANTS-1855 — read_log pure filtering helper (Qt6::Core-only).
    src/readlog.cpp
    # ANTS-2021 — read_region pure slicing helper (Qt6::Core-only).
    src/readregion.cpp
    # ANTS-2022 — apply_edits pure in-memory edit helper (Qt6::Core-only).
    src/applyedits.cpp
    # ANTS-1637 — codebase_index structural-map builder (Qt6::Core-only).
    src/codebaseindex.cpp
    # ANTS-2139 — docs_index documentation-map builder (Qt6::Core-only).
    src/docsindex.cpp
    # ANTS-2160 — per-project .ants/project.json settings loader (Qt6::Core-only).
    src/projectsettings.cpp
    # ANTS-1961 / ANTS-1962 — feedback-file parse + render helpers.
    src/feedbackfile.cpp
    # ANTS-1963 — spec_log pure markdown transforms (Qt6::Core-only).
    src/speclog.cpp
    # ANTS-1735 — model-switch effectiveness ledger. In core (not claude) so the
    # read-only model_switch_stats MCP verb (mainwindow dispatch) can reach it.
    src/modelswitchledger.cpp
    # ANTS-1894 — near-miss telemetry sibling ledger; same lib for the same
    # reason (model_switch_stats consumes both).
    src/modelnearmissledger.cpp
    # ANTS-1303 — find_definition / find_caller tree-wide symbol scanner.
    src/symbolquery.cpp
    # ANTS-1305 — similar_code shape matcher (reuses FileOutline extractor).
    src/similarcode.cpp
    # ANTS-1301 — recent_errors scrollback error parser.
    src/scrollbackerrors.cpp
    # ANTS-1636 — find_sources topic-to-files discovery.
    src/findsources.cpp
    src/gitwrap.cpp
    src/indiereviewengine.cpp
    src/debtsweepengine.cpp
    # ANTS-1457 — false-positive ledger helper shared across
    # the three AI-reviewer brief-assembly paths.
    src/falseposledger.cpp
    # ANTS-1708 — fingerprint-keyed learned-FP ledger for static audit.
    src/auditfpledger.cpp
    src/coldeyesengine.cpp
    src/verifyengine.cpp
    src/verifytrust.cpp
    src/plantemplateengine.cpp
    src/tokenusageengine.cpp
    src/sessionmemoryengine.cpp
    src/projectlayoutengine.cpp
    src/paginationengine.cpp
    src/tooldetectionengine.cpp
    # ANTS-1397 — test_audit_* trio engine (Qt6::Core only).
    src/testauditengine.cpp
    # ANTS-1299 + ANTS-1300 — build/test cache MCP tools. Live in
    # ants_core_lib (next to remotecontrol.cpp) so they reach the
    # MCP dispatch path without forcing ants_audit_lib into the
    # core lib's link graph; .audit_cache/ path is computed inline.
    src/buildcache.cpp
    src/testrescache.cpp
    # ANTS-1302 — focused_test changed-files -> ctest-pattern resolver.
    src/focusedtest.cpp
    # ANTS-1720 — `fields=` MCP response projection (pure helper).
    src/mcpprojection.cpp
    # ANTS-2094 — proactive result offload: spill large read bodies to a
    # content-addressed cache file + serve a head+pointer envelope.
    src/mcpspill.cpp
    # ANTS-1727 — shared dispatch-brief composer (fence-hardening +
    # section slicing) + LLM client / bounded dispatcher for the
    # review-dialog family (widget-free; Qt6::Network already linked).
    src/briefdispatch.cpp
    src/llmclient.cpp
    src/llmdispatcher.cpp
    src/roadmapindex.cpp
    src/changeloglog.cpp
    # ANTS-2126 — pass-headings (`#### Pass N.M`) roadmap_log writer
    # primitives (render / flip / annotate), mirror of changeloglog.cpp.
    src/passheadingwrite.cpp
    src/roadmapfoldin.cpp
    # ANTS-1665 — regex-safety helpers (catastrophic-shape detector +
    # LIMIT_MATCH hardener) shared by auditengine and terminalwidget.
    src/regexharden.cpp
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
# ANTS-2073: remotecontrol.cpp #includes the configure-time-generated
# build_info.h so session_orient can surface the running server's build
# identity (git SHA + build date) — clients self-diagnose a stale-binary
# deploy gap. Same wiring as ants_claude_lib (ANTS-1952); regenerated on
# every build by ants_build_info.
target_include_directories(ants_core_lib
    PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(ants_core_lib ants_build_info)

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
    src/verifytrustmodal.cpp
)
target_link_libraries(ants_chrome_lib
    PUBLIC  ants_vt_lib
    PRIVATE ants_warnings)
# ANTS-1323: mainwindow.cpp now reads ANTS_BUILD_DATE / ANTS_BUILD_TIME
# from the configure-time-generated build_info.h for the window-title
# build-badge — same generated header ants_dialogs_lib already uses
# (ANTS-1222).
target_include_directories(ants_chrome_lib
    PRIVATE "${CMAKE_BINARY_DIR}/generated")
set_target_properties(ants_chrome_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
# ANTS-1394: mainwindow.cpp #includes build_info.h — must wait for
# the build-time regenerator so the timestamp is fresh per build.
add_dependencies(ants_chrome_lib ants_build_info)
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
    src/modelrecommender.cpp
    # ANTS-1735 — autonomous model-switch gate. Lives here (not core) because
    # decide() reuses ModelRecommender::tierName (INV-9); core placement would
    # invert the core←claude layering.
    src/modelautoswitch.cpp
    src/claudetabtracker.cpp
    # ANTS-1873 — single-source-of-truth Claude state resolver shared by
    # the tab dot, the status-bar label, and (in due course) the
    # autonomous switcher's INV-2 gate.
    src/claudestateresolver.cpp
    src/claudetranscript.cpp
    src/claudeprojects.cpp
    src/claudebgtasks.cpp
    src/claudebgtasksdialog.cpp
    src/claudetasklist.cpp
    src/claudetasklistdialog.cpp
    # ANTS-1897 — MCP discoverability via SessionStart hook installer.
    src/mcporientation.cpp
)
target_link_libraries(ants_claude_lib
    PUBLIC  ants_chrome_lib
    PRIVATE ants_warnings)
set_target_properties(ants_claude_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
# ANTS-1952: claudeintegration.cpp #includes the configure-time-generated
# build_info.h to stamp the MCP serverInfo with git SHA + build time. The
# header lives in build/generated/ (ants_chrome_lib has it PRIVATE, so it
# does not propagate) and is regenerated on every build by ants_build_info.
target_include_directories(ants_claude_lib
    PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(ants_claude_lib ants_build_info)

# ANTS-1444: the audit engine/runner/hygiene core is split from the
# Widgets-using AuditDialog. ants_audit_lib holds the non-GUI TUs (none
# of these #include a Qt6::Widgets header); the QDialog moves to
# ants_audit_dialog_lib below. This keeps engine-only consumers (the MCP
# audit test bundle, future core consumers) free of the auditdialog.o
# translation unit. NB: ants_core_lib still links Qt6::Widgets PUBLIC,
# so the *transitive* Widgets surface persists until that is narrowed
# separately — this split is the structural prerequisite.
add_library(ants_audit_lib STATIC
    src/auditengine.cpp
    src/audithygiene.cpp
    src/featurecoverage.cpp
    src/auditrulequality.cpp
    # ANTS-1351 — server-side audit runner (Qt6::Core only).
    src/auditrunner.cpp
    # ANTS-1555 — per-project .audit_cache/ manifest + retention reaper.
    src/auditcache.cpp
    # ANTS-1504 — changed-file resolver for audit_run narrowing scopes.
    src/auditscope.cpp
    # ANTS-1870 — pure since-last-run findings delta.
    src/auditdelta.cpp
    # ANTS-1719 — native safe-list auto-fix engine.
    src/auditautofix.cpp
    # ANTS-1352 — indie-review dispatcher (Qt6::Network).
    src/indiereviewdispatcher.cpp
)
target_link_libraries(ants_audit_lib
    PUBLIC  ants_core_lib
    PRIVATE ants_warnings)
set_target_properties(ants_audit_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ANTS-1444: AuditDialog (QDialog) + its UI deps. References AuditEngine
# (ants_audit_lib), DialogChrome (ants_chrome_lib), and ToggleSwitch
# (ants_dialogs_lib). mainwindow.cpp (in ants_chrome_lib) constructs
# AuditDialog, so consumers form a chrome↔dialog symbol cycle resolved
# via --start-group at the final link.
add_library(ants_audit_dialog_lib STATIC
    src/auditdialog.cpp
)
target_link_libraries(ants_audit_dialog_lib
    PUBLIC  ants_audit_lib ants_chrome_lib ants_dialogs_lib
    PRIVATE ants_warnings)
set_target_properties(ants_audit_dialog_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ants_dialogs_lib STATIC
    src/settingsdialog.cpp
    src/roadmapdialog.cpp
    src/roadmapshortcutsdialog.cpp
    src/diffviewer.cpp
    src/sshdialog.cpp
    src/aidialog.cpp
    src/aboutdialogs.cpp
    src/toggleswitch.cpp
    # ANTS-1727 — shared review-dialog scaffold (ColdEyes/TestAudit base).
    src/reviewdialogbase.cpp
    # ANTS-1721 — native in-app cold-eyes documentation review dialog.
    src/coldeyesdialog.cpp
    # ANTS-1722 — native in-app test-suite review dialog.
    src/testauditdialog.cpp
    # ANTS-1258 — native in-app independent code review dialog.
    src/indiereviewdialog.cpp
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
# ANTS-1394: aboutdialogs.cpp #includes build_info.h.
add_dependencies(ants_dialogs_lib ants_build_info)

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

# ANTS-1553 — make ANTS_UNITY_BUILD=ON viable end-to-end: unity only on
# the libs that are ALWAYS linked in full.
#
# CMAKE_UNITY_BUILD batches each STATIC lib's TUs into a few coarse
# unity_N.o. That breaks the test bundles that link only a SUBSET of the
# lib stack via selective `--start-group`: ants_core_lib / ants_vt_lib /
# ants_audit_lib / ants_lua_lib are each linked alone (with core) by
# test_core / test_vt / test_audit / test_lua. Coarsening their objects
# drags cross-lib references those bundles do NOT link — chiefly core's
# aggregated AUTOMOC (mocs_compilation.cpp bundles EVERY core QObject's
# metaobject, so referencing one — e.g. vt's TerminalWidget touching
# VtStream — pulls the whole moc, which references RemoteControl reaching
# up into MainWindow/RoadmapDialog/AuditEngine) → dangling externals →
# broken link. Pre-unity each TU was its own .o island, so the selective
# pull never reached those symbols.
#
# So opt these four OUT of unity; they stay per-TU exactly as a non-unity
# build. chrome / claude / dialogs / audit_dialog are only ever linked in
# FULL (the full-stack bundles + the main exe), so their unity coarsening
# always resolves — and they are the Widgets-heavy, RAM-hungry cc1plus
# instances where unity pays off most. No-op unless ANTS_UNITY_BUILD=ON.
set(_ants_subset_linked_libs ants_core_lib ants_vt_lib ants_audit_lib)
if(LUA_FOUND)
    list(APPEND _ants_subset_linked_libs ants_lua_lib)
endif()
set_target_properties(${_ants_subset_linked_libs} PROPERTIES UNITY_BUILD OFF)

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
# ANTS-1444: the static-lib graph has symbol cycles (chrome↔claude↔dialogs
# via mainwindow.cpp, plus chrome↔audit_dialog via the AuditDialog ctor).
# Wrap the archives in --start-group so GNU ld/lld re-scan until every
# symbol resolves, regardless of archive order — the same guard the test
# bundles already use. ants_warnings is an INTERFACE flag lib and stays
# outside the group.
target_link_libraries(ants-terminal PRIVATE
    -Wl,--start-group
    ants_core_lib
    ants_vt_lib
    ants_chrome_lib
    ants_claude_lib
    ants_audit_lib
    ants_audit_dialog_lib
    ants_dialogs_lib
    $<$<BOOL:${LUA_FOUND}>:ants_lua_lib>
    -Wl,--end-group
    ants_warnings
)

# Precompile Qt aggregate headers. Qt headers dominate per-TU compile cost
# (~200-300 MiB working set in cc1plus) and are pulled in by nearly every
# .cpp under src/. PCH cuts that 30-50%, which keeps the build under
# RAM+swap on 32 GiB hosts as the source tree grows. Aggregate headers
# (no individual classes) so the PCH stays valid across edits.
#
# ANTS-1550 extends PCH from main.cpp's lone TU to every STATIC lib (where
# the bulk of compile time lives). Each lib builds its own PCH once and
# every .cpp inside reuses it — REUSE_FROM is avoided because per-lib
# compile flags drift (ANTS_LUA_PLUGINS / ANTS_WAYLAND_LAYER_SHELL /
# OpenGLWidgets define) would invalidate a single shared PCH.
function(ants_apply_qt_pch tgt)
    target_precompile_headers(${tgt} PRIVATE
        <QtCore/QtCore>
        <QtGui/QtGui>
        <QtWidgets/QtWidgets>
    )
endfunction()

ants_apply_qt_pch(ants-terminal)
ants_apply_qt_pch(ants_core_lib)
ants_apply_qt_pch(ants_vt_lib)
ants_apply_qt_pch(ants_chrome_lib)
ants_apply_qt_pch(ants_claude_lib)
ants_apply_qt_pch(ants_audit_lib)
ants_apply_qt_pch(ants_audit_dialog_lib)
ants_apply_qt_pch(ants_dialogs_lib)
if(LUA_FOUND)
    ants_apply_qt_pch(ants_lua_lib)
endif()

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
        # ANTS-1550 — opt out of CMAKE_UNITY_BUILD for test bundles.
        # Each tests/features/*/test_*.cpp defines its own `static int
        # runMain()` + anonymous-namespace `slurp` / `expect_*` helpers;
        # unifying multiple of them into one TU produces redefinition
        # errors. Test bundles compile fast already (≤ 4 TUs each) so
        # there's no win to chase — leave them on the per-TU path.
        set_target_properties(${name} PROPERTIES UNITY_BUILD OFF)
        target_link_libraries(${name} PRIVATE
            -Wl,--start-group ${B_LIBS} -Wl,--end-group
            ants_warnings GTest::gtest)
        # ANTS-1382: tests/_support/expect.h is included by feature
        # tests via "../../_support/expect.h" — relative path resolves
        # without an explicit -I, but we add tests/ on the include path
        # too so future helpers under tests/_support/ can be referenced
        # by short name.
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}/tests)
        # ANTS-1373/1368: do NOT REUSE_FROM ants-terminal's PCH. The PCH
        # is generated with the main exe's full flag set (PIC +
        # QT_OPENGL_LIB define from the OpenGLWidgets link). Test bundles
        # don't link OpenGLWidgets and don't need PIC, so cc1plus rejects
        # the PCH on every bundle TU with `-Winvalid-pch`. The bundles
        # are only ~4 TUs each, so cold-compiling Qt aggregate headers
        # without PCH is a noise-free no-op vs. emitting one warning per
        # TU per build. Keep ants-terminal's own PCH (where it pays off).
        gtest_discover_tests(${name}
            DISCOVERY_MODE PRE_TEST
            PROPERTIES LABELS "features;fast" TIMEOUT 10)
    endfunction()

    # Core-only bundle helper — Qt::Core only, no Widgets. PCH not reused
    # from ants-terminal (different INTERFACE flag set without Widgets).
    function(ants_add_core_bundle name)
        cmake_parse_arguments(B "" "" "LIBS;SOURCES" ${ARGN})
        add_executable(${name} tests/bundle_main_core.cpp ${B_SOURCES})
        # ANTS-1550 — same opt-out as the GUI bundle: anonymous-namespace
        # / static helpers across test files collide under Unity.
        set_target_properties(${name} PROPERTIES UNITY_BUILD OFF)
        target_link_libraries(${name} PRIVATE
            ${B_LIBS} ants_warnings GTest::gtest Qt6::Core)
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}/tests)
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

    # ANTS-2164/2165 — cut-rc.sh cadence-hardening + hotfix behavioural
    # conformance (shell harness drives the real script against throwaway git
    # repos; the C++ release_rc_pipeline test keeps the source-scrape layer).
    add_test(NAME cut_rc_behaviour
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/features/release_rc_pipeline/cut_rc_behaviour_test.sh")
    set_tests_properties(cut_rc_behaviour PROPERTIES
        LABELS "features;fast"
        TIMEOUT 120)

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
                tests/features/vt_string_rearm/test_vt_string_rearm.cpp
                tests/features/vt_escape_recovery/test_vt_escape_recovery.cpp
                tests/features/scrollback_hyperlink_restore_sync/test_scrollback_hyperlink_restore_sync.cpp
                tests/features/vtparser_print_run_coalesce/test_print_run_coalesce.cpp
                tests/features/combining_on_resize/test_combining.cpp
                tests/features/osc8_insert_delete_lines/test_osc8_linerange.cpp
                tests/features/scrollback_hyperlinks_reflow_lockstep/test_scrollback_hyperlinks_reflow_lockstep.cpp
                tests/features/combining_at_wide_right_edge/test_combining_at_wide_right_edge.cpp
                tests/features/sixel_raster_header_prebudget/test_sixel_raster_header_prebudget.cpp
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
                tests/features/decrqss/test_decrqss.cpp
                # ANTS-2192 — query-response amplification cap.
                tests/features/query_response_ratelimit/test_query_response_ratelimit.cpp
                # ANTS-1827 — CSI param-truncation flag.
                tests/features/vtparser_csi_param_truncation/test_csi_param_truncation.cpp
                # ANTS-1828/1829 — inline-image budget + iTerm2 base64 hardening.
                tests/features/terminalgrid_image_budget_hardening/test_terminalgrid_image_budget_hardening.cpp
                # ANTS-1841 — terminalwidget hot-path hygiene (source-scrape).
                tests/features/terminalwidget_hotpath_perf/test_terminalwidget_hotpath_perf.cpp
                # ANTS-2024 — theme-change DeferredDelete flush ordering guard.
                tests/features/theme_change_deferred_delete_flush/test_theme_change_deferred_delete_flush.cpp)
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
                ants_audit_dialog_lib
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
                tests/features/qpointer_destroyed_safe/test_qpointer_destroyed_safe.cpp
                tests/features/scroll_to_bottom_chip_size/test_scroll_to_bottom_chip_size.cpp
                tests/features/review_changes_branches/test_review_changes_branches.cpp
                tests/features/review_changes_clickable/test_review_changes_clickable.cpp
                tests/features/review_button_untracked/test_review_button_untracked.cpp
                tests/features/review_changes_scroll_preserve/test_review_changes_scroll_preserve.cpp
                tests/features/review_changes_nav/test_review_changes_nav.cpp
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
                tests/features/kwin_position_tracker/test_kwin_position_tracker.cpp
                # ANTS-1318 frozen-RC pipeline (release.yml + cut-rc.sh).
                tests/features/release_rc_pipeline/test_release_rc_pipeline.cpp)
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
        SRC_ROADMAPDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/roadmapdialog.cpp"
        SRC_AUDITDIALOG_H_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDITDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_RELEASE_WORKFLOW_PATH="${CMAKE_SOURCE_DIR}/.github/workflows/release.yml"
        CUT_RC_SH_PATH="${CMAKE_SOURCE_DIR}/packaging/cut-rc.sh"
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
                ants_audit_dialog_lib
                $<$<BOOL:${LUA_FOUND}>:ants_lua_lib>
        SOURCES tests/features/claude_status_bar/test_claude_status.cpp
                tests/features/claude_status_bar_per_tab/test_claude_status_bar_per_tab.cpp
                tests/features/claude_plan_mode_detection/test_claude_plan_mode.cpp
                tests/features/claude_tab_status_indicator/test_claude_tab_tracker.cpp
                tests/features/claude_transcript_robustness/test_claude_transcript_robustness.cpp
                tests/features/allowlist_add/test_allowlist_add.cpp
                # ANTS-1993 — permission-prompt structural gate (anti-spoof).
                tests/features/claude_permission_prompt_structure/test_claude_permission_prompt_structure.cpp
                # ANTS-1996 — wrapMcpData neutralises XML comment markers.
                tests/features/mcp_wrap_comment_escape/test_mcp_wrap_comment_escape.cpp
                # ANTS-2102 — nested-loop socket use-after-free guard lock.
                tests/features/socket_readyread_uaf_guard/test_socket_readyread_uaf_guard.cpp
                # ANTS-2131 — blocking MCP verbs dispatch off the main thread.
                tests/features/mcp_verb_offthread_guard/test_mcp_verb_offthread_guard.cpp
                tests/features/allowlist_perms_postcommit/test_allowlist_perms_postcommit.cpp
                tests/features/ssh_dash_host_rejected/test_ssh_dash.cpp
                tests/features/ssh_extra_args_sanitize/test_ssh_extra_args.cpp
                tests/features/shellutils_whitelist/test_shellutils_whitelist.cpp
                tests/features/ai_insert_command_sanitize/test_ai_insert_command.cpp
                tests/features/claude_git_context_hook/test_installer_grep.cpp
                tests/features/ai_context_redaction/test_ai_context_redaction.cpp
                tests/features/ssh_control_master/test_ssh_control_master.cpp
                tests/features/claude_bg_tasks_button/test_claude_bg_tasks_button.cpp
                # ANTS-1835/1840/1849 — prompt routing + model-chip focus.
                tests/features/claude_statusbar_focus_routing/test_claude_statusbar_focus_routing.cpp
                # ANTS-1850/1851 — per-shell prompt dedup + tab-switch re-show.
                tests/features/claude_prompt_lifecycle/test_claude_prompt_lifecycle.cpp
                # ANTS-1858 — AskUserQuestion lights the awaiting-input dot.
                tests/features/claude_question_prompt_dot/test_claude_question_prompt_dot.cpp
                tests/features/claude_state_dot_palette/test_claude_state_dot_palette.cpp
                tests/features/claude_statusbar_extraction/test_claude_statusbar_extraction.cpp
                tests/features/claude_task_list/test_claude_task_list.cpp
                tests/features/claude_session_freshness/test_claude_session_freshness.cpp
                tests/features/claude_task_list_session_isolation/test_claude_task_list_session_isolation.cpp
                tests/features/claude_pid_replacement/test_claude_pid_replacement.cpp
                # ANTS-1873 — single-source-of-truth Claude state resolver.
                tests/features/claude_state_resolver/test_claude_state_resolver.cpp
                # ANTS-1735 INV-14 — actuator wiring + chip suppression.
                tests/features/model_auto_switch_actuator/test_model_auto_switch_actuator.cpp
                # ANTS-1915 — deferred manual chip-switch wiring guard.
                tests/features/model_switch_deferred_chip/test_deferred_chip_wiring.cpp
                # ANTS-1735 §2.5 — outcome fill-in tick wiring guard.
                tests/features/model_auto_switch_outcome_fillin/test_model_auto_switch_outcome_fillin.cpp
                # ANTS-1894 — near-miss telemetry: controller-touching tests.
                # ClaudeStatusBarController::maybeEmitNearMiss lives in
                # ants_claude_lib; these two land here (test_claude links
                # both libs). The pure ledger/decide tests live on test_core.
                tests/features/model_near_miss_ledger/test_emit_on_sig_change.cpp
                tests/features/model_near_miss_ledger/test_emit_floor_throttle.cpp
                # ANTS-1946 INV-14 — target_equals_current suppression.
                tests/features/model_near_miss_ledger/test_no_emit_target_equals_current.cpp
                # ANTS-1893 — switch-event surfacing: config-defaults tests.
                # Full INV coverage (15 tests) deferred pending a
                # TerminalWidget test seam; see
                # tests/features/auto_switch_surfacing/spec.md.
                tests/features/auto_switch_surfacing/test_config_surfacing_defaults.cpp
                # ANTS-1890 INV-7 — behavioural cache wiring (live controller).
                tests/features/model_auto_switch_outcome_fillin/test_override_cache_controller.cpp
                tests/features/mcp_extra_tools/test_mcp_extra_tools.cpp
                tests/features/tasks_chip_done_over_total/test_tasks_chip_done_over_total.cpp
                # ANTS-1859 — tasks/refresh dedup key excludes transcript mtime.
                tests/features/tasks_refresh_log_dedup/test_tasks_refresh_log_dedup.cpp
                tests/features/mcp_roadmap_status_filter/test_mcp_roadmap_status_filter.cpp
                tests/features/mcp_tools_list_schema/test_mcp_tools_list_schema.cpp
                tests/features/mcp_workspace_search/test_mcp_workspace_search.cpp
                tests/features/mcp_file_outline/test_mcp_file_outline.cpp
                # ANTS-1855 — read_log filter verb.
                tests/features/mcp_read_log/test_mcp_read_log.cpp
                # ANTS-2021 — read_region slice verb.
                tests/features/mcp_read_region/test_mcp_read_region.cpp
                # ANTS-2022 — apply_edits batch edit verb.
                tests/features/mcp_apply_edits/test_mcp_apply_edits.cpp
                # ANTS-1637 — codebase_index structural-map verb.
                tests/features/mcp_codebase_index/test_mcp_codebase_index.cpp
                # ANTS-2139 — docs_index documentation-map verb.
                tests/features/mcp_docs_index/test_mcp_docs_index.cpp
                # ANTS-2160 — per-project .ants/project.json settings file.
                tests/features/project_settings/test_project_settings.cpp
                # ANTS-2161 — layout auto-detect + project_settings verb.
                tests/features/project_settings_verb/test_project_settings_verb.cpp
                # ANTS-2093 — project_query sandboxed server-side query verb.
                tests/features/project_query/test_project_query.cpp
                # ANTS-2159 — file_outline C++ scanner scope-awareness.
                tests/features/fileoutline_cpp_scanner/test_fileoutline_cpp_scanner.cpp
                # ANTS-2158 — eager-load high-frequency MCP verbs.
                tests/features/mcp_eager_load/test_mcp_eager_load.cpp
                # ANTS-2156 — similar_code include_bodies (exemplar retrieval).
                tests/features/similar_code_bodies/test_similar_code_bodies.cpp
                # ANTS-2157 — read_region call_sequence (integration brief).
                tests/features/read_region_call_sequence/test_read_region_call_sequence.cpp
                # ANTS-2222 — read_region symbol-mode aggregate full body.
                tests/features/read_region_aggregate_body/test_read_region_aggregate_body.cpp
                # ANTS-1961 / ANTS-1962 / ANTS-1963 — feedback + spec_log verbs.
                tests/features/mcp_feedback_query/test_mcp_feedback_query.cpp
                tests/features/mcp_feedback_log/test_mcp_feedback_log.cpp
                tests/features/mcp_spec_log/test_mcp_spec_log.cpp
                # ANTS-2129 — audit_falsepos_log write verb.
                tests/features/audit_falsepos_log/test_audit_falsepos_log.cpp
                # ANTS-1390 ~global / ~claude-config caller_cwd sentinel
                # for path tools (workspace_search, file_outline).
                tests/features/mcp_global_config_sentinel/test_mcp_global_config_sentinel.cpp
                # ANTS-1406 last_audit_summary `since_commit` short-circuit.
                tests/features/mcp_last_audit_summary_since_commit/test_mcp_last_audit_summary_since_commit.cpp
                tests/features/mcp_git_state/test_mcp_git_state.cpp
                # ANTS-1839 — GitWrap per-caller stdout budget.
                tests/features/gitwrap_stdout_cap/test_gitwrap_stdout_cap.cpp
                tests/features/mcp_subsystem/test_mcp_subsystem.cpp
                tests/features/mcp_provider_registry/test_mcp_provider_registry.cpp
                tests/features/mcp_last_audit_summary/test_mcp_last_audit_summary.cpp
                tests/features/mcp_indie_review_tools/test_mcp_indie_review_tools.cpp
                tests/features/indie_review_brief_manifest/test_indie_review_brief_manifest.cpp
                tests/features/indie_review_corroborate_dir/test_indie_review_corroborate_dir.cpp
                tests/features/mcp_debt_sweep_tools/test_mcp_debt_sweep_tools.cpp
                tests/features/mcp_verify_changes_tool/test_mcp_verify_changes_tool.cpp
                tests/features/mcp_plan_template_tool/test_mcp_plan_template_tool.cpp
                tests/features/mcp_token_usage_tool/test_mcp_token_usage_tool.cpp
                tests/features/mcp_roadmap_section_slice/test_mcp_roadmap_section_slice.cpp
                tests/features/mcp_cold_eyes/test_mcp_cold_eyes.cpp
                tests/features/mcp_session_memory/test_mcp_session_memory.cpp
                # ANTS-1724 — session_brief MCP tool.
                tests/features/mcp_session_brief/test_mcp_session_brief.cpp
                # ANTS-1723 — workflow_state MCP tool.
                tests/features/mcp_workflow_state/test_mcp_workflow_state.cpp
                tests/features/mcp_output_sanitisation/test_mcp_output_sanitisation.cpp
                tests/features/mcp_path_anchor/test_mcp_path_anchor.cpp
                # ANTS-1357 + ANTS-1346 MCP perf bundle.
                tests/features/mcp_idempotent_read_cache/test_mcp_idempotent_read_cache.cpp
                tests/features/roadmap_section_cache_lru/test_roadmap_section_cache_lru.cpp
                # ANTS-1359 verify_changes session build-cache.
                tests/features/verify_changes_build_cache/test_verify_changes_build_cache.cpp
                # ANTS-1360 mcp_trace ring buffer.
                tests/features/mcp_trace_ring_buffer/test_mcp_trace_ring_buffer.cpp
                # ANTS-1396 terminalForCaller cross-project isolation.
                tests/features/terminal_for_caller_isolation/test_terminal_for_caller_isolation.cpp
                # ANTS-1404 per-tool caller_cwd contracts (Phase 3a).
                tests/features/mcp_caller_cwd_contracts/test_mcp_caller_cwd_contracts.cpp
                # ANTS-1400 caller_cwd_info diagnostic verb.
                tests/features/caller_cwd_info_verb/test_caller_cwd_info_verb.cpp
                # ANTS-1409 callerCwdSuffix descriptor helper.
                tests/features/mcp_caller_cwd_suffix_helper/test_mcp_caller_cwd_suffix_helper.cpp
                # ANTS-1398 roadmap_query rollup-bullet filter.
                tests/features/roadmap_query_filter_section_headers/test_roadmap_query_filter_section_headers.cpp
                # ANTS-1437 roadmap_query section_index mode.
                tests/features/roadmap_query_section_index/test_roadmap_query_section_index.cpp
                # ANTS-1881 roadmap_query mode:"headline_only".
                tests/features/roadmap_query_headline_only/test_roadmap_query_headline_only.cpp
                # ANTS-1922 roadmap_query mode:"bundles".
                tests/features/mcp_roadmap_bundles/test_roadmap_bundles.cpp
                # ANTS-1876 workspace_search payload knobs.
                tests/features/workspace_search_payload_knobs/test_workspace_search_payload_knobs.cpp
                # ANTS-2220 workspace_search enclosing_symbol annotation.
                tests/features/workspace_search_enclosing_symbol/test_workspace_search_enclosing_symbol.cpp
                # ANTS-1882 roadmap_query per-section ETag invariance.
                tests/features/roadmap_query_per_section_etag/test_roadmap_query_per_section_etag.cpp
                # ANTS-1880 spec_query + project_layout phases-dir recognition.
                tests/features/spec_query_phases_layout/test_spec_query_phases_layout.cpp
                # ANTS-1877 roadmap_log op:append stable-prefix project hint.
                tests/features/roadmap_log_stable_prefix_hint/test_roadmap_log_stable_prefix_hint.cpp
                # ANTS-1883 session_orient bundle verb.
                tests/features/session_orient_bundle/test_session_orient_bundle.cpp
                # ANTS-1423 Current preset excludes ✅ shipped bullets.
                tests/features/roadmap_current_preset_excludes_shipped/test_current_preset_excludes_shipped.cpp
                # ANTS-1417 every registered MCP tool has explicit CallerCwdContract entry.
                tests/features/mcp_caller_cwd_contracts_coverage/test_caller_cwd_contracts_coverage.cpp
                # ANTS-1419 every registerToolProvider call declares its contract at the call site.
                tests/features/mcp_call_site_contract/test_mcp_call_site_contract.cpp
                # ANTS-1418 refusal envelopes name caller_cwd_info diagnostic path.
                tests/features/mcp_refusal_envelope_hints/test_refusal_envelope_hints.cpp
                # ANTS-1425 roadmap_query narrator-bullet filter v2.
                tests/features/roadmap_query_narrator_filter/test_narrator_filter.cpp
                # ANTS-1646 roadmap_query duplicate-ID detector envelope field.
                tests/features/roadmap_query_duplicate_ids/test_roadmap_query_duplicate_ids.cpp
                # ANTS-1354 MCP tool descriptor version field.
                tests/features/mcp_tool_descriptor_version/test_tool_descriptor_version.cpp
                # ANTS-1416 MCP Required-contract baseline (session_memory + peers).
                tests/features/mcp_required_contract_baseline/test_required_contract_baseline.cpp
                # ANTS-1415 Phase 3b TabSpecific contract enforcement.
                tests/features/mcp_tabspecific_contract/test_mcp_tabspecific_contract.cpp
                # ANTS-1435 session_memory + project_layout read-op tenancy split.
                tests/features/session_memory_read_caller_cwd/test_session_memory_read_caller_cwd.cpp
                # ANTS-1436 roadmap_query pagination + auto-truncate.
                tests/features/roadmap_query_pagination/test_roadmap_query_pagination.cpp
                # ANTS-1856 roadmap_query single-item id selector.
                tests/features/roadmap_query_by_id/test_roadmap_query_by_id.cpp
                # ANTS-1726 roadmap_query multi-item ids[] selector.
                tests/features/roadmap_query_by_ids/test_roadmap_query_by_ids.cpp
                # ANTS-1696 roadmap_query section_shape hint for non-bullet sections.
                tests/features/roadmap_query_section_shape/test_roadmap_query_section_shape.cpp
                # ANTS-1441 roadmap_log op:flip for ants-v1 native format.
                tests/features/roadmap_log_flip_ants_v1/test_roadmap_log_flip_ants_v1.cpp
                # ANTS-1410 /audit skill spec-anchor drift detection.
                tests/features/audit_skill_anchors/test_audit_skill_anchors.cpp
                # ANTS-1351 v1 audit_run MCP verb.
                tests/features/mcp_audit_run/test_mcp_audit_run.cpp
                # ANTS-1397 v1 test_audit_* MCP trio.
                tests/features/mcp_test_audit_trio/test_mcp_test_audit_trio.cpp
                # ANTS-2096 paginated partition keeps pre-pass findings for brief.
                tests/features/test_audit_pagination_prepass/test_pagination_prepass.cpp
                # ANTS-1451 walkTestFiles build-tree exclusions.
                tests/features/test_audit_walk_exclusions/test_walk_exclusions.cpp
                # ANTS-1624 libcheck C-unit framework detection.
                tests/features/test_audit_libcheck_detection/test_libcheck_detection.cpp
                # ANTS-1623 polyglot framework detection + ANTS-1627
                # Python literal/comment strip in pre-pass.
                tests/features/test_audit_polyglot_and_strip/test_polyglot_and_strip.cpp
                # ANTS-1399 tool_info(name) MCP verb.
                tests/features/mcp_tool_info_verb/test_mcp_tool_info_verb.cpp
                # ANTS-2079 description/detail split — trim wire blobs,
                # serve per-op detail via tool_info.
                tests/features/mcp_tool_detail_field/test_mcp_tool_detail_field.cpp
                # ANTS-1952 build-identity stamp in serverInfo / get_session_info.
                tests/features/mcp_build_identity/test_mcp_build_identity.cpp
                # ANTS-1402 dispatch-observer unification.
                tests/features/mcp_record_dispatch_unification/test_mcp_record_dispatch_unification.cpp
                # ANTS-1422 token_usage no_ci diagnostic envelope.
                tests/features/token_usage_no_ci_diagnostic/test_token_usage_no_ci_diagnostic.cpp
                # ANTS-1424 roadmap_log MCP write verb.
                tests/features/mcp_roadmap_log_verb/test_mcp_roadmap_log_verb.cpp
                # ANTS-1433 roadmap_log two-stage commit rollback seam.
                tests/features/mcp_roadmap_log_atomicity/test_mcp_roadmap_log_atomicity.cpp
                # ANTS-2031 roadmap_log format_mismatch on pass-headings docs.
                tests/features/mcp_roadmap_log_pass_format_mismatch/test_mcp_roadmap_log_pass_format_mismatch.cpp
                # ANTS-2126 roadmap_log pass-headings WRITER (append/flip/annotate).
                tests/features/mcp_roadmap_log_pass_writer/test_mcp_roadmap_log_pass_writer.cpp
                # ANTS-1717/1793 roadmap_log note append (annotate + flip-note).
                tests/features/roadmap_log_annotate/test_roadmap_log_annotate.cpp
                # ANTS-1690 roadmap_log op:flip_batch (N bullets, one commit).
                tests/features/roadmap_log_flip_batch/test_roadmap_log_flip_batch.cpp
                # ANTS-2059 roadmap_log flip/flip_batch on fully id-less ants-v1 bullets.
                tests/features/roadmap_log_flip_idless_antsv1/test_roadmap_log_flip_idless_antsv1.cpp
                # ANTS-2058 parseBullets inline `Kind: . Lanes: .` lanes extraction.
                tests/features/roadmap_query_inline_lanes/test_roadmap_query_inline_lanes.cpp
                # ANTS-1878 roadmap_log op:create_section.
                tests/features/mcp_roadmap_log_create_section/test_mcp_roadmap_log_create_section.cpp
                # ANTS-1879 roadmap_log op:append_batch.
                tests/features/mcp_roadmap_log_append_batch/test_mcp_roadmap_log_append_batch.cpp
                # ANTS-1691 roadmap_log op:bundle_row (Markdown bundle table).
                tests/features/roadmap_log_bundle_row/test_roadmap_log_bundle_row.cpp
                # ANTS-2097 theme switch defers restyle out of popup event loop.
                tests/features/theme_switch_popup_defer/test_theme_switch_popup_defer.cpp
                # ANTS-2043 roadmap_log non-blocking near-duplicate advisory.
                tests/features/roadmap_log_possible_duplicates/test_roadmap_log_possible_duplicates.cpp
                # ANTS-2076/2077 roadmap_log counter-prefix fallback + dry_run preview.
                tests/features/roadmap_log_prefix_and_dry_run/test_roadmap_log_prefix_and_dry_run.cpp
                # ANTS-1548 changelog_log token-frugal CHANGELOG writer.
                tests/features/changelog_log_writer/test_changelog_log_writer.cpp
                # ANTS-2044 changelog_log op:add_batch (N entries, 1 commit).
                tests/features/changelog_log_add_batch/test_changelog_log_add_batch.cpp
                # ANTS-1426 parseBullets blank-line continuation.
                tests/features/roadmap_parser_blank_line_continuation/test_roadmap_parser_blank_line_continuation.cpp
                # ANTS-1427 MCP dispatch debug-log audit trail.
                tests/features/mcp_dispatch_debug_log/test_mcp_dispatch_debug_log.cpp
                # ANTS-1429 roadmap_query/log unrecognised_format gate.
                tests/features/mcp_roadmap_unrecognised_format/test_mcp_roadmap_unrecognised_format.cpp
                # ANTS-1428 GFM-task-list adapter (Tier 1 read-side
                # + Tier 2 write-side op:"flip").
                tests/features/mcp_adapter_github_tasklist/test_adapter_read.cpp
                tests/features/mcp_adapter_github_tasklist/test_adapter_write.cpp
                # ANTS-1356 MCP per-tool rate-limit / quota.
                tests/features/mcp_rate_limit/test_mcp_rate_limit.cpp
                # ANTS-1352 indie_review_dispatch orchestrator.
                tests/features/indie_review_dispatch/test_indie_review_dispatch.cpp
                # ANTS-1727 review-dialog foundation (Q_OBJECT-bearing —
                # hosted here for the full lib set; see test_core note).
                tests/features/llm_client/test_llm_client.cpp
                tests/features/llm_dispatcher/test_llm_dispatcher.cpp
                # ANTS-1453 per-tool MCP selection_hint descriptor field.
                tests/features/mcp_selection_hint/test_mcp_selection_hint.cpp
                # ANTS-1510 cold_eyes_fold_in freeform-mode opt-in.
                tests/features/cold_eyes_fold_in_freeform/test_cold_eyes_fold_in_freeform.cpp
                # ANTS-1512 audit_run scoped-check mode.
                tests/features/audit_run_scoped_check/test_audit_run_scoped_check.cpp
                # ANTS-1446 audit_run compile_commands.json validation.
                tests/features/audit_run_compile_commands_validation/test_audit_run_compile_commands_validation.cpp
                # ANTS-2032 audit_run partial-envelope surface.
                tests/features/audit_run_partial_envelope/test_audit_run_partial_envelope.cpp
                # ANTS-1561 parseBullets multi-line bold headline span.
                tests/features/roadmap_parser_multiline_bold_headline/test_roadmap_parser_multiline_bold_headline.cpp
                # ANTS-2035 pass-heading sub-pass ID distinctness.
                tests/features/roadmap_parser_subpass_id/test_roadmap_parser_subpass_id.cpp
                # ANTS-2030 pass-heading bullet body = under-heading prose.
                tests/features/roadmap_parser_pass_body/test_roadmap_parser_pass_body.cpp
                # ANTS-2039 pass-heading emoji-prefixed status classification.
                tests/features/roadmap_parser_pass_emoji_status/test_roadmap_parser_pass_emoji_status.cpp
                # ANTS-1987 native-path bold-ID extraction for emoji bullets.
                tests/features/roadmap_query_emoji_bold_id/test_roadmap_query_emoji_bold_id.cpp
                # ANTS-1567 MCP tool prefix-tag bucket coverage.
                tests/features/mcp_tool_prefix_tags/test_mcp_tool_prefix_tags.cpp
                # ANTS-1568 etag-tip memo appended to etag-supporting tools.
                tests/features/mcp_etag_tip_memo/test_mcp_etag_tip_memo.cpp
                # ANTS-1578 caller_cwd_info description hint surfacing.
                tests/features/mcp_caller_cwd_info_description_hint/test_mcp_caller_cwd_info_description_hint.cpp
                # ANTS-1565 workspace_search configurable wall-clock budget.
                tests/features/mcp_workspace_search_timeout_sec/test_mcp_workspace_search_timeout_sec.cpp
                # ANTS-1583 roadmap_branch_drift MCP verb.
                tests/features/mcp_roadmap_branch_drift/test_mcp_roadmap_branch_drift.cpp
                # ANTS-1555 audit_run per-project .audit_cache/ infra.
                tests/features/audit_run_cache/test_audit_run_cache.cpp
                # ANTS-1504 audit_run since-last-run scope.
                tests/features/audit_run_since_last_run/test_audit_run_since_last_run.cpp
                # ANTS-1870 audit_run since-last-run findings delta.
                tests/features/audit_run_delta/test_audit_run_delta.cpp
                # ANTS-2045 workspace_search multi-word phrase hint.
                tests/features/workspace_search_phrase_hint/test_workspace_search_phrase_hint.cpp
                # ANTS-2052 session_orient active_bullets legacy fallback.
                tests/features/session_orient_legacy_active/test_session_orient_legacy_active.cpp
                # ANTS-1642 schema-vs-lambda dispatch-forward completeness.
                tests/features/mcp_dispatch_forward_completeness/test_mcp_dispatch_forward_completeness.cpp
                # ANTS-1569 current_state MCP aggregator.
                tests/features/mcp_current_state/test_mcp_current_state.cpp
                # ANTS-1644 cold_eyes_fold_in narrative_mode port.
                tests/features/cold_eyes_fold_in_narrative/test_cold_eyes_fold_in_narrative.cpp
                # ANTS-1644 indie_review_fold_in narrative_mode port.
                tests/features/indie_review_fold_in_narrative/test_indie_review_fold_in_narrative.cpp
                # ANTS-1630 cold_eyes/indie_review fold-in caller_cwd anchoring.
                tests/features/fold_in_caller_anchor/test_fold_in_caller_anchor.cpp
                # ANTS-1309 spec_query MCP tool.
                tests/features/mcp_spec_query/test_mcp_spec_query.cpp
                # ANTS-1308 invariant_check MCP tool.
                tests/features/mcp_invariant_check/test_mcp_invariant_check.cpp
                # ANTS-1299 build_status MCP tool.
                tests/features/mcp_build_status/test_mcp_build_status.cpp
                # ANTS-1300 test_results MCP tool.
                tests/features/mcp_test_results/test_mcp_test_results.cpp
                # ANTS-1303 find_definition + find_caller MCP tools.
                tests/features/mcp_symbol_query/test_mcp_symbol_query.cpp
                # ANTS-1305 similar_code shape matcher.
                tests/features/mcp_similar_code/test_mcp_similar_code.cpp
                # ANTS-1301 recent_errors scrollback error extraction.
                tests/features/mcp_recent_errors/test_mcp_recent_errors.cpp
                # ANTS-1312 last_selection MCP wiring contract.
                tests/features/mcp_last_selection/test_mcp_last_selection.cpp
                # ANTS-1636 find_sources MCP + pure-function tests.
                tests/features/mcp_find_sources/test_mcp_find_sources.cpp
                # ANTS-1306 task_priors MCP context bundler.
                tests/features/mcp_task_priors/test_mcp_task_priors.cpp
                # ANTS-1307 project_conventions MCP convention summary.
                tests/features/mcp_project_conventions/test_mcp_project_conventions.cpp
                # ANTS-1302 focused_test runner (lib + wiring).
                tests/features/mcp_focused_test/test_mcp_focused_test.cpp)
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
        ANTS_MCP_DETAIL_ANCHORS_DIR="${CMAKE_SOURCE_DIR}/tests/features/mcp_tool_detail_field/anchors"
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
        # ANTS-1880 — phases-layout source-grep paths.
        SRC_PROJECTLAYOUTENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/projectlayoutengine.cpp"
        SRC_PROJECTLAYOUTENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/projectlayoutengine.h"
        SRC_GITWRAP_CPP_PATH="${CMAKE_SOURCE_DIR}/src/gitwrap.cpp"
        SRC_GITWRAP_H_PATH="${CMAKE_SOURCE_DIR}/src/gitwrap.h"
        SRC_SUBSYSTEMMAP_CPP_PATH="${CMAKE_SOURCE_DIR}/src/subsystemmap.cpp"
        SRC_SUBSYSTEMMAP_H_PATH="${CMAKE_SOURCE_DIR}/src/subsystemmap.h"
        SRC_AUDIT_ENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.h"
        SRC_AUDIT_ENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.cpp"
        SRC_REMOTECONTROL_H_PATH="${CMAKE_SOURCE_DIR}/src/remotecontrol.h"
        # ANTS-1646 — live ROADMAP.md path for duplicate-id detector test.
        ROADMAP_MD_PATH="${CMAKE_SOURCE_DIR}/ROADMAP.md"
        # ANTS-1410 — spec-anchor drift test for the /audit skill.
        SPEC_ANTS_1410_PATH="${CMAKE_SOURCE_DIR}/docs/specs/ANTS-1410.md"
        # ANTS-1351 — audit_run engine source for source-scrape tests.
        SRC_AUDITRUNNER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditrunner.cpp"
        # ANTS-1512 — audit_run scoped-check mode source-scrape.
        SRC_AUDITRUNNER_H_PATH="${CMAKE_SOURCE_DIR}/src/auditrunner.h"
        # ANTS-1555 — audit_run per-project .audit_cache/ infra source-scrape.
        SRC_AUDITCACHE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditcache.cpp"
        SRC_AUDITCACHE_H_PATH="${CMAKE_SOURCE_DIR}/src/auditcache.h"
        # ANTS-1504 — audit_run since-last-run scope source-scrape.
        SRC_AUDITSCOPE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditscope.cpp"
        # ANTS-1870 — since-last-run findings delta source-scrape.
        SRC_AUDITDELTA_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditdelta.cpp"
        # ANTS-1299 + ANTS-1300 — build/test cache MCP source-scrape.
        SRC_BUILDCACHE_H_PATH="${CMAKE_SOURCE_DIR}/src/buildcache.h"
        SRC_TESTRESCACHE_H_PATH="${CMAKE_SOURCE_DIR}/src/testrescache.h"
        # ANTS-1397 — test_audit engine source for source-scrape tests.
        SRC_TESTAUDITENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/testauditengine.cpp"
        # ANTS-1352 — indie_review_dispatch source-scrape paths.
        SRC_INDIE_REVIEW_DISPATCHER_H_PATH="${CMAKE_SOURCE_DIR}/src/indiereviewdispatcher.h"
        SRC_INDIE_REVIEW_DISPATCHER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/indiereviewdispatcher.cpp"
        SRC_INDIE_REVIEW_ENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/indiereviewengine.h"
        SRC_INDIE_REVIEW_ENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/indiereviewengine.cpp"
        # ANTS-1727 — fence kernel relocated to BriefDispatch (G-15 scrape)
        # + review-dialog foundation source-grep paths (INV-16 widget-free
        # guard, INV-8 dispatcher no-retention).
        SRC_BRIEFDISPATCH_CPP_PATH="${CMAKE_SOURCE_DIR}/src/briefdispatch.cpp"
        SRC_BRIEFDISPATCH_H_PATH="${CMAKE_SOURCE_DIR}/src/briefdispatch.h"
        SRC_LLMCLIENT_H_PATH="${CMAKE_SOURCE_DIR}/src/llmclient.h"
        SRC_LLMCLIENT_CPP_PATH="${CMAKE_SOURCE_DIR}/src/llmclient.cpp"
        SRC_LLMDISPATCHER_H_PATH="${CMAKE_SOURCE_DIR}/src/llmdispatcher.h"
        SRC_LLMDISPATCHER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/llmdispatcher.cpp"
        # ANTS-1510 — cold_eyes_fold_in freeform-mode source-scrape.
        SRC_COLDEYESENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/coldeyesengine.h"
        SRC_COLDEYESENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/coldeyesengine.cpp"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        MCP_ERROR_CODES_DOC_PATH="${CMAKE_SOURCE_DIR}/docs/standards/mcp-error-codes.md"
        FIXTURE_MIN_SARIF="${CMAKE_SOURCE_DIR}/tests/features/mcp_last_audit_summary/fixture_min.sarif"
        FIXTURE_EMPTY_SARIF="${CMAKE_SOURCE_DIR}/tests/features/mcp_last_audit_summary/fixture_empty.sarif"
        FIXTURE_CPPCHECK_XML="${CMAKE_SOURCE_DIR}/tests/features/mcp_last_audit_summary/fixture_cppcheck.xml"
        FIXTURE_PROVENANCE_SARIF="${CMAKE_SOURCE_DIR}/tests/features/mcp_last_audit_summary/fixture_provenance.sarif"
        SRC_AUDITDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        ANTS_CLAUDE_MD_PATH="${CMAKE_SOURCE_DIR}/CLAUDE.md"
        ANTS_MCP_TOOLS_MD_PATH="${CMAKE_SOURCE_DIR}/docs/standards/mcp-tools.md"
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
                tests/features/debt_sweep_engine/test_debt_sweep_engine.cpp
                tests/features/verify_changes_engine/test_verify_changes_engine.cpp
                tests/features/plan_template_engine/test_plan_template_engine.cpp
                tests/features/token_usage_engine/test_token_usage_engine.cpp
                # ANTS-1432 — failed-call accumulator on TokenUsageEngine.
                tests/features/token_usage_failed_metric/test_token_usage_failed_metric.cpp
                tests/features/roadmap_index_engine/test_roadmap_index_engine.cpp
                tests/features/cold_eyes_engine/test_cold_eyes_engine.cpp
                tests/features/session_memory_engine/test_session_memory_engine.cpp
                tests/features/tool_detection_engine/test_tool_detection_engine.cpp
                # ANTS-1364 — Set/Delete serialize-once refactor.
                tests/features/session_memory_serialize_once/test_session_memory_serialize_once.cpp
                # ANTS-1430 — project_layout scan helper.
                tests/features/mcp_project_layout_scan/test_project_layout_scan.cpp
                # ANTS-1457 — false-positive ledger helper.
                tests/features/audit_falsepos_ledger/test_audit_falsepos_ledger.cpp
                # ANTS-1455 — glob walk + reports_dir opt-in + synth mode/pagination.
                tests/features/test_audit_glob_walk_synth_mode/test_audit_glob_walk_synth_mode.cpp
                # ANTS-1579 — verify_changes timeout_sec end-to-end plumb.
                tests/features/mcp_verify_changes_timeout_headroom/test_mcp_verify_changes_timeout_headroom.cpp
                # ANTS-1339 — applyFilter streaming line-split.
                tests/features/audit_engine_stream_line_split/test_audit_engine_stream_line_split.cpp
                # ANTS-1343 — mypy stub-hint pre-collapse count preserved.
                tests/features/audit_mypy_stub_count_preserved/test_audit_mypy_stub_count_preserved.cpp
                # ANTS-1344 — indie_review_corroborate truncation flag.
                tests/features/indie_review_truncation_flag/test_indie_review_truncation_flag.cpp
                # ANTS-1345 — cold-eyes derivePartition deterministic tiebreak.
                tests/features/cold_eyes_partition_deterministic/test_cold_eyes_partition_deterministic.cpp
                # ANTS-1615/1616/1617 — 2026-05-19 test_audit bundle fold-in.
                tests/features/test_audit_bundle_2026_05_19/test_test_audit_bundle.cpp
                # ANTS-1513 — test_audit_recheck deferred-cite verifier.
                tests/features/test_audit_recheck/test_test_audit_recheck.cpp
                # ANTS-1709 — centralised dir-exclusion set + openUrl idiom.
                tests/features/audit_exclusion_set/test_audit_exclusion_set.cpp
                # ANTS-1708 — fingerprint-keyed learned-FP ledger.
                tests/features/audit_fp_ledger/test_audit_fp_ledger.cpp
                # ANTS-1719 — native safe-list auto-fix engine.
                tests/features/audit_low_confidence_autofix/test_audit_low_confidence_autofix.cpp
                # ANTS-1826/1830 — AI-triage cleartext-key refusal + verdict-badge
                # title double-quote escaping.
                tests/features/audit_dialog_render_hardening/test_audit_dialog_render_hardening.cpp)
    target_compile_definitions(test_audit PRIVATE
        SRC_AUDIT_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_ENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_ENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/auditengine.h"
        # ANTS-1665 — regex-DoS watchdog now also scans regexharden.cpp, the
        # extracted home of isCatastrophicRegex / hardenUserRegex.
        SRC_REGEXHARDEN_CPP_PATH="${CMAKE_SOURCE_DIR}/src/regexharden.cpp"
        SRC_AUDIT_CPP="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_AUDIT_H="${CMAKE_SOURCE_DIR}/src/auditdialog.h"
        SRC_AUDIT_ENGINE_CPP="${CMAKE_SOURCE_DIR}/src/auditengine.cpp"
        SRC_AUDIT_ENGINE_H="${CMAKE_SOURCE_DIR}/src/auditengine.h"
        SRC_AUDITDIALOG_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        # ANTS-1372 source-grep tests: roadmap_fold_in needs visibility
        # into remotecontrol.cpp + roadmapfoldin.cpp for S1-S5.
        SRC_REMOTECONTROL_CPP_PATH="${CMAKE_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_ROADMAPFOLDIN_CPP_PATH="${CMAKE_SOURCE_DIR}/src/roadmapfoldin.cpp"
        # ANTS-1457 — false-positive ledger source-scrape paths.
        SRC_FALSEPOS_H_PATH="${CMAKE_SOURCE_DIR}/src/falseposledger.h"
        SRC_FALSEPOS_CPP_PATH="${CMAKE_SOURCE_DIR}/src/falseposledger.cpp"
        SRC_COLDEYESENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/coldeyesengine.cpp"
        SRC_TESTAUDITENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/testauditengine.cpp"
        SRC_INDIE_REVIEW_ENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/indiereviewengine.cpp"
        # ANTS-1344 — truncation-flag source-grep needs the engine header.
        SRC_INDIE_REVIEW_ENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/indiereviewengine.h"
        # ANTS-1455 — taxonomy doc for new error-code rows.
        MCP_ERROR_CODES_DOC_PATH="${CMAKE_SOURCE_DIR}/docs/standards/mcp-error-codes.md"
        # ANTS-1579 — verify_changes description source-scrape (INV-4).
        SRC_CLAUDE_INTEGRATION_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeintegration.cpp"
        # ANTS-1709 — centralised exclusion-set drift guard (INV-7).
        SRC_FEATURECOVERAGE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/featurecoverage.cpp"
        # ANTS-1844 — sliceSection perf-shape lock (roadmap_index_engine ENG-7).
        SRC_ROADMAPINDEX_CPP_PATH="${CMAKE_SOURCE_DIR}/src/roadmapindex.cpp")

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
                ants_audit_dialog_lib
                $<$<BOOL:${LUA_FOUND}>:ants_lua_lib>
        SOURCES tests/features/settings_dependency_gating/test_settings_dependency_gating.cpp
                # ANTS-1727 — review-dialog foundation scaffold.
                tests/features/review_dialog_base/test_review_dialog_base.cpp
                # ANTS-1721 — ColdEyesDialog (native cold-eyes review).
                tests/features/cold_eyes_dialog/test_cold_eyes_dialog.cpp
                # ANTS-1722 — TestAuditDialog (native test-suite review).
                tests/features/test_audit_dialog/test_test_audit_dialog.cpp
                # ANTS-1258 — IndieReviewDialog (native independent code review).
                tests/features/indie_review_dialog/test_indie_review_dialog.cpp
                # ANTS-1257 — AuditDialog v2 affordances (allow / fold / since-baseline).
                tests/features/audit_dialog_v2/test_audit_dialog_v2.cpp
                # ANTS-1259 — AuditDialog Debt Sweep tab (fix / defer / allow).
                tests/features/debt_sweep_dialog/test_debt_sweep_dialog.cpp
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
                # ANTS-1428 GFM-task-list adapter Tier 3 — renderer fork.
                tests/features/mcp_adapter_github_tasklist/test_adapter_render.cpp
                # ANTS-1438 GFM-adapter bold-ID multi-token widening.
                tests/features/gfm_adapter_bold_id_multitoken/test_gfm_adapter_bold_id_multitoken.cpp
                # ANTS-1405 stable-ID regex widened to [PROJ-NNNN] shape.
                tests/features/roadmap_query_external_project_ids/test_roadmap_query_external_project_ids.cpp
                tests/features/diffviewer_extraction/test_diffviewer_extraction.cpp
                tests/features/remote_control_roadmap_query/test_remote_control_roadmap_query.cpp
                # ANTS-2225 — section= surfaces nested #### Pass N.M bullets.
                tests/features/roadmap_query_section_pass_heading/test_roadmap_query_section_pass_heading.cpp
                # ANTS-1638 + ANTS-1639 — Task List dialog right-click copy
                # + malformed-markup flagging.
                tests/features/task_list_dialog_context_menu/test_task_list_dialog_context_menu.cpp
                # ANTS-1842 — DialogChrome D2–D4 affordances.
                tests/features/dialog_chrome_affordances/test_dialog_chrome_affordances.cpp)
    target_compile_definitions(test_dialogs PRIVATE
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SETTINGSDIALOG_H_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.h"
        # ANTS-1843 — review-dialog interaction source-scrape (INV-17/18).
        SRC_REVIEWDIALOGBASE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/reviewdialogbase.cpp"
        SRC_CLAUDEALLOWLIST_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeallowlist.cpp"
        SRC_MAINWINDOW_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_MAINWINDOW_CPP_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        SRC_DIFFVIEWER_H_PATH="${CMAKE_SOURCE_DIR}/src/diffviewer.h"
        SRC_DIFFVIEWER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/diffviewer.cpp"
        ROADMAPDIALOG_H="${CMAKE_SOURCE_DIR}/src/roadmapdialog.h"
        ROADMAPDIALOG_CPP="${CMAKE_SOURCE_DIR}/src/roadmapdialog.cpp"
        # ANTS-1405 — source-scrape against claudeintegration.cpp for the
        # roadmap_query descriptor sentence (INV-9).
        SRC_CLAUDE_INTEGRATION_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeintegration.cpp"
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
                    tests/features/lua_pcall_nesting_timeout/test_lua_pcall_nesting_timeout.cpp
                    tests/features/lua_threading/test_lua_threading.cpp
                    tests/features/plugin_manifest_safety/test_plugin_manifest_safety.cpp)
        target_compile_definitions(test_lua PRIVATE
            ANTS_LUA_PLUGINS=1
            SRC_LUAENGINE_PATH="${CMAKE_SOURCE_DIR}/src/luaengine.cpp"
            SRC_LUAENGINE_H_PATH="${CMAKE_SOURCE_DIR}/src/luaengine.h"
            SRC_PLUGINMANAGER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/pluginmanager.cpp"
            SRC_PLUGINMANAGER_H_PATH="${CMAKE_SOURCE_DIR}/src/pluginmanager.h"
            SRC_MAINWINDOW_PATH="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp")
        # ANTS-1332 — A1/A2 loop-nested attacks finish in ~1.5s post-fix
        # and the whole runtime+source suite is ~3-4s total, well under
        # the bundle's gtest_discover_tests default TIMEOUT 10s. Pre-fix
        # regressions hang inside A1; CTest's 10s timeout terminates
        # them and reports the failure cleanly.
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
                tests/features/rc_c1_byte_filter/test_rc_c1_byte_filter.cpp
                tests/features/rc_get_text_byte_cap/test_rc_get_text_byte_cap.cpp
                tests/features/rc_read_tool_byte_cap/test_rc_read_tool_byte_cap.cpp
                tests/features/rc_socket_dir_hardening/test_rc_socket_dir_hardening.cpp
                tests/features/rc_launch_cwd_anchor/test_rc_launch_cwd_anchor.cpp
                tests/features/verify_trust_gate/test_verify_trust_gate.cpp
                tests/features/claude_dot_restored_tabs/test_claude_dot_restored_tabs.cpp
                tests/features/remote_control_new_tab/test_remote_control_new_tab.cpp
                tests/features/remote_control_select_window/test_remote_control_select_window.cpp
                tests/features/remote_control_set_title/test_remote_control_set_title.cpp
                tests/features/remote_control_get_text/test_remote_control_get_text.cpp
                tests/features/remote_control_launch/test_remote_control_launch.cpp
                tests/features/local_subagent_framework/test_local_subagent_framework.cpp
                tests/features/flatpak_host_shell/test_flatpak_host_shell.cpp
                tests/features/flatpak_lua_module/test_flatpak_lua_module.cpp
                tests/features/mcp_master_toggle/test_mcp_master_toggle.cpp
                tests/features/flathub_manifest_transform/test_flathub_manifest_transform.cpp
                tests/features/config_parse_failure_guard/test_config_parse_failure.cpp
                tests/features/config_reload_loop_safety/test_config_reload_loop_safety.cpp
                tests/features/debuglog_perms/test_debuglog_perms.cpp
                tests/features/persistence_post_rename_chmod/test_persistence_post_rename_chmod.cpp
                tests/features/concurrent_writer_lock/test_concurrent_writer_lock.cpp
                tests/features/secureio_configbackup_split/test_secureio_configbackup_split.cpp
                # ANTS-1821/1822/1823/1824/1836 — local-persistence write-safety bundle.
                tests/features/local_persistence_hardening/test_local_persistence_hardening.cpp
                tests/features/build_warning_repo_visibility_null_deref/test_build_warning_repo_visibility_null_deref.cpp
                # ANTS-1450 — test_audit pattern/dimension JSON drift guard.
                tests/features/test_audit_pattern_drift/test_pattern_drift.cpp
                # ANTS-1226 — ModelRecommender scorer.
                tests/features/model_recommender/test_model_recommender.cpp
                # ANTS-1974 — home/baseline tier band (tierForScore + score home arg).
                tests/features/model_home_tier/test_model_home_tier.cpp
                src/modelrecommender.cpp
                # ANTS-1735 — autonomous model-switch gate (pure decide/clampToFloor).
                # modelautoswitch.cpp + modelrecommender.cpp compiled directly here
                # (test_core links ants_core_lib only, not ants_claude_lib).
                tests/features/model_auto_switch/test_model_auto_switch.cpp
                # ANTS-1920 — output-driven model-switch confirm: pure detector
                # + actuator source-grep (uses SRC_CLAUDESTATUSWIDGETS_CPP_PATH
                # + SRC_MODELAUTOSWITCH_H_PATH defined on test_core below).
                tests/features/model_switch_confirm/test_switch_confirm_visible.cpp
                tests/features/model_switch_confirm/test_switch_confirm_actuator.cpp
                # ANTS-1951 — user-typed /model auto-confirm gate (pure helper).
                tests/features/model_switch_confirm/test_unarmed_confirm.cpp
                src/modelautoswitch.cpp
                # ANTS-1735 — effectiveness ledger (modelswitchledger.cpp is in
                # ants_core_lib, which test_core links — only the test TU here).
                tests/features/model_switch_ledger/test_model_switch_ledger.cpp
                # ANTS-1988/1989 — file-write safety bundle (private cache dirs
                # at 0700 + ConfigWriteLock around ledger read-modify-write).
                tests/features/ledger_write_safety/test_ledger_write_safety.cpp
                # ANTS-1735 — model_switch_stats verb: behavioural aggregation +
                # source-grep wiring contract (uses SRC_RC_HEADER / SRC_RC_CPP /
                # SRC_MAINWINDOW_CPP / SRC_CLAUDE_INTEGRATION_CPP_PATH defs below).
                tests/features/mcp_model_switch_stats/test_mcp_model_switch_stats.cpp
                # ANTS-1891 — trust-signal v2: inconclusive bucket, regret-folds-
                # under-route, weighted clean-end, headline floor, quiet-window
                # settlement. 13 TEST() cases mapping 1:1 to INV-1..INV-13.
                tests/features/model_switch_stats_v2/test_model_switch_stats_v2.cpp
                # ANTS-1894 — near-miss telemetry. Pure tests on the new
                # modelnearmissledger module (in ants_core_lib) + the
                # decide() evaluate-all refactor. Controller-touching tests
                # (INV-5/INV-6) live on test_claude below.
                tests/features/model_near_miss_ledger/test_decide_act_iff_unblocked.cpp
                tests/features/model_near_miss_ledger/test_decide_all_blockers.cpp
                tests/features/model_near_miss_ledger/test_effective_min_dwell.cpp
                tests/features/model_near_miss_ledger/test_decision_tiers_always_set.cpp
                tests/features/model_near_miss_ledger/test_ledger_file_shape.cpp
                tests/features/model_near_miss_ledger/test_evict_to_cap.cpp
                tests/features/model_near_miss_ledger/test_blocker_taxonomy.cpp
                tests/features/model_near_miss_ledger/test_stats_slim_dominant.cpp
                tests/features/model_near_miss_ledger/test_stats_full_windows.cpp
                tests/features/model_near_miss_ledger/test_json_roundtrip.cpp
                tests/features/mcp_model_switch_stats_near_misses/test_mcp_model_switch_stats_near_misses.cpp
                # ANTS-1897 — MCP discoverability SessionStart hook installer.
                # mcporientation.cpp lives in ants_claude_lib; test_core only
                # links ants_core_lib so the TU is compiled directly here
                # (same precedent as modelautoswitch.cpp + modelrecommender.cpp
                # above).
                tests/features/mcp_orientation_install/test_mcp_orientation_install.cpp
                src/mcporientation.cpp
                # ANTS-1888 — per-tab model + thinking-level chip wiring contract.
                # Pure source-grep over claudestatuswidgets.{h,cpp} + mainwindow.cpp.
                tests/features/model_state_chip/test_model_state_chip.cpp
                # ANTS-1720 — MCP fields= response projection.
                tests/features/mcp_projection/test_mcp_projection.cpp
                # ANTS-2094 — proactive result offload (observation masking).
                # mcpspill.cpp is in ants_core_lib (linked); the wiring INVs
                # source-scrape claudeintegration / mainwindow / remotecontrol.
                tests/features/mcp_result_offload/test_mcp_result_offload.cpp
                # ANTS-1727 — ai_review_concurrency config clamp (INV-14).
                tests/features/config_ai_review_concurrency/test_config_ai_review_concurrency.cpp
                # ANTS-1764 — tab_title_format fixed-enum getter/setter validation.
                tests/features/config_tab_title_format/test_config_tab_title_format.cpp
                # ANTS-1727 — BriefDispatch is moc-free, links here. The
                # LlmClient/LlmDispatcher tests reference Q_OBJECT classes,
                # which pull ants_core_lib's aggregated moc (co-references
                # RemoteControl/VtStream → unlinked GUI libs); they live in
                # test_claude where the full lib set resolves.
                tests/features/brief_dispatch_fence/test_brief_dispatch_fence.cpp
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
        # ANTS-1720 — dispatch-ordering scrape for the fields= projection.
        SRC_CLAUDE_INTEGRATION_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudeintegration.cpp"
        # ANTS-2094 — result-offload fail-open source-scrape (INV-11).
        SRC_MCPSPILL_CPP_PATH="${CMAKE_SOURCE_DIR}/src/mcpspill.cpp"
        SRC_SETTINGSDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/settingsdialog.cpp"
        SRC_SECUREIO_H_PATH="${CMAKE_SOURCE_DIR}/src/secureio.h"
        SRC_CONFIGBACKUP_H_PATH="${CMAKE_SOURCE_DIR}/src/configbackup.h"
        # ANTS-1821/1823/1824/1836 — local-persistence write-safety wiring greps.
        SRC_SESSIONMEMORYENGINE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/sessionmemoryengine.cpp"
        SRC_AUDITFPLEDGER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditfpledger.cpp"
        SRC_AUDITAUTOFIX_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditautofix.cpp"
        SRC_TESTAUDITDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/testauditdialog.cpp"
        # ANTS-1988/1989 — file-write safety bundle wiring greps.
        SRC_MODELSWITCHLEDGER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/modelswitchledger.cpp"
        SRC_MODELNEARMISSLEDGER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/modelnearmissledger.cpp"
        SRC_AUDITCACHE_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditcache.cpp"
        SRC_AUDITRUNNER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditrunner.cpp"
        SRC_AUDITDIALOG_CPP_PATH="${CMAKE_SOURCE_DIR}/src/auditdialog.cpp"
        SRC_DEBUGLOG_PATH="${CMAKE_SOURCE_DIR}/src/debuglog.cpp"
        SRC_RC_HEADER="${CMAKE_SOURCE_DIR}/src/remotecontrol.h"
        SRC_RC_CPP="${CMAKE_SOURCE_DIR}/src/remotecontrol.cpp"
        SRC_MAINWINDOW_H="${CMAKE_SOURCE_DIR}/src/mainwindow.h"
        SRC_MAINWINDOW_CPP="${CMAKE_SOURCE_DIR}/src/mainwindow.cpp"
        # ANTS-1888 — chip wiring contract.
        SRC_CLAUDESTATUSWIDGETS_H_PATH="${CMAKE_SOURCE_DIR}/src/claudestatuswidgets.h"
        SRC_CLAUDESTATUSWIDGETS_CPP_PATH="${CMAKE_SOURCE_DIR}/src/claudestatuswidgets.cpp"
        # ANTS-1890 — modelrecommender source-grep paths (INV-5/INV-9 purity).
        SRC_MODELRECOMMENDER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/modelrecommender.cpp"
        SRC_MODELRECOMMENDER_H_PATH="${CMAKE_SOURCE_DIR}/src/modelrecommender.h"
        SRC_MODELAUTOSWITCH_H_PATH="${CMAKE_SOURCE_DIR}/src/modelautoswitch.h"
        SRC_MAIN_CPP="${CMAKE_SOURCE_DIR}/src/main.cpp"
        SRC_CMAKELISTS="${CMAKE_SOURCE_DIR}/CMakeLists.txt"
        FLATPAK_MANIFEST_PATH="${CMAKE_SOURCE_DIR}/packaging/flatpak/org.ants.Terminal.yml"
        TRANSFORMER_PATH="${CMAKE_SOURCE_DIR}/packaging/flatpak/make-flathub-manifest.sh"
        # ANTS-1450 — test_audit pattern/dimension drift guard.
        TEST_AUDIT_PATTERNS_JSON="${CMAKE_SOURCE_DIR}/docs/standards/test-audit-grep-patterns.json"
        # ANTS-1901 — master MCP toggle registration grep.
        MCP_ERROR_CODES_MD_PATH="${CMAKE_SOURCE_DIR}/docs/standards/mcp-error-codes.md")
endif()
