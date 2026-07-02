// Flatpak Lua module — source-grep regression test.
// See spec.md for the full contract.
//
// Pins the shape of the `lua` module in packaging/flatpak/za.co.antsprojectshub.AntsTerminal.yml.
// A regression that strips the module, removes -fPIC, forgets to
// pin a sha256, reorders the module after ants-terminal, or drops
// the x-checker-data block fails at ctest time.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>


#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"
#ifndef FLATPAK_MANIFEST_PATH
#error "FLATPAK_MANIFEST_PATH compile definition required"
#endif


static int runMain() {
    const std::string src = ants_test::slurpFile(FLATPAK_MANIFEST_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };
    auto has = [&](const std::string &needle) {
        return src.find(needle) != std::string::npos;
    };

    // INV-1 — Lua module appears before ants-terminal module.
    auto luaModulePos = src.find("- name: lua");
    auto antsModulePos = src.find("- name: ants-terminal");
    if (luaModulePos == std::string::npos) {
        fail("INV-1: manifest must contain a `- name: lua` module");
    }
    if (antsModulePos == std::string::npos) {
        fail("INV-1: manifest must contain a `- name: ants-terminal` module");
    }
    if (luaModulePos != std::string::npos &&
        antsModulePos != std::string::npos &&
        luaModulePos > antsModulePos) {
        fail("INV-1: `- name: lua` must appear before "
             "`- name: ants-terminal` so CMake finds liblua.a at "
             "configure time");
    }

    // INV-2 — archive source with pinned sha256.
    if (!has("type: archive")) {
        fail("INV-2: Lua module must use `type: archive`");
    }
    if (!has("url: https://www.lua.org/ftp/lua-5.4.")) {
        fail("INV-2: Lua module url must point at "
             "https://www.lua.org/ftp/lua-5.4.X.tar.gz");
    }
    if (!has("sha256:")) {
        fail("INV-2: Lua module must carry a pinned sha256:");
    }

    // INV-3 — -fPIC in MYCFLAGS.
    if (!has("MYCFLAGS=\"-fPIC\"")) {
        fail("INV-3: Lua make invocation must pass "
             "MYCFLAGS=\"-fPIC\" so liblua.a links into the PIE "
             "ants-terminal executable");
    }

    // INV-4 — install to /app.
    if (!has("make install INSTALL_TOP=/app")) {
        fail("INV-4: Lua install step must use "
             "`make install INSTALL_TOP=/app` so headers + liblua.a "
             "land where CMake's FindLua looks");
    }

    // INV-5 — readline-free Lua build target. In Lua 5.4.6+ the
    // top-level `linux` target is itself readline-free (src/Makefile
    // aliases `linux` -> `linux-noreadline`); `linux-readline` is the
    // variant that links -lreadline. The top level exposes only
    // `linux` / `linux-readline` via PLATS — `make linux-noreadline`
    // at the top level is NOT a valid target and fails the build with
    // "No rule to make target". So the manifest must invoke the
    // readline-free `make linux` and must not use `make linux-readline`.
    if (!has("make linux ")) {
        fail("INV-5: Lua build must invoke the readline-free top-level "
             "`make linux` target (5.4.6+ aliases it to linux-noreadline "
             "in src/Makefile); note `make linux-noreadline` is not a "
             "valid TOP-level target and fails the build");
    }
    if (has("make linux-readline")) {
        fail("INV-5: Lua build must not use `make linux-readline` — it "
             "links -lreadline into the sandbox; use `make linux`");
    }

    // INV-6 — x-checker-data stanza present on the Lua module.
    if (!has("x-checker-data:")) {
        fail("INV-6: Lua module must carry an x-checker-data: "
             "stanza so Flathub CI auto-refreshes sha256 on bumps");
    }
    if (!has("version-pattern: lua-(5\\.4\\.\\d+)\\.tar\\.gz")) {
        fail("INV-6: x-checker-data version-pattern must match "
             "`lua-(5\\.4\\.\\d+)\\.tar\\.gz` so only 5.4.x "
             "releases auto-update (not 5.5.x majors)");
    }
    if (!has("url-template: https://www.lua.org/ftp/lua-$version.tar.gz")) {
        fail("INV-6: x-checker-data url-template must be "
             "https://www.lua.org/ftp/lua-$version.tar.gz");
    }

    if (failures) {
        std::fprintf(stderr,
            "flatpak_lua_module: %d assertion(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stdout, "flatpak_lua_module: all invariants passed\n");
    return 0;
}

TEST(FlatpakLuaModule, Main) {
    ASSERT_EQ(0, runMain());
}
