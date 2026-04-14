cmake_minimum_required(VERSION 3.20)
project(ants-terminal VERSION 0.6.14 LANGUAGES CXX)

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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets DBus)

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

install(TARGETS ants-terminal DESTINATION bin)

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
endif()
