// Flatpak host-shell wiring — source-grep regression test.
// See spec.md for the full contract.
//
// Why source-grep: the branch fires inside a forkpty child that can't
// be exercised headlessly — there's no Flatpak sandbox on CI runners
// and the only side effect is execvp() replacing the process. What the
// spec guarantees is "this branch exists with this exact shape", which
// a regex/find check on ptyhandler.cpp locks in cheaply.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_PTYHANDLER_PATH
#error "SRC_PTYHANDLER_PATH compile definition required"
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
    const std::string src = slurp(SRC_PTYHANDLER_PATH);
    int failures = 0;
    auto fail = [&](const char *msg) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++failures;
    };
    auto has = [&](const std::string &needle) {
        return src.find(needle) != std::string::npos;
    };

    // INV-1 — Flatpak detection probes both FLATPAK_ID and /.flatpak-info
    // in an OR so either signal triggers the host-shell branch.
    if (!has("\"FLATPAK_ID\"")) {
        fail("INV-1: ptyhandler.cpp must check getenv(\"FLATPAK_ID\")");
    }
    if (!has("/.flatpak-info")) {
        fail("INV-1: ptyhandler.cpp must check /.flatpak-info existence");
    }
    if (!has("getenv(\"FLATPAK_ID\")")) {
        fail("INV-1: FLATPAK_ID check must use getenv()");
    }
    if (!has("access(\"/.flatpak-info\"")) {
        fail("INV-1: /.flatpak-info check must use access()");
    }
    {
        // Anchor on the call-site literals, not the bare tokens, so
        // comment text mentioning FLATPAK_ID doesn't skew the position.
        const std::string getenvCall = "getenv(\"FLATPAK_ID\")";
        const std::string accessCall = "access(\"/.flatpak-info\"";
        auto getenvPos = src.find(getenvCall);
        auto accessPos = src.find(accessCall);
        if (getenvPos != std::string::npos &&
            accessPos != std::string::npos) {
            size_t lo = std::min(getenvPos, accessPos);
            size_t hi = std::max(getenvPos, accessPos);
            auto orPos = src.find("||", lo);
            if (orPos == std::string::npos || orPos > hi) {
                fail("INV-1: getenv(\"FLATPAK_ID\") and "
                     "access(\"/.flatpak-info\", ...) must be joined by "
                     "a logical OR (||) so either signal triggers "
                     "the branch");
            }
        }
    }

    // INV-2 — host branch invokes flatpak-spawn --host with -- separator.
    if (!has("execvp(\"flatpak-spawn\"")) {
        fail("INV-2: host branch must execvp(\"flatpak-spawn\", ...)");
    }
    if (!has("\"flatpak-spawn\"")) {
        fail("INV-2: argv[0] literal \"flatpak-spawn\" must appear");
    }
    if (!has("\"--host\"")) {
        fail("INV-2: argv must include a literal \"--host\" token");
    }
    if (!has("\"--\"")) {
        fail("INV-2: argv must include a \"--\" separator before the "
             "shell path");
    }

    // INV-3 — TERM family crosses the sandbox via --env=.
    const char *envVars[] = {
        "\"--env=TERM=xterm-256color\"",
        "\"--env=COLORTERM=truecolor\"",
        "\"--env=TERM_PROGRAM=AntsTerminal\"",
        "\"--env=COLORFGBG=15;0\"",
    };
    for (const char *needle : envVars) {
        if (!has(needle)) {
            std::fprintf(stderr,
                "FAIL: INV-3: host-branch argv must include %s\n",
                needle);
            ++failures;
        }
    }
    // TERM_PROGRAM_VERSION must pull from ANTS_VERSION (so a bump
    // propagates without editing this file). Accepts either the
    // string-concat form (`"--env=TERM_PROGRAM_VERSION=" + version`)
    // or the snprintf-format form (`"--env=TERM_PROGRAM_VERSION=%s"`,
    // ANTS-1046's heap-free variant).
    const bool hasVerPrefixConcat =
        has("\"--env=TERM_PROGRAM_VERSION=\"");
    const bool hasVerPrefixSnprintf =
        has("\"--env=TERM_PROGRAM_VERSION=%s\"");
    if ((!hasVerPrefixConcat && !hasVerPrefixSnprintf) ||
        !has("ANTS_VERSION")) {
        fail("INV-3: TERM_PROGRAM_VERSION must be either "
             "--env=TERM_PROGRAM_VERSION= concatenated with ANTS_VERSION "
             "OR --env=TERM_PROGRAM_VERSION=%s formatted with "
             "ANTS_VERSION (ANTS-1046 stack-only argv form)");
    }

    // INV-4 — working directory crosses via --directory=. Accepts
    // either the string-concat form (`"--directory=" + workDir`) or
    // the snprintf-format form (`"--directory=%s"`, ANTS-1046's
    // heap-free variant).
    if (!has("\"--directory=\"") && !has("\"--directory=%s\"")) {
        fail("INV-4: host-branch argv must include a "
             "\"--directory=\" prefix or \"--directory=%s\" format "
             "string so workDir crosses the sandbox");
    }
    // The --directory handling must be conditional — appending it
    // unconditionally with an empty workDir would make flatpak-spawn
    // try to cd into "", which errors.
    if (!has("workDir.isEmpty()")) {
        fail("INV-4: --directory insertion must be gated on "
             "!workDir.isEmpty()");
    }

    // INV-5 — direct-exec fallback preserved. The original execlp call
    // must still be present, un-touched, for the non-Flatpak branch.
    if (!has("execlp(shellCStr, argv0, nullptr)")) {
        fail("INV-5: non-Flatpak branch must retain "
             "execlp(shellCStr, argv0, nullptr)");
    }
    // The Flatpak branch must come BEFORE the direct-exec call so we
    // actually take it when the detection succeeds.
    {
        auto flatpakBranchPos = src.find("execvp(\"flatpak-spawn\"");
        auto directExecPos = src.find("execlp(shellCStr, argv0, nullptr)");
        if (flatpakBranchPos != std::string::npos &&
            directExecPos != std::string::npos &&
            flatpakBranchPos > directExecPos) {
            fail("INV-5: Flatpak execvp branch must precede the direct "
                 "execlp call so the detection branch wins");
        }
    }

    // INV-6 — failure mode matches direct exec. The Flatpak branch's
    // post-execvp must fall through to _exit(127) the same way the
    // direct-exec path does.
    {
        auto flatpakBranchPos = src.find("execvp(\"flatpak-spawn\"");
        if (flatpakBranchPos != std::string::npos) {
            // Scan forward a bounded window for _exit(127) — it must
            // appear before the next function/class boundary.
            auto window = src.substr(flatpakBranchPos, 400);
            if (window.find("_exit(127)") == std::string::npos) {
                fail("INV-6: Flatpak branch must fall through to "
                     "_exit(127) on execvp failure");
            }
        }
    }

    if (failures) {
        std::fprintf(stderr,
            "flatpak_host_shell: %d assertion(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stdout, "flatpak_host_shell: all invariants passed\n");
    return 0;
}
