// ANTS-1677 auditdialog piece 2/5 — the check catalogue
#include "auditdialog.h"
#include "auditdialog_internal.h"
#include "auditautofix.h"
#include "auditfpledger.h"
#include "audithygiene.h"
#include "debtsweepengine.h"
#include "llmclient.h"
#include "dialogchrome.h"
#include "tooldetectionengine.h"
#include "config.h"
#include "secretredact.h"
#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QNetworkReply>
#include <QUrl>

using namespace auditdialogdetail;

// ---------------------------------------------------------------------------
// Check catalogue
// ---------------------------------------------------------------------------

void AuditDialog::populateChecks() {
    const bool isQt = m_detectedTypes.contains("Qt");

    // ========== General ==========
    addGrepCheck("todo_scan", "TODO / FIXME Scanner",
                 "Find TODO, FIXME, HACK, XXX annotations", "General",
                 "'(TODO|FIXME|HACK|XXX)(\\(|:|\\s)'",
                 CheckType::Info, Severity::Info, true);

    addFindCheck("large_files", "Large File Finder",
                 "Files larger than 500 KB", "General",
                 "-type f -size +500k -exec ls -lh {} \\;"
                 " | awk '{print $5, $9}' | sort -rh | head -30",
                 CheckType::Info, Severity::Info, true);

    addFindCheck("line_stats", "Line Count Statistics",
                 "Lines of code by file (top 25)", "General",
                 "-type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
                 " -o -name '*.py' -o -name '*.js' -o -name '*.ts' -o -name '*.go'"
                 " -o -name '*.rs' -o -name '*.sh' -o -name '*.lua' -o -name '*.java' \\)"
                 " | xargs wc -l | sort -rn | head -25",
                 CheckType::Info, Severity::Info, false);

    m_checks.append({
        "readme_check", "README & License Check",
        "Verify documentation files exist", "General",
        "echo '=== README ===' && { f=$(ls README* readme* 2>/dev/null);"
        " [ -n \"$f\" ] && echo \"$f\" || echo 'No README found'; }"
        " && echo '=== LICENSE ===' && { f=$(ls LICENSE* license* COPYING* 2>/dev/null);"
        " [ -n \"$f\" ] && echo \"$f\" || echo 'No LICENSE found'; }",
        CheckType::Info, Severity::Info, {}, false, true, nullptr
    });

    // Self-consistency: every addGrepCheck() id in the AuditDialog sources —
    // src/auditdialog.cpp and src/auditdialog_*.cpp — must have a fixture
    // directory under tests/audit_fixtures/<id>/. Catches new rules merged
    // without regression coverage — the exact gap that slipped through in
    // 0.6.5 (todo_scan / format_string / hardcoded_ips / weak_crypto shipped
    // for a cycle with no fixtures). No-op on projects without
    // src/auditdialog.cpp. ANTS-1677: where that file exists and the grep
    // yields no ids, the check reports it rather than passing in silence.
    //
    // Shape: enumerate `addGrepCheck("<id>", …)` lines, dedup by id (awk
    // '!seen[$1]++' keeps the first occurrence deterministically — some rules
    // have two calls for the Qt/non-Qt branches), and report each id without
    // a matching fixture dir. Output uses the parseFindings()-friendly
    // `file:line: message` shape so findings link directly to the registration
    // site.
    m_checks.append({
        "audit_fixture_coverage", "Audit Rule Fixture Coverage",
        "addGrepCheck() rules missing tests/audit_fixtures/<id>/", "General",
        "[ -f src/auditdialog.cpp ] || exit 0; "
        "found=$(grep -HnE 'addGrepCheck\\(\"[a-zA-Z_][a-zA-Z0-9_-]*\"' "
        "    src/auditdialog.cpp src/auditdialog_*.cpp 2>/dev/null); "
        "if [ -z \"$found\" ]; then "
        "    echo 'src/auditdialog.cpp:1: no addGrepCheck() rule ids found in "
        "src/auditdialog.cpp or src/auditdialog_*.cpp'; "
        "    exit 0; "
        "fi; "
        "printf '%s\\n' \"$found\" "
        "| while IFS= read -r entry; do "
        "    file=\"${entry%%:*}\"; rest=\"${entry#*:}\"; lineno=\"${rest%%:*}\"; "
        "    id=$(printf '%s' \"$entry\" "
        "         | sed -n 's/.*addGrepCheck(\"\\([a-zA-Z_][a-zA-Z0-9_-]*\\)\".*/\\1/p'); "
        "    [ -z \"$id\" ] && continue; "
        "    printf '%s\\t%s\\t%s\\n' \"$id\" \"$file\" \"$lineno\"; "
        "  done "
        "| awk '!seen[$1]++' "
        "| while IFS=$'\\t' read -r id file lineno; do "
        "    [ -d \"tests/audit_fixtures/$id\" ] && continue; "
        "    printf '%s:%s: rule \"%s\" has no fixture directory "
        "(tests/audit_fixtures/%s/)\\n' \"$file\" \"$lineno\" \"$id\" \"$id\"; "
        "  done",
        CheckType::CodeSmell, Severity::Minor, {}, true, true, nullptr
    });

    // ========== Feature-coverage lanes ==========
    //
    // Each of the three lanes below addresses a distinct "feature-e2e
    // quality" gap that unit tests and static analysis won't catch:
    //   1. Spec referencing code that was renamed/removed (drift)
    //   2. CHANGELOG advertising features that shipped without a test
    //   3. Tests that are skipped/disabled/xfailed — silently green
    // Self-disable cleanly on projects that don't use the convention.
    //
    // Lane 1 — spec ↔ code drift. For each tests/features/*/spec.md,
    // extract backtick-fenced identifier tokens and verify each still
    // exists somewhere under src/. A rename or deletion that didn't
    // update the spec shows up here at commit time.
    {
        AuditCheck c;
        c.id          = "spec_code_drift";
        c.name        = "Spec ↔ Code Drift";
        c.description = "Identifiers referenced in tests/features/*/spec.md that no longer appear in src/";
        c.category    = "General";
        c.type        = CheckType::CodeSmell;
        c.severity    = Severity::Minor;
        c.autoSelect  = true;
        c.available   = true;
        c.inProcessRunner = &AuditDialog::runSpecDriftCheck;
        m_checks.append(std::move(c));
    }

    // Lane 1b — contract-doc ↔ code literal drift (ANTS-3600). Back-ticked
    // literals in a contract-doc directory that no longer appear in project
    // sources. Human-triage (Minor), never auto-fixed, non-blocking.
    //
    // ANTS-3849 — TWO lanes, one per directory, deliberately not one over
    // both. Measured: the specs half ran ~1,100 findings against the
    // standards half's ~64, so a shared category was over 80% of the whole
    // report and nobody read it. Splitting does not suppress anything — both
    // lanes still report — it makes the small half legible on its own.
    //
    // Both are left UNCAPPED (maxLines = 0): the FP-heavier docs/specs corpus
    // exceeds the default 100-line cap, which would silently drop every
    // alphabetically-later doc's findings (docs/specs/ANTS-3600.md § 2.2).
    {
        AuditCheck c;
        c.id          = "contract_doc_drift_standards";
        c.name        = "Contract-Doc ↔ Code Drift (standards)";
        c.description = "Back-ticked literals in docs/standards that no longer appear in project sources";
        c.category    = "General";
        c.type        = CheckType::CodeSmell;
        c.severity    = Severity::Minor;
        c.autoSelect  = true;
        c.available   = true;
        c.filter.maxLines = 0;   // UNCAPPED — see § 2.2 / INV-10
        c.inProcessRunner = &AuditDialog::runContractDocDriftStandardsCheck;
        m_checks.append(std::move(c));
    }
    {
        AuditCheck c;
        c.id          = "contract_doc_drift_specs";
        c.name        = "Contract-Doc ↔ Code Drift (specs)";
        c.description = "Back-ticked literals in docs/specs that no longer appear in project sources";
        c.category    = "General";
        c.type        = CheckType::CodeSmell;
        c.severity    = Severity::Minor;
        c.autoSelect  = true;
        c.available   = true;
        c.filter.maxLines = 0;   // UNCAPPED — see § 2.2 / INV-10
        c.inProcessRunner = &AuditDialog::runContractDocDriftSpecsCheck;
        m_checks.append(std::move(c));
    }

    // Lane 2 — CHANGELOG bullet ↔ feature-test coverage. Scans the top
    // `## [x.y.z]` section of CHANGELOG.md for Added/Fixed bullets that
    // don't plausibly match any tests/features/*/spec.md title. Info-
    // severity because the match is fuzzy — surfaces release-note claims
    // without a locking test, without hard-gating.
    {
        AuditCheck c;
        c.id          = "changelog_test_coverage";
        c.name        = "CHANGELOG ↔ Feature Tests";
        c.description = "Top-section Added/Fixed bullets without a matching tests/features/*/spec.md";
        c.category    = "General";
        c.type        = CheckType::Info;
        c.severity    = Severity::Info;
        c.autoSelect  = true;
        c.available   = true;
        c.inProcessRunner = &AuditDialog::runChangelogCoverageCheck;
        m_checks.append(std::move(c));
    }

    // Lane 3 — test-health. Surfaces skipped / disabled / xfail / only-
    // blocks inside tests/ subtrees. An "all green" suite with one of
    // these sprinkled in is silently lying about coverage. Runs through
    // a plain grep so the rule pattern is inspectable and fixture-
    // testable via tests/audit_fixtures/test_health/ later.
    //
    // Covers the canonical markers across C++ (GTEST_SKIP, QSKIP),
    // Python (pytest.mark.skip/xfail, unittest.skip/skipIf),
    // Java/Kotlin (@Disabled, @Ignore), Jest/Mocha (it.skip, it.only,
    // describe.skip, xdescribe, fdescribe, xit, fit). The regex is
    // deliberately anchored — bare `skip` in prose won't trip it.
    m_checks.append({
        "test_health", "Test-Health (skipped / disabled / only)",
        "Markers that silently reduce coverage: skipped, disabled, xfail, .only blocks",
        "General",
        "for d in tests test spec __tests__ Tests; do "
        "  [ -d \"$d\" ] || continue; "
        "  grep -rnIE"
        " --include='*.cpp' --include='*.h' --include='*.c' --include='*.hpp'"
        " --include='*.py' --include='*.js' --include='*.ts' --include='*.tsx'"
        " --include='*.go' --include='*.rs' --include='*.java' --include='*.kt'"
        " --include='*.scala' --include='*.rb'"
        + kGrepExcl +
        " '(GTEST_SKIP|QSKIP|@Disabled|@Ignore|pytest\\.mark\\.(skip|xfail)|unittest\\.skip|skipIf\\(|skipUnless\\(|[[:space:]\\.]it\\.(skip|only)[[:space:]\\(]|[[:space:]\\.]describe\\.(skip|only)[[:space:]\\(]|\\bxdescribe\\(|\\bfdescribe\\(|\\bxit\\(|\\bfit\\(|\\btodo\\.only)'"
        " \"$d\" 2>/dev/null; "
        "done",
        CheckType::CodeSmell, Severity::Minor, OutputFilter{}, true, true, nullptr
    });

    // Observability: `catch (...) { }` with no logging / rethrow silently
    // swallows errors, masking bugs that only manifest as "nothing happened".
    // Conservative first cut: flag the *empty-body* same-line form only.
    // Matches `catch (...) {}`, `catch (const E& e) { }`, etc. Single-statement
    // trivial bodies (`catch (...) { return -1; }`) are out of scope for this
    // pattern — extending requires multi-line regex plumbing (`grep -Pzo`)
    // that addGrepCheck doesn't expose today. See ROADMAP 0.7 §Dev experience.
    addGrepCheck("silent_catch", "Silent catch(...) Handler",
                 "Empty catch blocks swallow exceptions without logging or rethrow",
                 "General",
                 "'catch\\s*\\([^)]*\\)\\s*\\{\\s*\\}'",
                 CheckType::CodeSmell, Severity::Minor, true);

    // Build-flag recommender. Nudges toward better compile-time coverage by
    // flagging absence of battle-tested warning flags in the top-level
    // CMakeLists.txt. Severity Minor — missing flags aren't bugs, just missed
    // opportunities. Anchors at line 1 (grep doesn't locate the exact hit,
    // and the finding is file-level anyway). No-op on projects without a
    // CMakeLists.txt (Meson, Bazel, plain Makefile, etc.).
    //
    // Comment-line filter (`grep -v '^[[:space:]]*#'`) is crucial — any
    // CMakeLists that *discusses* a flag in a comment (for instance ours,
    // explaining why -Wconversion is disabled) would otherwise cause a false
    // negative. We strip comment-only lines before scanning.
    m_checks.append({
        "missing_build_flags", "Missing Compiler Warning Flags",
        "CMakeLists.txt lacks recommended -W flags for compile-time coverage",
        "General",
        "f=CMakeLists.txt; [ -f \"$f\" ] || exit 0; "
        "code=$(grep -v '^[[:space:]]*#' \"$f\"); "
        "printf '%s' \"$code\" | grep -qE -- '-Wformat=2\\b' || "
        "  printf '%s:1: recommended compiler flag missing: -Wformat=2\\n' \"$f\"; "
        "printf '%s' \"$code\" | grep -qE -- '-Wshadow\\b' || "
        "  printf '%s:1: recommended compiler flag missing: -Wshadow (or -Wshadow=local)\\n' \"$f\"; "
        "printf '%s' \"$code\" | grep -qE -- '-Wnull-dereference\\b' || "
        "  printf '%s:1: recommended compiler flag missing: -Wnull-dereference\\n' \"$f\"; "
        "printf '%s' \"$code\" | grep -qE -- '-Wconversion\\b' || "
        "  printf '%s:1: recommended compiler flag missing: -Wconversion\\n' \"$f\"",
        // Info tier (not Minor): this is a build-hygiene recommendation,
        // not a bug. The 2026-04-16 triage flagged leaving it at Minor
        // as "severity leak" because it shared the tier with real smells.
        CheckType::Info, Severity::Info, {}, true, true, nullptr
    });

    // 0.6.22 FP-reduction heuristic — CMake find_package without a version
    // floor is a common hygiene miss: `find_package(Qt6 REQUIRED ...)` accepts
    // any Qt6 >= 6.0, which lets rolling distros or multi-install environments
    // silently pick up an incompatible minor. Match `find_package(<Pkg>
    // REQUIRED ...)` where *no* version string sits between the package name
    // and REQUIRED. Scoped to CMakeLists*.txt only — unrelated files named
    // find_package() happen to contain the word mean nothing.
    //
    // Precision: the regex requires `REQUIRED` to follow the package name with
    // only whitespace between. If a numeric version is present (`Qt6 6.2
    // REQUIRED`) it breaks the match — so we correctly distinguish the pinned
    // from unpinned forms without needing a negative lookbehind. Tested
    // against both Qt6 and non-Qt calls (LLVM, Boost, Protobuf) in the wild.
    m_checks.append({
        "cmake_no_version_floor", "CMake find_package Without Version Floor",
        "find_package(<Pkg> REQUIRED) with no version constraint — may pick "
        "up incompatible minor on rolling distros", "Build",
        "for f in CMakeLists.txt $(find . -name 'CMakeLists.txt' -not -path '*/build*' -not -path '*/.git/*' 2>/dev/null); do "
        "  [ -f \"$f\" ] || continue; "
        "  grep -nE 'find_package\\s*\\(\\s*[A-Za-z_][A-Za-z0-9_]*\\s+REQUIRED' \"$f\"; "
        "done | sort -u",
        CheckType::CodeSmell, Severity::Minor, {}, false, true, nullptr
    });

    // 0.6.22 — `bash -c` / `sh -c` with a non-literal argument is a classic
    // command-injection sink whenever the argument incorporates user-controlled
    // data (project paths, config file fields, commit messages, clipboard).
    // Match the typical Qt pattern `QProcess::start(..., {"-c", <ident>})` and
    // the C-level `execl("/bin/sh", ..., "-c", <ident>)` — both funnel into
    // the same risk. A string literal after `-c` is safe (the command is
    // hard-coded); an identifier/expression is the red flag.
    //
    // Tradeoff: catches legitimate cases where the non-literal has already
    // been sanitised (allowlist-validated, shell-escaped, etc). Severity Minor
    // signals "review needed, not auto-fix"; the grep-noise filter strips
    // test/example files.
    // bash -c / sh -c with non-literal — 2026-04-16 triage saw the one
    // finding (mainwindow.cpp:3065 on `actionValue` from `auto_profile_rules`)
    // as a FP: Config is the project's declared trust boundary (STANDARDS.md
    // §Security), and plugin manifests / auto_profile_rules are the same
    // trust tier. Context-window filter suppresses when the non-literal
    // originates from `m_config.` in ±5 lines.
    addGrepCheck("bash_c_non_literal", "bash -c / sh -c With Non-Literal Argument",
                 "Passing a non-literal to shell -c — review for command-injection risk",
                 "Security",
                 "'\\b(bash|sh)\\b[^=]*\"-c\"\\s*,\\s*[A-Za-z_][A-Za-z0-9_]*'",
                 CheckType::CodeSmell, Severity::Minor, false,
                 OutputFilter{
                     /*dropIfContains*/ {},
                     "", {}, 30,
                     /*dropIfContextContains*/ {"m_config.", "m_cfg.",
                                                "Config::instance",
                                                "config()->"},
                     /*contextWindow*/ 5 });

    // 0.6.22 — Cross-file version-drift detector. A release is "done" only
    // when every version-bearing file agrees with the authoritative source
    // (CMakeLists.txt PROJECT VERSION). Historic drift has been a shipping
    // hazard: 0.6.22's packaging commit had to bump four files
    // (spec, PKGBUILD, debian/changelog, README) that had silently lagged
    // three releases behind the CMake floor.
    //
    // 0.6.25 — The ninth audit (2026-04-15) flagged two failures:
    //   1. The rule missed AppStream metainfo (file couples to every
    //      release, was not scanned).
    //   2. The rule ran only when the dev remembered to open the audit
    //      dialog — honor-system enforcement failed for the 0.6.23 tag.
    // Both closed by extracting the check logic into
    // `packaging/check-version-drift.sh` and invoking that script from
    // here AND from `.github/workflows/ci.yml`. The script is the single
    // source of truth; auditdialog and CI can't drift apart from each
    // other any more. The script itself covers AppStream metainfo too.
    //
    // The script emits one finding per drifted file on stdout in the
    // audit `FILE:LINE: message` format, and exits with a count of
    // drifts (capped at 125). The audit dialog's parser ignores the
    // exit code; CI uses the non-zero exit to fail the job. No-op
    // (exit 0, no output) when CMakeLists.txt is absent or unparseable.
    m_checks.append({
        "packaging_version_drift", "Packaging Version Drift",
        "Packaging files out of sync with CMakeLists.txt project(VERSION)",
        "Build",
        "[ -x packaging/check-version-drift.sh ] "
        "  && bash packaging/check-version-drift.sh || exit 0",
        CheckType::CodeSmell, Severity::Minor, {}, false, true, nullptr
    });

    // CI presence. A project with no continuous-integration config ships
    // regressions silently — the rule flags projects missing *all* of the
    // common CI file conventions. Severity Major — not actionable by
    // automated fix, but worth a loud "you have no safety net" warning.
    // Covers the five most common self-hosted/SaaS CI systems; other
    // bespoke setups (Drone, Woodpecker, Buildkite pipelines checked in as
    // `.buildkite/`) can be added on demand via audit_rules.json overrides.
    m_checks.append({
        "no_ci", "Continuous Integration Missing",
        "Project has no CI configuration — regressions may ship silently",
        "General",
        "found=''; "
        "if [ -d .github/workflows ] && ls .github/workflows/*.y*ml >/dev/null 2>&1; then found=.github/workflows; fi; "
        "[ -f .gitlab-ci.yml ] && found=.gitlab-ci.yml; "
        "[ -d .circleci ] && found=.circleci; "
        "[ -f .travis.yml ] && found=.travis.yml; "
        "[ -f Jenkinsfile ] && found=Jenkinsfile; "
        "[ -n \"$found\" ] && exit 0; "
        "printf '.:1: no CI configuration detected "
        "(checked: .github/workflows/, .gitlab-ci.yml, .circleci/, .travis.yml, Jenkinsfile)\\n'",
        CheckType::CodeSmell, Severity::Major, {}, true, true, nullptr
    });

    // 2026-04-17: prefer `git ls-files` over `find` when the project is a
    // git checkout. The raw `find` approach matched gitignored paths
    // (__pycache__, .claude/worktrees/, ...) because kFindExcl is a static
    // list and can't keep up with every project's .gitignore. Shell `||`
    // falls through to the legacy `find` when not in a git repo.
    m_checks.append({
        "dup_files", "Duplicate File Detection",
        "Files with identical content", "General",
        QString("( git ls-files -z 2>/dev/null | tr '\\0' '\\n'"
                " | grep -E '\\.(cpp|h|hpp|hxx|hh|c|py|js|jsx|ts|tsx)$'"
                " || find ." + kFindExcl + " -type f \\( -name '*.cpp'"
                " -o -name '*.h' -o -name '*.hpp' -o -name '*.py'"
                " -o -name '*.js' -o -name '*.ts' \\) 2>/dev/null )"
                " | while read f; do [ -s \"$f\" ] && [ $(wc -c < \"$f\") -gt 100 ]"
                " && md5sum \"$f\"; done"
                " | sort | uniq -Dw32 | head -30"),
        CheckType::CodeSmell, Severity::Minor,
        { {}, "", {}, 30 },
        false, true, nullptr
    });

    addFindCheck("dangling_symlinks", "Dangling Symlinks",
                 "Symlinks pointing to non-existent targets", "General",
                 "-type l ! -exec test -e {} \\; -print | head -30",
                 CheckType::Bug, Severity::Minor, false);

    // Exactly 7 sigil chars anchored at start of line, followed by whitespace
    // or end of line. Without the trailing anchor, section-heading underlines
    // (e.g. `===========` in vendored single-header libraries) get flagged as
    // merge conflicts. Includes `|{7}` for the diff3 merge-base marker.
    addGrepCheck("conflict_markers", "Git Conflict Markers",
                 "Unresolved merge conflicts in source", "General",
                 "'^(<{7}|\\|{7}|={7}|>{7})(\\s|$)'",
                 CheckType::Bug, Severity::Blocker, true);

    // 2026-04-17: prefer `git ls-files` for the same reason as dup_files.
    // The triage on Vestige showed 30/30 binary_in_repo hits were
    // `.pyc` files under gitignored `__pycache__/` — `git ls-files` returns
    // nothing for those paths, killing the false positives at source.
    m_checks.append({
        "binary_in_repo", "Binary Files in Source",
        "Non-text files tracked alongside source", "General",
        QString("( git ls-files 2>/dev/null"
                " || find ." + kFindExcl + " -type f 2>/dev/null )"
                " | grep -E '\\.(exe|dll|dylib|bin|dat|db|sqlite|class|pyc|pyo)$'"
                " | head -30"),
        CheckType::CodeSmell, Severity::Minor,
        { {}, "", {}, 30 },
        false, true, nullptr
    });

    addFindCheck("encoding_check", "Source Encoding Check",
                 "Non-UTF-8 or BOM-prefixed source files", "General",
                 "-type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
                 " -o -name '*.py' -o -name '*.js' -o -name '*.ts' \\)"
                 " -exec file {} \\;"
                 " | grep -viE '(UTF-8|ASCII|empty)' | head -20",
                 CheckType::Bug, Severity::Minor, false);

    // Overly Long Source Files — advisory, not a bug. Info tier so it
    // doesn't share the Minor tier with actual code smells. (2026-04-16
    // triage flagged this as a severity leak.)
    addFindCheck("long_files", "Overly Long Source Files",
                 "Source files exceeding 1000 lines", "General",
                 "-type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
                 " -o -name '*.py' -o -name '*.js' -o -name '*.ts' -o -name '*.go'"
                 " -o -name '*.rs' -o -name '*.java' \\)"
                 " -exec awk 'END{if(NR>1000)print NR\" \"FILENAME}' {} \\;"
                 " | sort -rn | head -20",
                 CheckType::Info, Severity::Info, false);

    // Debug leftovers: separate commands per language, joined with ';' in the shell.
    // Kept inline because the language-specific patterns can't collapse cleanly.
    // Debug / temp code — 2026-04-16 triage saw `qDebug()` calls gated by
    // `if (on)` flagged as FPs. Those are legitimate user-triggered debug-
    // log toggles (Ctrl+Shift+D), not forgotten debug prints. Context-
    // window suppression drops the finding when an obvious debug-gate
    // conditional appears in the enclosing ±8 lines.
    m_checks.append({
        "debug_leftovers", "Debug / Temp Code",
        "console.log, print(), debug statements left in source", "General",
        "grep -rnI" + kGrepExcl +
        " --include='*.js' --include='*.ts' --include='*.jsx' --include='*.tsx'"
        " -E '\\bconsole\\.(log|debug|trace)\\(' . 2>/dev/null | head -20;"
        " grep -rnI" + kGrepExcl + " --include='*.py' -E '^\\s*(print\\(|pdb\\.|breakpoint\\()' . 2>/dev/null | head -20;"
        " grep -rnI" + kGrepExcl + " --include='*.cpp' --include='*.c' --include='*.h'"
        " -E '\\b(qDebug|std::cerr|std::cout)\\s*(<{2}|\\()' . 2>/dev/null | head -20",
        CheckType::CodeSmell, Severity::Minor,
        OutputFilter{
            /*dropIfContains*/ {"//", "/*", "error", "warn", "fatal"},
            "", {}, 80,
            /*dropIfContextContains*/ {"if (m_debug", "if (debug",
                                       "if (on)", "if (verbose",
                                       "if (m_verbose", "if (log_",
                                       "#ifdef DEBUG", "#ifdef NDEBUG",
                                       "#if DEBUG_"},
            /*contextWindow*/ 8 },
        false, true, nullptr
    });

    // ========== Security ==========

    // 0.6.22 — Trivy filesystem scanner. Three lanes in one invocation:
    //   * vuln       — dependency CVEs (lockfiles, SBOM-derivable manifests)
    //   * secret     — high-entropy + signature-based secret detection
    //   * misconfig  — Dockerfile / k8s / Terraform / GitHub Actions hardening
    //                  rules
    //
    // Self-disables when trivy isn't on PATH. JSON output piped to jq for
    // a flat `path:line: severity: scanner/rule-id: title` format that
    // parseFindings() can consume directly. Severity floor MEDIUM keeps the
    // signal/noise reasonable for a generalist scanner; CRITICAL trivy hits
    // (real CVE in a pinned dep, hardcoded production secret) are typically
    // worth dropping everything for.
    //
    // The jq filter uses `.Title // .Description // "(no description)"` for
    // vulnerabilities because Trivy occasionally omits Title for low-noise
    // CVEs but always populates Description. Empty-result skip via the `?`
    // suffix on each array selector means a clean tree produces no output
    // (rather than `null` or "{}" lines that would parse as fake findings).
    const bool hasTrivy = toolExists("trivy");
    const bool hasJq    = toolExists("jq");
    // ANTS-1709: --skip-dirs comes from AuditEngine::trivySkipDirsCsv()
    // (build glob included), so trivy can no longer scan build-fast /
    // build-workstation the way the old static list silently did.
    const QString trivyCmd =
        QStringLiteral("trivy fs --quiet --scanners vuln,secret,misconfig "
                       "--format json --severity MEDIUM,HIGH,CRITICAL --skip-dirs ")
        + AuditEngine::trivySkipDirsCsv()
        + QString::fromLatin1(R"TRIVY( . 2>/dev/null | jq -r '.Results[]? | .Target as $f | (.Vulnerabilities[]? | "\($f):1: \(.Severity): vuln/\(.VulnerabilityID): \(.Title // .Description // "(no description)")"), (.Secrets[]? | "\($f):\(.StartLine): \(.Severity): secret/\(.RuleID): \(.Title)"), (.Misconfigurations[]? | "\($f):1: \(.Severity): misconfig/\(.ID): \(.Title)")' 2>/dev/null | head -100)TRIVY");
    const QString trivyDesc = (hasTrivy && hasJq)
        ? QString("Filesystem-wide vuln + secret + misconfig scan (MEDIUM+)")
        : (!hasTrivy
            ? QString("(trivy not installed — zypper in trivy)")
            : QString("(jq not installed — needed to parse Trivy JSON; zypper in jq)"));
    m_checks.append({
        "trivy_fs", "Trivy Filesystem Scan", trivyDesc, "Security",
        trivyCmd,
        CheckType::Vulnerability, Severity::Major,
        { /*dropIfContains*/ {}, "", {}, 100 },
        hasTrivy && hasJq,   // auto-select when usable
        hasTrivy && hasJq,   // available
        nullptr
    });

    // Hardcoded secrets — the 2026-04-16 triage showed the old regex
    // `(api_key|password|...)\s*[:=]` matched variable-name references
    // like `m_aiApiKey = new QLineEdit(tab)` or `m_apiKey = apiKey;`.
    // The fix: require the RHS to be a quoted string literal of at least
    // 16 characters. Real secrets are long, opaque strings; variable
    // assignments and pointer constructors never have that shape.
    //
    // The regex allows both "…" and '…' quotes, YAML-style unquoted
    // (`api_key: ghp_...`) single tokens of 16+ non-space chars, and JSON
    // `"api_key": "…"` form. Placeholder strings like `"changeme"` and
    // `"YOUR_KEY_HERE"` remain on the dropIfContains list.
    addGrepCheck("secrets_scan", "Hardcoded Secrets Scan",
                 "API keys / passwords / tokens as literal strings (≥16 chars)",
                 "Security",
                 "'(api[_-]?key|password|secret[_-]?key|auth[_-]?token|credentials)"
                 "\\s*[:=]\\s*(\"[^\"]{16,}\"|'\\''[^'\\'']{16,}'\\''"
                 "|[^[:space:]\"'\\''#,]{16,})'",
                 CheckType::Hotspot, Severity::Critical, true,
                 { /*dropIfContains*/ {"EchoMode", "setPlaceholder",
                                       "// example", "# example",
                                       "Config::set", "setAiApiKey",
                                       "keybinding", "Keybinding",
                                       "const char",  // string literal pattern names
                                       "description", "Description",
                                       "changeme", "YOUR_", "xxxxxxxx",
                                       "placeholder", "example.com",
                                       // ANTS-1710 — reading a secret FROM the
                                       // environment / keychain is the safe
                                       // idiom, not a hardcoded literal. The
                                       // call expression trips the ≥16-char arm.
                                       "getenv", "qEnvironmentVariable",
                                       "qgetenv", "secretFromKeychain"},
                   "", {}, 50 },
                 {"-i",
                  "--include='*.json' --include='*.yaml' --include='*.yml'",
                  "--include='*.toml' --include='*.cfg' --include='*.ini'"});

    if (isPosixFilesystem()) {
        // 2026-04-17: dropped `-perm -020` (group-writable). Mode 664 is
        // the default umask result on most Linux distros — flagging every
        // file as a security finding produced 30/30 false positives in the
        // 2026-04-16 Vestige triage. Keep only true world-writable
        // (`o+w`, mask &002), which is the actual security concern.
        addFindCheck("file_perms", "World-Writable Files",
                     "World-writable files (mode o+w, CWE-732)", "Security",
                     "-type f -perm -002 | head -30",
                     CheckType::Vulnerability, Severity::Major, false);
    } else {
        // On non-POSIX filesystems every file appears world-writable because
        // the mount maps all Unix perms to 0777. Running the check produces
        // ~every file in the tree as a "finding" — noise with no signal.
        // Emit an INFO placeholder that explains the skip instead.
        const QString fs = m_projectFsType.isEmpty() ? "(unknown)" : m_projectFsType;
        const QString msg =
            QString("Scan skipped: project root is on filesystem '%1', which "
                    "does not enforce POSIX permissions. All files would "
                    "appear world-writable regardless of intent. Re-run on "
                    "an ext4/xfs/btrfs/apfs/zfs mount to audit permissions.")
                .arg(fs);
        // `printf` keeps the message on a single output line so it parses
        // cleanly as one Finding rather than being split per newline.
        m_checks.append({
            "file_perms", "World-Writable Files",
            "Skipped on non-POSIX filesystem", "Security",
            "printf '%s\\n' " + QString("\"%1\"").arg(msg),
            CheckType::Info, Severity::Info,
            { /*dropIfContains*/ {}, "", {}, 0 },
            false, true, nullptr
        });
    }

    // Unsafe C functions — tightened. sprintf/strtok are flagged but common
    // safe uses (format-string literal) are filtered out in-app.
    addGrepCheck("unsafe_c_funcs", "Unsafe C/C++ Functions",
                 "strcpy, gets, mktemp, etc.", "Security",
                 "'\\b(strcpy|strcat|sprintf|vsprintf|gets|mktemp|tmpnam|scanf)\\s*\\('",
                 CheckType::Vulnerability, Severity::Major, true,
                 { /*dropIfContains*/ {"QString::sprintf", "qsnprintf",
                                       "snprintf(", "// safe", "/*safe"},
                   "", {}, 50 });

    // Command injection: process-exec APIs. Whitelist legit cases:
    //   - menu.exec / app.exec / dialog.exec are Qt event-loop calls
    //   - execlp inside forkpty child is a terminal-emulator requirement
    //   - QProcess startDetached with an args list is safe
    // Command injection — 2026-04-16 triage saw `execlp(shellCStr, argv0,
    // nullptr)` in ptyhandler.cpp:117 (login-shell spawn) flagged as a FP.
    // That canonical shape — `exec*(prog, ..., nullptr)` with a literal
    // null terminator — is NOT a shell command string and so can't carry
    // an injected shell metacharacter. Add `, nullptr)` and `, NULL)` as
    // drop markers for the exec family. system()/popen() with dynamic
    // args remain hot — those are the real injection vectors.
    addGrepCheck("cmd_injection", "Command Injection Patterns",
                 "system(), popen(), exec*() with dynamic arguments", "Security",
                 "'\\b(system|popen|execlp|execvp|execl|execv|execle)\\s*\\('",
                 CheckType::Hotspot, Severity::Critical, true,
                 { /*dropIfContains*/ {".exec(", "app.exec", "menu.exec",
                                       "dialog.exec", "QApplication",
                                       "QProcess", "forkpty", "setArguments",
                                       ", nullptr)", ", NULL)"},
                   "", {}, 30 });

    // Python/JS subprocess patterns — separate check
    m_checks.append({
        "cmd_injection_dyn", "Dynamic Process Spawn",
        "subprocess shell=True, child_process.exec, os.system", "Security",
        "grep -rnI" + kGrepExclSec + " --include='*.py' -E "
        "'(subprocess\\.(call|run|Popen).*shell\\s*=\\s*True|os\\.system)' . 2>/dev/null | head -20;"
        " grep -rnI" + kGrepExclSec + " --include='*.js' --include='*.ts'"
        " -E 'child_process\\.exec[^F]' . 2>/dev/null | head -20",
        CheckType::Vulnerability, Severity::Critical,
        { {}, "", {}, 40 },
        true, true, nullptr
    });

    addGrepCheck("format_string", "Format String Risks",
                 "printf-family with non-literal format argument", "Security",
                 "'\\b[fs]?n?printf\\s*\\([^\"]*\\b\\w+\\s*\\)'",
                 CheckType::Vulnerability, Severity::Major, false,
                 { /*dropIfContains*/ {"printf(\"", "fprintf(stderr, \"",
                                       "snprintf(", "QString::", "DBGLOG"},
                   "", {}, 30 });

    // insecure_http — match every `http://` and lean on the OutputFilter
    // (dropIfContains: localhost / 127.0.0.1 / example.com / schema URLs)
    // for the exclusions. The old `http://[^l][^o][^c]` positional-class
    // hack tried to bake the localhost exclusion into the ERE itself, but it
    // over-excluded any l-/o-/c-positioned host (`http://logging…`,
    // `http://login…`) as a false negative — and a PCRE `(?!localhost)`
    // lookahead is unavailable because the check runs under `grep -nE`
    // (POSIX ERE). So localhost lives in dropIfContains, not the regex
    // (ANTS-1612). dropIfContextContains still suppresses a scheme-gate
    // `startsWith("http…")` within ±5 lines (the 2026-04-16 triage's one
    // surviving finding was such a guard).
    addGrepCheck("insecure_http", "Insecure HTTP URLs",
                 "http:// in config / source (not schema / localhost)", "Security",
                 "'http://'",
                 CheckType::Hotspot, Severity::Minor, true,
                 OutputFilter{
                   /*dropIfContains*/ {"localhost", "127.0.0.1", "0.0.0.0",
                                       "example.com", "// comment", "placeholder",
                                       // Schema / namespace URLs
                                       "json-schema.org", "www.w3.org",
                                       "schemas.xmlsoap.org", "tempuri.org",
                                       "xmlns", "namespace", "XSD",
                                       // ANTS-1710 — license-header / spec URLs
                                       // are documentation, not live endpoints;
                                       // they appear in nearly every source tree
                                       // and were the dominant insecure_http FP.
                                       "apache.org/licenses", "gnu.org/licenses",
                                       "creativecommons.org",
                                       "opensource.org/licenses",
                                       "mozilla.org/MPL", "purl.org",
                                       "docbook.org", "oasis-open.org",
                                       "schemas.android.com"},
                   "", {}, 30,
                   /*dropIfContextContains*/ {"startsWith(\"http",
                                              "startsWith(QStringLiteral(\"http",
                                              "startsWith(QLatin1String(\"http"},
                   /*contextWindow*/ 5 },
                 {"--include='*.json' --include='*.yaml' --include='*.yml'",
                  "--include='*.toml' --include='*.xml' --include='*.cfg'"});

    m_checks.append({
        "unsafe_deser", "Unsafe Deserialization",
        "eval(), pickle.loads, yaml.load without SafeLoader", "Security",
        "grep -rnI" + kGrepExclSec + " --include='*.py' -E "
        "'\\b(pickle\\.loads?|yaml\\.load|marshal\\.loads?|eval)\\s*\\(' . 2>/dev/null"
        " | grep -viE '(SafeLoader|safe_load|ast\\.literal_eval)' | head -20;"
        " grep -rnI" + kGrepExclSec + " --include='*.js' --include='*.ts'"
        " -E '\\beval\\s*\\(' . 2>/dev/null | head -20;"
        " grep -rnI" + kGrepExclSec + " --include='*.php' -E '\\b(unserialize|eval)\\s*\\(' . 2>/dev/null | head -20",
        CheckType::Vulnerability, Severity::Critical,
        { {}, "", {}, 40 },
        true, true, nullptr
    });

    // hardcoded_ips: drop version strings, license dates, checksums.
    // ANTS-1710 — each group is constrained to a valid octet (0-255). A
    // dotted-quad with a component >255 (e.g. a build number "1.2.300.4")
    // cannot be an IPv4 address, so the old `[0-9]{1,3}` form false-fired on
    // that class. Real IPs always have octets <=255, so true-positive
    // coverage is unchanged. Version strings whose components stay <=255 are
    // still caught at the OutputFilter (version/Version context drop) below.
    addGrepCheck("hardcoded_ips", "Hardcoded IPs & Ports",
                 "IPv4 literals in source", "Security",
                 "'\\b((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}"
                 "(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\b'",
                 CheckType::Hotspot, Severity::Minor, false,
                 { /*dropIfContains*/ {"127.0.0.1", "0.0.0.0", "255.255",
                                       "example", "version", "license",
                                       "Version", "Copyright"},
                   // Drop likely version strings: e.g. "v1.2.3.4" or "1.2.3.4"
                   // following a letter/VersionString keyword.
                   R"(version|Version|VERSION)", {}, 20 });

    addFindCheck("env_files", "Exposed Environment Files",
                 ".env, credentials, key files in repo", "Security",
                 "-type f \\( -name '.env' -o -name '.env.*' -o -name 'credentials'"
                 " -o -name '*.pem' -o -name '*.key' -o -name '*.p12' -o -name '*.pfx'"
                 " -o -name 'id_rsa' -o -name 'id_ed25519' -o -name '*.keystore' \\)"
                 " | head -20",
                 CheckType::Vulnerability, Severity::Blocker, true);

    addFindCheck("temp_files", "Temporary/Backup Files",
                 "Editor backups, OS metadata leaked into repo", "Security",
                 "-type f \\( -name '*.swp' -o -name '*.swo' -o -name '*~'"
                 " -o -name '.DS_Store' -o -name 'Thumbs.db' -o -name '*.bak'"
                 " -o -name '*.orig' -o -name '*.tmp' \\)"
                 " | head -20",
                 CheckType::CodeSmell, Severity::Minor, false);

    // Weak crypto — tightened to avoid file-integrity / cache-key false positives.
    addGrepCheck("weak_crypto", "Weak Cryptography",
                 "MD5, SHA1, DES, RC4, ECB mode usage", "Security",
                 "'\\b(md5|sha1|des|rc4|ecb)\\b'",
                 CheckType::Vulnerability, Severity::Major, false,
                 { /*dropIfContains*/ {"git", "checksum", "Checksum",
                                       "hash file", "file hash",
                                       "integrity", "etag", "ETag",
                                       "cache key", "cacheKey",
                                       "content hash", "contentHash",
                                       "md5sum"},
                   "", {}, 30 });

    // ssh_argv_dash_host — CVE-2017-1000117 class: when ssh argv is constructed
    // by shellQuoting a user-controlled host token, a host that begins with "-"
    // is parsed by ssh(1) as an option (e.g. "-oProxyCommand=evil"). The fix
    // is a literal `args << "--"` before the host, which makes ssh stop option
    // parsing. The 0.7.6 hardening pass shipped this guard in sshdialog.cpp;
    // this rule catches the next caller that forgets.
    //
    // Pattern matches the ssh-bookmark shellQuote-of-host shape. Runtime
    // filter drops the finding when `<< "--"` appears in the ±5-line window
    // (the canonical guard). Window is 5 so that a host shellQuote in the
    // `else` branch of an if/else user-prefix split still sees the guard in
    // the preceding enclosing block — e.g. sshdialog.cpp:67-71.
    addGrepCheck("ssh_argv_dash_host", "SSH argv — host without -- terminator",
                 "ssh(1) argv construction that shellQuotes a host token "
                 "without a preceding `--` argv terminator (CVE-2017-1000117 class)",
                 "Security",
                 "'<<\\s*shellQuote\\s*\\([^)]*\\bhost\\b'",
                 CheckType::Vulnerability, Severity::Major, true,
                 OutputFilter{
                   /*dropIfContains*/ {},
                   "", {}, 30,
                   /*dropIfContextContains*/ {"<< \"--\"", "<< QStringLiteral(\"--\")"},
                   /*contextWindow*/ 5 });

    // ========== Git (if applicable) ==========
    if (m_detectedTypes.contains("Git")) {
        m_checks.append({
            "git_status", "Uncommitted Changes",
            "git status --short", "Git", "git status --short 2>/dev/null",
            CheckType::Info, Severity::Info, { {}, "", {}, 0 },
            true, true, nullptr
        });

        m_checks.append({
            "git_stale", "Branch Overview",
            "Merged and unmerged branches", "Git",
            "echo '=== Unmerged ===' && git branch -v --no-merged 2>/dev/null"
            " && echo '=== Merged (can delete) ===' && git branch -v --merged 2>/dev/null | grep -v '^\\*'",
            CheckType::Info, Severity::Info, { {}, "", {}, 0 },
            false, true, nullptr
        });

        m_checks.append({
            "git_large_history", "Large Files in Git History",
            "Blobs > 1 MB ever committed", "Git",
            "git rev-list --objects --all 2>/dev/null"
            " | git cat-file --batch-check='%(objecttype) %(objectsize) %(rest)' 2>/dev/null"
            " | awk '$1==\"blob\" && $2>1048576 {printf \"%.1fMB %s\\n\", $2/1048576, $3}'"
            " | sort -rn | head -20",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 20 },
            false, true, nullptr
        });

        m_checks.append({
            "git_sensitive", "Sensitive Files in Git",
            "Private keys / secrets ever tracked", "Git",
            "git ls-files 2>/dev/null | grep -iE "
            "'(id_rsa|id_ed25519|\\.pem|\\.key|\\.env|credentials|secret|password)'",
            CheckType::Vulnerability, Severity::Critical,
            { {"example", "template", "sample", "test", "mock", "lock", "CLAUDE.md",
               "settings.local.json" /* Claude Code perms, not secrets */}, "", {}, 20 },
            false, true, nullptr
        });
    }

    // ========== C/C++ ==========
    if (m_detectedTypes.contains("C/C++")) {
        const QString qtLib = isQt ? " --library=qt" : "";

        m_checks.append({
            "cppcheck", "cppcheck Static Analysis",
            QString("Warnings, performance, portability%1").arg(isQt ? " (Qt-aware)" : ""),
            "C/C++",
            "cppcheck --enable=warning,performance,portability --quiet --inline-suppr" + qtLib +
            " --suppress=missingInclude --suppress=missingIncludeSystem"
            " --suppress=unmatchedSuppression --suppress=unknownMacro"
            // invalidSuppression is cppcheck's own parser complaining when a
            // doc comment mentions the literal "cppcheck-suppress" token
            // (e.g. our own inline-suppression passthrough docs). It never
            // surfaces a real code bug, only a tool-noise annoyance, so we
            // silence the category globally.
            " --suppress=invalidSuppression"
            // Exclude every build-dir variant via AuditEngine's runtime
            // glob (ANTS-1709 centralisation). cppcheck's -i takes a path
            // prefix and can't glob, so the helper expands `build*` at run
            // time — ANTS-1707 root cause: a static list missed build-fast,
            // so cppcheck scanned build-fast/_deps/googletest-src/ (~53
            // vendored FPs). New presets now self-exclude.
            + AuditEngine::cppcheckIgnoreShellExpr() +
            " -j$(nproc) . 2>&1 | head -100",
            CheckType::Bug, Severity::Major, { {}, "", {}, 100 },
            toolExists("cppcheck"), toolExists("cppcheck"), nullptr
        });

        m_checks.append({
            "cppcheck_unused", "Dead Code Detection",
            "Unused functions (single-threaded scan)", "C/C++",
            "cppcheck --enable=unusedFunction --quiet --inline-suppr" + qtLib +
            " --suppress=missingInclude --suppress=missingIncludeSystem"
            " --suppress=unmatchedSuppression --suppress=unknownMacro"
            " --suppress=invalidSuppression"
            // Same AuditEngine runtime glob as the cppcheck check above
            // (ANTS-1709 centralisation / ANTS-1707 root cause).
            + AuditEngine::cppcheckIgnoreShellExpr() +
            " . 2>&1 | head -50",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 50 },
            false, toolExists("cppcheck"), nullptr
        });

        // clang-tidy — needs a compile_commands.json to resolve Qt
        // system headers. Without one it emits `'QString' file not found`
        // (and 33 more identical lines per Qt header in the TU). The
        // 2026-04-16 triage saw 34/34 findings in this category collapse
        // to that single driver-level configuration error.
        //
        // Fixes applied:
        //   1. Auto-select is gated on presence of compile_commands.json
        //      (reuses the clazy build-dir probe).
        //   2. The shell pipeline collapses `file not found` storms into
        //      a single banner line — if headers still don't resolve for
        //      some reason, the user sees one actionable diagnostic
        //      ("regenerate compile_commands.json") rather than 34.
        const bool hasClangTidy = toolExists("clang-tidy");
        // ANTS-3367 — shared probe (was an inline copy of the build-dir
        // list, now AuditEngine::resolveBuildDir).
        const QString tidyBuildDir = AuditEngine::resolveBuildDir(m_projectPath);
        const QString tidyCmd = tidyBuildDir.isEmpty()
            ? QString("echo 'clang-tidy: no compile_commands.json found — "
                      "build the project first (CMAKE_EXPORT_COMPILE_COMMANDS=ON)'")
            : QString("find . -name '*.cpp'%1 | head -15"
                      " | xargs -I{} clang-tidy -p %2 {} 2>&1"
                      " | awk '/file not found/ {"
                      "     if (!banner) { print \"clang-tidy: headers not resolved "
                      "(regenerate compile_commands.json)\"; banner=1 } next }"
                      "   { print }' | head -100")
                      .arg(kFindExcl, tidyBuildDir);
        const QString tidyDesc = !hasClangTidy
            ? QString("(clang-tidy not installed)")
            : (tidyBuildDir.isEmpty()
                ? QString("(no compile_commands.json — build the project first)")
                : QString("Modernize, readability, performance checks"));
        m_checks.append({
            "clang_tidy", "clang-tidy Analysis", tidyDesc, "C/C++",
            tidyCmd,
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 100 },
            false, hasClangTidy && !tidyBuildDir.isEmpty(), nullptr
        });

        // 2026-04-17: widened from `head -5` + `_H` suffix to `head -30` +
        // any `#ifndef <token>`. 2026-05-20 (ANTS-1707): self-audit caught
        // testauditengine.h / coldeyesengine.h / indiereviewengine.h as
        // "missing guard" — all three carry `#pragma once`, but a long
        // top-of-file doc comment pushed it past line 30. `#pragma once` is
        // positionally unambiguous, so match it ANYWHERE in the file; only
        // the traditional `#ifndef` form needs to live near the top (it must
        // precede real code), so keep a generous 50-line window for that.
        // The `_H` suffix requirement was also wrong (macro conventions vary
        // — `FOO_HPP`, `FOO_GUARD`, `__FOO__` all count as a valid guard).
        addFindCheck("header_guards", "Missing Header Guards",
                     "Headers without #pragma once or ifndef guard", "C/C++",
                     "\\( -name '*.h' -o -name '*.hpp' -o -name '*.hxx'"
                     " -o -name '*.hh' \\) -type f | while read f; do"
                     " grep -qE '#pragma[[:space:]]+once' \"$f\""
                     " || head -50 \"$f\""
                     " | grep -qE '^[[:space:]]*#ifndef[[:space:]]+[A-Za-z_][A-Za-z0-9_]*'"
                     " || echo \"$f\"; done | head -20",
                     CheckType::Bug, Severity::Major, false);

        m_checks.append({
            "compiler_warnings", "Compiler Warnings",
            "Build from scratch with -Wall -Wextra and capture warnings (slow: a full build)",
            "C/C++",
            // ANTS-5039 — one scratch tree per user and project, removed
            // before the build (a run killed mid-way leaves at most this
            // one, which the next run clears) and on exit. TERM and INT
            // exit so the EXIT trap runs when the dialog stops the group.
            "if [ -f CMakeLists.txt ]; then"
            " d=\"${TMPDIR:-/tmp}/ants-audit-warnings-$(id -u)-$(pwd | cksum | cut -d' ' -f1)\";"
            " rm -rf \"$d\"; trap 'rm -rf \"$d\"' EXIT; trap 'exit 143' TERM INT;"
            " mkdir -p \"$d\""
            " && cmake -S . -B \"$d\" -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wno-unused-parameter' >/dev/null 2>&1"
            " && cmake --build \"$d\" -j$(( ($(nproc)+3)/4 )) 2>&1 | grep -E '(warning:|error:)' | head -60;"
            " fi",
            CheckType::Bug, Severity::Major, { {}, "", {}, 60 },
            false, true, nullptr, {},
            20 * 60 * 1000  // a full build is the whole job; the check is opt-in
        });

        // Memory pattern check — Qt-aware. The old approach matched any
        // `new ` / `malloc` / `calloc` / `realloc` and relied on a long
        // dropIfContains list to suppress Qt parent-child idioms — which
        // only worked when the parent expression happened to be one of
        // the handful of names on that list. An audit triage on 2026-04-16
        // found 30/30 findings in this category were false positives
        // (parent expressions like `dlg`, `tab`, `row`, `m_foo`, `&dialog`
        // that the blacklist didn't know about).
        //
        // Tightened regex inverts the logic: only flag `new X()`,
        // `new X(nullptr)`, `new X(NULL)` (empty-or-null parens = NOT
        // parented, potentially leaky) plus malloc/calloc/realloc. Any
        // identifier inside the parens is treated as a potential Qt
        // parent and suppressed. Over-matches on templated constructors
        // (`new QVector<int>(...)`) are acceptable — `<` breaks the class
        // match, so the pattern simply won't fire on those, which is
        // fine because they're rare and not the common FP source.
        if (isQt) {
            addGrepCheck("memory_patterns", "Memory Management (Qt-aware)",
                         "new X() / new X(nullptr) / malloc family (excl. Qt parent-child)",
                         "C/C++",
                         "'(\\bnew [A-Za-z_][A-Za-z0-9_:]*[[:space:]]*"
                         "\\([[:space:]]*(nullptr|NULL)?[[:space:]]*\\))"
                         "|\\b(malloc|calloc|realloc)\\b'",
                         CheckType::CodeSmell, Severity::Minor, false,
                         { /*dropIfContains*/ {
                               "unique_ptr", "shared_ptr", "make_unique", "make_shared",
                               "nothrow", "placement",
                               "Q_NEW", "qMalloc"
                           }, "", {}, 30 });
        } else {
            addGrepCheck("memory_patterns", "Memory Management Patterns",
                         "new/malloc without smart pointers", "C/C++",
                         "'\\b(new |malloc|calloc|realloc)\\b'",
                         CheckType::CodeSmell, Severity::Minor, false,
                         { {"unique_ptr", "shared_ptr", "make_unique", "make_shared",
                            "delete", "free", "RAII", "nothrow", "placement"},
                           "", {}, 30 });
        }
    }

    // ========== Qt-specific (checks derived from real audit findings) ==========
    if (isQt) {
        // clazy (KDAB) — AST-aware Qt static analysis. Subsumes our older
        // regex-based findChild / connect-capture / hardcoded-colour checks
        // with semantic equivalents that don't false-positive on
        // comment/string contexts or unrelated identifiers.
        //
        // Requires a compile_commands.json — our CMake config already emits
        // one (CMAKE_EXPORT_COMPILE_COMMANDS=ON). We probe common build-dir
        // names; user must have built at least once for clazy to work.
        const bool hasClazy = toolExists("clazy-standalone");
        // ANTS-3367 — shared probe (was an inline copy of the build-dir
        // list, now AuditEngine::resolveBuildDir).
        const QString clazyBuildDir = AuditEngine::resolveBuildDir(m_projectPath);
        const QString clazyDesc = hasClazy
            ? (clazyBuildDir.isEmpty()
                ? QString("(no compile_commands.json — build the project first)")
                : QString("Qt-aware AST checks (connect-3arg-lambda, container-inside-loop, etc.)"))
            : QString("(clazy-standalone not installed — zypper in clazy)");
        //
        // Check set chosen for signal/noise:
        //   connect-3arg-lambda        — lambda capturing `this` in connect without receiver
        //   lambda-in-connect          — same family, different shape
        //   container-inside-loop      — QVector/QList COW detach in tight loops
        //   old-style-connect          — SIGNAL()/SLOT() macro connects
        //   range-loop-detach          — `for (auto x : container)` on Qt containers
        //   qstring-arg                — QString::arg misuse
        //   qgetenv                    — prefer qEnvironmentVariable*
        //
        // NOT in the list: qt-keywords. The 2026-04-16 triage found 48/55
        // clazy findings were qt-keywords false positives — the project
        // uses the bare-keyword style (`signals:` / `slots:` / `emit`)
        // as a documented convention (see STANDARDS.md §Plugin System
        // Standards). Disabling the check project-wide eliminates ~87 %
        // of clazy noise without losing any real signal.
        //
        // clazy-standalone output shape:
        //   path/file.cpp:LINE:COL: warning: message [-Wclazy-check-name]
        // Matches our file:line:col: regex in parseFindings() cleanly.
        //
        // Driver noise filter also strips clazy's own diagnostics (unknown
        // -W options, "Processing file N/M" progress banners, bare `|`
        // continuation lines from clang pretty-printer) which surface as
        // findings otherwise.
        m_checks.append({
            "clazy", "clazy (Qt AST analysis)", clazyDesc, "Qt",
            QString("cd %1 && clazy-standalone -p . "
                    "--checks=connect-3arg-lambda,lambda-in-connect,"
                    "container-inside-loop,old-style-connect,"
                    "range-loop-detach,qstring-arg,qgetenv "
                    "../src/*.cpp 2>&1 | head -100").arg(clazyBuildDir.isEmpty()
                                                         ? "build" : clazyBuildDir),
            CheckType::Bug, Severity::Major,
            // Filter out clang/clazy driver noise; keep warning lines with check tags.
            { /*dropIfContains*/ {"In file included from", "error generated",
                                  "warnings generated", "warning generated",
                                  "unknown warning option", "Processing file",
                                  "[-Wclazy-qt-keywords]"},
              "", {}, 100 },
            hasClazy && !clazyBuildDir.isEmpty(),
            hasClazy && !clazyBuildDir.isEmpty(),
            nullptr
        });

        // OSC 8 / URL scheme allowlist — not covered by clazy; project-specific
        // invariant (we always front-load a scheme allowlist before openUrl).
        //
        // The 2026-04-16 triage saw 3/3 findings in this category as FPs —
        // every openUrl call either had a startsWith scheme gate on the
        // immediately-preceding line, or used a string-literal URL. The
        // context-window filter suppresses both patterns when a scheme-
        // check token appears within ±5 lines of the match.
        //
        // ANTS-1709 — also model `QUrl::fromLocalFile()`: it builds a
        // file:// URL from a trusted local path, so an openUrl on its
        // result needs no scheme gate. Without this, idiomatic
        // "open this file/dir in the file manager" calls were the
        // visible residual FP class. Same-line form drops via
        // dropIfContains; adjacent-construction form via the context list.
        addGrepCheck("qt_openurl_unchecked", "Qt openUrl Without Scheme Check",
                     "QDesktopServices::openUrl called on unvalidated URIs", "Qt",
                     "'QDesktopServices::openUrl'",
                     CheckType::Vulnerability, Severity::Major, true,
                     OutputFilter{
                       /*dropIfContains*/ {"scheme() ==", "validScheme",
                                           "allowScheme", "isLocal",
                                           "https://", "QUrl::TolerantMode",
                                           "fromLocalFile"},
                       "", {}, 20,
                       /*dropIfContextContains*/ {
                           "startsWith(\"http", "startsWith(\"https",
                           "startsWith(\"file", "startsWith(\"mailto",
                           "startsWith(\"ftp", "// ants-audit: scheme-validated",
                           // string-literal URL constructor on adjacent line
                           "QUrl(\"http", "QUrl(\"https", "QUrl(\"file",
                           "QUrl(\"mailto", "QUrl(\"ftp",
                           "QUrl::fromUserInput", "QUrl::fromLocalFile"},
                       /*contextWindow*/ 5 });

        // Unbounded callback payloads. Detects PTY / OSC / IPC byte buffers
        // forwarded straight into a user-supplied `*Callback(...)` without a
        // length cap. Motivating case: pre-0.6.5 OSC 9 / OSC 777 notifier
        // shovelled the entire escape body (potentially MB) into the desktop
        // notification callback, which then crashed the notification daemon
        // and/or amplified a malformed-OSC DoS.
        //
        // Same-line shape: any identifier ending in `Callback` invoked with
        // `QString::fromUtf8(<expr>.c_str()…)` somewhere in its argument list.
        // The runtime filter then drops lines that already cap with `.left(`,
        // `.truncate(`, `.mid(`, `.chopped(`, or `.chop(` — the canonical
        // bounding primitives. Multi-line callback invocations are out of
        // scope (would need `grep -Pzo`).
        // The 2026-04-16 triage saw 1/1 finding here as an FP — the truncate()
        // call was 3-8 lines above the callback invocation, not on the same
        // line. Extended with a ±5-line context filter that also recognises
        // explicit size-caps (`<= kMaxFoo`, `<= 128`, and generic
        // `constexpr.*kMax…` declarations in the same function).
        addGrepCheck("unbounded_callback_payloads", "Unbounded Callback Payloads",
                     "Raw byte-buffer forwarded to a *Callback() without "
                     ".left()/.truncate() cap — DoS amplifier", "Qt",
                     "'\\w*[Cc]allback\\s*\\(.*QString::fromUtf8\\([^)]*\\.c_str\\(\\)'",
                     CheckType::Vulnerability, Severity::Major, true,
                     OutputFilter{
                       /*dropIfContains*/ {".left(", ".truncate(", ".mid(",
                                           ".chopped(", ".chop("},
                       "", {}, 30,
                       /*dropIfContextContains*/ {".truncate(", ".left(",
                                                  ".chopped(", ".chop(",
                                                  "constexpr int kMax",
                                                  "constexpr size_t kMax",
                                                  ".size() <=", ".size() <"},
                       /*contextWindow*/ 10 });

        // QNetworkReply 3-arg lambda lifetime trap. Matches the dangerous
        // shape from the pre-0.6.5 AiDialog incident: a 3-arg connect to a
        // QNetworkReply signal whose third argument is a bare lambda. With
        // no context object, Qt cannot auto-disconnect when the lambda's
        // captured owner is destroyed. If the owning dialog is closed
        // mid-flight, the reply still fires and the lambda dereferences a
        // dangling pointer → use-after-free.
        //
        // The safe alternatives are all 4-arg shapes (sender, signal,
        // context, slot) — both the member-function-pointer slot and the
        // lambda slot variants auto-disconnect when `context` is destroyed.
        //
        // Pattern detects 3-arg-lambda by requiring `[` immediately after
        // the signal's trailing comma. 4-arg shapes have an identifier
        // (`this`, `mgr`, `m_widget`, etc.) in that slot, so they don't
        // match. Single-line only — multi-line connect formatting is a
        // known false-negative (rare in practice).
        addGrepCheck("qnetworkreply_no_abort", "QNetworkReply Connect Without Context",
                     "3-arg connect() to QNetworkReply lambda — no auto-disconnect, "
                     "use-after-free risk if owner is destroyed mid-flight (use the "
                     "4-arg form with `this` as context)", "Qt",
                     "'connect\\s*\\([^,]*,\\s*&QNetworkReply::(readyRead|finished|errorOccurred|sslErrors)\\s*,\\s*\\['",
                     CheckType::Vulnerability, Severity::Major, true);

        // qimage_load_without_peek — image-bomb class CVE vector. A malicious
        // PNG can encode 65535×65535 in the IHDR while the compressed payload
        // is <1 KB; plain QImage::loadFromData then allocates ~17 GB before
        // the dimension sanity check fires. The 0.7.6 fix peeked dimensions
        // via QImageReader before calling loadFromData, gated by a
        // `MAX_IMAGE_DIM = 4096` cap. This rule catches the next loadFromData
        // call that skips the peek.
        //
        // Filter drops any line tagged `// image-peek-ok` (explicit reviewer
        // sign-off) or any call preceded by `QImageReader` within ±5 lines
        // (the canonical peek pattern).
        addGrepCheck("qimage_load_without_peek", "QImage::loadFromData without QImageReader peek",
                     "QImage / QPixmap loadFromData() call not preceded by a "
                     "QImageReader size-peek — image-bomb DoS vector", "Qt",
                     "'\\.loadFromData\\s*\\('",
                     CheckType::Vulnerability, Severity::Minor, true,
                     OutputFilter{
                       /*dropIfContains*/ {"image-peek-ok"},
                       "", {}, 30,
                       /*dropIfContextContains*/ {"QImageReader"},
                       /*contextWindow*/ 5 });

        // setPermissions_pair_no_helper — hygiene rule. The 0.7.7 audit pass
        // consolidated 11 copies of `setPermissions(ReadOwner | WriteOwner)`
        // behind `setOwnerOnlyPerms()` in src/secureio.h. Routing every 0600
        // permission set through one helper makes the owner-only intent the
        // only way to call it — a typo that adds ReadGroup / ReadOther would
        // otherwise silently widen access to files holding API keys. This
        // rule nudges the next contributor toward the helper instead of
        // hand-typing the bitmask pair.
        //
        // Pattern is deliberately strict: the bitmask must terminate with `)`
        // immediately after WriteOwner so permissions with additional flags
        // (0755 hook-script perms in settingsdialog.cpp) don't false-fire.
        // The helper definition itself in secureio.h is suppressed via
        // `// ants-audit: disable=setPermissions_pair_no_helper`.
        addGrepCheck("setPermissions_pair_no_helper",
                     "setPermissions(ReadOwner|WriteOwner) without helper",
                     "Raw 0600 bitmask — prefer setOwnerOnlyPerms() from "
                     "src/secureio.h to prevent accidental access widening", "Qt",
                     "'setPermissions\\s*\\([^)]*QFileDevice::ReadOwner\\s*\\|\\s*QFileDevice::WriteOwner\\s*\\)'",
                     CheckType::CodeSmell, Severity::Info, false);
    }

    // ========== Semgrep (structural pattern matching) ==========
    //
    // Optional extra lane alongside clazy / cppcheck. Semgrep's strength is
    // structural patterns (it reads the AST, not just regex) so its findings
    // are lower-FP than grep-based checks. Community rule packs `p/c` and
    // `p/cpp` cover buffer overflows, int overflow, unsafe memory ops,
    // etc. — things our hardcoded regex `unsafe_c_funcs` approximates more
    // crudely.
    //
    // Silent no-op when `semgrep` is missing. User can pin a project-local
    // `audit_rules.semgrep.yaml` (or `.semgrep.yml`) and it's picked up
    // automatically. We ask for text output (file:line:col:msg) so our
    // existing parseFindings() pattern matches without a JSON pivot.
    const bool hasSemgrep = toolExists("semgrep");
    if (hasSemgrep) {
        const bool hasCpp = m_detectedTypes.contains("C/C++");
        const bool hasPy  = m_detectedTypes.contains("Python");
        const bool hasJs  = m_detectedTypes.contains("JavaScript");
        // Pick community packs matching detected languages. `p/security-audit`
        // is universally useful. Project-local `.semgrep.yml` (if present)
        // is auto-included by semgrep.
        QStringList packs = {"p/security-audit"};
        if (hasCpp) packs << "p/c" << "p/cpp";
        if (hasPy)  packs << "p/python";
        if (hasJs)  packs << "p/javascript" << "p/typescript";
        // ANTS-1257 — framework-specific packs (flask/django/react/vue) from
        // audithygiene's project-marker detection. semgrepRulePacks returns
        // {"--config", "p/flask", …}; collect the pack names so they share
        // the `--config=p/X` shell form below and appear in the badge label.
        const QStringList fwArgs = AuditHygiene::semgrepRulePacks(
            AuditHygiene::detectProjectFrameworks(m_projectPath));
        for (const QString &a : fwArgs)
            if (a != QStringLiteral("--config")) packs << a;
        QString cfg;
        for (const QString &p : std::as_const(packs)) cfg += " --config=" + p;
        // Respect project-local `.semgrep.yml` "Excluded upstream rules"
        // header block — see semgrepExcludeFlags() for the contract.
        const QString excludeFlags = semgrepExcludeFlags();
        m_checks.append({
            "semgrep", "Semgrep (structural patterns)",
            "AST-aware pattern matching (" + packs.join(", ") + ")",
            "Security",
            "semgrep --timeout 30 --quiet --error --disable-version-check"
            + cfg + excludeFlags +
            " --exclude build --exclude 'build-*' --exclude node_modules"
            " --exclude .audit_cache --exclude vendor"
            " . 2>&1 | head -120",
            CheckType::Vulnerability, Severity::Major,
            { /*dropIfContains*/ {"❯", "Scan Status", "Scanning", "Ran ",
                                  "Scan complete", "files scanned"},
              "", {}, 120 },
            false, true, nullptr
        });
    }

    // ========== ast-grep — polyglot structural search (opt-in) ==========
    //
    // Complements semgrep with Tree-sitter-based AST patterns for languages
    // semgrep covers weakly (Rust, Kotlin, Swift) and for user-authored
    // rules. Rule-pack-driven — without a `sgconfig.yml` at project root
    // there's nothing to run, so we gate on that file.
    // Only probe the canonical `ast-grep` binary, not the `sg` shortcut —
    // on Linux `/usr/bin/sg` is `newgrp` (setgroups), which would yield a
    // false positive here.
    const bool hasAstGrep = toolExists("ast-grep");
    const bool hasAstGrepCfg =
        QFile::exists(m_projectPath + "/sgconfig.yml") ||
        QFile::exists(m_projectPath + "/sgconfig.yaml");
    if (hasAstGrep && hasAstGrepCfg) {
        m_checks.append({
            "ast_grep", "ast-grep (structural search)",
            "Tree-sitter AST patterns (sgconfig.yml)", "Security",
            "ast-grep scan 2>&1 | head -100",
            CheckType::Bug, Severity::Major, { {}, "", {}, 100 },
            true, true, nullptr
        });
    }

    // ========== osv-scanner — multi-ecosystem CVE lookup ==========
    //
    // Single binary replaces the per-ecosystem SCA lanes (npm audit,
    // cargo-audit, pip-audit) by reading every supported manifest and
    // cross-referencing the OSV.dev advisory DB. Complementary to trivy,
    // which is container/image-focused. SARIF-native output; we ask for
    // the table format since our parseFindings reads `file:line: msg`.
    const bool hasOsv = toolExists("osv-scanner");
    if (hasOsv) {
        m_checks.append({
            "osv_scanner", "OSV Scanner (multi-ecosystem CVE)",
            "Cross-ref npm/cargo/pip/go/maven manifests against OSV.dev",
            "Security",
            "osv-scanner scan source --recursive . 2>&1 | tail -80",
            CheckType::Vulnerability, Severity::Critical,
            { /*dropIfContains*/ {"Scanning dir", "Scanned ", "No issues",
                                  "Loaded ", "--- Ended "},
              "", {}, 80 },
            true, true, nullptr
        });
    }

    // ========== trufflehog — verified secret scanning ==========
    //
    // Off by default because `--only-verified` makes live API calls to
    // confirm the discovered credential is active. Users opt in via the
    // toggle; the no-verification mode is equivalent to gitleaks noise
    // level and adds little, so we skip it.
    const bool hasTrufflehog = toolExists("trufflehog");
    if (hasTrufflehog) {
        m_checks.append({
            "trufflehog", "TruffleHog (verified secrets)",
            "Scan filesystem and verify found credentials against the live API",
            "Security",
            "trufflehog filesystem --only-verified --no-update "
            "--exclude-paths=<(printf '%s\\n' build .git node_modules .audit_cache vendor) "
            ". 2>&1 | tail -60",
            CheckType::Vulnerability, Severity::Blocker,
            { /*dropIfContains*/ {"🐷", "TruffleHog", "no credentials", "chunks"},
              "", {}, 60 },
            false, true, nullptr
        });
    }

    // ========== hadolint — Dockerfile linter ==========
    if (m_detectedTypes.contains("Docker")) {
        const bool hasHadolint = toolExists("hadolint");
        // Glob-based Dockerfile discovery; hadolint accepts multiple files.
        m_checks.append({
            "hadolint", "Hadolint (Dockerfile)",
            hasHadolint ? "Dockerfile best-practice + embedded shellcheck"
                        : "(hadolint not installed)",
            "Security",
            "find . -type f \\( -name 'Dockerfile' -o -name 'Dockerfile.*' "
            "-o -name '*.Dockerfile' \\)" + kFindExcl +
            " -print0 2>/dev/null | xargs -0 -r hadolint --no-color 2>&1 | head -80",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 80 },
            hasHadolint, hasHadolint, nullptr
        });
    }

    // ========== checkov — IaC scanner (Terraform / K8s / Dockerfile / GH Actions) ==========
    //
    // Checkov can produce hundreds of findings on a large Terraform tree
    // at default severity; we filter to HIGH + CRITICAL and cap output.
    // The `--compact` flag gives one-line-per-finding output that
    // parseFindings can consume directly.
    const bool hasCheckov = toolExists("checkov");
    const bool hasIaC = m_detectedTypes.contains("Terraform")
                      || m_detectedTypes.contains("Kubernetes")
                      || m_detectedTypes.contains("GitHub Actions")
                      || m_detectedTypes.contains("Docker");
    if (hasCheckov && hasIaC) {
        m_checks.append({
            "checkov", "Checkov (IaC)",
            "Terraform / K8s / Dockerfile / GH Actions policy checks",
            "Security",
            "checkov -d . --compact --quiet --output=cli "
            "--soft-fail --skip-path build --skip-path node_modules "
            "--skip-path .audit_cache --skip-path vendor 2>&1 | tail -120",
            CheckType::Hotspot, Severity::Major,
            { /*dropIfContains*/ {"_     _", " By bridgecrew.io", "version: ",
                                  "Update available", "Passed checks:",
                                  "Skipped checks:"},
              "", {}, 120 },
            true, true, nullptr
        });
    }

    // ========== Python ==========
    if (m_detectedTypes.contains("Python")) {
        const bool hasPylint = toolExists("pylint");
        m_checks.append({
            "pylint", "Pylint Analysis",
            hasPylint ? "Error-level checks" : "(pylint not installed)", "Python",
            "find . -name '*.py'" + kFindExcl + " | head -20"
            " | xargs pylint --errors-only 2>&1 | head -100",
            CheckType::Bug, Severity::Major, { {}, "", {}, 100 },
            hasPylint, hasPylint, nullptr
        });
        const bool hasBandit = toolExists("bandit");
        // Respect project-local pyproject.toml [tool.ruff.lint.ignore] S-codes
        // as a bandit skip list — see banditSkipFlags() for the contract.
        const QString banditSkip = banditSkipFlags();
        m_checks.append({
            "bandit", "Bandit Security Scan",
            hasBandit ? "Python security issue detection" : "(bandit not installed)", "Python",
            "bandit -r . -q --exclude=./build,./build-test,./node_modules,./.audit_cache"
            + banditSkip +
            " 2>&1 | head -100",
            CheckType::Vulnerability, Severity::Critical, { {}, "", {}, 100 },
            hasBandit, hasBandit, nullptr
        });
        const bool hasMypy = toolExists("mypy");
        m_checks.append({
            "mypy", "mypy Type Check",
            hasMypy ? "Static type analysis" : "(mypy not installed)", "Python",
            "mypy . --ignore-missing-imports 2>&1 | tail -20",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 20 },
            false, hasMypy, nullptr
        });
        const bool hasRuff = toolExists("ruff");
        m_checks.append({
            "ruff", "Ruff Linter",
            hasRuff ? "Fast Python linting" : "(ruff not installed)", "Python",
            "ruff check . 2>&1 | head -80",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 80 },
            hasRuff, hasRuff, nullptr
        });
    }

    // ========== JavaScript / TypeScript ==========
    if (m_detectedTypes.contains("JavaScript")) {
        const bool hasNpm = toolExists("npm");
        m_checks.append({
            "npm_audit", "npm Dependency Audit",
            hasNpm ? "Check for known vulnerabilities" : "(npm not installed)", "JavaScript",
            "npm audit --production 2>&1 | tail -30",
            CheckType::Vulnerability, Severity::Major, { {}, "", {}, 30 },
            hasNpm, hasNpm, nullptr
        });
        m_checks.append({
            "eslint", "ESLint Analysis",
            hasNpm ? "Lint JavaScript/TypeScript" : "(npm not installed)", "JavaScript",
            "npx eslint . --max-warnings=50 2>&1 | head -100",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 100 },
            false, hasNpm, nullptr
        });
        m_checks.append({
            "outdated_deps", "Outdated Dependencies",
            hasNpm ? "Check for outdated npm packages" : "(npm not installed)", "JavaScript",
            "npm outdated 2>&1 | head -30",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 30 },
            false, hasNpm, nullptr
        });
    }

    // ========== Rust ==========
    if (m_detectedTypes.contains("Rust")) {
        const bool hasCargo = toolExists("cargo");
        m_checks.append({
            "cargo_clippy", "Cargo Clippy",
            hasCargo ? "Rust lint checks" : "(cargo not installed)", "Rust",
            "cargo clippy 2>&1 | head -100",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 100 },
            hasCargo, hasCargo, nullptr
        });
        const bool hasAudit = toolExists("cargo-audit");
        m_checks.append({
            "cargo_audit", "Cargo Audit",
            hasAudit ? "Dependency vulnerability scan" : "(cargo-audit not installed)", "Rust",
            "cargo audit 2>&1 | head -100",
            CheckType::Vulnerability, Severity::Major, { {}, "", {}, 100 },
            hasAudit, hasAudit, nullptr
        });
    }

    // ========== Go ==========
    if (m_detectedTypes.contains("Go")) {
        const bool hasGo = toolExists("go");
        m_checks.append({
            "go_vet", "Go Vet",
            hasGo ? "Report likely mistakes" : "(go not installed)", "Go",
            "go vet ./... 2>&1 | head -100",
            CheckType::Bug, Severity::Major, { {}, "", {}, 100 },
            hasGo, hasGo, nullptr
        });
        const bool hasVuln = toolExists("govulncheck");
        m_checks.append({
            "govulncheck", "Go Vulnerability Check",
            hasVuln ? "Scan dependencies for known vulnerabilities" : "(govulncheck not installed)", "Go",
            "govulncheck ./... 2>&1 | head -50",
            CheckType::Vulnerability, Severity::Major, { {}, "", {}, 50 },
            false, hasVuln, nullptr
        });
        const bool hasLint = toolExists("golangci-lint");
        m_checks.append({
            "golangci_lint", "golangci-lint",
            hasLint ? "Comprehensive Go linting" : "(golangci-lint not installed)", "Go",
            "golangci-lint run ./... 2>&1 | head -100",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 100 },
            false, hasLint, nullptr
        });
    }

    // ========== Shell ==========
    if (m_detectedTypes.contains("Shell")) {
        const bool hasSC = toolExists("shellcheck");
        m_checks.append({
            "shellcheck", "ShellCheck Analysis",
            hasSC ? "Static analysis for shell scripts" : "(shellcheck not installed)", "Shell",
            "find . -name '*.sh'" + kFindExcl + " -exec shellcheck {} + 2>&1 | head -100",
            CheckType::Bug, Severity::Major, { {}, "", {}, 100 },
            hasSC, hasSC, nullptr
        });
    }

    // ========== Lua ==========
    if (m_detectedTypes.contains("Lua")) {
        const bool hasLuacheck = toolExists("luacheck");
        m_checks.append({
            "luacheck", "Luacheck Analysis",
            hasLuacheck ? "Static analysis for Lua scripts" : "(luacheck not installed)", "Lua",
            "find . -name '*.lua'" + kFindExcl + " -exec luacheck {} + 2>&1 | head -100",
            CheckType::Bug, Severity::Major, { {}, "", {}, 100 },
            hasLuacheck, hasLuacheck, nullptr
        });
    }

    // ========== Java ==========
    if (m_detectedTypes.contains("Java")) {
        const bool hasSpotbugs = toolExists("spotbugs");
        m_checks.append({
            "spotbugs", "SpotBugs Analysis",
            hasSpotbugs ? "Find bug patterns in Java code" : "(spotbugs not installed)", "Java",
            "find . -name '*.class'" + kFindExcl + " | head -1 >/dev/null 2>&1"
            " && spotbugs -textui -effort:max . 2>&1 | head -80 || echo 'No compiled .class files found'",
            CheckType::Bug, Severity::Major, { {}, "", {}, 80 },
            false, hasSpotbugs, nullptr
        });
    }

    // Per-tool timeout overrides — global default is 30 s but a few
    // tools genuinely need longer on real-world projects:
    //   - cppcheck full-tree: AST per TU, parallel but per-TU latency dominates
    //   - cppcheck_unused: --enable=unusedFunction is single-threaded whole-program
    //   - clang_tidy: re-parses every TU; worse on Qt-heavy code
    //   - clazy: clang-based AST walk over the same TU set
    //   - semgrep: rule-pack compile + AST traversal across language tree
    //   - osv_scanner: network-bound by OSV.dev rate limits
    //   - trufflehog: full git-history scan with regex eval per blob
    // Without these, the slow tools false-positive on the 30 second
    // default and pollute the report with tool-health warnings instead
    // of findings.
    for (auto &c : m_checks) {
        if (c.id == "cppcheck" || c.id == "cppcheck_unused" ||
            c.id == "clang_tidy" || c.id == "clazy") {
            c.timeoutMs = 60000;
        } else if (c.id == "semgrep") {
            c.timeoutMs = 90000;
        } else if (c.id == "osv_scanner" || c.id == "trufflehog") {
            c.timeoutMs = 120000;
        }
    }
}
