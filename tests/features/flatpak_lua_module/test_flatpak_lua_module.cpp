// Flatpak Lua module — source-grep regression test.
// See spec.md for the full contract.
//
// Pins the shape of the `lua` module in packaging/flatpak/org.ants.Terminal.yml.
// A regression that strips the module, removes -fPIC, forgets to
// pin a sha256, reorders the module after ants-terminal, or drops
// the x-checker-data block fails at ctest time.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef FLATPAK_MANIFEST_PATH
#error "FLATPAK_MANIFEST_PATH compile definition required"
#endif

static std::string slurp(const char *path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int main() {
    const std::string src = slurp(FLATPAK_MANIFEST_PATH);
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

    // INV-5 — linux-noreadline target.
    if (!has("make linux-noreadline")) {
        fail("INV-5: Lua build target must be `linux-noreadline` "
             "to avoid pulling readline into the sandbox");
    }
    // Belt-and-braces: the default `linux` target (which aliases to
    // linux-readline) must not appear as a standalone `make linux` call.
    // Allow `linux-noreadline` as a substring match.
    {
        auto pos = src.find("make linux ");
        if (pos != std::string::npos) {
            fail("INV-5: manifest must not invoke `make linux ` "
                 "(the default aliases to linux-readline); use "
                 "`make linux-noreadline` instead");
        }
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
