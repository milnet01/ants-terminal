// Feature-conformance test for spec.md — asserts rule normalisation
// and generalisation cover every documented splitter (&&, ;, |, ||)
// and honor the safety denylist.
//
// Links against src/claudeallowlist.cpp. Widget/event-loop lifecycle
// covered by spec §C but not by this test (out of scope — needs
// QApplication + MainWindow harness).
//
// Exit 0 = all assertions hold. Non-zero = regression.

#include "claudeallowlist.h"

#include <QCoreApplication>

#include <cstdio>

namespace {

struct NormCase {
    const char *label;
    const char *input;
    const char *expected;
};

// `normalizeRule` contract. Each raw input → canonical form.
const NormCase kNormalize[] = {
    {"colon-to-space",       "Bash(git:*)",                "Bash(git *)"},
    {"colon-space-to-space", "Bash(git: *)",               "Bash(git *)"},
    {"bare-single-word",     "Bash(git)",                  "Bash(git *)"},
    {"bare-make-kept",       "Bash(make)",                 "Bash(make)"},
    {"bare-env-kept",        "Bash(env)",                  "Bash(env)"},
    {"webfetch-add-domain",  "WebFetch(example.com)",      "WebFetch(domain:example.com)"},
    {"read-add-slashslash",  "Read(/etc/passwd)",          "Read(//etc/passwd)"},
    {"write-add-slashslash", "Write(/tmp/x)",              "Write(//tmp/x)"},
};

struct GenCase {
    const char *label;
    const char *input;      // already normalized
    const char *expected;   // "" = return empty (no generalisation)
};

// `generalizeRule` contract — the splitter coverage is the headline.
const GenCase kGeneralize[] = {
    // Simple single-command cases.
    {"simple-git",           "Bash(git status --short)",           "Bash(git *)"},
    {"simple-gh",            "Bash(gh pr create --title x)",       "Bash(gh *)"},
    {"already-wildcarded",   "Bash(git *)",                        ""},
    {"already-double-wild",  "Bash(find **)",                      ""},

    // Safety denylist.
    {"rm-denylist",          "Bash(rm -rf /tmp/foo)",              ""},
    {"bare-sudo-denylist",   "Bash(sudo apt install foo)",         ""},
    {"SUDO_OTHER-denylist",  "Bash(SUDO_OTHER=x do-thing arg)",    ""},

    // SUDO_ASKPASS — specific form.
    {"sudo-askpass-specific",
     "Bash(SUDO_ASKPASS=/usr/libexec/ssh/ksshaskpass sudo -A zypper install foo)",
     "Bash(SUDO_ASKPASS=/usr/libexec/ssh/ksshaskpass sudo -A zypper *)"},

    // `&&` chains — preserve structure, wildcard each side.
    {"cd-and-cmd",           "Bash(cd /mnt/Storage && cmake --build build)",
                             "Bash(cd * && cmake *)"},
    {"make-and-echo",        "Bash(make && echo done)",
                             "Bash(make && echo *)"},
    {"triple-and",           "Bash(a && b && c x y)",
                             "Bash(a && b && c *)"},

    // `;` chains.
    {"semicolon-pair",       "Bash(git pull; make)",
                             "Bash(git * ; make)"},

    // `|` pipes.
    {"pipe-pair",            "Bash(cat file.txt | grep foo)",
                             "Bash(cat * | grep *)"},

    // `||` chains.
    {"or-pair",              "Bash(make || echo failed)",
                             "Bash(make || echo *)"},

    // rm on either side of a compound → denylist triggers.
    {"compound-with-rm-lhs", "Bash(rm -rf x && make)",             ""},
    {"compound-with-rm-rhs", "Bash(make && rm -rf x)",             ""},
};

// ----------------------------------------------------------------------------

int checkNormalize() {
    int failures = 0;
    for (const auto &c : kNormalize) {
        const QString got = ClaudeAllowlistDialog::normalizeRule(c.input);
        const bool ok = (got == QString::fromUtf8(c.expected));
        std::fprintf(stderr,
                     "[norm/%-24s] input='%s' got='%s' want='%s'  %s\n",
                     c.label, c.input, qUtf8Printable(got), c.expected,
                     ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    // Idempotency: normalize(normalize(x)) == normalize(x) for every case.
    for (const auto &c : kNormalize) {
        const QString once  = ClaudeAllowlistDialog::normalizeRule(c.input);
        const QString twice = ClaudeAllowlistDialog::normalizeRule(once);
        if (once != twice) {
            std::fprintf(stderr,
                "[norm/idempotent:%s] once='%s' twice='%s'  FAIL\n",
                c.label, qUtf8Printable(once), qUtf8Printable(twice));
            ++failures;
        }
    }
    return failures;
}

int checkGeneralize() {
    int failures = 0;
    for (const auto &c : kGeneralize) {
        const QString got = ClaudeAllowlistDialog::generalizeRule(c.input);
        const bool ok = (got == QString::fromUtf8(c.expected));
        std::fprintf(stderr,
                     "[gen/ %-24s] input='%s' got='%s' want='%s'  %s\n",
                     c.label, c.input, qUtf8Printable(got), c.expected,
                     ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
    return failures;
}

struct SubCase {
    const char *label;
    const char *broad;
    const char *narrow;
    bool expected;
};

// `ruleSubsumes` contract — asserts segment-aware semantics. The key
// regressions this guards against:
//   (a) A simple "Bash(cmd *)" rule must NOT be reported as subsuming a
//       compound narrow rule whose segments don't all start with "cmd " —
//       that was the 0.6.22 bug where "Bash(make *)" wrongly blocked
//       "Bash(make * | tail * && ctest * | tail *)" from being added.
//   (b) Simple-vs-simple behavior unchanged: prefix match still subsumes.
//   (c) Compound narrow where EVERY segment does match the broad prefix
//       is legitimately subsumed (e.g. Bash(git *) covers
//       Bash(git status && git push)).
const SubCase kSubsume[] = {
    // (b) Simple subsumption — preserved from pre-0.6.23 behavior.
    {"simple-prefix",          "Bash(git *)",     "Bash(git status)",           true},
    {"simple-exact-prefix",    "Bash(git *)",     "Bash(git)",                  true},
    {"simple-different-cmd",   "Bash(git *)",     "Bash(make test)",            false},
    {"bare-tool-any",          "Read",            "Read(//etc/passwd)",         true},
    {"bare-tool-diff",         "Read",            "Write(//tmp/x)",             false},
    {"path-glob",              "Read(//etc/**)",  "Read(//etc/passwd)",         true},
    {"path-glob-diff-prefix",  "Read(//etc/**)",  "Read(//tmp/x)",              false},

    // (c) Compound narrow, every segment covered → legitimately subsumed.
    {"compound-all-git-and",   "Bash(git *)",
     "Bash(git status && git push)",                                            true},
    {"compound-all-git-pipe",  "Bash(git *)",
     "Bash(git log | git show)",                                                true},
    {"compound-all-make-semi", "Bash(make *)",
     "Bash(make build ; make test)",                                            true},

    // (a) THE HEADLINE BUG: simple broad does NOT subsume compound narrow
    // when any segment diverges. The exact 0.6.22 reproduction case:
    {"reproduction-case",      "Bash(make *)",
     "Bash(make * | tail * && ctest * | tail *)",                               false},
    {"compound-pipe-diverge",  "Bash(make *)",
     "Bash(make x | tail y)",                                                   false},
    {"compound-and-diverge",   "Bash(make *)",
     "Bash(make x && echo done)",                                               false},
    {"compound-or-diverge",    "Bash(make *)",
     "Bash(make x || echo failed)",                                             false},
    {"compound-semi-diverge",  "Bash(git *)",
     "Bash(git status ; make test)",                                            false},

    // Compound broad: err on the side of false (safe) — matcher semantics
    // for segment-wise compound-vs-compound aren't modeled yet. A false
    // negative means at worst a redundant rule; a false positive blocks
    // the user from adding a rule they need.
    {"compound-broad-skip",    "Bash(make * && echo *)",
     "Bash(make test && echo done)",                                            false},
};

int checkSubsume() {
    int failures = 0;
    for (const auto &c : kSubsume) {
        const bool got = ClaudeAllowlistDialog::ruleSubsumes(
            QString::fromUtf8(c.broad), QString::fromUtf8(c.narrow));
        const bool ok = (got == c.expected);
        std::fprintf(stderr,
                     "[sub/ %-24s] broad='%s' narrow='%s' got=%s want=%s  %s\n",
                     c.label, c.broad, c.narrow,
                     got ? "true" : "false",
                     c.expected ? "true" : "false",
                     ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
    return failures;
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    int failures = 0;
    failures += checkNormalize();
    failures += checkGeneralize();
    failures += checkSubsume();

    if (failures > 0) {
        std::fprintf(stderr, "\n%d case(s) failed.\n", failures);
        return 1;
    }
    return 0;
}
