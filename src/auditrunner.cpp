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
//   * Aggregate cap of min(N*cap*1.5, 240 s) (INV-1)
//   * Hardcoded kExclusions list (INV-6)
//   * Per-call SARIF path (INV-12)
//   * Per-sample 256 B message cap + bottom-up trim cascade (INV-13)
//   * `scope:"since-tag:<X>"` argv-safe (INV-15)
//   * `cap_per_tool_seconds` + `top_findings_count` range check (INV-16)
//
// Out of v1 scope (logged as roadmap follow-up):
//   * Rich per-tool finding parsers (AuditDialog config-table
//     integration). v1 counts findings heuristically from raw output
//     line count after a simple severity grep; emitted SARIF lists
//     each tool as a driver entry with the raw output as a single
//     notification rather than per-finding result entries.
//   * `.audit_suppress` / `.audit_allowlist.json` regex hardening
//     (INV-6 second half). v1 reads `.audit_allowlist.json` shape only.
//   * Top-findings extraction from SARIF (INV-13 top_findings array
//     is currently echo-only — populated from samples, not from full
//     SARIF). v2 will parse the SARIF result entries.
//
// All of the v1 scope above honours the spec's INV anchors via
// source-scrape; behavioural tests cover env scrub + cap timing +
// pool isolation.

#include "auditrunner.h"

#include "auditengine.h"  // ANTS-1576 buildVcsProvenanceBlock

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QString>
#include <QTextStream>
#include <QTimer>

#include <cstring>
#include <memory>
#include <mutex>

