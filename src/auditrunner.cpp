// ANTS-1351 — server-side audit runner v1.
//
// v1 scope: ship the infrastructure end-to-end:
//   * QProcess + QEventLoop multiplexer (§ 2.5)
//   * Worker-thread dispatch via the caller's QThreadPool
//   * PathValidation on caller_cwd (INV-2)
//   * Env scrub allowlist/blocklist (INV-10)
//   * Absolute-path tool resolution via QStandardPaths::findExecutable
//     with 60 s TTL cache (INV-10)
//   * Per-tool wall-clock cap with SIGTERM/SIGKILL (INV-5)
//   * Aggregate cap of min(N*cap*1.5, 900 s) (INV-1; ANTS-3585)
//   * Hardcoded kExclusions list (INV-6)
//   * Per-call SARIF path (INV-12)
//   * Per-sample 256 B message cap + bottom-up trim cascade (INV-13)
//   * `scope:"since-tag:<X>"` argv-safe (INV-15)
//   * `cap_per_tool_seconds` + `top_findings_count` range check (INV-16)
//
// ANTS-1870 — the v2 per-finding parser landed: parseToolOutput now
// materialises the FULL `findings[]` set (each with its fp), writeSarif
// emits per-finding result entries from it (with a carry-forward run
// under since-last-run), and the runner computes a precise added/removed/
// carried-forward delta against the prior run's findings sidecar. The two
// "v2 will parse the SARIF result entries" deferrals below are resolved.
//
// Out of scope (logged as roadmap follow-up):
//   * Cross-tool finding correlation (cppcheck + clazy reporting the same
//     underlying defect as one) — each tool diffs within its own checkId
//     namespace (ANTS-1870 § 5).
//   * `.audit_suppress` — still GUI-only. It is keyed by the line-grain
//     `Finding::dedupKey` the runner never materialises, and the
//     drift-resilient learned-FP ledger (ANTS-1820) supersedes it here.
//     (`.audit_allowlist.json` IS applied as of ANTS-3615, through the
//     shared AuditEngine loader/matcher — hardening included.)
//   * Line-precise finding identity — the fingerprint is line-insensitive
//     by design (ANTS-1820 / ANTS-1870 § 2.2).
//
// All of the v1 scope above honours the spec's INV anchors via
// source-scrape; behavioural tests cover env scrub + cap timing +
// pool isolation.

#include "auditrunner.h"

#include "auditcache.h"     // ANTS-1555
#include "auditdelta.h"     // ANTS-1870 since-last-run findings delta
#include "auditscope.h"     // ANTS-1504 changed-file resolver
#include "auditengine.h"    // ANTS-1576 buildVcsProvenanceBlock
#include "auditfpledger.h"  // ANTS-1820 learned-FP ledger (headless gap)
#include "featurecoverage.h"  // ANTS-3605 in-process drift lanes (GUI parity)
#include "secureio.h"       // setOwnerOnlyPerms — 0600 on SARIF/HTML
#include "secretredact.h"   // ANTS-2188 scrub secret shapes from raw output

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>
#include <QUuid>
#include <QTimer>

#include <cstring>
#include <memory>
#include <mutex>

namespace AuditRunner {

namespace {

// ───────────────────────────── ANTS-1351-INV-1 ──
// kAggregateCapMs (900'000) now lives in auditrunner.h — ANTS-3611 promoted
// it to the public header so the MCP layer's stale-slot reap windows derive
// from it instead of re-hardcoding a stale copy. Used unqualified below; the
// enclosing `namespace AuditRunner` resolves it.
constexpr int kCapPerToolMin             = 5;
// ANTS-3585 — raised from 60 so a big C/C++ sweep (cppcheck on a 193-file
// tree with an 8,900-line TU) can finish instead of false-timing-out. Opt-in
// per request; the default (30 s, in auditrunner.h) is unchanged.
constexpr int kCapPerToolMax             = 300;
// (Default 30 s is encoded in auditrunner.h struct initialiser.)
constexpr int kTopFindingsMin            = 0;
constexpr int kTopFindingsMax            = 100;
// ───────────────────────────── ANTS-1351-INV-9 (ANTS-3612) ──
// Aggregate concurrency cap. The v1 contract promised an `m_auditPool`
// worker pool that was never built, so N audits against N DIFFERENT project
// roots each spawned an ad-hoc QThread with no aggregate ceiling — the
// per-root in-flight gate only stops a SECOND run on the SAME root, and the
// async job registry allows 16. One sweep's external tools peak around
// 1.9 GiB RSS, so 16 concurrent sweeps is ~30 GiB: a live OOM path on a
// 32 GiB host.
//
// The cap lives HERE rather than at the MCP dispatch site so every caller
// (sync verb, async job worker, any future CLI) inherits it from the engine.
// It is deliberately a flat 2, NOT the spec's old `max(2, nproc/8)`: the
// binding resource is RAM per concurrent tool-set, not cores, and nproc/8 on
// a many-core host would authorise a memory footprint the host cannot hold.
// Two concurrent sweeps ≈ 3.8 GiB, which is the ~5 GiB audit ceiling the
// spec's budget assumed.
constexpr int kMaxConcurrentRuns         = 2;
int            g_activeRuns = 0;
std::mutex     g_activeRunsMutex;

bool runSlotTryAcquire() {
    std::lock_guard<std::mutex> lk(g_activeRunsMutex);
    if (g_activeRuns >= kMaxConcurrentRuns) return false;
    ++g_activeRuns;
    return true;
}

void runSlotRelease() {
    std::lock_guard<std::mutex> lk(g_activeRunsMutex);
    if (g_activeRuns > 0) --g_activeRuns;
}

// RAII so every `return r;` below the acquisition frees the slot — runAudit
// has ~20 early-return paths and a leaked slot bricks the cap permanently.
struct RunSlotGuard {
    bool held = false;
    ~RunSlotGuard() { if (held) runSlotRelease(); }
};

// ───────────────────────────── ANTS-1351-INV-5 ──
constexpr int kKillGraceMs               = 2'000;
// ───────────────────────────── ANTS-1351-INV-10 ──
constexpr int kToolResolveCacheTtlMs     = 60'000;
// ───────────────────────────── ANTS-1351-INV-13 ──
constexpr int kSampleMessageMaxBytes     = 256;
constexpr int kEnvelopeSoftCapBytes      = 18 * 1024;
constexpr int kEnvelopeHardCapBytes      = 20 * 1024;
constexpr int kSamplesPerToolDefault     = 10;
// SARIF v2.1.0 reserves nothing useful below 10K findings; cap per
// ANTS-1351 v4 § 6 SARIF cap.
constexpr int kSarifFindingsMax          = 10'000;

// ───────────────────────────── ANTS-1351-INV-6 (kExclusions) ──
// Hardcoded — see § 8 decision "Exclusion list hardcoded in engine".
// Skill markdown documents the same set inline. ANTS-3394 — this set is now
// wired into the whole-tree (non-scoped) invocation of each find-everything
// tool via toolExclusionArgs(); a scope:"full" sweep no longer drowns the
// real findings in build-output / vendored noise (a Flask app's dist/ +
// node_modules/ flooded mypy with 130+ stub-file findings).
const QStringList &kExclusions() {
    static const QStringList v = {
        QStringLiteral("logs"),
        QStringLiteral("data"),
        QStringLiteral("database"),
        QStringLiteral("node_modules"),
        QStringLiteral("dist"),
        QStringLiteral("build"),
        QStringLiteral("_build"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".venv"),
        QStringLiteral("venv"),
        QStringLiteral("env"),
    };
    return v;
}

// ANTS-3394 — per-tool exclusion flags for the whole-tree (non-scoped)
// invocation of each find-everything tool, built from kExclusions() so the
// dir set stays single-source. Scoped invocations already narrow to an
// explicit changed-file list and get NO exclusions (those files were chosen
// on purpose). cppcheck/clazy/clang-tidy run against src/ or the compile DB
// (never the build-output tree), and gitleaks is filtered via its generated
// --config (ANTS-2016), so none of them appear here.
//
// .gitignore: ruff and semgrep honour it by default (no flag needed); the
// explicit set is what covers the gitignore-unaware tools (bandit, mypy,
// trivy). A caller who really wants to audit a build tree can pass `paths`
// (a scoped invocation), which bypasses these entirely.
QStringList toolExclusionArgs(const QString &tool) {
    const QStringList &dirs = kExclusions();
    if (tool == QLatin1String("ruff"))
        // ruff --extend-exclude ADDS to the default + .gitignore excludes.
        return {QStringLiteral("--extend-exclude"),
                dirs.join(QLatin1Char(','))};
    if (tool == QLatin1String("bandit"))
        // bandit -x matches each comma-listed dir name during its walk.
        return {QStringLiteral("-x"), dirs.join(QLatin1Char(','))};
    if (tool == QLatin1String("semgrep")) {
        // semgrep --exclude skips any path whose name matches; repeatable.
        QStringList a;
        for (const QString &d : dirs)
            a += {QStringLiteral("--exclude"), d};
        return a;
    }
    if (tool == QLatin1String("trivy")) {
        // trivy --skip-dirs takes a glob; `**/<name>` skips the dir anywhere.
        QStringList a;
        for (const QString &d : dirs)
            a += {QStringLiteral("--skip-dirs"), QStringLiteral("**/") + d};
        return a;
    }
    if (tool == QLatin1String("mypy")) {
        // mypy --exclude is one regex matched against each file path.
        QStringList escaped;
        for (const QString &d : dirs)
            escaped += QRegularExpression::escape(d);
        return {QStringLiteral("--exclude"),
                QStringLiteral("(^|/)(") + escaped.join(QLatin1Char('|'))
                    + QStringLiteral(")/")};
    }
    return {};
}

// ───────────────────────────── ANTS-1351-INV-10 (env policy) ──
const QStringList &kEnvAllowlist() {
    static const QStringList v = {
        QStringLiteral("PATH"),
        QStringLiteral("HOME"),
        QStringLiteral("USER"),
        QStringLiteral("LANG"),
        QStringLiteral("TERM"),
        QStringLiteral("TMPDIR"),
        QStringLiteral("XDG_CACHE_HOME"),
        // LC_* matched via prefix below.
    };
    return v;
}

const QStringList &kEnvBlocklist() {
    static const QStringList v = {
        QStringLiteral("SUDO_ASKPASS"),
        QStringLiteral("SSH_AUTH_SOCK"),
        QStringLiteral("GH_TOKEN"),
        QStringLiteral("OPENAI_API_KEY"),
        QStringLiteral("ANTHROPIC_API_KEY"),
        // AWS_* matched via prefix below.
    };
    return v;
}

const QStringList &kKnownTools() {
    static const QStringList v = {
        QStringLiteral("cppcheck"),
        QStringLiteral("clazy"),
        QStringLiteral("clang-tidy"),   // ANTS-1512 — scoped-check mode
        QStringLiteral("ruff"),
        QStringLiteral("bandit"),
        QStringLiteral("semgrep"),
        QStringLiteral("gitleaks"),
        QStringLiteral("trivy"),
        QStringLiteral("shellcheck"),
        QStringLiteral("mypy"),
    };
    return v;
}

// ANTS-3418 — tools auto-selected when the caller omits `tools`. This is
// kKnownTools() MINUS mypy: the audit runner invokes tools deps-less (no
// `uv sync` / venv), so a full-sweep mypy emits dozens of import-not-found /
// import-untyped false positives on any project that imports third-party
// libraries (the project's REAL deps-installed `uv run mypy`, run in CI and
// pre-commit, is clean). A deps-less mypy adds no signal, only noise that
// dominates every sweep's raw count. mypy stays a KNOWN tool — an explicit
// `tools:["mypy"]` still runs — it is just no longer auto-detected.
const QStringList &kAutoDetectTools() {
    static const QStringList v = [] {
        QStringList t = kKnownTools();
        t.removeAll(QStringLiteral("mypy"));
        return t;
    }();
    return v;
}

// ANTS-1512 — tools that honour the `checks` filter. Other tools that
// receive `checks` refuse with `bad_args` rather than silently ignore
// them — silent-ignore is a footgun (caller assumes their narrow scope
// is applied; gets a full run instead).
bool toolHonoursChecks(const QString &tool) {
    return tool == QLatin1String("clang-tidy");
}

// ANTS-1456 / ANTS-1464 — load project-side audit-config.json if
// present. Probed locations (first wins):
//   <root>/.audit-config.json                   canonical dotfile
//   <root>/docs/private/audit/audit-config.json RetroArch-style
// Schema (per-tool args override): {"<tool>": {"args": ["...","..."]}}.
// When matched, the tool's argv is replaced wholesale by the array.
// Malformed JSON / missing file → empty object (default argv used).
//
// ANTS-1456 cold-eyes follow-up — config file is bounded to
// kAuditConfigMaxBytes (64 KiB) and per-arg sanitisation is applied
// by isAuditArgSafe() at toolArgv() consume time. The config lives
// inside the project root which IS the audit target, so an
// attacker who can edit the file already controls the tree being
// audited; the cap + regex are defence in depth against a
// wrong-tab CC session auditing untrusted third-party clones.
constexpr qint64 kAuditConfigMaxBytes = 64 * 1024;

QJsonObject loadProjectAuditConfig(const QString &projectRoot) {
    const QStringList candidates = {
        projectRoot + QLatin1String("/.audit-config.json"),
        projectRoot + QLatin1String("/docs/private/audit/audit-config.json"),
    };
    for (const QString &p : candidates) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) continue;
        if (f.size() > kAuditConfigMaxBytes) continue;
        const QByteArray raw = f.readAll();
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            return doc.object();
        }
    }
    return {};
}

