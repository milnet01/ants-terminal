// Flathub manifest transformer — conformance test.
// See spec.md for the full contract.
//
// Runs packaging/flatpak/make-flathub-manifest.sh and asserts the
// output shape against the six invariants.

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>


#include <gtest/gtest.h>
#ifndef TRANSFORMER_PATH
#error "TRANSFORMER_PATH compile definition required"
#endif
#ifndef DEV_MANIFEST_PATH
#error "DEV_MANIFEST_PATH compile definition required"
#endif

// Whole-file read; empty string when the path cannot be opened, which the
// caller reports as its own failure rather than silently passing.
static std::string readFile(const char *path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// The sha256 value of the `lua` module's archive source, taken from a
// manifest's text. Scoped to that module: the search starts at
// `- name: lua` and stops at the next top-level module, so a sha256
// belonging to some later module can never be returned in its place.
static std::string luaSha256(const std::string &manifest) {
    const auto lua = manifest.find("- name: lua");
    if (lua == std::string::npos) return {};
    auto end = manifest.find("- name: ", lua + 1);
    if (end == std::string::npos) end = manifest.size();
    const auto key = manifest.find("sha256: ", lua);
    if (key == std::string::npos || key >= end) return {};
    const auto from = key + 8;
    auto to = manifest.find_first_not_of(
        "0123456789abcdefABCDEF", from);
    if (to == std::string::npos) to = manifest.size();
    return manifest.substr(from, to - from);
}

// Run a shell command, capture stdout. Returns exit status; appends
// captured output to `out`.
// Hard wall-clock cap on the transformer subprocess. `timeout(1)` from
// coreutils kills a wedged script (returning 124) so a hang surfaces as a
// test failure instead of stalling the whole bundle indefinitely (ANTS-1594).
static constexpr const char *kTimeoutPrefix = "timeout 30 ";

static int runCapture(const std::string &cmd, std::string &out) {
    std::array<char, 4096> buf{};
    std::string full = kTimeoutPrefix + cmd + " 2>/dev/null";
    std::unique_ptr<FILE, int(*)(FILE *)> pipe(popen(full.c_str(), "r"), pclose);
    if (!pipe) return -1;
    while (std::fgets(buf.data(), buf.size(), pipe.get())) {
        out.append(buf.data());
    }
    int status = pclose(pipe.release());
    return WEXITSTATUS(status);
}

// Same, but keep stderr too (for invalid-input test).
static int runCaptureBoth(const std::string &cmd, std::string &out) {
    std::array<char, 4096> buf{};
    std::string full = kTimeoutPrefix + cmd + " 2>&1";
    std::unique_ptr<FILE, int(*)(FILE *)> pipe(popen(full.c_str(), "r"), pclose);
    if (!pipe) return -1;
    while (std::fgets(buf.data(), buf.size(), pipe.get())) {
        out.append(buf.data());
    }
    int status = pclose(pipe.release());
    return WEXITSTATUS(status);
}

static int runMain() {
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };

    const std::string script = TRANSFORMER_PATH;

    // Exercise the auto-detect path first so INV-4 runs with a real
    // manifest produced from CMakeLists.txt.
    std::string out;
    int rc = runCapture(script, out);
    if (rc != 0) {
        std::fprintf(stderr, "transformer exited %d with no args\n", rc);
        return 1;
    }

    auto has = [&](const std::string &needle) {
        return out.find(needle) != std::string::npos;
    };

    // INV-1 — dir source removed.
    if (has("type: dir")) {
        fail("INV-1: transformed manifest must not contain `type: dir`");
    }
    if (has("path: ../..")) {
        fail("INV-1: transformed manifest must not contain `path: ../..`");
    }

    // INV-2 — git source with tag.
    if (!has("type: git")) {
        fail("INV-2: transformed manifest must contain `type: git`");
    }
    if (!has("url: https://github.com/milnet01/ants-terminal")) {
        fail("INV-2: transformed manifest must point `url:` at "
             "https://github.com/milnet01/ants-terminal");
    }
    if (out.find("tag: v") == std::string::npos) {
        fail("INV-2: transformed manifest must carry a `tag: v<VERSION>` line");
    }

    // INV-3 — Lua module preserved.
    if (!has("- name: lua")) {
        fail("INV-3: Lua module dropped from transformed manifest");
    }
    if (!has("x-checker-data:")) {
        fail("INV-3: x-checker-data stanza dropped");
    }
    // Read the sha256 out of the DEV manifest and require the transformed
    // one to carry the same line. A literal here was red on any legitimate
    // Lua bump — and, worse, could never fail for the reason the invariant
    // names: a transformer that rewrote the hash would be caught only when
    // it happened to rewrite it to something other than one fixed constant.
    {
        const std::string devSha = luaSha256(readFile(DEV_MANIFEST_PATH));
        if (devSha.empty()) {
            fail("INV-3: no `sha256:` under the dev manifest's lua module");
        } else if (!has("sha256: " + devSha)) {
            fail("INV-3: Lua tarball sha256 changed (should be byte-identical to dev)");
        }
    }
    // Lua before ants-terminal — same ordering as dev manifest.
    auto luaPos = out.find("- name: lua");
    auto antsPos = out.find("- name: ants-terminal");
    if (luaPos != std::string::npos && antsPos != std::string::npos && luaPos > antsPos) {
        fail("INV-3: `- name: lua` must still appear before `- name: ants-terminal`");
    }

    // INV-4 — auto-detected version matches CMakeLists.txt. We can't
    // hardcode a version, but we can assert the tag parses as vX.Y.Z
    // and matches the pattern of a well-formed SemVer.
    {
        auto pos = out.find("tag: v");
        if (pos != std::string::npos) {
            // Parse "tag: v" + triple.
            pos += 6; // skip "tag: v"
            size_t dots = 0;
            size_t i = pos;
            while (i < out.size() && (std::isdigit(static_cast<unsigned char>(out[i])) || out[i] == '.')) {
                if (out[i] == '.') ++dots;
                ++i;
            }
            std::string ver = out.substr(pos, i - pos);
            if (dots != 2 || ver.empty()) {
                std::string msg = "INV-4: auto-detected tag is not X.Y.Z: '" + ver + "'";
                fail(msg.c_str());
            }
        }
    }

    // INV-5 — generated-file header at top of output.
    if (out.find("# GENERATED FILE") != 0) {
        fail("INV-5: output must begin with `# GENERATED FILE` banner");
    }
    if (!has("Source:") || !has("Generator:")) {
        fail("INV-5: banner must name the source manifest and the generator");
    }

    // INV-6 — invalid VERSION rejected.
    {
        std::string bogusOut;
        int rc2 = runCaptureBoth(script + " not-a-version", bogusOut);
        if (rc2 == 0) {
            fail("INV-6: transformer must exit non-zero when given "
                 "an invalid VERSION argument");
        }
        if (bogusOut.find("invalid version") == std::string::npos) {
            fail("INV-6: transformer must emit 'invalid version' on stderr "
                 "when rejecting a bogus VERSION");
        }
    }

    // INV-7 — `--metainfo` mode emits the release metainfo with the
    // unreleased "Patron RC preview" channel-opener <release> placeholder
    // stripped (so the shipped stable leads the Flathub <releases> list).
    {
        std::string mout;
        int rc3 = runCapture(script + " --metainfo", mout);
        if (rc3 != 0) {
            fail("INV-7: `--metainfo` mode must exit 0");
        }
        if (mout.find("<component") == std::string::npos) {
            fail("INV-7: `--metainfo` output must be a metainfo <component>");
        }
        if (mout.find("preview channel") != std::string::npos) {
            fail("INV-7: `--metainfo` must strip the channel-opener "
                 "'preview channel' placeholder <release>");
        }
    }

    if (failures) {
        std::fprintf(stderr,
            "flathub_manifest_transform: %d assertion(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stdout, "flathub_manifest_transform: all invariants passed\n");
    return 0;
}

TEST(FlathubManifestTransform, Main) {
    ASSERT_EQ(0, runMain());
}