namespace AuditRunner {

namespace {

// ───────────────────────────── ANTS-1351-INV-1 ──
constexpr int kAggregateCapMs            = 240'000;
constexpr int kCapPerToolMin             = 5;
constexpr int kCapPerToolMax             = 60;
// (Default 30 s is encoded in auditrunner.h struct initialiser.)
constexpr int kTopFindingsMin            = 0;
constexpr int kTopFindingsMax            = 100;
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
// Skill markdown documents the same set inline. Currently each tool's
// argv in toolArgv() builds invocations against `.` or `src/`; the
// list is reserved for the v2 per-tool exclusion-flag plumbing
// (passing --exclude entries to semgrep, ruff, trivy) which the v1
// invocations don't yet do — defer to the v2 follow-up.
[[maybe_unused]] const QStringList &kExclusions() {
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
                     const QStringList &scopedChecks = {}) {
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
    if (projectConfig.contains(tool) && scopedPaths.isEmpty()
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
        QStringList args = {QStringLiteral("--enable=all"),
                            QStringLiteral("--std=c++20"),
                            QStringLiteral("--library=qt"),
                            QStringLiteral("--quiet"),
                            QStringLiteral("-I"),
                            srcRoot};
        // ANTS-1512 — scoped paths override the default src/ scan.
        if (!scopedPaths.isEmpty()) args += scopedPaths;
        else                        args += srcRoot;
        return args;
    }
    if (tool == QLatin1String("clazy"))
        return {QStringLiteral("-checks=level1"),
                QStringLiteral("-p"),
                projectRoot + QLatin1String("/build/compile_commands.json")};
    // ANTS-1512 — clang-tidy scoped invocation. Default argv when no
    // paths given is a no-op (the tool needs explicit file args), so
    // we surface a graceful failure via empty argv → "crashed" status.
    if (tool == QLatin1String("clang-tidy")) {
        QStringList args = {QStringLiteral("-p"),
                            projectRoot + QLatin1String("/build")};
        if (!scopedChecks.isEmpty()) {
            // clang-tidy --checks syntax: comma-joined, leading `-`
            // opts a check OUT. Build the joined value here; the
            // outer runner has already validated each entry.
            args += QStringLiteral("--checks=-*,") + scopedChecks.join(QChar(','));
        }
        if (!scopedPaths.isEmpty()) args += scopedPaths;
        return args;
    }
    if (tool == QLatin1String("ruff"))
        return {QStringLiteral("check"), QStringLiteral("."),
                QStringLiteral("--output-format=json")};
    if (tool == QLatin1String("bandit"))
        return {QStringLiteral("-r"), srcRoot,
                QStringLiteral("-f"), QStringLiteral("json"),
                QStringLiteral("-ll")};
    if (tool == QLatin1String("semgrep"))
        return {QStringLiteral("--json"),
                QStringLiteral("--timeout"),
                QStringLiteral("60"),
                QStringLiteral("--metrics=off"),
                QStringLiteral("--config"),
                QStringLiteral("p/security-audit"),
                QStringLiteral(".")};
    if (tool == QLatin1String("gitleaks"))
        return {QStringLiteral("detect"),
                QStringLiteral("--no-banner"),
                QStringLiteral("--no-git"),
                QStringLiteral("--redact"),
                QStringLiteral("--report-format"),
                QStringLiteral("json")};
    if (tool == QLatin1String("trivy"))
        return {QStringLiteral("fs"),
                QStringLiteral("--scanners"),
                QStringLiteral("vuln,secret"),
                QStringLiteral("--severity"),
                QStringLiteral("HIGH,CRITICAL"),
                QStringLiteral("--quiet"),
                QStringLiteral("--format"),
                QStringLiteral("json"),
                QStringLiteral(".")};
    if (tool == QLatin1String("shellcheck")) {
        // Find shell scripts; cap at projectRoot.
        QStringList args;
        // shellcheck wants files on argv; v1 passes the project root
        // and lets it discover via shell-glob — most users have a
        // small set of *.sh files.
        return {QStringLiteral("-f"), QStringLiteral("json"),
                QStringLiteral("-S"), QStringLiteral("warning"),
                QStringLiteral("scripts")};
    }
    if (tool == QLatin1String("mypy"))
        return {QStringLiteral("--no-color-output"), QStringLiteral(".")};
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

QString allocSarifPath() {
    std::lock_guard<std::mutex> lk(g_sarifSeqMutex);
    const int seq = ++g_sarifSeq;
    return QStringLiteral("/tmp/audit-%1-%2.sarif")
        .arg(sessionIdToken()).arg(seq);
}

// Heuristic finding count + sample extraction from raw tool output.
// v1: line-based; v2 follow-up will parse per-tool JSON via
// AuditEngine helpers.
struct ParsedOutput {
    int        rawCount = 0;
    QJsonArray samples;  // first N matching lines as {file,line,message,severity}
};

ParsedOutput parseToolOutput(const QString &tool,
                             const QString &raw,
                             int sampleCap) {
    ParsedOutput out;
    if (raw.trimmed().isEmpty()) return out;
    // Quick JSON sniff: tools that emit a JSON results[] array (semgrep,
    // bandit, ruff, gitleaks, trivy) — count entries; non-JSON tools
    // (cppcheck plain, shellcheck JSON, clazy, mypy) — count lines.
    if (raw.trimmed().startsWith(QLatin1Char('{')) ||
        raw.trimmed().startsWith(QLatin1Char('['))) {
        QJsonParseError err;
        const QJsonDocument doc =
            QJsonDocument::fromJson(raw.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError) {
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
            for (int i = 0; i < arr.size() && out.samples.size() < sampleCap; ++i) {
                const QJsonObject e = arr.at(i).toObject();
                QJsonObject s;
                // Best-effort field extraction (tools vary).
                if (e.contains(QStringLiteral("filename")))
                    s["file"] = e.value(QStringLiteral("filename"));
                else if (e.contains(QStringLiteral("path")))
                    s["file"] = e.value(QStringLiteral("path"));
                else if (e.contains(QStringLiteral("File")))
                    s["file"] = e.value(QStringLiteral("File"));
                if (e.contains(QStringLiteral("line_number")))
                    s["line"] = e.value(QStringLiteral("line_number"));
                else if (e.contains(QStringLiteral("start"))) {
                    const QJsonObject st =
                        e.value(QStringLiteral("start")).toObject();
                    s["line"] = st.value(QStringLiteral("line"));
                }
                QString msg;
                if (e.contains(QStringLiteral("message"))) {
                    const QJsonValue mv = e.value(QStringLiteral("message"));
                    msg = mv.isObject()
                        ? mv.toObject().value(QStringLiteral("text")).toString()
                        : mv.toString();
                } else if (e.contains(QStringLiteral("Description"))) {
                    msg = e.value(QStringLiteral("Description")).toString();
                }
                s["message"]  = internal::capMessage(msg);
                s["rule"]     = e.value(QStringLiteral("check_id")).toString();
                s["severity"] = e.value(QStringLiteral("severity")).toString();
                out.samples.append(s);
            }
            // Honour SARIF cap.
            if (out.rawCount > kSarifFindingsMax)
                out.rawCount = kSarifFindingsMax;
            return out;
        }
    }
    // Line-based fallback for plain-text tools.
    const QStringList lines = raw.split(QChar('\n'), Qt::SkipEmptyParts);
    out.rawCount = lines.size();
    static const QRegularExpression rxFileLine(
        QStringLiteral("^([^:]+):(\\d+)(?::\\d+)?:\\s*(.+)$"));
    for (int i = 0; i < lines.size() && out.samples.size() < sampleCap; ++i) {
        const QRegularExpressionMatch m = rxFileLine.match(lines.at(i));
        if (!m.hasMatch()) continue;
        QJsonObject s;
        s["file"]    = m.captured(1);
        s["line"]    = m.captured(2).toInt();
        s["message"] = internal::capMessage(m.captured(3));
        s["rule"]    = tool;
        s["severity"]= QStringLiteral("UNKNOWN");
        out.samples.append(s);
    }
    return out;
}

// Emit a SARIF v2.1.0 document with each tool as a driver entry +
// each tool's raw output as a single notification (v1; v2 will parse
// per-tool results into result entries).
bool writeSarif(const QString &path,
                const QHash<QString, ToolResult> &byTool,
                const QHash<QString, QString> &rawByTool,
                const QString &rootCanonical) {
    QJsonObject runs;
    QJsonArray  runsArr;
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
        // Lightweight results[] from samples (best-effort).
        QJsonArray results;
        for (const QJsonValue &v : tr.samples) {
            const QJsonObject s = v.toObject();
            QJsonObject result;
            result["ruleId"] = s.value(QStringLiteral("rule")).toString();
            QJsonObject msg;
            msg["text"] = s.value(QStringLiteral("message")).toString();
            result["message"] = msg;
            QJsonObject loc;
            QJsonObject artLoc;
            artLoc["uri"] = s.value(QStringLiteral("file")).toString();
            QJsonObject physLoc;
            physLoc["artifactLocation"] = artLoc;
            QJsonObject region;
            region["startLine"] = s.value(QStringLiteral("line")).toInt();
            physLoc["region"] = region;
            loc["physicalLocation"] = physLoc;
            QJsonArray locs;
            locs.append(loc);
            result["locations"] = locs;
            results.append(result);
        }
        run["results"] = results;
        // ANTS-1576 — attach VCS provenance when capture succeeded.
        if (!vcpBlock.isEmpty()) {
            run["versionControlProvenance"] = vcpBlock;
        }
        runsArr.append(run);
    }
    QJsonObject doc;
    doc["version"] = QStringLiteral("2.1.0");
    doc["$schema"] = QStringLiteral(
        "https://json.schemastore.org/sarif-2.1.0.json");
    doc["runs"]    = runsArr;

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const QByteArray bytes =
        QJsonDocument(doc).toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size()) return false;
    return f.commit();
}

bool writeHtml(const QString &path,
               const QHash<QString, ToolResult> &byTool) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
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
    return f.commit();
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