// ANTS-1456 cold-eyes follow-up — per-arg argv-injection guard
// for the audit-config.json override path. Same shape as
// isScopeTagSafe() (ANTS-1351-INV-15). Allowlist of safe chars,
// length cap, and explicit reject of `-o`/`-O` (the canonical
// argv-injection gadget: ssh-style `-o ProxyCommand=…`). Args
// that fail validation cause the tool's whole override to be
// discarded and the default argv runs — fail-safe over fail-open.
bool isAuditArgSafe(const QString &arg) {
    if (arg.isEmpty() || arg.size() > 256) return false;
    static const QRegularExpression rx(
        QStringLiteral("^[A-Za-z0-9._/=:,+@~\\-]+$"));
    if (!rx.match(arg).hasMatch()) return false;
    if (arg == QLatin1String("-o") || arg == QLatin1String("-O"))
        return false;
    return true;
}

// ANTS-1512 — per-check sanitiser. Check IDs look like
// `bugprone-integer-division`, `clang-analyzer-core.*`,
// `-readability-magic-numbers` (the leading `-` opts a check OUT in
// clang-tidy's --checks syntax). Allowlist is intentionally tight to
// keep this an argv-injection chokepoint.
bool isAuditCheckSafe(const QString &check) {
    if (check.isEmpty() || check.size() > 128) return false;
    static const QRegularExpression rx(
        QStringLiteral("^-?[A-Za-z0-9_*.,-]+$"));
    if (!rx.match(check).hasMatch()) return false;
    return true;
}

// ANTS-2185 — make a scoped positional safe to hand to a child tool as
// an argument. A path that begins with `-` (a file named e.g. `-rf.cpp`
// in a hostile-clone tree) would otherwise be parsed as a FLAG by the
// tool (argv option-injection). Prefixing `./` makes it unambiguously a
// path for every tool we spawn. Only relative, dash-leading names are
// rewritten — absolute paths and ordinary relative paths pass through
// byte-identical, so the since-last-run findings delta keeps matching
// them across runs. A `--` end-of-options separator is deliberately NOT
// used: ruff/bandit/shellcheck/mypy append flags AFTER the path list,
// and everything after `--` would be swallowed as a positional.
QString flagSafeScopedPathImpl(const QString &p) {
    return p.startsWith(QLatin1Char('-'))
               ? QStringLiteral("./") + p
               : p;
}

// Per-tool argv builder. v1 uses minimal sane defaults (parallel to
// the /audit skill's step 5). ANTS-1456 — `src/` existence is
// auto-detected so flat-layout projects (RetroArch et al.) no longer
// pass `-I src/` against a missing directory and silently parse no
// sources. ANTS-1464 — `projectConfig` overrides argv per-tool when
// the project ships a `.audit-config.json` or
// `docs/private/audit/audit-config.json`.
QStringList toolArgv(const QString &tool, const QString &projectRoot,
                     const QJsonObject &projectConfig = {},
                     const QStringList &scopedPaths = {},
                     const QStringList &scopedChecks = {},
                     const QString &gitleaksConfig = {}) {
    // ANTS-2185 — guard every scoped positional against argv
    // option-injection before it reaches any tool branch's bare append.
    // This is the single chokepoint where positionals (from the resolved
    // scope set OR the `req.paths` fallback) become child argv.
    QStringList scoped;
    scoped.reserve(scopedPaths.size());
    for (const QString &p : scopedPaths)
        scoped += flagSafeScopedPathImpl(p);
    // ANTS-1464 — project-side override wins. ANTS-1456 cold-eyes
    // follow-up: every arg is validated through isAuditArgSafe()
    // before it reaches child argv. If ANY arg fails, the whole
    // override is discarded (fail-safe — the tool falls back to the
    // hardened built-in argv) so a single bad entry can't silently
    // drop adjacent safe-looking flags.
    //
    // ANTS-1512 — when scopedPaths/scopedChecks are passed, the
    // project-config override is BYPASSED. Scoped invocations are a
    // narrow-on-purpose mode; the project's full-run defaults would
    // re-broaden the scope.
    if (projectConfig.contains(tool) && scoped.isEmpty()
        && scopedChecks.isEmpty()) {
        const QJsonObject cfg =
            projectConfig.value(tool).toObject();
        const QJsonValue v = cfg.value(QStringLiteral("args"));
        if (v.isArray()) {
            QStringList args;
            bool allSafe = true;
            const QJsonArray arr = v.toArray();
            for (const QJsonValue &av : arr) {
                const QString s = av.toString();
                if (!isAuditArgSafe(s)) { allSafe = false; break; }
                args.append(s);
            }
            if (allSafe && !args.isEmpty()) return args;
            // else: fall through to the default argv path.
        }
    }
    // ANTS-1456 — auto-detect src/ for flat-layout projects.
    const bool hasSrcDir = QFileInfo(
        projectRoot + QLatin1String("/src")).isDir();
    const QString srcRoot = hasSrcDir
        ? QStringLiteral("src")
        : QStringLiteral(".");
    if (tool == QLatin1String("cppcheck")) {
        // ANTS-2182 — hardened base flags. Suppress the include-resolution
        // categories that flood the MCP audit path (missingIncludeSystem
        // etc.) — mirrors the in-app AuditDialog suppress set, and is the
        // no-compile-DB fallback that keeps the result usable without one.
        QStringList args = {QStringLiteral("--library=qt"),
                            QStringLiteral("--enable=all"),
                            QStringLiteral("--std=c++20"),
                            QStringLiteral("--quiet"),
                            QStringLiteral("--inline-suppr"),
                            QStringLiteral("--suppress=missingInclude"),
                            QStringLiteral("--suppress=missingIncludeSystem"),
                            QStringLiteral("--suppress=unmatchedSuppression"),
                            QStringLiteral("--suppress=unknownMacro"),
                            QStringLiteral("--suppress=invalidSuppression")};
        // ANTS-1512 — a narrowed (since-last-run) scope wants exactly those
        // files; it bypasses the whole-project compile DB on purpose.
        if (!scoped.isEmpty()) {
            args += QStringLiteral("-I");
            args += srcRoot;
            args += scoped;
            return args;
        }
        // ANTS-2182 — full run: drive cppcheck off the compile DB when one
        // exists so it resolves Qt system headers + per-TU flags from the
        // build (kills the missingIncludeSystem flood AND the `namespace X {`
        // mis-parsed-as-C syntaxErrors). Fall back to the src/ scan otherwise.
        const QString compileDb = AuditEngine::resolveCompileCommands(projectRoot);
        if (!compileDb.isEmpty()) {
            args += QStringLiteral("--project=") + compileDb;
        } else {
            args += QStringLiteral("-I");
            args += srcRoot;
            args += srcRoot;
        }
        return args;
    }
    if (tool == QLatin1String("clazy")) {
        // ANTS-2182 — resolve the compile DB via the shared probe (build/,
        // build-fast/, …) instead of a hardcoded build/ path: clazy returns
        // 0 findings against a non-existent -p target when the DB lives in
        // an iteration tree like build-fast/.
        const QString compileDb = AuditEngine::resolveCompileCommands(projectRoot);
        QStringList args = {QStringLiteral("-checks=level1"),
                            QStringLiteral("-p"),
                            compileDb.isEmpty()
                                ? projectRoot + QLatin1String("/build/compile_commands.json")
                                : compileDb};
        // ANTS-1504 — narrowing scopes append the changed source files;
        // clazy-standalone takes source positionals like clang-tidy.
        if (!scoped.isEmpty()) args += scoped;
        return args;
    }
    // ANTS-1512 — clang-tidy scoped invocation. Default argv when no
    // paths given is a no-op (the tool needs explicit file args), so
    // we surface a graceful failure via empty argv → "crashed" status.
    if (tool == QLatin1String("clang-tidy")) {
        // ANTS-2182 — clang-tidy -p takes the build DIR holding
        // compile_commands.json; resolve it via the shared probe rather than
        // hardcoding build/.
        const QString compileDb = AuditEngine::resolveCompileCommands(projectRoot);
        const QString tidyBuildDir = compileDb.isEmpty()
            ? projectRoot + QLatin1String("/build")
            : QFileInfo(compileDb).absolutePath();
        QStringList args = {QStringLiteral("-p"), tidyBuildDir};
        if (!scopedChecks.isEmpty()) {
            // clang-tidy --checks syntax: comma-joined, leading `-`
            // opts a check OUT. Build the joined value here; the
            // outer runner has already validated each entry.
            args += QStringLiteral("--checks=-*,") + scopedChecks.join(QChar(','));
        }
        if (!scoped.isEmpty()) args += scoped;
        return args;
    }
    // ANTS-1504 — the file-oriented tools below take the narrowed file set
    // (when non-empty) in place of their default whole-tree target.
    if (tool == QLatin1String("ruff")) {
        QStringList args = {QStringLiteral("check")};
        if (!scoped.isEmpty()) args += scoped;
        else { args += QStringLiteral("."); args += toolExclusionArgs(tool); }
        args += QStringLiteral("--output-format=json");
        return args;
    }
    if (tool == QLatin1String("bandit")) {
        QStringList args;
        if (!scoped.isEmpty()) args += scoped;  // explicit files
        else {
            args += {QStringLiteral("-r"), srcRoot};       // recurse src dir
            args += toolExclusionArgs(tool);               // ANTS-3394
        }
        args += {QStringLiteral("-f"), QStringLiteral("json"),
                 QStringLiteral("-ll")};
        return args;
    }
    if (tool == QLatin1String("semgrep")) {
        QStringList args = {QStringLiteral("--json"),
                            QStringLiteral("--timeout"),
                            QStringLiteral("60"),
                            QStringLiteral("--metrics=off"),
                            QStringLiteral("--config"),
                            QStringLiteral("p/security-audit")};
        if (!scoped.isEmpty()) args += scoped;
        else { args += QStringLiteral("."); args += toolExclusionArgs(tool); }
        return args;
    }
    if (tool == QLatin1String("gitleaks")) {
        QStringList args = {QStringLiteral("detect"),
                            QStringLiteral("--no-banner"),
                            QStringLiteral("--no-git"),
                            QStringLiteral("--redact"),
                            QStringLiteral("--report-format"),
                            QStringLiteral("json")};
        // ANTS-2016 — the allowlist config prunes build/ + .audit_cache/
        // from the --no-git filesystem walk (those dirs are .gitignored, so
        // gitleaks would otherwise scan multi-GB of build output + archived
        // audit JSON). Empty path = config write failed → run unfiltered.
        if (!gitleaksConfig.isEmpty())
            args << QStringLiteral("--config") << gitleaksConfig;
        return args;
    }
    if (tool == QLatin1String("trivy")) {
        // trivy fs is never file-scoped (it skips under narrowing), so it
        // always scans the whole tree — the exclusion set keeps it off the
        // build-output / media dirs that make it choke (ANTS-3394).
        QStringList args = {QStringLiteral("fs"),
                            QStringLiteral("--scanners"),
                            QStringLiteral("vuln,secret"),
                            QStringLiteral("--severity"),
                            QStringLiteral("HIGH,CRITICAL"),
                            QStringLiteral("--quiet"),
                            QStringLiteral("--format"),
                            QStringLiteral("json")};
        args += toolExclusionArgs(tool);
        args += QStringLiteral(".");
        return args;
    }
    if (tool == QLatin1String("shellcheck")) {
        // shellcheck wants files on argv; v1 passes the `scripts` dir and
        // lets it discover *.sh. ANTS-1504 — narrowing scopes pass the
        // changed shell files instead.
        QStringList args = {QStringLiteral("-f"), QStringLiteral("json"),
                            QStringLiteral("-S"), QStringLiteral("warning")};
        if (!scoped.isEmpty()) args += scoped;
        else                        args += QStringLiteral("scripts");
        return args;
    }
    if (tool == QLatin1String("mypy")) {
        QStringList args = {QStringLiteral("--no-color-output")};
        if (!scoped.isEmpty()) args += scoped;
        else { args += toolExclusionArgs(tool); args += QStringLiteral("."); }
        return args;
    }
    return {};
}

// ANTS-1351-INV-10 — TTL cache for QStandardPaths::findExecutable.
struct ToolResolveCacheEntry {
    QString absolutePath;
    qint64  resolvedAtMs = 0;
};
QHash<QString, ToolResolveCacheEntry> g_toolResolveCache;
std::mutex                            g_toolResolveCacheMutex;

QString resolveToolAbsolute(const QString &tool) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard<std::mutex> lk(g_toolResolveCacheMutex);
        auto it = g_toolResolveCache.find(tool);
        if (it != g_toolResolveCache.end() &&
            now - it->resolvedAtMs < kToolResolveCacheTtlMs) {
            return it->absolutePath;
        }
    }
    QString resolved = QStandardPaths::findExecutable(tool);
    {
        std::lock_guard<std::mutex> lk(g_toolResolveCacheMutex);
        g_toolResolveCache[tool] =
            ToolResolveCacheEntry{resolved, now};
    }
    return resolved;
}

// Build the scrubbed env for a child tool — ANTS-1351-INV-10.
QProcessEnvironment buildChildEnv() {
    const QProcessEnvironment src =
        QProcessEnvironment::systemEnvironment();
    QProcessEnvironment out;
    const QStringList keys = src.keys();
    for (const QString &k : keys) {
        // Blocklist match (exact OR AWS_* prefix).
        if (kEnvBlocklist().contains(k)) continue;
        if (k.startsWith(QLatin1String("AWS_"))) continue;
        // Allowlist match (exact OR LC_* prefix).
        const bool allowExact = kEnvAllowlist().contains(k);
        const bool allowLcPrefix = k.startsWith(QLatin1String("LC_"));
        if (allowExact || allowLcPrefix) {
            out.insert(k, src.value(k));
        }
    }
    return out;
}

// ANTS-2016 — write the throwaway gitleaks config that prunes the
// build/.audit_cache dirs from the --no-git walk. `[extend] useDefault`
// keeps the full default secret-rule pack; `[allowlist] paths` are regexes
// gitleaks (≥ 8) checks against each path before reading it, so the walk
// itself is pruned (verified 68 s → ~3 s on this repo). Returns the config
// path, or {} on any write failure — the caller then runs gitleaks
// unfiltered (slow, but never broken).
QString writeGitleaksExcludeConfig(const QString &canonProject) {
    static const char kToml[] =
        "[extend]\n"
        "useDefault = true\n"
        "[allowlist]\n"
        "paths = [\n"
        "  '''(^|/)build(-[A-Za-z0-9._-]+)?/''',\n"
        "  '''(^|/)\\.audit_cache/''',\n"
        "]\n";
    const QString dir = canonProject + QLatin1String("/.audit_cache");
    if (!QDir().mkpath(dir)) return {};
    // Lives under the allowlisted .audit_cache/ dir, so it never scans itself.
    QSaveFile f(dir + QLatin1String("/.gitleaks-audit-run.toml"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    if (f.write(kToml, static_cast<qint64>(sizeof(kToml) - 1)) < 0) return {};
    if (!f.commit()) return {};
    return f.fileName();
}

// ANTS-1351-INV-15 — sanity-check the since-tag string for argv injection.
bool isScopeTagSafe(const QString &tag) {
    static const QRegularExpression rx(
        QStringLiteral("^[A-Za-z0-9._/+-]{1,128}$"));
    return rx.match(tag).hasMatch() &&
           !tag.startsWith(QLatin1Char('-'));
}

QString sessionIdToken() {
    // ANTS-1351-INV-12 — PID + start-time epoch makes the SARIF path
    // unique across server restarts (PID-reuse safe).
    static const QString sid =
        QStringLiteral("%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QDateTime::currentSecsSinceEpoch());
    return sid;
}

int g_sarifSeq = 0;
std::mutex g_sarifSeqMutex;

// Fallback SARIF path when .audit_cache is read-only.
// Prefer XDG cache (`~/.cache/Ants Terminal/audit/`) so fallbacks land
// in a 0700-eligible per-user directory; /tmp only as a last resort and
// with a 16-byte random suffix to defeat the predictable-filename
// pre-create attack that the older `/tmp/audit-<pid>-<epoch>-<seq>`
// scheme allowed (indie-review-2026-05-19 audit-pipeline H2).
// ── ANTS-3614 — reap orphaned fallback artifacts.
// The PRIMARY artifacts land in `<root>/.audit_cache/` and are garbage-
// collected by AuditCache's manifest-driven reaper. The fallbacks written
// here do not: they live outside the cache dir, which that reaper refuses
// to touch by design. So every read-only-root sweep used to leak its SARIF
// (and sibling HTML) forever. Sweep them here — lazily, once per process,
// on the first fallback allocation — so the cleanup costs nothing on the
// overwhelmingly common writable-root path.
//
// Only our own `audit-*.{sarif,html}` names are considered, symlinks are
// excluded (never follow a squatted link out of the directory), and in
// /tmp another user's same-named file simply fails to unlink under the
// sticky bit.
constexpr qint64 kFallbackReapAgeSecs = 7 * 24 * 60 * 60;  // one week

void reapStaleFallbackArtifacts(const QString &dirPath) {
    QDir dir(dirPath);
    if (!dir.exists()) return;
    const QDateTime cutoff =
        QDateTime::currentDateTime().addSecs(-kFallbackReapAgeSecs);
    const QFileInfoList stale = dir.entryInfoList(
        {QStringLiteral("audit-*.sarif"), QStringLiteral("audit-*.html")},
        QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &fi : stale) {
        if (fi.lastModified() < cutoff)
            QFile::remove(fi.absoluteFilePath());
    }
}

void reapFallbackArtifactsOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        const QString cacheRoot =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!cacheRoot.isEmpty())
            reapStaleFallbackArtifacts(cacheRoot + QLatin1String("/audit"));
        reapStaleFallbackArtifacts(QStringLiteral("/tmp"));
    });
}

QString allocSarifPath() {
    std::lock_guard<std::mutex> lk(g_sarifSeqMutex);
    reapFallbackArtifactsOnce();
    const int seq = ++g_sarifSeq;
    const QString rand =
        QUuid::createUuid().toRfc4122().toHex().left(16);
    // Try $XDG_CACHE_HOME/Ants Terminal/audit/<random>.sarif first.
    const QString cacheRoot =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!cacheRoot.isEmpty()) {
        const QString dir = cacheRoot + QLatin1String("/audit");
        if (QDir().mkpath(dir)) {
            return QStringLiteral("%1/audit-%2-%3-%4.sarif")
                .arg(dir, sessionIdToken(), QString::number(seq), rand);
        }
    }
    // /tmp last-resort with a random suffix — still predictable-by-pid
    // but the random tail breaks the symlink-squat window.
    return QStringLiteral("/tmp/audit-%1-%2-%3.sarif")
        .arg(sessionIdToken(), QString::number(seq), rand);
}

// Heuristic finding count + sample extraction from raw tool output.
// v1: line-based; v2 follow-up will parse per-tool JSON via
// AuditEngine helpers.
struct ParsedOutput {
    int        rawCount = 0;
    int        suppressedCount = 0;  // ANTS-1820 — learned-FP matches
    QJsonArray samples;  // first N matching lines as {file,line,message,severity}
    // ANTS-1870 — the FULL per-finding set (every non-suppressed finding,
    // not just the `sampleCap` preview), each carrying its `fp`. Bounded at
    // kSarifFindingsMax; `findingsTruncated` is set when that ceiling is hit.
    QJsonArray findings;
    bool       findingsTruncated = false;
    // ANTS-3395 — a JSON-emitting tool logged a fatal abort (e.g. trivy
    // "run error: fs scan error") and produced no parseable findings doc.
    // The runner promotes this to a "crashed" status so the tool surfaces in
    // incomplete_tools[] (ANTS-2032) instead of a misleading clean run.
    bool       aborted = false;
    // ANTS-3585 — files cppcheck flagged with a frontend parse-failure id
    // (syntaxError / internalError / …): the whole TU failed to parse, so it
    // got zero real coverage. Deduped; empty for non-cppcheck / clean runs.
    QStringList parseFailureFiles;
};

// ANTS-1820 — learned-FP suppression for the headless path. The GUI's
// `applyLearnedFpSuppressions` operates on `Finding` objects, which the v1
// runner never materialises; this path consumes the same ledger via the
// shared content fingerprint. Cross-path matching works for the line-based
// tools (cppcheck/clazy/clang-tidy/mypy), whose check id IS the tool name —
// matching the GUI's `Finding::checkId`. (shellcheck was listed here but is
// a JSON tool per isJsonFindingTool below — ANTS-3395.) JSON tools key on the tool's own
// `check_id`, a different namespace, so they don't cross-match (the same v1
// limitation under which the runner already does no per-finding filtering).
bool isLearnedFp(const QSet<QString> &learnedFps, const QString &file,
                 const QString &checkId, const QString &message) {
    if (learnedFps.isEmpty()) return false;
    return learnedFps.contains(
        ants::auditfp::computeFingerprint(file, checkId, message));
}

// ANTS-3395 — tools whose findings are a single JSON document. Their output
// must be parsed as JSON ONLY: the line-based fallback would mis-read a
// progress bar (bandit's Rich bar) or a log line (trivy's FATAL) as a
// finding. The plain-text tools (cppcheck / clazy / clang-tidy / mypy) stay
// line-based — their findings have no JSON form.
bool isJsonFindingTool(const QString &tool) {
    return tool == QLatin1String("ruff")    || tool == QLatin1String("bandit")
        || tool == QLatin1String("semgrep") || tool == QLatin1String("gitleaks")
        || tool == QLatin1String("trivy")   || tool == QLatin1String("shellcheck");
}

// ANTS-3395 — extract the single top-level JSON document embedded in a
// (possibly progress-bar-prefixed / log-suffixed) tool stream: the slice from
// the first '{'/'[' to the matching last '}'/']'. Returns empty when no JSON
// is present (e.g. trivy aborted before emitting any). This is what lets a
// trailing Rich progress bar or log line no longer break the JSON parse and
// dump the whole tool into the line-based fallback.
QString extractJsonDocument(const QString &raw) {
    const int objStart = raw.indexOf(QLatin1Char('{'));
    const int arrStart = raw.indexOf(QLatin1Char('['));
    int   start = -1;
    QChar close;
    if (arrStart >= 0 && (objStart < 0 || arrStart < objStart)) {
        start = arrStart;
        close = QLatin1Char(']');
    } else if (objStart >= 0) {
        start = objStart;
        close = QLatin1Char('}');
    }
    if (start < 0) return {};
    const int end = raw.lastIndexOf(close);
    if (end <= start) return {};
    return raw.mid(start, end - start + 1);
}

// ANTS-3395 — a JSON tool that emitted NO parseable findings doc but logged a
// fatal abort. trivy's log line is `<iso-ts>\tFATAL\trun error: fs scan
// error: ...`; the combination (FATAL level + a run/scan error) means the
// scan aborted rather than ran clean. Matched conservatively (both tokens
// required) so a finding message merely containing the word "fatal" never
// trips it.
bool hasToolAbortMarker(const QString &raw) {
    static const QRegularExpression rxLevel(QStringLiteral("(^|\\s)FATAL(\\s|$)"));
    static const QRegularExpression rxErr(
        QStringLiteral("run error:|scan error"),
        QRegularExpression::CaseInsensitiveOption);
    return rxLevel.match(raw).hasMatch() && rxErr.match(raw).hasMatch();
}