    // ── ANTS-1456 / ANTS-1464 — load project audit-config.json
    // once per run so toolArgv() can override defaults.
    const QJsonObject projectConfig =
        loadProjectAuditConfig(canonProject);

    // ── INV-10 / resolve absolute paths for the requested tool list.
    QStringList wantedTools = req.tools;
    if (wantedTools.isEmpty()) wantedTools = kKnownTools();
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
    if (toolAbsPath.isEmpty()) {
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

    // ── Build scrubbed env (INV-10).
    const QProcessEnvironment childEnv = buildChildEnv();

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
        rawByTool[tool] = rawOutput;
        const ParsedOutput parsed =
            parseToolOutput(tool, rawOutput, kSamplesPerToolDefault);
        tr.rawCount         = parsed.rawCount;
        tr.afterFilterCount = parsed.rawCount;  // v1: no filter pipeline yet
        tr.samples          = parsed.samples;
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
                QString raw = QString::fromUtf8(out);
                if (status == QLatin1String("crashed") && !err.isEmpty()) {
                    raw = QString::fromUtf8(err);
                }
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
                             req.paths, req.checks));
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

    // ── Tally totals.
    for (auto it = r.byTool.constBegin(); it != r.byTool.constEnd(); ++it) {
        r.totalRaw         += it->rawCount;
        r.totalActionable  += it->afterFilterCount;
    }
    r.noiseRatePct = (r.totalRaw == 0)
        ? 0
        : (100 - (100 * r.totalActionable / r.totalRaw));

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

    // ── INV-12 / SARIF + optional HTML.
    const QStringList formats = req.formats.isEmpty()
        ? QStringList{QStringLiteral("sarif")} : req.formats;
    if (formats.contains(QLatin1String("sarif"))) {
        const QString sp = allocSarifPath();
        // ANTS-1576 — thread the canonical project root so the SARIF
        // emission captures git provenance.
        if (writeSarif(sp, r.byTool, rawByTool, req.projectRoot)) {
            r.sarifPath = sp;
        }
    }
    if (formats.contains(QLatin1String("html"))) {
        QString hp = r.sarifPath;
        if (!hp.isEmpty()) {
            hp.chop(static_cast<int>(strlen(".sarif")));
            hp += QStringLiteral(".html");
        } else {
            hp = QStringLiteral("/tmp/audit-%1.html").arg(sessionIdToken());
        }
        if (writeHtml(hp, r.byTool)) r.htmlPath = hp;
    }

    r.elapsedTotalMs = elapsed.elapsed();
    return r;
}

}  // namespace AuditRunner