ParsedOutput parseToolOutput(const QString &tool,
                             const QString &raw,
                             int sampleCap,
                             const QSet<QString> &learnedFps,
                             // ANTS-3615 — project-local `.audit_allowlist.json`
                             // entries, empty when suppressions:"none". Applied
                             // at the same seam as the learned-FP ledger so both
                             // suppression sources share one drop point.
                             const QList<AuditEngine::AllowlistEntry> &allowlist
                                 = {}) {
    ParsedOutput out;
    if (raw.trimmed().isEmpty()) return out;
    // ANTS-3395 — JSON tools (semgrep/bandit/ruff/gitleaks/trivy/shellcheck)
    // emit a single JSON document, sometimes wrapped in progress-bar / log
    // noise on the merged stream. Parse ONLY the extracted JSON span, and
    // never fall through to the line-based path below (which would turn a
    // progress bar or a FATAL log line into a phantom finding). The plain-text
    // tools fall straight through to the line-based parser.
    if (isJsonFindingTool(tool)) {
        const QString jsonDoc = extractJsonDocument(raw);
        QJsonParseError err;
        const QJsonDocument doc = jsonDoc.isEmpty()
            ? QJsonDocument()
            : QJsonDocument::fromJson(jsonDoc.toUtf8(), &err);
        if (!jsonDoc.isEmpty() && err.error == QJsonParseError::NoError) {
            // Recognise common shapes; fall through to line-count if unknown.
            QJsonArray arr;
            if (doc.isArray()) arr = doc.array();
            else if (doc.isObject()) {
                const QJsonObject o = doc.object();
                if (o.contains(QStringLiteral("results")) &&
                    o.value(QStringLiteral("results")).isArray()) {
                    arr = o.value(QStringLiteral("results")).toArray();
                } else if (o.contains(QStringLiteral("Results")) &&
                           o.value(QStringLiteral("Results")).isArray()) {
                    arr = o.value(QStringLiteral("Results")).toArray();
                }
            }
            out.rawCount = arr.size();
            // ANTS-3590 — count of non-finding placeholder entries dropped
            // below; subtracted from rawCount after the loop.
            int emptyEntries = 0;
            // ANTS-1870 — iterate EVERY entry so `findings` collects the full
            // non-suppressed set; the `< sampleCap` gate now applies only to
            // the `samples` preview append below.
            for (int i = 0; i < arr.size(); ++i) {
                const QJsonObject e = arr.at(i).toObject();
                // Best-effort field extraction (tools vary).
                QString fileStr;
                if (e.contains(QStringLiteral("filename")))
                    fileStr = e.value(QStringLiteral("filename")).toString();
                else if (e.contains(QStringLiteral("path")))
                    fileStr = e.value(QStringLiteral("path")).toString();
                else if (e.contains(QStringLiteral("File")))
                    fileStr = e.value(QStringLiteral("File")).toString();
                QString msg;
                if (e.contains(QStringLiteral("message"))) {
                    const QJsonValue mv = e.value(QStringLiteral("message"));
                    msg = mv.isObject()
                        ? mv.toObject().value(QStringLiteral("text")).toString()
                        : mv.toString();
                } else if (e.contains(QStringLiteral("Description"))) {
                    msg = e.value(QStringLiteral("Description")).toString();
                }
                const QString ruleStr =
                    e.value(QStringLiteral("check_id")).toString();
                // ANTS-3590 — a JSON entry that yields NO file, NO message AND
                // NO rule is not a finding: Trivy's native `Results[]` are
                // per-target containers, so a clean scan emits one such empty
                // entry that would otherwise become a blank placeholder SARIF
                // result (uri:"", ruleId:"", message:"", startLine:0) and read
                // as "1 actionable". Drop it — never count, sample, or emit.
                if (fileStr.isEmpty() && msg.isEmpty() && ruleStr.isEmpty()) {
                    ++emptyEntries;
                    continue;
                }
                // ANTS-1820 — drop learned false positives before the sample
                // is built; rawCount keeps the tool's raw total.
                if (isLearnedFp(learnedFps, fileStr, ruleStr, msg)) {
                    ++out.suppressedCount;
                    continue;
                }
                // ANTS-3615 — same drop for a `.audit_allowlist.json` match.
                // JSON tools key on their own check id, matching the GUI's
                // Finding::checkId for these detectors.
                if (AuditEngine::allowlisted(allowlist, ruleStr, fileStr, msg)) {
                    ++out.suppressedCount;
                    continue;
                }
                QJsonObject s;
                if (!fileStr.isEmpty()) s["file"] = fileStr;
                if (e.contains(QStringLiteral("line_number")))
                    s["line"] = e.value(QStringLiteral("line_number"));
                else if (e.contains(QStringLiteral("start"))) {
                    const QJsonObject st =
                        e.value(QStringLiteral("start")).toObject();
                    s["line"] = st.value(QStringLiteral("line"));
                }
                s["message"]  = internal::capMessage(msg);
                s["rule"]     = ruleStr;
                s["severity"] = e.value(QStringLiteral("severity")).toString();
                // ANTS-1870 — full set with fp (line-insensitive identity),
                // bounded at kSarifFindingsMax. JSON tools key the fingerprint
                // on the tool's own check_id (ruleStr), matching § 2.2.
                if (out.findings.size() < kSarifFindingsMax) {
                    QJsonObject f = s;
                    f["fp"] = ants::auditfp::computeFingerprint(
                        fileStr, ruleStr, msg);
                    out.findings.append(f);
                } else {
                    out.findingsTruncated = true;
                }
                if (out.samples.size() < sampleCap) out.samples.append(s);
            }
            // ANTS-3590 — exclude the dropped placeholder entries from the raw
            // total so a clean scan reports 0, not 1.
            out.rawCount = arr.size() - emptyEntries;
            // Honour SARIF cap.
            if (out.rawCount > kSarifFindingsMax)
                out.rawCount = kSarifFindingsMax;
            return out;
        }
        // ANTS-3395 — a JSON tool with no parseable findings doc. Do NOT fall
        // through to the line-based parser (it would manufacture findings from
        // a progress bar / log line). If the stream carries a fatal-abort
        // marker, flag it so the runner records the tool as crashed.
        if (hasToolAbortMarker(raw)) out.aborted = true;
        return out;
    }
    // Line-based fallback for plain-text tools.
    const QStringList lines = raw.split(QChar('\n'), Qt::SkipEmptyParts);
    static const QRegularExpression rxFileLine(
        QStringLiteral("^([^:]+):(\\d+)(?::\\d+)?:\\s*(.+)$"));
    // ANTS-1816 — count only location-shaped lines, not every non-empty line.
    // Tool banners, "N files scanned", and blank-separated context were being
    // counted as findings, inflating rawCount / noiseRatePct (the headline
    // numbers audit_run returns + persists) for plain-text tools.
    int located = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch m = rxFileLine.match(lines.at(i));
        if (!m.hasMatch()) continue;
        const QString fileStr = m.captured(1);
        const QString msg     = m.captured(3);
        // ANTS-3472 — mypy `note:` lines (the [annotation-unchecked] /
        // check-untyped-defs hints, and the "see here" context under an
        // error) are informational, never a standalone finding. A deps-less
        // mypy over untyped helpers emits them alone, so counting them
        // inflates rawCount / total_actionable and can mis-route a
        // /close-phase triage into a phantom fix-pass on a tree the gated
        // `mypy` reports clean. Drop them before `located`; the paired
        // `error:` line (if any) still counts.
        if (tool == QLatin1String("mypy")
            && msg.startsWith(QLatin1String("note:"), Qt::CaseInsensitive)) {
            continue;
        }
        // ANTS-3585 — cppcheck tags every finding with its check-id as a
        // trailing `[id]` (default template). A parse-failure id means the
        // whole TU failed to parse (cppcheck's frontend can't handle the
        // dialect, e.g. C++23) — that file got ZERO coverage, so record it so
        // a caller isn't misled by its absence from the findings. Recorded
        // before the learned-FP `continue` below: the coverage gap is real
        // even if the diagnostic itself is suppressed.
        if (tool == QLatin1String("cppcheck")) {
            static const QRegularExpression rxCheckId(
                QStringLiteral("\\[([A-Za-z0-9_]+)\\]\\s*$"));
            static const QSet<QString> kParseFailureIds = {
                QStringLiteral("syntaxError"),
                QStringLiteral("internalError"),
                QStringLiteral("internalAstError"),
                QStringLiteral("preprocessorErrorDirective"),
                QStringLiteral("cppcheckError"),
            };
            const QRegularExpressionMatch idm = rxCheckId.match(msg);
            if (idm.hasMatch()
                && kParseFailureIds.contains(idm.captured(1))
                && !out.parseFailureFiles.contains(fileStr)) {
                out.parseFailureFiles.append(fileStr);
            }
        }
        ++located;  // rawCount keeps the raw total, learned FPs included
        // ANTS-1820 — the line-based tools (cppcheck/clazy/clang-tidy/mypy)
        // key the ledger by tool name, matching the GUI's Finding::checkId,
        // so learned FPs recorded in the dialog suppress here too.
        // (shellcheck is NOT one of them — isJsonFindingTool routes it down
        // the JSON branch above, where the tool's own check_id is the key.)
        if (isLearnedFp(learnedFps, fileStr, tool, msg)) {
            ++out.suppressedCount;
            continue;
        }
        // ANTS-3615 — same drop for a `.audit_allowlist.json` match. The
        // line-based tools' check id IS the tool name (as above).
        if (AuditEngine::allowlisted(allowlist, tool, fileStr, msg)) {
            ++out.suppressedCount;
            continue;
        }
        QJsonObject s;
        s["file"]    = fileStr;
        s["line"]    = m.captured(2).toInt();
        s["message"] = internal::capMessage(msg);
        s["rule"]    = tool;
        s["severity"]= QStringLiteral("UNKNOWN");
        // ANTS-1870 — full set with fp; line-based tools key the fingerprint
        // on the tool name (matching the GUI's Finding::checkId), § 2.2.
        if (out.findings.size() < kSarifFindingsMax) {
            QJsonObject f = s;
            f["fp"] = ants::auditfp::computeFingerprint(fileStr, tool, msg);
            out.findings.append(f);
        } else {
            out.findingsTruncated = true;
        }
        if (out.samples.size() < sampleCap) out.samples.append(s);
    }
    out.rawCount = located;
    return out;
}

// ANTS-1870 — build one SARIF result entry from a `{file,line,rule,
// severity,message,fp}` finding object. `carried` tags the result with
// `properties.carried_forward:true` so a consumer can tell a freshly
// re-scanned finding from an assumed-still-present one.
QJsonObject sarifResultFromFinding(const QJsonObject &s, bool carried) {
    QJsonObject result;
    result["ruleId"] = s.value(QStringLiteral("rule")).toString();
    QJsonObject msg;
    msg["text"] = s.value(QStringLiteral("message")).toString();
    result["message"] = msg;
    QJsonObject artLoc;
    artLoc["uri"] = s.value(QStringLiteral("file")).toString();
    QJsonObject region;
    region["startLine"] = s.value(QStringLiteral("line")).toInt();
    QJsonObject physLoc;
    physLoc["artifactLocation"] = artLoc;
    physLoc["region"]          = region;
    QJsonObject loc;
    loc["physicalLocation"] = physLoc;
    QJsonArray locs;
    locs.append(loc);
    result["locations"] = locs;
    if (carried) {
        QJsonObject props;
        props["carried_forward"] = true;
        result["properties"] = props;
    }
    return result;
}

// Emit a SARIF v2.1.0 document with each tool as a driver entry +
// each tool's raw output as a single notification. ANTS-1870 — the
// per-tool `results[]` now carry the FULL parsed finding set
// (`findingsByTool`), not the capped samples; under since-last-run a
// non-empty `carriedForward` (prior findings on untouched files) is
// emitted as one extra synthetic run tagged carried_forward. The whole
// document is bounded at kSarifFindingsMax results, current-first — the
// carried-forward run sheds first on overflow.
bool writeSarif(const QString &path,
                const QHash<QString, ToolResult> &byTool,
                const QHash<QString, QString> &rawByTool,
                const QString &rootCanonical,
                const QHash<QString, QJsonArray> &findingsByTool,
                const QJsonArray &carriedForward = {}) {
    QJsonArray  runsArr;
    int         emitted = 0;   // running result count vs kSarifFindingsMax
    // ANTS-1576 — capture once, attach to every run we emit. The probe
    // forks at most three short git subprocesses; runs once per
    // writeSarif call (not per tool).
    const QJsonArray vcpBlock = AuditEngine::buildVcsProvenanceBlock(
        rootCanonical);
    for (auto it = byTool.constBegin(); it != byTool.constEnd(); ++it) {
        const ToolResult &tr = it.value();
        QJsonObject driver;
        driver["name"]    = tr.tool;
        driver["version"] = QStringLiteral("auto");
        QJsonObject tool;
        tool["driver"] = driver;
        QJsonObject run;
        run["tool"] = tool;
        QJsonObject inv;
        inv["executionSuccessful"] = (tr.status == QLatin1String("ok"));
        inv["exitCode"]            =
            (tr.status == QLatin1String("ok"))         ? 0 :
            (tr.status == QLatin1String("timed_out"))  ? 124 : 1;
        // ANTS-1456(c) — distinguish "ran cleanly, zero findings"
        // from "ran cleanly but emitted notifications" (typically
        // config-load warnings that suppressed all findings). The
        // SARIF spec keeps `executionSuccessful: true` either way,
        // so the signal lives on `invocation.properties` for any
        // caller that wants to detect "looks clean but isn't".
        const QString rawForProp = rawByTool.value(it.key());
        const bool hasNotifications =
            (tr.status == QLatin1String("ok")) &&
            !rawForProp.isEmpty() &&
            tr.rawCount == 0;
        if (hasNotifications) {
            QJsonObject invProps;
            invProps["executionSuccessfulWithConfigWarnings"] = true;
            inv["properties"] = invProps;
        }
        QJsonArray invs;
        invs.append(inv);
        run["invocations"] = invs;
        // Notifications carry the raw tool output excerpt.
        const QString raw = rawByTool.value(it.key());
        if (!raw.isEmpty()) {
            QJsonObject notif;
            notif["level"]   = (tr.status == QLatin1String("ok"))
                ? QStringLiteral("note")
                : QStringLiteral("error");
            QJsonObject msg;
            msg["text"] = raw.left(kSarifFindingsMax * 256);
            notif["message"] = msg;
            QJsonArray notifs;
            notifs.append(notif);
            run["toolExecutionNotifications"] = notifs;
        }
        // ANTS-1870 — results[] from the FULL parsed finding set (falls back
        // to the capped samples only when no full set was threaded, e.g. a
        // /tmp legacy caller). current-first cap vs kSarifFindingsMax.
        QJsonArray results;
        const QJsonArray &src = findingsByTool.contains(it.key())
            ? findingsByTool[it.key()] : tr.samples;
        for (const QJsonValue &v : src) {
            if (emitted >= kSarifFindingsMax) break;
            results.append(sarifResultFromFinding(v.toObject(), false));
            ++emitted;
        }
        run["results"] = results;
        // ANTS-1576 — attach VCS provenance when capture succeeded.
        if (!vcpBlock.isEmpty()) {
            run["versionControlProvenance"] = vcpBlock;
        }
        runsArr.append(run);
    }
    // ANTS-1870 — carry-forward synthetic run: prior findings on untouched
    // files, assumed still present, tagged carried_forward. Sheds first on
    // the kSarifFindingsMax overflow (emitted from current runs above first).
    if (!carriedForward.isEmpty()) {
        QJsonArray results;
        for (const QJsonValue &v : carriedForward) {
            if (emitted >= kSarifFindingsMax) break;
            results.append(sarifResultFromFinding(v.toObject(), true));
            ++emitted;
        }
        QJsonObject driver;
        driver["name"]    = QStringLiteral("carried-forward");
        driver["version"] = QStringLiteral("auto");
        QJsonObject tool;
        tool["driver"] = driver;
        QJsonObject run;
        run["tool"]    = tool;
        run["results"] = results;
        if (!vcpBlock.isEmpty())
            run["versionControlProvenance"] = vcpBlock;
        runsArr.append(run);
    }
    QJsonObject doc;
    doc["version"] = QStringLiteral("2.1.0");
    doc["$schema"] = QStringLiteral(
        "https://json.schemastore.org/sarif-2.1.0.json");
    doc["runs"]    = runsArr;

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    setOwnerOnlyPerms(f);
    const QByteArray bytes =
        QJsonDocument(doc).toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size()) return false;
    if (!f.commit()) return false;
    setOwnerOnlyPerms(path);
    fsyncParentDir(path);  // ANTS-1810 — durable like the cache manifest
    return true;
}

bool writeHtml(const QString &path,
               const QHash<QString, ToolResult> &byTool) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    setOwnerOnlyPerms(f);
    QTextStream s(&f);
    s << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
         "<title>Audit report</title></head><body>"
         "<h1>Audit report</h1><table border='1' cellpadding='4'>"
         "<tr><th>tool</th><th>status</th><th>raw</th><th>after_filter</th>"
         "<th>elapsed_ms</th></tr>";
    for (auto it = byTool.constBegin(); it != byTool.constEnd(); ++it) {
        const ToolResult &tr = it.value();
        s << "<tr><td>" << tr.tool.toHtmlEscaped() << "</td>"
          << "<td>" << tr.status.toHtmlEscaped() << "</td>"
          << "<td>" << tr.rawCount << "</td>"
          << "<td>" << tr.afterFilterCount << "</td>"
          << "<td>" << tr.elapsedMs << "</td></tr>";
    }
    s << "</table></body></html>";
    s.flush();
    if (!f.commit()) return false;
    setOwnerOnlyPerms(path);
    fsyncParentDir(path);  // ANTS-1810 — durable like the cache manifest
    return true;
}

// ── ANTS-1446 — compile_commands.json include-path validation.
//
// Clazy / clang-tidy consume compile_commands.json via the `-p` flag
// and follow every -I / -isystem / -include / -iquote arg verbatim,
// reading those paths' contents into the audit run. A hostile or
// misconfigured file with `-include /home/user/.ssh/id_rsa` directly
// loads the named file into every TU; samples shown back in the audit
// envelope can carry the file's bytes.
//
// v2 validation: walk the JSON, extract every include-style path from
// each entry's `arguments[]` (or `command` string), and refuse the
// audit run if any path escapes the project root AND isn't on the
// system-include allowlist. System paths (`/usr/include`, `/usr/lib`,
// `/opt`, …) are legitimate for any C/C++ project; only paths under
// the user's home / outside the build sandbox raise the alarm.
//
// Same-uid trust model still applies (an attacker with the user's UID
// already has access to anything in the user's tree); this catches
// hostile-clone vectors where the JSON itself is the attack surface
// before the user has noticed.
//
// Helpers below carry an `Impl` suffix and live in the anonymous
// namespace (file-local); `internal::` wrappers exported via the
// header delegate to them so the test bundle can drive them.

// Hardcoded allowlist of safe prefixes for system include locations.
// Order doesn't matter; the helper uses startsWith().
const QStringList &kSystemIncludePrefixes() {
    static const QStringList v = {
        QStringLiteral("/usr/include"),
        QStringLiteral("/usr/lib"),
        QStringLiteral("/usr/lib64"),
        QStringLiteral("/usr/local/include"),
        QStringLiteral("/usr/local/lib"),
        QStringLiteral("/usr/local/lib64"),
        QStringLiteral("/usr/share"),
        QStringLiteral("/opt"),
        QStringLiteral("/lib"),
        QStringLiteral("/lib64"),
    };
    return v;
}

// Extract include-style paths from a clang/gcc argument list. Handles
// both `-I/abs/path` (glued) and `-I /abs/path` (split) forms; same
// for -isystem / -iquote / -include. Returns the raw path strings.
QStringList extractIncludeArgsImpl(const QStringList &args) {
    QStringList out;
    static const QStringList kFlags = {
        QStringLiteral("-I"),
        QStringLiteral("-isystem"),
        QStringLiteral("-iquote"),
        QStringLiteral("-include"),
    };
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args.at(i);
        for (const QString &f : kFlags) {
            if (a == f) {
                if (i + 1 < args.size()) {
                    out << args.at(i + 1);
                    ++i;
                }
                break;
            }
            if (a.startsWith(f) && a.size() > f.size()
                // Don't match `-isystem-something-else`; the next char
                // must be `/`, `.`, `~`, or an alnum for a glued arg.
                && (a.at(f.size()) == QLatin1Char('/')
                 || a.at(f.size()) == QLatin1Char('.')
                 || a.at(f.size()) == QLatin1Char('~')
                 || a.at(f.size()).isLetterOrNumber())) {
                out << a.mid(f.size());
                break;
            }
        }
    }
    return out;
}

// Cheap shell-style splitter for compile_commands.json's `command`
// string when no `arguments[]` array is present. Honours plain
// whitespace and escaped quotes; good enough for the CMake-generated
// shape, which is what we actually consume.
QStringList splitCommandStringImpl(const QString &cmd) {
    QStringList out;
    QString cur;
    bool inDquote = false;
    bool inSquote = false;
    for (int i = 0; i < cmd.size(); ++i) {
        const QChar c = cmd.at(i);
        if (c == QLatin1Char('\\') && i + 1 < cmd.size()) {
            cur.append(cmd.at(i + 1));
            ++i;
            continue;
        }
        if (c == QLatin1Char('"') && !inSquote) {
            inDquote = !inDquote;
            continue;
        }
        if (c == QLatin1Char('\'') && !inDquote) {
            inSquote = !inSquote;
            continue;
        }
        if (c.isSpace() && !inDquote && !inSquote) {
            if (!cur.isEmpty()) { out << cur; cur.clear(); }
            continue;
        }
        cur.append(c);
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

// Decide whether `includePath` is allowed under our policy.
//   - Empty or control-char → reject (`reason` filled in).
//   - Under project root → allow.
//   - Starts with a system prefix → allow.
//   - Else → reject as escape.
// `entryDir` is the compile_commands.json entry's `directory` field
// (used to resolve relative paths). Resolution uses lexical
// concatenation + QDir::cleanPath rather than canonicalisation, so a
// non-existent path still gets a verdict rather than slipping past.
bool isIncludePathAllowedImpl(const QString &includePath,
                              const QString &entryDir,
                              const QString &projectRoot,
                              QString *reason) {
    if (includePath.isEmpty()) {
        if (reason) *reason = QStringLiteral("empty include path");
        return false;
    }
    for (QChar c : includePath) {
        if (c.unicode() < 0x20 || c == QLatin1Char('\\')) {
            if (reason) *reason = QStringLiteral(
                "include path contains control char or backslash");
            return false;
        }
    }

    QString resolved = includePath;
    if (!QDir::isAbsolutePath(resolved)) {
        // Relative to the entry's `directory` field; fall back to
        // projectRoot if `directory` is empty.
        const QString anchor = entryDir.isEmpty() ? projectRoot : entryDir;
        resolved = QDir(anchor).filePath(resolved);
    }
    resolved = QDir::cleanPath(resolved);

    // If the path exists, canonicalise to resolve symlinks (catches
    // /tmp/foo → ../../etc/passwd).
    const QFileInfo fi(resolved);
    if (fi.exists()) {
        const QString canon = fi.canonicalFilePath();
        if (!canon.isEmpty()) resolved = canon;
    }

    if (resolved == projectRoot
     || resolved.startsWith(projectRoot + QLatin1Char('/'))) {
        return true;
    }
    for (const QString &p : kSystemIncludePrefixes()) {
        if (resolved == p
         || resolved.startsWith(p + QLatin1Char('/'))) {
            return true;
        }
    }

    if (reason) {
        *reason = QStringLiteral("path \"%1\" escapes project root and "
                                 "is not under a system-include prefix")
                      .arg(resolved);
    }
    return false;
}

// Validate every include-style path across every entry. Returns true
// on success; on failure, `*offending` carries the first escape (form:
// "{file: …, include: …, reason: …}") and the function short-circuits.
constexpr qint64 kCompileCommandsMaxBytes = 32 * 1024 * 1024;  // 32 MiB
constexpr int    kCompileCommandsMaxEntries = 50000;

bool validateCompileCommandsImpl(const QString &canonProject,
                                 QString *errReason) {
    // Probe the same two locations clazy's default argv uses.
    const QStringList candidates = {
        canonProject + QLatin1String("/build/compile_commands.json"),
        canonProject + QLatin1String("/compile_commands.json"),
    };
    QString chosen;
    for (const QString &c : candidates) {
        if (QFile::exists(c)) { chosen = c; break; }
    }
    if (chosen.isEmpty()) {
        // No JSON → nothing to validate. Clazy will fail at runtime
        // and surface as status "crashed"; that's the v1 behaviour.
        return true;
    }

    QFile f(chosen);
    if (!f.open(QIODevice::ReadOnly)) return true;  // unreadable → skip
    if (f.size() > kCompileCommandsMaxBytes) {
        if (errReason) *errReason = QStringLiteral(
            "compile_commands.json exceeds %1 MiB cap")
                .arg(kCompileCommandsMaxBytes / (1024 * 1024));
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        // Malformed → can't validate; let clazy fail at runtime.
        return true;
    }
    const QJsonArray arr = doc.array();
    if (arr.size() > kCompileCommandsMaxEntries) {
        if (errReason) *errReason = QStringLiteral(
            "compile_commands.json exceeds %1-entry cap")
                .arg(kCompileCommandsMaxEntries);
        return false;
    }
    for (const QJsonValue &v : arr) {
        if (!v.isObject()) continue;
        const QJsonObject e = v.toObject();
        const QString entryDir = e.value(QStringLiteral("directory"))
                                  .toString();
        QStringList args;
        if (e.value(QStringLiteral("arguments")).isArray()) {
            for (const QJsonValue &av :
                 e.value(QStringLiteral("arguments")).toArray()) {
                args << av.toString();
            }
        } else if (e.value(QStringLiteral("command")).isString()) {
            args = splitCommandStringImpl(
                e.value(QStringLiteral("command")).toString());
        }
        const QStringList incl = extractIncludeArgsImpl(args);
        for (const QString &p : incl) {
            QString reason;
            if (!isIncludePathAllowedImpl(p, entryDir, canonProject,
                                          &reason)) {
                if (errReason) {
                    const QString file = e.value(QStringLiteral("file"))
                                          .toString();
                    *errReason = QStringLiteral(
                        "include path %1 (entry %2): %3")
                            .arg(p).arg(file).arg(reason);
                }
                return false;
            }
        }
    }
    return true;
}

}  // namespace

namespace internal {

QString capMessage(const QString &msg) {
    QByteArray u = msg.toUtf8();
    if (u.size() <= kSampleMessageMaxBytes) return msg;
    // Trim to ≤ cap-1 to leave room for "…" (3 bytes UTF-8).
    u.truncate(kSampleMessageMaxBytes - 3);
    // Drop trailing partial UTF-8 sequence.
    while (!u.isEmpty() &&
           (static_cast<unsigned char>(u.back()) & 0xC0) == 0x80) {
        u.chop(1);
    }
    return QString::fromUtf8(u) + QStringLiteral("…");
}

qint64 measureEnvelopeBytes(const QJsonArray &samples,
                            const QHash<QString, ToolResult> &byTool) {
    // Quick measurement: serialise a representative envelope shell +
    // samples to get a Compact byte count.
    QJsonObject env;
    QJsonObject byToolJ;
    for (auto it = byTool.constBegin(); it != byTool.constEnd(); ++it) {
        QJsonObject t;
        t["status"]              = it->status;
        t["elapsed_ms"]          = it->elapsedMs;
        t["raw_count"]           = it->rawCount;
        t["after_filter_count"]  = it->afterFilterCount;
        t["samples"]             = it->samples;
        byToolJ[it.key()]        = t;
    }
    env["by_tool"] = byToolJ;
    if (!samples.isEmpty()) env["top_findings"] = samples;
    return QJsonDocument(env).toJson(QJsonDocument::Compact).size();
}

QStringList incompleteToolNames(const QHash<QString, ToolResult> &byTool) {
    QStringList names;
    for (auto it = byTool.constBegin(); it != byTool.constEnd(); ++it) {
        if (it->status != QLatin1String("ok")) names.append(it.key());
    }
    names.sort();
    return names;
}

// ANTS-3585 — {tool, status, elapsed_ms, truncated} per non-ok tool, sorted by
// tool name (reuses incompleteToolNames for the sorted key set).
QJsonArray incompleteToolsDetail(const QHash<QString, ToolResult> &byTool) {
    QJsonArray out;
    for (const QString &name : incompleteToolNames(byTool)) {
        const ToolResult &tr = byTool.value(name);
        QJsonObject o;
        o[QStringLiteral("tool")]       = name;
        o[QStringLiteral("status")]     = tr.status;
        o[QStringLiteral("elapsed_ms")] = tr.elapsedMs;
        o[QStringLiteral("truncated")]  =
            (tr.status == QLatin1String("timed_out"));
        out.append(o);
    }
    return out;
}

// ANTS-3585 — deduped, ascending union of every tool's parseFailureFiles.
QStringList parseFailureFiles(const QHash<QString, ToolResult> &byTool) {
    QSet<QString> uniq;
    for (auto it = byTool.constBegin(); it != byTool.constEnd(); ++it) {
        for (const QString &f : it->parseFailureFiles) uniq.insert(f);
    }
    QStringList out(uniq.constBegin(), uniq.constEnd());
    out.sort();
    return out;
}

void trimSamplesCascade(QHash<QString, ToolResult> &byTool,
                        bool &samplesTruncated) {
    auto totalSize = [&]() {
        return measureEnvelopeBytes({}, byTool);
    };
    if (totalSize() <= kEnvelopeSoftCapBytes) return;
    samplesTruncated = true;
    const QList<int> trims = {5, 3};
    for (int cap : trims) {
        for (auto it = byTool.begin(); it != byTool.end(); ++it) {
            while (it->samples.size() > cap) {
                it->samples.removeLast();
            }
        }
        if (totalSize() <= kEnvelopeHardCapBytes) return;
    }
}

// ANTS-1446 — wrappers exposing the anonymous-namespace validators
// to the feature-conformance test bundle.
bool validateCompileCommands(const QString &canonProject,
                             QString *errReason) {
    return validateCompileCommandsImpl(canonProject, errReason);
}
bool isIncludePathAllowed(const QString &includePath,
                          const QString &entryDir,
                          const QString &projectRoot,
                          QString *reason) {
    return isIncludePathAllowedImpl(includePath, entryDir,
                                    projectRoot, reason);
}
QStringList extractIncludeArgs(const QStringList &args) {
    return extractIncludeArgsImpl(args);
}
QStringList splitCommandString(const QString &cmd) {
    return splitCommandStringImpl(cmd);
}
// ANTS-2185 — test hook for the scoped-positional argv-injection guard.
QString flagSafeScopedPath(const QString &p) {
    return flagSafeScopedPathImpl(p);
}

// ANTS-3394 — test hook delegating to the anonymous-namespace argv builder.
QStringList toolArgv(const QString &tool, const QString &projectRoot,
                     const QStringList &scopedPaths) {
    return AuditRunner::toolArgv(tool, projectRoot, {}, scopedPaths);
}

// ANTS-1820 — test hook delegating to the anonymous-namespace parser.
ParsedCounts parseWithSuppression(const QString &tool, const QString &raw,
                                  int sampleCap,
                                  const QSet<QString> &learnedFps,
                                  const QString &allowlistPath) {
    // ANTS-3615 — compile the allowlist through the same engine loader
    // runAudit uses, so the test exercises the real path (hardening included).
    const QList<AuditEngine::AllowlistEntry> allowlist =
        allowlistPath.isEmpty() ? QList<AuditEngine::AllowlistEntry>{}
                                : AuditEngine::loadAllowlist(allowlistPath);
    const ParsedOutput p =
        parseToolOutput(tool, raw, sampleCap, learnedFps, allowlist);
    // ANTS-1870 — every finding must carry a 16-lowercase-hex fp.
    static const QRegularExpression rxFp(QStringLiteral("^[0-9a-f]{16}$"));
    bool allHex = !p.findings.isEmpty();
    for (const QJsonValue &v : p.findings) {
        if (!rxFp.match(v.toObject().value(QStringLiteral("fp")).toString())
                 .hasMatch()) {
            allHex = false;
            break;
        }
    }
    ParsedCounts c{};
    c.rawCount              = p.rawCount;
    c.afterFilterCount      = p.rawCount - p.suppressedCount;
    c.sampleCount           = static_cast<int>(p.samples.size());
    c.findingsCount        = static_cast<int>(p.findings.size());
    c.findingsTruncated     = p.findingsTruncated;
    c.allFindingsHaveHexFp  = allHex;
    c.aborted               = p.aborted;  // ANTS-3395
    c.parseFailureFiles     = p.parseFailureFiles;  // ANTS-3585
    return c;
}

}  // namespace internal

RunResult runAudit(const RunRequest &req) {
    RunResult r;
    QElapsedTimer elapsed;
    elapsed.start();

    // ── INV-2 / PathValidation. caller_cwd → canonical projectRoot.
    QFileInfo callerFi(req.projectRoot);
    const QString canonProject = callerFi.canonicalFilePath();
    if (canonProject.isEmpty() || !QFileInfo(canonProject).isDir()) {
        r.ok = false;
        r.code  = QStringLiteral("bad_path");
        r.error = QStringLiteral(
            "audit_run: caller_cwd \"%1\" does not canonicalise to "
            "an existing directory").arg(req.projectRoot);
        return r;
    }

    // ── INV-16 / range checks.
    if (req.capPerToolSeconds < kCapPerToolMin ||
        req.capPerToolSeconds > kCapPerToolMax) {
        r.ok = false;
        r.code  = QStringLiteral("bad_args");
        r.error = QStringLiteral(
            "audit_run: cap_per_tool_seconds %1 out of [%2, %3]")
                .arg(req.capPerToolSeconds)
                .arg(kCapPerToolMin).arg(kCapPerToolMax);
        return r;
    }
    if (req.topFindingsCount < kTopFindingsMin ||
        req.topFindingsCount > kTopFindingsMax) {
        r.ok = false;
        r.code  = QStringLiteral("bad_args");
        r.error = QStringLiteral(
            "audit_run: top_findings_count %1 out of [%2, %3]")
                .arg(req.topFindingsCount)
                .arg(kTopFindingsMin).arg(kTopFindingsMax);
        return r;
    }

    // ── INV-15 / scope tag sanitisation.
    if (req.scope.startsWith(QLatin1String("since-tag:"))) {
        const QString tag = req.scope.mid(10);
        if (!isScopeTagSafe(tag)) {
            r.ok = false;
            r.code  = QStringLiteral("bad_scope");
            r.error = QStringLiteral(
                "audit_run: scope \"%1\" fails since-tag sanitisation "
                "(must match ^[A-Za-z0-9._/+-]{1,128}$ and not start "
                "with '-')").arg(req.scope);
            return r;
        }
    }

    // ── ANTS-1512 / scoped-paths sanitisation. Each path is run through
    // the same isAuditArgSafe gate as audit-config.json args. Refuse the
    // whole call on any unsafe entry — silently dropping bad paths would
    // mask a typo + still run the unscoped tool, which violates "narrow
    // means narrow".
    for (const QString &p : req.paths) {
        if (!isAuditArgSafe(p)) {
            r.ok = false;
            r.code  = QStringLiteral("bad_args");
            r.error = QStringLiteral(
                "audit_run: paths entry \"%1\" fails argv-safety "
                "sanitisation").arg(p);
            return r;
        }
    }
    // ── ANTS-1512 / scoped-checks sanitisation + tool-compatibility
    // gate. checks is honoured by clang-tidy only; refuse for other
    // tools rather than silently ignore.
    for (const QString &c : req.checks) {
        if (!isAuditCheckSafe(c)) {
            r.ok = false;
            r.code  = QStringLiteral("bad_args");
            r.error = QStringLiteral(
                "audit_run: checks entry \"%1\" fails sanitisation "
                "(allowed: ^-?[A-Za-z0-9_*.,-]+$, length ≤ 128)").arg(c);
            return r;
        }
    }
    if (!req.checks.isEmpty()) {
        for (const QString &t : req.tools) {
            if (!toolHonoursChecks(t)) {
                r.ok = false;
                r.code  = QStringLiteral("bad_args");
                r.error = QStringLiteral(
                    "audit_run: checks parameter is honoured only by "
                    "clang-tidy; requested tool \"%1\" does not support "
                    "it. Pass tools=[\"clang-tidy\"] or drop checks=.")
                        .arg(t);
                return r;
            }
        }
    }

    // ── ANTS-3612 / INV-9 — aggregate concurrency cap. Taken AFTER the
    // cheap argument validation above (a malformed request should get its
    // deterministic bad_args refusal whether or not the host is busy) and
    // BEFORE any filesystem or process work. Released by the guard on
    // every exit path below.
    RunSlotGuard slot;
    if (!runSlotTryAcquire()) {
        r.ok = false;
        r.code  = QStringLiteral("server_busy");
        r.error = QStringLiteral(
            "audit_run: %1 audits already running (aggregate concurrency "
            "cap); retry shortly").arg(kMaxConcurrentRuns);
        return r;
    }
    slot.held = true;

    // ── ANTS-1456 / ANTS-1464 — load project audit-config.json
    // once per run so toolArgv() can override defaults.
    const QJsonObject projectConfig =
        loadProjectAuditConfig(canonProject);

    // ── ANTS-3615 — honour `suppressions`. Before this the field was parsed
    // into req.suppressionsMode and then never read: a caller passing
    // "none" or "path:<file>" got no effect AND no refusal, which is worse
    // than an unadvertised gap. The headless engine has two suppression
    // sources — the ANTS-1820 learned-FP ledger and the project-local
    // `.audit_allowlist.json` — and `suppressions` now governs both.
    // (`.audit_suppress` stays GUI-only: it is keyed by the line-grain
    // dedupKey the runner never materialises, and the drift-resilient
    // learned-FP ledger supersedes it for the headless path.)
    bool    suppressionsOn   = true;
    QString allowlistPath    =
        canonProject + QLatin1String("/.audit_allowlist.json");
    if (req.suppressionsMode.isEmpty()
        || req.suppressionsMode == QLatin1String("auto")) {
        // defaults above
    } else if (req.suppressionsMode == QLatin1String("none")) {
        suppressionsOn = false;
    } else if (req.suppressionsMode.startsWith(QLatin1String("path:"))) {
        // INV-2 — a caller-named suppression file must resolve INSIDE the
        // project root; otherwise `path:` is an arbitrary-file-read oracle
        // (a malformed-JSON qWarning would confirm existence).
        const QString named = req.suppressionsMode.mid(5);
        const QString canonNamed = QFileInfo(named).isAbsolute()
            ? QFileInfo(named).canonicalFilePath()
            : QFileInfo(canonProject + QLatin1Char('/') + named)
                  .canonicalFilePath();
        if (canonNamed.isEmpty()
            || !(canonNamed == canonProject
                 || canonNamed.startsWith(canonProject + QLatin1Char('/')))) {
            r.ok = false;
            r.code  = QStringLiteral("bad_path");
            r.error = QStringLiteral(
                "audit_run: suppressions path \"%1\" does not resolve to an "
                "existing file under the project root").arg(named);
            return r;
        }
        allowlistPath = canonNamed;
    } else {
        r.ok = false;
        r.code  = QStringLiteral("bad_args");
        r.error = QStringLiteral(
            "audit_run: suppressions \"%1\" unrecognised; expected \"auto\", "
            "\"none\" or \"path:<file>\"").arg(req.suppressionsMode);
        return r;
    }

    // ── ANTS-1820 — load the drift-resilient learned-FP ledger once per run.
    // The GUI dialog records FPs into `.audit_cache/learned-fp.jsonl`; without
    // this the headless MCP/CI sweep re-surfaces every learned false positive.
    const QSet<QString> learnedFps = suppressionsOn
        ? ants::auditfp::fingerprintSet(
              ants::auditfp::loadEntries(canonProject))
        : QSet<QString>{};
    // ── ANTS-3615 — the project-local cross-detector allowlist, shared with
    // the GUI dialog via AuditEngine. Compiled once per run (each entry's
    // line_regex goes through isCatastrophicRegex + hardenUserRegex inside
    // the loader), then matched per finding in parseToolOutput.
    const QList<AuditEngine::AllowlistEntry> allowlist = suppressionsOn
        ? AuditEngine::loadAllowlist(allowlistPath)
        : QList<AuditEngine::AllowlistEntry>{};

    // ── INV-10 / resolve absolute paths for the requested tool list.
    QStringList wantedTools = req.tools;
    if (wantedTools.isEmpty()) wantedTools = kAutoDetectTools();  // ANTS-3418
    for (const QString &t : wantedTools) {
        if (!kKnownTools().contains(t)) {
            r.ok = false;
            r.code  = QStringLiteral("bad_tool");
            r.error = QStringLiteral(
                "audit_run: unknown tool \"%1\"; allowed: %2")
                    .arg(t).arg(kKnownTools().join(QLatin1String(", ")));
            return r;
        }
    }

    QHash<QString, QString> toolAbsPath;
    for (const QString &t : wantedTools) {
        const QString abs = resolveToolAbsolute(t);
        if (abs.isEmpty()) {
            r.toolsSkipped.append({t,
                QStringLiteral("not found on PATH")});
        } else {
            toolAbsPath[t] = abs;
        }
    }

    // ── INV-14 / explicit-tool refusal.
    // ANTS-3605 — refuse ONLY when the caller named tools explicitly and none
    // resolved (intent honoured). An auto-detect sweep (empty req.tools) with
    // no external tool on PATH is NOT refused: the in-process drift lanes below
    // need no external binary, so a tool-less host still gets that coverage —
    // matching the GUI dialog, which always runs the lanes. The second
    // explicit-list check below (already guarded by !req.tools.isEmpty())
    // preserves the strict refusal for a named-but-unresolvable tool.
    if (toolAbsPath.isEmpty() && !req.tools.isEmpty()) {
        r.ok = false;
        r.code  = QStringLiteral("no_tools_runnable");
        r.error = QStringLiteral(
            "audit_run: no requested tools resolved on PATH");
        return r;
    }
    // When the caller passed an explicit non-empty list and ANY of
    // those failed to resolve, honour intent (INV-14).
    if (!req.tools.isEmpty()) {
        for (const QString &t : req.tools) {
            if (!toolAbsPath.contains(t)) {
                r.ok = false;
                r.code  = QStringLiteral("no_tools_runnable");
                r.error = QStringLiteral(
                    "audit_run: requested tool \"%1\" not runnable").arg(t);
                return r;
            }
        }
    }

    // ── ANTS-1504 — narrowing-scope resolution. A narrowing scope
    // (since-last-run / files / branch-diff / since-tag) resolves the
    // changed-file set and runs each file-oriented tool against only its
    // matching files; repo-global tools (gitleaks/trivy) and tools with no
    // matching changed file are skipped. `auto` (default) keeps the full
    // tree. Each tool's scoped list (or the caller's `req.paths` under
    // `auto` / a demoted full scan) flows through `perToolPaths`.
    QHash<QString, QStringList> perToolPaths;
    // ANTS-1870 — captured for the since-last-run findings delta below.
    // `sinceLastRunActive` is set only for an actually-narrowed, non-empty
    // `since-last-run`; `priorFindingsFile` is the prior run's sidecar
    // basename (empty → no baseline → no_prior_findings).
    QSet<QString> sinceLastRunChangedFiles;
    bool          sinceLastRunActive = false;
    QString       priorFindingsFile;
    // ANTS-3605 — set true only under a narrowed (file-diff) scope with
    // changes; gates OFF the whole-project in-process drift lanes below, which
    // would otherwise report drift unrelated to the changed-file set (mirroring
    // the not_file_scoped skip of repo-global tools like gitleaks).
    bool          scopeNarrowed = false;
    {
        auto applyFullScan = [&](const QString &resolvedLabel) {
            r.scopeResolved = resolvedLabel;
            for (auto it = toolAbsPath.constBegin();
                 it != toolAbsPath.constEnd(); ++it)
                perToolPaths[it.key()] = req.paths;
        };

        // ── ANTS-2015 — `full`: deterministic whole-tree sweep, independent
        // of git diff state. clazy/clang-tidy need explicit source positionals
        // or they scan nothing, so `auto` (empty positionals) makes a full
        // clazy run impossible on a clean tree. Hand the file-scoped tools the
        // tracked src/ list; the repo-global scanners (gitleaks/trivy) and any
        // tool whose language has no src/ file fall back to their own full scan
        // via empty paths. No tool is skipped — full means full.
        if (req.scope == QLatin1String("full")) {
            r.scopeResolved = QStringLiteral("full");
            const QStringList sources =
                AuditScope::enumerateSourceFiles(canonProject);
            r.changedFilesCount = static_cast<int>(sources.size());
            for (auto it = toolAbsPath.constBegin();
                 it != toolAbsPath.constEnd(); ++it) {
                const QString tool = it.key();
                if (!AuditScope::isFileScopedTool(tool)) {
                    perToolPaths[tool] = req.paths;  // gitleaks/trivy: full repo
                    continue;
                }
                QStringList safe;
                for (const QString &p : AuditScope::filterForTool(sources, tool))
                    if (isAuditArgSafe(p)) safe.append(p);
                perToolPaths[tool] = safe.isEmpty() ? req.paths : safe;
            }
        } else {
        const QJsonObject priorLastRun =
            AuditCache::loadManifest(canonProject).lastRun;
        const QString priorCommit =
            priorLastRun.value(QStringLiteral("commit")).toString();
        const AuditScope::Resolution sr =
            AuditScope::resolveChangedFiles(canonProject, req.scope, priorCommit);

        if (!sr.narrowed) {
            applyFullScan(req.scope.isEmpty() ? QStringLiteral("auto")
                                              : req.scope);
        } else if (!sr.demotedReason.isEmpty()) {
            // Stale-cache fallback → full scan (INV-6).
            r.scopeDemoted       = QStringLiteral("full");
            r.scopeDemotedReason = sr.demotedReason;
            applyFullScan(QStringLiteral("full"));
        } else if (sr.noChanges) {
            // Empty-changeset short-circuit (INV-7): spawn nothing, record
            // nothing — preserve the prior anchor for the next run. Returns
            // before the SARIF write + recordRun block below.
            r.ok                = true;
            r.scopeResolved     = req.scope;
            r.scopeAnchorCommit = sr.anchorCommit;
            r.changedFilesCount = 0;
            r.noChanges         = true;
            return r;
        } else {
            // Narrowed with changes: per-tool filtered file sets
            // (INV-3 language filter / INV-4 global-tool skip / INV-5 path
            // safety drop).
            r.scopeResolved     = req.scope;
            r.scopeAnchorCommit = sr.anchorCommit;
            r.changedFilesCount = static_cast<int>(sr.files.size());
            scopeNarrowed       = true;  // ANTS-3605 — skip whole-project lanes
            // ANTS-1870 — only `since-last-run` produces a findings delta
            // (§ 2.8 / INV-10). Capture the changed set + the prior sidecar
            // basename for the delta computation after the run completes.
            if (req.scope == QLatin1String("since-last-run")) {
                sinceLastRunActive = true;
                for (const QString &f : sr.files)
                    sinceLastRunChangedFiles.insert(f);
                priorFindingsFile = priorLastRun
                    .value(QStringLiteral("findings_file")).toString();
            }
            QStringList toDrop;
            for (auto it = toolAbsPath.constBegin();
                 it != toolAbsPath.constEnd(); ++it) {
                const QString tool = it.key();
                if (!AuditScope::isFileScopedTool(tool)) {
                    r.toolsSkipped.append({tool,
                        QStringLiteral("not_file_scoped")});
                    toDrop.append(tool);
                    continue;
                }
                QStringList safe;
                for (const QString &p : AuditScope::filterForTool(sr.files, tool))
                    if (isAuditArgSafe(p)) safe.append(p);
                if (safe.isEmpty()) {
                    r.toolsSkipped.append({tool,
                        QStringLiteral("no_changed_files_for_languages")});
                    toDrop.append(tool);
                    continue;
                }
                perToolPaths[tool] = safe;
            }
            for (const QString &t : toDrop) toolAbsPath.remove(t);
        }
        }
    }

    // ── ANTS-1446 — compile_commands.json include-path validation.
    // Only relevant when clazy or clang-tidy is in the resolved tool
    // list; both consume the JSON via `-p`. Cheap when absent (no
    // file, no parse, no refusal). Refusal short-circuits the whole
    // audit run with code:"compile_commands_escape" so the assistant
    // gets a clear error instead of an opaque "samples carry secrets"
    // outcome.
    const bool usesCompileCommands =
        toolAbsPath.contains(QStringLiteral("clazy"))
     || toolAbsPath.contains(QStringLiteral("clang-tidy"));
    if (usesCompileCommands) {
        QString reason;
        if (!validateCompileCommandsImpl(canonProject, &reason)) {
            r.ok    = false;
            r.code  = QStringLiteral("compile_commands_escape");
            r.error = QStringLiteral(
                "audit_run: compile_commands.json validation failed: %1")
                    .arg(reason);
            return r;
        }
    }

    // ── Build scrubbed env (INV-10).
    const QProcessEnvironment childEnv = buildChildEnv();

    // ── ANTS-2016 — generate the gitleaks walk-exclusion config once per run
    // (only when gitleaks is actually in the resolved set). Empty on failure →
    // gitleaks runs unfiltered.
    const QString gitleaksConfig =
        toolAbsPath.contains(QStringLiteral("gitleaks"))
            ? writeGitleaksExcludeConfig(canonProject)
            : QString();

    // ── INV-1 / aggregate cap.
    const int perToolMs = req.capPerToolSeconds * 1000;
    const int aggCapMs  = std::min(
        static_cast<int>(toolAbsPath.size() * perToolMs * 3 / 2),
        kAggregateCapMs);

    // ── Spawn QProcesses on a local event loop (§ 2.5).
    QEventLoop loop;
    QHash<QString, QString>            rawByTool;
    QHash<QString, std::shared_ptr<QProcess>> procs;
    QHash<QString, QElapsedTimer>      perToolTimer;
    // ANTS-1870 — the FULL per-tool finding set (uncapped, with fp), kept
    // out of ToolResult so the envelope stays lean. Feeds the carry-forward
    // SARIF, the delta, and the recorded sidecar.
    QHash<QString, QJsonArray>         fullFindingsByTool;
    bool                               anyFindingsTruncated = false;
    int                                pending = 0;
    QTimer aggTimer;
    aggTimer.setSingleShot(true);

    auto finish = [&](const QString &tool, const QString &status,
                      const QString &rawOutput, qint64 elapsedMs) {
        if (r.byTool.contains(tool)) return;  // dedup; INV-4
        ToolResult tr;
        tr.tool       = tool;
        tr.status     = status;
        tr.elapsedMs  = elapsedMs;
        // ANTS-2188 — scrub well-known secret shapes from the raw tool
        // output at this single capture point, before it reaches either
        // sink: rawByTool (→ the SARIF notification text written to
        // .audit_cache/*.sarif) or parseToolOutput (→ samples /
        // top_findings returned over MCP). trivy's `--scanners secret`
        // surfaces the literal secret value; gitleaks already runs
        // `--redact` but trivy did not, so the secret would otherwise
        // leak verbatim to disk and back to the LLM (OWASP LLM06).
        // Scrubbing before the JSON parse is safe: the [REDACTED:*] token
        // and the secret char-classes contain no quotes, so JSON string
        // values stay balanced and rawCount is unaffected.
        const QString scrubbed = SecretRedact::scrub(rawOutput).text;
        rawByTool[tool] = scrubbed;
        const ParsedOutput parsed =
            parseToolOutput(tool, scrubbed, kSamplesPerToolDefault,
                            learnedFps, allowlist);
        tr.rawCount         = parsed.rawCount;
        // ANTS-1820 / ANTS-3615 — the runner's two suppression sources (the
        // learned-FP ledger and `.audit_allowlist.json`) both drop into
        // suppressedCount; afterFilterCount is the raw total minus them.
        tr.afterFilterCount = parsed.rawCount - parsed.suppressedCount;
        tr.samples          = parsed.samples;
        // ANTS-3395 — a JSON tool that logged a fatal abort (e.g. trivy "run
        // error: fs scan error") produced no real findings; mark it crashed so
        // it surfaces in incomplete_tools[] (ANTS-2032) rather than as a clean
        // zero-finding run.
        if (tr.status == QLatin1String("ok") && parsed.aborted)
            tr.status = QStringLiteral("crashed");
        tr.parseFailureFiles = parsed.parseFailureFiles;      // ANTS-3585
        fullFindingsByTool[tool] = parsed.findings;           // ANTS-1870
        if (parsed.findingsTruncated) anyFindingsTruncated = true;
        r.byTool[tool]      = tr;
        --pending;
        if (pending <= 0) loop.quit();
    };

    for (auto it = toolAbsPath.constBegin();
         it != toolAbsPath.constEnd(); ++it) {
        const QString tool = it.key();
        auto proc = std::make_shared<QProcess>();
        proc->setProcessEnvironment(childEnv);
        proc->setWorkingDirectory(canonProject);  // INV-2
        proc->closeWriteChannel();
        QProcess::connect(proc.get(),
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            [tool, proc, &finish, &perToolTimer](
                int code, QProcess::ExitStatus es) {
                const qint64 ms = perToolTimer.value(tool).elapsed();
                const QString status = (es == QProcess::CrashExit)
                    ? QStringLiteral("crashed")
                    : QStringLiteral("ok");
                Q_UNUSED(code);
                const QByteArray out = proc->readAllStandardOutput();
                const QByteArray err = proc->readAllStandardError();
                // ANTS-2105 — cppcheck/clazy/clang-tidy write their findings to
                // STDERR (only the JSON tools — ruff/bandit/semgrep/trivy/mypy/
                // shellcheck — put results on STDOUT). ANTS-2118 — fold both
                // channels via the shared engine helper so this headless path
                // and the GUI dialog (auditdialog.cpp) feed byte-identical input
                // to parseFindings; the prior stdout-else-stderr form silently
                // dropped stderr for a tool that wrote findings to BOTH.
                const QString raw = AuditEngine::mergeToolChannels(
                    QString::fromUtf8(out), QString::fromUtf8(err));
                finish(tool, status, raw, ms);
            });
        QProcess::connect(proc.get(), &QProcess::errorOccurred,
            [tool, proc, &finish, &perToolTimer](QProcess::ProcessError) {
                const qint64 ms = perToolTimer.value(tool).elapsed();
                finish(tool, QStringLiteral("crashed"),
                       QString::fromUtf8(proc->readAllStandardError()),
                       ms);
            });

        // Per-tool wall-clock cap (INV-5): SIGTERM at cap, SIGKILL +2s.
        QTimer::singleShot(perToolMs, proc.get(), [tool, proc, &finish, &perToolTimer]() {
            if (proc->state() == QProcess::NotRunning) return;
            proc->terminate();
            QTimer::singleShot(kKillGraceMs, proc.get(),
                [tool, proc, &finish, &perToolTimer]() {
                if (proc->state() != QProcess::NotRunning) {
                    proc->kill();
                }
                const qint64 ms = perToolTimer.value(tool).elapsed();
                finish(tool, QStringLiteral("timed_out"), QString(), ms);
            });
        });

        procs[tool] = proc;
        perToolTimer[tool].start();
        ++pending;
        proc->start(it.value(),
                    toolArgv(tool, canonProject, projectConfig,
                             perToolPaths.value(tool), req.checks,
                             gitleaksConfig));
    }

    // Aggregate cap.
    QObject::connect(&aggTimer, &QTimer::timeout, &loop, [&]() {
        // Kill all still-running.
        for (auto it = procs.begin(); it != procs.end(); ++it) {
            if (it.value()->state() != QProcess::NotRunning) {
                it.value()->kill();
                if (!r.byTool.contains(it.key())) {
                    const qint64 ms = perToolTimer.value(it.key()).elapsed();
                    finish(it.key(),
                           QStringLiteral("timed_out"), QString(), ms);
                }
            }
        }
        loop.quit();
    });
    aggTimer.start(aggCapMs);

    if (pending > 0) loop.exec();
    aggTimer.stop();

    // ── ANTS-3605 — in-process audit lanes (spec↔code / contract-doc /
    // changelog↔test drift). These are GUI-free FeatureCoverage free functions,
    // not QProcess tools, so they run outside the multiplexer above — after the
    // external tools have finished, before the tally so their counts fold into
    // the totals. Each emits `file:line: message`, which finish()'s
    // parseToolOutput line-fallback parses like any plain-text tool (the lane
    // ids are not JSON tools, so INV-17's JSON-only path is not taken).
    //
    // They run ONLY on a default auto-detect sweep (empty req.tools) at full
    // scope: an explicit tools=[…] request scopes to those tools, and a
    // narrowed file-diff scope skips whole-project checks (scopeNarrowed).
    // This mirrors AuditDialog::populateChecks, which registers all three
    // autoSelect. finish() runs post-loop, so its `--pending` / `loop.quit()`
    // are harmless no-ops (the loop has already exited).
    if (req.tools.isEmpty() && !scopeNarrowed) {
        struct InProcessLane {
            const char *id;
            QString (*fn)(const QString &);
        };
        static const InProcessLane kInProcessLanes[] = {
            { "spec_code_drift",         &FeatureCoverage::runSpecDriftCheck },
            { "contract_doc_drift",      &FeatureCoverage::runContractDocDriftCheck },
            { "changelog_test_coverage", &FeatureCoverage::runChangelogCoverageCheck },
        };
        for (const auto &lane : kInProcessLanes) {
            QElapsedTimer laneTimer;
            laneTimer.start();
            const QString out = lane.fn(canonProject);
            finish(QString::fromLatin1(lane.id), QStringLiteral("ok"),
                   out, laneTimer.elapsed());
        }
    }

    // ── Tally totals.
    for (auto it = r.byTool.constBegin(); it != r.byTool.constEnd(); ++it) {
        r.totalRaw         += it->rawCount;
        r.totalActionable  += it->afterFilterCount;
    }
    r.noiseRatePct = (r.totalRaw == 0)
        ? 0
        : (100 - (100 * r.totalActionable / r.totalRaw));

    // ── ANTS-2032 / explicit partiality. A tool that timed out or
    // crashed leaves the rest of the run intact (it's recorded with its
    // status and the others still complete); surface that as a top-level
    // flag so a caller doesn't have to scan by_tool[].status to learn the
    // run was incomplete. The SARIF artifact below is written regardless,
    // so a partial run still leaves a recoverable artifact on disk.
    r.incompleteTools = internal::incompleteToolNames(r.byTool);
    r.partial         = !r.incompleteTools.isEmpty();
    // ANTS-3585 — richer partiality + zero-coverage surfaces derived once here.
    r.incompleteToolsDetail = internal::incompleteToolsDetail(r.byTool);
    r.parseFailures         = internal::parseFailureFiles(r.byTool);

    // ── INV-13 / sample-trim cascade.
    internal::trimSamplesCascade(r.byTool, r.samplesTruncated);

    // ── INV-13 / top_findings (v1: from samples, not full SARIF).
    if (req.topFindingsCount > 0) {
        QJsonArray top;
        for (auto it = r.byTool.constBegin();
             it != r.byTool.constEnd() && top.size() < req.topFindingsCount;
             ++it) {
            for (int i = 0;
                 i < it->samples.size() && top.size() < req.topFindingsCount;
                 ++i) {
                top.append(it->samples.at(i));
            }
        }
        r.topFindings = top;
    }

    // ── ANTS-1870 / since-last-run findings delta + carry-forward set.
    //
    // Build the flat whole-of-this-run finding set from the per-tool full
    // findings, then (for a narrowed since-last-run with a readable,
    // untruncated baseline) diff it against the prior sidecar. The
    // carry-forward array feeds the SARIF; `mergedForRecord` is the
    // whole-tree set persisted to the next sidecar (current ∪
    // priorOnUntouched), so a chain of since-last-run calls does not shed
    // untouched-file findings (INV-7).
    r.findingsTruncated = anyFindingsTruncated;
    QJsonArray currentFindings;
    for (auto it = r.byTool.constBegin(); it != r.byTool.constEnd(); ++it)
        for (const QJsonValue &v : fullFindingsByTool.value(it.key()))
            currentFindings.append(v);

    QJsonArray carriedForwardForSarif;   // priorOnUntouched, tagged in SARIF
    QJsonArray mergedForRecord = currentFindings;  // default: auto/full = current
    bool       mergedTruncated = anyFindingsTruncated;
    if (sinceLastRunActive) {
        const AuditCache::SidecarLoad prior =
            AuditCache::readFindingsSidecar(canonProject, priorFindingsFile);
        if (priorFindingsFile.isEmpty()) {
            r.deltaUnavailableReason = QStringLiteral("no_prior_findings");
        } else if (!prior.valid) {
            r.deltaUnavailableReason =
                QStringLiteral("prior_findings_unreadable");
        } else {
            const AuditDelta::DeltaResult d = AuditDelta::computeDelta(
                currentFindings, prior.findings, sinceLastRunChangedFiles);
            mergedForRecord = d.merged;
            // carriedForward ∖ current = the priorOnUntouched subset (the
            // current∩priorOnChanged half is already emitted by the per-tool
            // SARIF runs, so only the untouched-file findings need tagging).
            QSet<QString> curFps;
            for (const QJsonValue &v : currentFindings)
                curFps.insert(v.toObject().value(QStringLiteral("fp")).toString());
            for (const QJsonValue &v : d.carriedForward)
                if (!curFps.contains(
                        v.toObject().value(QStringLiteral("fp")).toString()))
                    carriedForwardForSarif.append(v);
            mergedTruncated = anyFindingsTruncated || prior.truncated
                || mergedForRecord.size() > kSarifFindingsMax;
            if (prior.truncated || anyFindingsTruncated) {
                // Truncated either side → the diff would mis-report; suppress
                // the envelope delta but still write the merged SARIF/sidecar.
                r.deltaUnavailableReason =
                    QStringLiteral("findings_truncated");
            } else {
                // Envelope arrays cap at kSamplesPerToolDefault (NOT the
                // caller's top_findings_count, which defaults to 0) and drop
                // the internal `fp`; the *_count fields stay exact (§ 2.8).
                auto previewArray = [](const QJsonArray &src) {
                    QJsonArray out;
                    for (int i = 0; i < src.size()
                                 && out.size() < kSamplesPerToolDefault; ++i) {
                        QJsonObject o = src.at(i).toObject();
                        o.remove(QStringLiteral("fp"));
                        out.append(o);
                    }
                    return out;
                };
                QJsonObject delta;
                delta[QStringLiteral("added")]   = previewArray(d.added);
                delta[QStringLiteral("removed")] = previewArray(d.removed);
                delta[QStringLiteral("added_count")]   = d.addedCount;
                delta[QStringLiteral("removed_count")] = d.removedCount;
                delta[QStringLiteral("carried_forward_count")] =
                    d.carriedForwardCount;
                r.delta = delta;
            }
        }
    }
    r.findingsTruncated = mergedTruncated;

    // ── INV-12 / SARIF + optional HTML.
    //
    // ANTS-1555 — route through `<root>/.audit_cache/` when the
    // project root is writeable; fall back to /tmp on failure so
    // `sarifPath` stays meaningful for callers. `cachePath` is set
    // only on the cache-hit branch.
    const QStringList formats = req.formats.isEmpty()
        ? QStringList{QStringLiteral("sarif")} : req.formats;

    const AuditCache::IsoNow iso     = AuditCache::isoNow();
    const AuditCache::GitInfo gitI   = AuditCache::gitInfo(canonProject);

    QString cacheSarifAbs;
    QString cacheHtmlAbs;
    if (formats.contains(QLatin1String("sarif"))) {
        cacheSarifAbs = AuditCache::sarifPathFor(canonProject,
                                                 iso.forFilename,
                                                 gitI.shortSha);
        if (!cacheSarifAbs.isEmpty()
         && ensurePrivateDir(AuditCache::cacheDir(canonProject))  // ANTS-1988 — 0700
         && writeSarif(cacheSarifAbs, r.byTool, rawByTool, req.projectRoot,
                       fullFindingsByTool, carriedForwardForSarif)) {
            r.sarifPath = cacheSarifAbs;
            r.cachePath = cacheSarifAbs;
        } else {
            // Fall back to legacy /tmp path so callers depending on
            // sarifPath still work even on read-only roots.
            cacheSarifAbs.clear();
            const QString sp = allocSarifPath();
            if (writeSarif(sp, r.byTool, rawByTool, req.projectRoot,
                           fullFindingsByTool, carriedForwardForSarif)) {
                r.sarifPath = sp;
            }
        }
    }
    if (formats.contains(QLatin1String("html"))) {
        QString hp;
        if (!r.cachePath.isEmpty()) {
            cacheHtmlAbs = AuditCache::htmlPathFor(canonProject,
                                                   iso.forFilename,
                                                   gitI.shortSha);
            hp = cacheHtmlAbs;
        } else if (!r.sarifPath.isEmpty()) {
            // /tmp fallback: mirror the sibling .html naming.
            hp = r.sarifPath;
            hp.chop(static_cast<int>(strlen(".sarif")));
            hp += QStringLiteral(".html");
        } else {
            // No SARIF + no cache: emit HTML alongside a randomised path so
            // an attacker can't pre-create a symlink at a known location.
            // ANTS-3614 — this branch allocates a fallback without going
            // through allocSarifPath(), so trigger the reaper here too.
            reapFallbackArtifactsOnce();
            const QString rand =
                QUuid::createUuid().toRfc4122().toHex().left(16);
            const QString cacheRoot = QStandardPaths::writableLocation(
                QStandardPaths::CacheLocation);
            if (!cacheRoot.isEmpty()
             && QDir().mkpath(cacheRoot + QLatin1String("/audit"))) {
                hp = QStringLiteral("%1/audit/audit-%2-%3.html")
                    .arg(cacheRoot, sessionIdToken(), rand);
            } else {
                hp = QStringLiteral("/tmp/audit-%1-%2.html")
                    .arg(sessionIdToken(), rand);
            }
        }
        if (writeHtml(hp, r.byTool)) r.htmlPath = hp;
    }

    // ── ANTS-1555 / record the run in `.audit_cache/index.json`.
    // Only fires when the cache write succeeded — fallback /tmp
    // writes don't touch the manifest. priorRun is filled by
    // recordRun() from the manifest's pre-existing last_run.
    if (!r.cachePath.isEmpty()) {
        QJsonObject lastRunJson;
        lastRunJson[QStringLiteral("iso_timestamp")] = iso.forManifest;
        lastRunJson[QStringLiteral("commit")]        = gitI.shortSha;
        if (!gitI.branch.isEmpty()) {
            lastRunJson[QStringLiteral("branch")] = gitI.branch;
        }
        lastRunJson[QStringLiteral("scope")] =
            req.scope.isEmpty() ? QStringLiteral("auto") : req.scope;
        // Store sarif/html as basenames inside the manifest; the
        // reaper resolves them to absolute paths relative to
        // .audit_cache/ (matches the "self-contained per-project
        // dir" invariant).
        lastRunJson[QStringLiteral("sarif")] =
            QFileInfo(cacheSarifAbs).fileName();
        if (!cacheHtmlAbs.isEmpty() && !r.htmlPath.isEmpty()
            && r.htmlPath == cacheHtmlAbs) {
            lastRunJson[QStringLiteral("html")] =
                QFileInfo(cacheHtmlAbs).fileName();
        }
        lastRunJson[QStringLiteral("elapsed_total_ms")] =
            static_cast<double>(elapsed.elapsed());
        lastRunJson[QStringLiteral("total_raw")]        = r.totalRaw;
        lastRunJson[QStringLiteral("total_actionable")] = r.totalActionable;
        QJsonObject byToolJson;
        for (auto it = r.byTool.constBegin();
             it != r.byTool.constEnd(); ++it) {
            QJsonObject t;
            t[QStringLiteral("elapsed_ms")] =
                static_cast<double>(it->elapsedMs);
            t[QStringLiteral("raw_count")]  = it->rawCount;
            t[QStringLiteral("status")]     = it->status;
            byToolJson[it.key()] = t;
        }
        lastRunJson[QStringLiteral("by_tool")] = byToolJson;
        // ANTS-1870 — the sidecar's `truncated` flag mirrors this; a later
        // clean run records false and self-heals the baseline (§ 2.6).
        lastRunJson[QStringLiteral("findings_truncated")] = mergedTruncated;

        QJsonObject prior;
        // ANTS-1870 — persist the whole-tree merged findings as the next
        // sidecar baseline (current ∪ priorOnUntouched under since-last-run;
        // the current set under auto/full). Always seeds a baseline so the
        // next since-last-run can diff (§ 2.3).
        AuditCache::recordRun(canonProject, lastRunJson, &prior,
                              mergedForRecord);
        r.priorRun = prior;
    }

    r.elapsedTotalMs = elapsed.elapsed();
    return r;
}

}  // namespace AuditRunner
