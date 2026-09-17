#include "auditdialog.h"
#include "auditdialog_internal.h"
#include "auditautofix.h"
#include "auditcache.h"
#include "auditfpledger.h"
#include "audithygiene.h"
#include "roadmapfoldin.h"
#include "debtsweepengine.h"
#include "llmclient.h"
#include "dialogchrome.h"
#include "featurecoverage.h"
#include "secureio.h"
#include "configbackup.h"  // ConfigWriteLock — ANTS-1989
#include "toggleswitch.h"
#include "tooldetectionengine.h"
#include "config.h"
#include "processgroup.h"  // ANTS-5038 — stop a check's whole process tree

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QTabWidget>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QHash>
#include <QFileInfo>
#include <QDirIterator>
#include <QFont>
#include <QScrollBar>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QCryptographicHash>
#include <QDate>
#include <QInputDialog>
#include <QMessageBox>
#include <QUrl>
#include <QUrlQuery>
#include <QTextBrowser>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QScopeGuard>
#include <QRegularExpression>
#include <QThread>
#include "secretredact.h"   // ANTS-4448 — scrub the AI-triage prompt

#include <algorithm>
#include <memory>

using namespace auditdialogdetail;

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------
//
// Centralised so individual check commands don't re-list the same excludes.
// Edit once, applies everywhere.
namespace auditdialogdetail {

// find(1) / grep(1) exclude expressions. ANTS-1709: the directory set
// (VCS, vendored code, language caches, our own artifact dirs, the
// build* glob family) now lives in ONE place — AuditEngine — so the
// find / grep / trivy / cppcheck / FeatureCoverage copies can't drift.
// `external/` + `third_party/` + `vendor/` hold code we don't maintain;
// the cache/scratch dirs (__pycache__/, .venv/, target/, .tox/, …) are
// gitignored but the audit isn't git-aware, so the set lists them
// explicitly (works on non-git projects too). See AuditEngine for the
// per-tool formatter rationale.
const QString kFindExcl = AuditEngine::findExcludeExpr();

// grep(1) exclude-dir list (bare, no file-include filter).
const QString kGrepExcl = AuditEngine::grepExcludeExpr();

// Security scans also skip auditdialog.cpp/.h — its check descriptions
// contain the very patterns being searched for (unsafe function names,
// URL schemes, …), which would otherwise produce self-referential hits.
//
// GNU grep 3.12 caveat: specifying `--exclude=<file>` BEFORE any
// `--include=<glob>` flags silently disables the --include filter (grep
// falls back to scanning every file). `--exclude-dir` doesn't trigger
// this — only the file-level `--exclude`. To work around it we keep
// `kGrepExclSec` as exclude-dir-only (safe in any position) and emit the
// auditdialog.cpp/.h file-excludes via `kGrepFileExclSec`, which callers
// MUST append *after* any --include flags. See addGrepCheck().
const QString kGrepExclSec = kGrepExcl;
const QString kGrepFileExclSec =
    " --exclude=auditdialog.cpp --exclude=auditdialog.h";

// Default set of source file globs
const QString kGrepIncludeSource =
    " --include='*.cpp' --include='*.h' --include='*.c' --include='*.hpp'"
    " --include='*.py' --include='*.js' --include='*.ts'"
    " --include='*.go' --include='*.rs' --include='*.sh'"
    " --include='*.lua' --include='*.java'";

// Noise substrings that almost always indicate a false positive. Applied as a
// baseline OutputFilter.dropIfContains on security-category checks.
const QStringList kCommonNoiseExcludes = {
    "test", "tests", "Test.", "_test.",
    "mock", "Mock",
    "example", "Example",
    "sample", "Sample",
    "placeholder", "Placeholder",
    "dummy", "Dummy",
    "TODO:", "FIXME:",
    "//  removed", "// cppcheck-suppress",
};

// Filesystems that don't enforce POSIX permission bits. On these, every file
// reports as world-writable regardless of intent, so the `file_perms` check
// produces 100% false positives. Values match the `stat -f -c %T` output
// on Linux (FUSE mounts commonly report "fuseblk"; NTFS-3G appears as
// "fuseblk" too, but explicit "ntfs"/"ntfs3" covers the kernel driver).
const QSet<QString> kNonPosixFilesystems = {
    "fuseblk", "fuse", "fuse.sshfs", "fuse.glusterfs",
    "ntfs", "ntfs3",
    "msdos", "vfat", "exfat",
    "cifs", "smbfs", "smb2",
    "9p",
};

// ANTS-5083 — ~QProcess kills a running child and waits for it again, and a
// child stuck on a hung mount survives the kill. So a process that may still
// be running is handed to its own finished signal rather than destroyed.
namespace {
void releaseProcess(QProcess *p) {
    if (p->state() == QProcess::NotRunning) {
        delete p;
        return;
    }
    QObject::connect(p, &QProcess::finished, p, &QObject::deleteLater);
    p->kill();
}
}  // namespace

} // namespace auditdialogdetail

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AuditDialog::AuditDialog(const QString &projectPath,
                         QWidget *parent,
                         Config *config)
    : QDialog(parent), m_projectPath(projectPath), m_config(config) {
    setWindowTitle(QStringLiteral("Project Audit — ants-audit v") + ANTS_VERSION);
    setMinimumSize(900, 600);
    resize(1050, 750);

    // ANTS-1242 — frameless + theme-aware TitleBar.
    auto chrome = DialogChrome::install(this,
                                        m_config ? m_config->theme() : QString());
    m_contentArea = chrome.contentArea;

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_projectPath);
    connectProcessSignals();

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        if (m_process->state() != QProcess::NotRunning) {
            // Disconnect all m_process→this signals to prevent double-
            // advance after kill, then re-establish the full set
            // (finished + drain slots) before the next check.
            disconnect(m_process, nullptr, this, nullptr);
            // ANTS-5038 — stop the check's whole process group: TERM with a
            // short grace (this is the GUI thread), then KILL the rest.
            const qint64 pgid = m_process->processId();
            ProcessGroup::signalGroup(pgid, SIGTERM);
            m_process->waitForFinished(500);
            ProcessGroup::signalGroup(pgid, SIGKILL);
            m_process->kill();
            m_process->waitForFinished(1000);
            connectProcessSignals();
            if (m_currentCheck >= 0 && m_currentCheck < m_checks.size()) {
                const int capMs = m_checks[m_currentCheck].timeoutMs;
                CheckResult r;
                r.checkId = m_checks[m_currentCheck].id;
                r.checkName = m_checks[m_currentCheck].name;
                r.category = m_checks[m_currentCheck].category;
                // Tool health, NOT a finding — downgrade severity to Info
                // and retype to Info, per 2026-04-16 triage. Previously
                // the timeout inherited the check's own severity (Major
                // or Critical), which polluted the severity-sorted list
                // with "(warning) Timed out" entries indistinguishable
                // from real findings. The `warning` flag already marks
                // the entry; severity demotion keeps it from sorting to
                // the top.
                r.type = CheckType::Info;
                r.severity = Severity::Info;
                r.output = QString("Timed out (%1s) — tool-health issue, "
                                   "not a finding")
                               .arg(capMs / 1000);
                r.warning = true;
                m_completedResults.append(r);
            }
            ++m_checksRun;
            runNextCheck();
        }
    });

    detectProject();
    m_blameEnabled = m_detectedTypes.contains("Git");

    // Probe the filesystem type once (e.g. "ext2/ext3", "btrfs", "fuseblk",
    // "ntfs", "vfat"). Used by populateChecks() to conditionally skip
    // POSIX-only checks on filesystems that don't enforce them.
    {
        auto *st = new QProcess;
        st->start("stat", {"-f", "-c", "%T", m_projectPath});
        if (st->waitForFinished(2000) && st->exitCode() == 0)
            m_projectFsType = QString::fromUtf8(st->readAllStandardOutput()).trimmed();
        auditdialogdetail::releaseProcess(st);
    }

    loadBaseline();
    loadSuppressions();
    loadAllowlist();
    // 0.6.31 self-learning layer — load the per-rule fire/suppression
    // history from <project>/audit_rule_quality.json. Tracker writes back
    // on destruction (RAII) so save() is automatic at dialog close;
    // recordSuppression() also force-flushes immediately because user
    // actions deserve durability.
    m_qualityTracker = std::make_unique<RuleQualityTracker>(m_projectPath);
    populateChecks();
    const int userRules = loadUserRules();
    if (userRules > 0)
        m_detectedTypes << QString("User rules: %1").arg(userRules);
    if (!m_pathRules.isEmpty())
        m_detectedTypes << QString("Path rules: %1").arg(m_pathRules.size());
    if (m_skippedUntrustedRules > 0) {
        // Surface the gated-rule count so GUI users who didn't open a
        // terminal see the same signal the qWarning puts on stderr.
        // The tooltip explanation is wired onto the types label where
        // the list is rendered (see buildUI → m_typesLabel).
        m_detectedTypes << QString("Untrusted rules: %1")
                               .arg(m_skippedUntrustedRules);
    }
    buildUI();
}

QList<AuditCheck> AuditDialog::checksForTest() const {
    return m_checks;
}

// Returns false when the project lives on a filesystem that doesn't enforce
// POSIX permission bits (FAT/NTFS/FUSE/SMB/9p). On those mounts the kernel
// typically maps every file to u=rwx,g=rwx,o=rwx, so permission-based
// security checks are meaningless.
bool AuditDialog::isPosixFilesystem() const {
    if (m_projectFsType.isEmpty()) return true;  // detection failed — assume POSIX
    return !kNonPosixFilesystems.contains(m_projectFsType);
}

// ---------------------------------------------------------------------------
// Project detection (languages + frameworks)
// ---------------------------------------------------------------------------

void AuditDialog::detectProject() {
    QDir dir(m_projectPath);

    if (dir.exists(".git"))
        m_detectedTypes << "Git";
    if (dir.exists("CMakeLists.txt") || dir.exists("Makefile"))
        m_detectedTypes << "C/C++";
    if (dir.exists("package.json"))
        m_detectedTypes << "JavaScript";
    if (dir.exists("Cargo.toml"))
        m_detectedTypes << "Rust";
    if (dir.exists("go.mod"))
        m_detectedTypes << "Go";
    if (dir.exists("pom.xml") || dir.exists("build.gradle") || dir.exists("build.gradle.kts"))
        m_detectedTypes << "Java";

    // Source-file sweep (top level + src/). Caps file count so this stays fast
    // even on large trees.
    bool hasPy = false, hasSh = false, hasLua = false, hasJava = false;
    bool hasCpp = false;
    auto scanSuffixes = [&](const QString &subPath) {
        QDirIterator it(subPath, QDir::Files, QDirIterator::NoIteratorFlags);
        int scanned = 0;
        while (it.hasNext() && scanned < 200) {
            it.next();
            ++scanned;
            const QString suf = it.fileInfo().suffix().toLower();
            if (suf == "py" || suf == "pyw") hasPy = true;
            else if (suf == "sh" || suf == "bash") hasSh = true;
            else if (suf == "lua") hasLua = true;
            else if (suf == "java") hasJava = true;
            else if (suf == "cpp" || suf == "c" || suf == "cc" || suf == "cxx") hasCpp = true;
        }
    };
    scanSuffixes(m_projectPath);
    scanSuffixes(m_projectPath + "/src");

    if (hasCpp && !m_detectedTypes.contains("C/C++"))
        m_detectedTypes << "C/C++";
    if (hasPy) m_detectedTypes << "Python";
    if (hasSh) m_detectedTypes << "Shell";
    if (hasLua) m_detectedTypes << "Lua";
    if (hasJava && !m_detectedTypes.contains("Java"))
        m_detectedTypes << "Java";

    // Framework detection — lets us pick better tool flags (e.g. cppcheck
    // --library=qt) and decide which rule packs to apply.
    //
    // ANTS-4124 — the shared predicate, not a local copy. This block used to
    // reimplement it (CMake marker, then a Q_OBJECT scan over src/), which is
    // the same duplication whose divergence produced ANTS-4094: the headless
    // runner hardcoded --library=qt while this gate was correct. One caller
    // gaining a signal the other lacks is exactly how that recurs. Adopting it
    // also FIXES a gap here — the helper falls back to the project root when
    // there is no src/, so a flat-layout qmake Qt project is now detected.
    if (AuditEngine::projectUsesQt(m_projectPath)) m_detectedTypes << "Qt";

    // IaC / container / CI detection — used by populateChecks() to decide
    // whether hadolint / checkov lanes should auto-enable. Kept cheap: a
    // shallow QDirIterator that stops at the first match per kind.
    auto hasAnyFile = [&](const QStringList &patterns, int maxDepth = 3) {
        // ANTS-5084 — list only maxDepth directory levels. A recursive
        // iterator walked every build and dependency tree before the depth
        // cap applied to a match.
        constexpr int kMaxDirs = 2000;
        int dirsSeen = 0;
        QStringList level{m_projectPath};
        for (int depth = 0; depth <= maxDepth && !level.isEmpty(); ++depth) {
            QStringList next;
            for (const QString &d : std::as_const(level)) {
                const QDir levelDir(d);
                if (!levelDir.entryList(patterns, QDir::Files).isEmpty()) return true;
                if (depth == maxDepth) continue;
                const QStringList subs = levelDir.entryList(
                    QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
                for (const QString &sub : subs) {
                    if (++dirsSeen > kMaxDirs) return false;
                    next << levelDir.filePath(sub);
                }
            }
            level = next;
        }
        return false;
    };

    if (hasAnyFile({"Dockerfile", "Dockerfile.*", "*.Dockerfile"}))
        m_detectedTypes << "Docker";
    if (hasAnyFile({"*.tf", "*.tfvars"}))
        m_detectedTypes << "Terraform";
    if (QDir(m_projectPath + "/.github/workflows").exists())
        m_detectedTypes << "GitHub Actions";
    // Kubernetes / K8s manifests — conservative: require both a telltale
    // directory name AND at least one YAML file under it. A blanket
    // *-deployment.yaml glob would false-positive on unrelated YAML.
    if (QDir(m_projectPath + "/k8s").exists()
        || QDir(m_projectPath + "/kubernetes").exists()
        || QDir(m_projectPath + "/manifests").exists())
        m_detectedTypes << "Kubernetes";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool AuditDialog::toolExists(const QString &tool) {
    // ANTS-1286 — delegate to process-lifetime PATH-keyed cache.
    return ToolDetectionEngine::exists(tool);
}

void AuditDialog::addGrepCheck(const QString &id, const QString &name,
                               const QString &desc, const QString &category,
                               const QString &pattern, CheckType type, Severity sev,
                               bool autoSelect, const OutputFilter &filter,
                               const QStringList &extraGrepArgs) {
    QString extra;
    for (const QString &a : extraGrepArgs) extra += " " + a;
    // Order matters: ALL --include flags (the standard source globs plus any
    // caller-supplied extras, which for e.g. `insecure_http` add *.json /
    // *.xml / *.toml) must precede the --exclude=<file> flags. GNU grep 3.12
    // silently disables --include when a file-level --exclude appears first.
    // See the kGrepExclSec comment for the regression history.
    const QString cmd =
        "grep -rnI" + kGrepExclSec + kGrepIncludeSource + extra +
        kGrepFileExclSec + " -E " + pattern + " . 2>/dev/null";
    AuditCheck c{id, name, desc, category, cmd, type, sev, filter, autoSelect, true, nullptr};
    // Apply the common-noise filter on top of the caller's excludes.
    for (const QString &n : kCommonNoiseExcludes) {
        if (!c.filter.dropIfContains.contains(n))
            c.filter.dropIfContains << n;
    }
    m_checks.append(std::move(c));
}

void AuditDialog::addFindCheck(const QString &id, const QString &name,
                               const QString &desc, const QString &category,
                               const QString &findArgs, CheckType type, Severity sev,
                               bool autoSelect, const OutputFilter &filter) {
    const QString cmd = "find ." + kFindExcl + " " + findArgs + " 2>/dev/null";
    m_checks.append({id, name, desc, category, cmd, type, sev, filter, autoSelect, true, nullptr});
}

void AuditDialog::addToolCheck(const QString &id, const QString &name,
                               const QString &desc, const QString &category,
                               const QString &tool, const QString &commandTemplate,
                               CheckType type, Severity sev, bool autoSelect,
                               const OutputFilter &filter) {
    const bool ok = toolExists(tool);
    const QString actualDesc = ok ? desc : QString("(%1 not installed)").arg(tool);
    m_checks.append({id, name, actualDesc, category, commandTemplate, type, sev,
                     filter, autoSelect && ok, ok, nullptr});
}

// ---------------------------------------------------------------------------
// Filter application
// ---------------------------------------------------------------------------

// applyFilter / parseFindings / capFindings now live in
// auditengine.cpp (ANTS-1119 v1). Call sites use the
// `AuditEngine::` namespace directly.

// ---------------------------------------------------------------------------
// Parsing raw command output into Findings
// ---------------------------------------------------------------------------
//
// Most checker output follows a small family of shapes:
//   file:line:col: message               (cppcheck, clang-tidy, gcc, grep -n)
//   file:line: message                   (shellcheck, luacheck, various)
//   file                                 (find, ls, wc output)
//   free-form                            (git status, build logs, whatever)
//
// We parse in that order and fall back to treating the line as a free-form
// finding with no file/line. Parsing is best-effort; incorrectly-parsed
// lines still appear, just without navigable location metadata.

// sourceForCheck() lives in AuditEngine — ANTS-1123 indie-review
// H1 unification. ANTS-1136 (0.7.67) dropped the local trampoline
// here; the single call site at onCheckFinished now uses the
// fully-qualified `AuditEngine::sourceForCheck(check.id)` form.

// computeDedup() moved into AuditEngine (auditengine.cpp anonymous
// namespace) per ANTS-1119 v1; was only referenced by the now-extracted
// parseFindings.

// parseFindings / capFindings extracted to AuditEngine — see
// auditengine.cpp (ANTS-1119 v1).

// ---------------------------------------------------------------------------
// Comment/string-aware line classification — the single biggest false-
// positive source for grep-style pattern checks. Runs a tiny state machine
// over the file up to the target line and reports whether that line's
// contents are actually code (vs. // comment, /* comment */, or "string").
// ---------------------------------------------------------------------------

QString AuditDialog::resolveProjectPath(const QString &maybeRelative) const {
    if (maybeRelative.isEmpty() || m_projectPath.isEmpty()) return {};
    // Build the candidate absolute path without yet canonicalizing the
    // relative join — QFileInfo::canonicalFilePath follows symlinks and
    // resolves `..`, so we get a single-pass check of both traversal vectors.
    const QString candidate = QFileInfo(maybeRelative).isAbsolute()
        ? maybeRelative
        : (m_projectPath + QStringLiteral("/") + maybeRelative);
    QString canonCandidate = QFileInfo(candidate).canonicalFilePath();   // non-const: returned by move
    if (canonCandidate.isEmpty()) return {};  // file doesn't exist
    const QString canonProject = QFileInfo(m_projectPath).canonicalFilePath();
    if (canonProject.isEmpty()) return {};
    // Require a path-segment boundary after the project root so that
    // sibling dirs sharing a prefix (e.g. /proj-foo vs /proj) don't escape.
    const QString anchored = canonProject.endsWith('/')
        ? canonProject
        : (canonProject + QStringLiteral("/"));
    if (!canonCandidate.startsWith(anchored) && canonCandidate != canonProject)
        return {};
    return canonCandidate;
}

bool AuditDialog::lineIsCode(const QString &absPath, int line) {
    if (line <= 0 || absPath.isEmpty()) return true;
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return true;
    // Scanning arbitrary files on audit is I/O-heavy. Cap to 2 MB so a
    // runaway check doesn't stall the dialog; larger files fall through
    // as "code" (the safe default that preserves findings).
    if (f.size() > 2LL * 1024 * 1024) return true;
    const QByteArray all = f.readAll();
    f.close();
    // ANTS-2210 — the per-language comment/string lexer (ANTS-1270 / ANTS-1759)
    // lives in AuditHygiene (ants_audit_lib) so test_audit can drive it without
    // linking this QDialog TU. This method is now just the file-I/O front door.
    return AuditHygiene::lineHasCode(QString::fromUtf8(all), absPath, line);
}

void AuditDialog::dropFindingsInCommentsOrStrings(CheckResult &r) const {
    // Only makes sense for checks that produce file:line findings.
    if (r.findings.isEmpty()) return;

    QList<Finding> kept;
    for (const Finding &f : std::as_const(r.findings)) {
        if (f.file.isEmpty() || f.line <= 0) { kept.append(f); continue; }
        // Resolve relative path against the project root. Reject
        // paths that escape the project via `..` or symlink — a
        // user-authored rule could otherwise have us open /etc/passwd
        // here to run the comment/string classifier against it.
        const QString abs = resolveProjectPath(f.file);
        if (abs.isEmpty()) continue;  // traversal or non-existent: drop
        if (lineIsCode(abs, f.line)) kept.append(f);
    }
    r.findings = std::move(kept);
}

// ---------------------------------------------------------------------------
// Inline suppression directives
// ---------------------------------------------------------------------------
//
// Recognises both ants-native markers and the de-facto conventions used by
// every mature tool. A finding is suppressed if any of these appear on the
// finding's line or the line immediately above it (or on any of the first 20
// lines for file-scope markers):
//
//   ants-native (preferred for cross-rule hits):
//     // ants-audit: disable                      — suppress everything on this line
//     // ants-audit: disable=rule-id,other-rule   — targeted
//     // ants-audit: disable-next-line[=rule-id]  — applies to the line below
//     // ants-audit: disable-file[=rule-id]       — file-scope (first 20 lines)
//
//   Passthrough (respect what the upstream tool already honors):
//     clang-tidy      : // NOLINT / NOLINT(rule)
//     clang-tidy prev : // NOLINTNEXTLINE / NOLINTNEXTLINE(rule)
//     cppcheck        : // <the cppcheck suppress token>[=id | [id1,id2]]
//     flake8          : # noqa / noqa: E501
//     bandit          : # nosec / nosec B101
//     semgrep         : # nosemgrep / nosemgrep: rule-id
//     gitleaks        : # gitleaks:allow
//     eslint          : // eslint-disable-line / -next-line [rule]
//     pylint          : # pylint: disable=code
//
// (The cppcheck example line above avoids using the bare "cppcheck-" +
// "suppress" token at the start of a comment so that cppcheck's own
// --inline-suppr parser doesn't mistake this documentation for a real
// suppression directive and emit invalidSuppression.)
//
// Parsing deliberately lenient: we extract the comment body (anything after
// //, /*, #, --, ;, <!--) and check for the tokens anywhere in it.

namespace {

// Strip any common comment-prefix from the start of a line; returns the body
// (or the original string if nothing matches). Also strips the closing `*/`
// or `-->` for block forms.
QString commentBody(const QString &rawLine) {
    QString s = rawLine.trimmed();
    // Order matters — longer tokens first.
    struct Prefix { const char *open; const char *close; };
    static const Prefix kPrefixes[] = {
        {"<!--", "-->"},
        {"/*",   "*/"},
        {"//",   nullptr},
        {"--",   nullptr},   // SQL / Lua / Haskell
        {"#",    nullptr},   // Python / shell / Perl / Ruby / YAML / make
        {";",    nullptr},   // ini / asm
    };
    for (const auto &p : kPrefixes) {
        if (s.startsWith(QLatin1String(p.open))) {
            s = s.mid(int(QLatin1String(p.open).size()));
            if (p.close) {
                const int end = s.lastIndexOf(QLatin1String(p.close));
                if (end >= 0) s = s.left(end);
            }
            return s.trimmed();
        }
    }
    // Fall back: trailing comment on a code line (e.g. `x = 1; // ...`).
    // Handle `//` and `#` and ` -- ` (SQL) specifically.
    int pos = s.indexOf("//");
    if (pos < 0) pos = s.indexOf(" #");
    if (pos < 0) pos = s.indexOf(" -- ");
    // ANTS-1647 — hoist the literal pattern out of the per-call path.
    static const QRegularExpression reLeadComment(R"(^[/#\s-]+)");
    if (pos >= 0) return s.mid(pos).replace(reLeadComment, "").trimmed();
    return {};
}

// Read the file (cached by the caller) into lines. Cheap wrapper for the
// directive scanner; shared with readSnippet() via m_fileLineCache.
QStringList readFileLines(const QString &absPath, QHash<QString, QStringList> &cache) {
    auto it = cache.constFind(absPath);
    if (it != cache.constEnd()) return it.value();
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) {
        cache.insert(absPath, {});
        return {};
    }
    // Cap at 4 MB to bound worst-case memory on enormous generated files.
    const QByteArray all = f.size() < 4LL * 1024 * 1024 ? f.readAll() : QByteArray();
    f.close();
    const QStringList lines = QString::fromUtf8(all).split('\n', Qt::KeepEmptyParts);
    cache.insert(absPath, lines);
    return lines;
}

} // namespace

bool AuditDialog::inlineSuppressed(const Finding &f) const {
    if (f.file.isEmpty() || f.line <= 0) return false;
    // Path-traversal guard: a finding whose `file` is `../../etc/hosts`
    // must not cause us to read /etc/hosts looking for suppression
    // markers.
    const QString abs = resolveProjectPath(f.file);
    if (abs.isEmpty()) return false;
    const QStringList lines = readFileLines(abs, m_fileLineCache);
    if (lines.isEmpty()) return false;

    // 1. file-scope: scan first 20 lines for disable-file markers.
    const int fileHeaderEnd = std::min<int>(20, lines.size());
    for (int i = 0; i < fileHeaderEnd; ++i) {
        const QString body = commentBody(lines[i]);
        if (body.contains("ants-audit:", Qt::CaseInsensitive) &&
            body.contains("disable-file", Qt::CaseInsensitive)) {
            if (AuditEngine::commentSuppresses(body, f.checkId)) return true;
        }
    }

    // 2. same-line: a trailing comment on the finding's line.
    const int lineIdx = f.line - 1;
    if (lineIdx >= 0 && lineIdx < lines.size()) {
        const QString body = commentBody(lines[lineIdx]);
        if (!body.isEmpty() && AuditEngine::commentSuppresses(body, f.checkId)) return true;
    }

    // 3. previous-line: disable-next-line / NOLINTNEXTLINE / eslint-disable-next-line.
    const int prevIdx = f.line - 2;
    if (prevIdx >= 0 && prevIdx < lines.size()) {
        const QString body = commentBody(lines[prevIdx]);
        if (!body.isEmpty() &&
            (body.contains("next-line", Qt::CaseInsensitive) ||
             body.contains("NOLINTNEXTLINE") ||
             body.contains("disable-next"))) {
            if (AuditEngine::commentSuppresses(body, f.checkId)) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Generated-file detection & glob handling
// ---------------------------------------------------------------------------

bool AuditDialog::isGeneratedFile(const QString &path) {
    if (path.isEmpty()) return false;
    const QString base = QFileInfo(path).fileName();

    // Qt MOC / UIC / RCC outputs
    if (base.startsWith("moc_"))         return true;
    if (base.startsWith("qrc_"))         return true;
    if (base.startsWith("ui_"))          return true;
    if (base.endsWith(".moc"))           return true;

    // Protobuf / gRPC
    if (base.endsWith(".pb.cc") || base.endsWith(".pb.h"))   return true;
    if (base.endsWith(".grpc.pb.cc") || base.endsWith(".grpc.pb.h")) return true;

    // Flex/Bison/ANTLR
    if (base.endsWith(".yy.cc") || base.endsWith(".tab.cc")) return true;

    // `*_generated.*` — gRPC-lite, flatbuffers, etc.
    static const QRegularExpression reGenerated(
        R"(_generated\.[a-z0-9]+$)", QRegularExpression::CaseInsensitiveOption);
    if (reGenerated.match(base).hasMatch()) return true;

    // /generated/ directory anywhere in the path
    if (path.contains("/generated/", Qt::CaseInsensitive)) return true;
    if (path.contains("/__generated__/", Qt::CaseInsensitive)) return true;

    // Build dirs (defensive; populateChecks already excludes these)
    if (path.contains("/build/") || path.contains("/build-"))  return true;
    if (path.contains("/CMakeFiles/")) return true;

    // Prior in-app audit artifacts — gitleaks flags the high-entropy
    // SHA-256 dedup keys stored in AUTOMATED_AUDIT_REPORT_*.json as
    // credentials, and the audit self-test re-fires on its own rule
    // catalogue when scanning `.audit_cache/baseline.json` or
    // `audit_rule_quality.json`. These are our own outputs; auditing
    // them is circular and produces the "19 of 23 gitleaks hits are
    // our own dedup hashes" noise pattern documented in the 10th
    // audit (2026-04-19). Scoped here rather than as a per-project
    // path_rule so the skip ships built-in and doesn't require every
    // project to author the rule.
    static const QRegularExpression reAuditReport(
        R"(^(?:.*/)?(?:docs/)?AUTOMATED_AUDIT_REPORT[_\-][^/]*\.(?:json|md|html|txt)$)",
        QRegularExpression::CaseInsensitiveOption);
    if (reAuditReport.match(path).hasMatch()) return true;
    if (base == "audit_rule_quality.json")   return true;
    if (path.contains("/.audit_cache/"))     return true;

    // Audit self-test fixtures — `bad.*` files are intentional trip-wires
    // for the project's own regex rule suite (every file carries an
    // `@expect <rule-id>` marker). Letting third-party tools (gitleaks
    // in particular) flag them as real findings floods the UI with
    // "secrets" that are hand-crafted test payloads. `good.*` files are
    // the negative case for the same suite and contain no real source
    // code we'd want audited. Scoped to path segments so a project
    // named "audit_fixtures" wouldn't false-match.
    if (path.contains("/audit_fixtures/") ||
        path.contains("/tests/audit_fixtures/")) return true;

    return false;
}

// ANTS-3615 — body moved to AuditEngine so the headless audit_run path
// compiles allowlist globs identically. Kept as a thin delegate: the
// path-rule call-sites below and the feature tests both key on this name.
QRegularExpression AuditDialog::globToRegex(const QString &glob) {
    return AuditEngine::globToRegex(glob);
}

bool AuditDialog::applyPathRules(Finding &f) const {
    if (f.file.isEmpty()) return true;

    // Generated-file skip is absolute — happens before user overrides.
    if (isGeneratedFile(f.file)) return false;

    // ANTS-1271 — path-rule globs are project-relative (globToRegex of e.g.
    // "tests/audit_fixtures/**" → "^tests/audit_fixtures/.*$"). Scanners that
    // emit absolute paths (clang-tidy, semgrep, mypy on cross-cwd inputs)
    // produce f.file == "/home/.../proj/tests/…", which never matches such a
    // rule. Match against a project-relative form so the rule applies no
    // matter how the upstream tool spelled the path. Findings outside the
    // project root keep their absolute path (those rules shouldn't apply).
    QString relFile = f.file;
    if (!m_projectPath.isEmpty()) {
        const QString prefix = m_projectPath.endsWith(QLatin1Char('/'))
            ? m_projectPath : m_projectPath + QLatin1Char('/');
        if (relFile.startsWith(prefix))
            relFile = relFile.mid(prefix.size());
    }

    for (const PathRule &rule : m_pathRules) {
        if (!rule.compiled.match(relFile).hasMatch()) continue;

        if (rule.skipRules.contains(f.checkId)) return false;
        if (rule.skip) return false;

        if (rule.severityShift != 0) {
            int s = static_cast<int>(f.severity) + rule.severityShift;
            s = std::clamp(s, 0, 4);
            f.severity = static_cast<Severity>(s);
        }
        // Keep evaluating — later rules can further tune severity.
    }
    return true;
}

// ---------------------------------------------------------------------------
// Code-snippet reader — ±radius lines around the finding line
// ---------------------------------------------------------------------------

QString AuditDialog::readSnippet(const QString &absPath, int line, int radius,
                                  int *startLineOut) const {
    if (absPath.isEmpty() || line <= 0) return {};
    const QStringList lines = readFileLines(absPath, m_fileLineCache);
    if (lines.isEmpty()) return {};

    const int lineIdx = line - 1;
    const int begin = std::max<int>(0, lineIdx - radius);
    const int end   = std::min<int>(lines.size() - 1, lineIdx + radius);
    if (startLineOut) *startLineOut = begin + 1;

    QStringList out;
    out.reserve(end - begin + 1);
    for (int i = begin; i <= end; ++i) out.append(lines[i]);
    return out.join('\n');
}

// ---------------------------------------------------------------------------
// Git blame enrichment — per (file, line) cached
// ---------------------------------------------------------------------------

// ANTS-5040 — the cache half of blame enrichment. True when there is nothing
// to ask git for: blame is off, the finding has no usable location, or the
// line is cached (an empty entry records a blame that failed).
bool AuditDialog::applyCachedBlame(Finding &f) const {
    if (!m_blameEnabled) return true;
    if (f.file.isEmpty() || f.line <= 0) return true;
    // ANTS-2003 — f.file is scanner-supplied. The `--` separator in
    // startQueuedBlame() already stops argv-injection, but refuse an absolute
    // path or a `..` traversal so `git blame` can never be pointed outside the
    // project tree.
    if (QDir::isAbsolutePath(f.file) ||
        f.file.split(QLatin1Char('/')).contains(QStringLiteral("..")))
        return true;
    const auto it =
        m_blameCache.constFind(f.file + QLatin1Char(':') + QString::number(f.line));
    if (it == m_blameCache.constEnd()) return false;
    f.blameAuthor = it->author;
    f.blameDate   = it->date;
    f.blameSha    = it->sha;
    return true;
}

// ANTS-5040 — one `git blame --line-porcelain` per file, a few at a time,
// never waited on. A finished job caches every line it was asked about (an
// empty entry where git gave none, so no line is asked twice), and the last
// one re-renders to apply them. Mid-run it does not: the run's own final
// render applies the cache.
void AuditDialog::startQueuedBlame() {
    constexpr int kMaxBlameJobs = 4;
    constexpr int kBlameTimeoutMs = 30000;
    while (m_blameInFlight < kMaxBlameJobs) {
        // ANTS-5217 — a const iterator checked against constEnd(). begin()
        // detaches, and GCC then warned -Wnull-dereference on key() because it
        // could not tie the node to the isEmpty() check.
        const auto next = m_blameQueue.constBegin();
        if (next == m_blameQueue.constEnd()) break;
        const QString file = next.key();
        QList<int> lines = next.value().values();
        m_blameQueue.erase(next);
        std::sort(lines.begin(), lines.end());

        QStringList args{QStringLiteral("blame"), QStringLiteral("--line-porcelain")};
        for (int line : std::as_const(lines))
            args << QStringLiteral("-L") << QStringLiteral("%1,%1").arg(line);
        args << QStringLiteral("HEAD") << QStringLiteral("--") << file;

        auto *git = new QProcess(this);
        git->setWorkingDirectory(m_projectPath);
        ++m_blameInFlight;
        auto done = [this, git, file, lines](bool ok) {
            const QHash<int, BlameEntry> parsed =
                ok ? GitBlame::parseLinePorcelain(git->readAllStandardOutput())
                   : QHash<int, BlameEntry>{};
            for (int line : lines)
                m_blameCache.insert(file + QLatin1Char(':') + QString::number(line),
                                    parsed.value(line));
            git->deleteLater();
            --m_blameInFlight;
            startQueuedBlame();
            if (m_blameInFlight == 0 && m_blameQueue.isEmpty() &&
                m_runBtn->isEnabled() && !m_completedResults.isEmpty())
                renderResults();
        };
        connect(git, &QProcess::finished, this,
                [done](int code, QProcess::ExitStatus st) {
                    done(st == QProcess::NormalExit && code == 0);
                });
        connect(git, &QProcess::errorOccurred, this,
                [done](QProcess::ProcessError e) {
                    if (e == QProcess::FailedToStart) done(false);
                });
        QTimer::singleShot(kBlameTimeoutMs, git, [git] { git->kill(); });
        git->start(QStringLiteral("git"), args);
    }
}

// ---------------------------------------------------------------------------
// Confidence score — replaces the binary `highConfidence` ★
// ---------------------------------------------------------------------------
//
// ANTS-1262 — the weighted-sum logic moved to AuditEngine::computeConfidence
// (pure data-transform, no widget state) so non-GUI consumers can score a
// finding without linking Qt6::Widgets. This static method forwards so the
// existing call-sites + the public static surface are unchanged.

int AuditDialog::computeConfidence(const Finding &f) {
    return AuditEngine::computeConfidence(f);
}

// ---------------------------------------------------------------------------
// Suppression file
// ---------------------------------------------------------------------------

QString AuditDialog::suppressionPath() const {
    return m_projectPath + "/.audit_suppress";
}

namespace {

// ANTS-5083 — the project controls audit_rules.json, .audit_suppress and the
// .audit_cache files, and each is read whole on the GUI thread. Refuse one
// past this size and treat it as absent. Generous: a baseline holds one
// fingerprint per finding.
constexpr qint64 kMaxAuditStateFileBytes = 16LL * 1024 * 1024;

bool readAuditStateFile(const QString &path, QByteArray *out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (f.size() > kMaxAuditStateFileBytes) {
        qWarning("audit: skipping %s — larger than %lld bytes",
                 qPrintable(path), static_cast<long long>(kMaxAuditStateFileBytes));
        return false;
    }
    *out = f.readAll();
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// User-defined rule loader — <project>/audit_rules.json
// ---------------------------------------------------------------------------
//
// Schema is a thin translation of the AuditCheck struct to JSON so users
// can add project-specific checks (or tune existing ones by overriding the
// hardcoded id) without rebuilding. Rules load after populateChecks() so
// hardcoded checks remain the baseline; user rules only augment.
//
// The loader is permissive — missing fields get sensible defaults, unknown
// fields are ignored. Tool commands are NOT sandboxed (they run through
// /bin/bash like every other check), so audit_rules.json is a trust
// boundary. This is a local dev tool, so we treat it like .gitattributes
// or .git/hooks — your repo, your rules.
QString AuditDialog::userRulesPath() const {
    return m_projectPath + "/audit_rules.json";
}

int AuditDialog::loadUserRules() {
    m_skippedUntrustedRules = 0;
    QByteArray rulesBytes;
    if (!readAuditStateFile(userRulesPath(), &rulesBytes)) return 0;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(rulesBytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning("audit_rules.json: %s", qPrintable(err.errorString()));
        return 0;
    }
    const QJsonArray rules = doc.object().value("rules").toArray();

    // Enum string decoders — match severityLabel / typeLabel lowercase.
    auto parseSeverity = [](const QString &s) -> Severity {
        const QString v = s.toLower();
        if (v == "blocker")  return Severity::Blocker;
        if (v == "critical") return Severity::Critical;
        if (v == "major")    return Severity::Major;
        if (v == "minor")    return Severity::Minor;
        return Severity::Info;
    };
    auto parseType = [](const QString &s) -> CheckType {
        const QString v = s.toLower();
        if (v == "smell" || v == "code_smell" || v == "codesmell") return CheckType::CodeSmell;
        if (v == "bug")      return CheckType::Bug;
        if (v == "hotspot")  return CheckType::Hotspot;
        if (v == "vuln" || v == "vulnerability") return CheckType::Vulnerability;
        return CheckType::Info;
    };

    // Load `path_rules[]` first so count-based reporting reflects both axes.
    const QJsonArray pathRules = doc.object().value("path_rules").toArray();
    m_pathRules.clear();
    for (const QJsonValue &v : pathRules) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        const QString glob = o.value("glob").toString();
        if (glob.isEmpty()) continue;
        PathRule pr;
        pr.glob          = glob;
        pr.compiled      = globToRegex(glob);
        pr.skip          = o.value("skip").toBool(false);
        pr.severityShift = o.value("severity_shift").toInt(0);
        const QJsonArray sr = o.value("skip_rules").toArray();
        for (const QJsonValue &x : sr) pr.skipRules << x.toString();
        m_pathRules.append(pr);
    }

    // Rule packs carrying a `command` field bash-exec that string verbatim.
    // A cloned-but-untrusted repo can plant a hostile audit_rules.json and
    // run arbitrary code the moment the user opens the Audit dialog. 0.7.13
    // scopes trust per-project-and-per-hash (see Config::isAuditRulePackTrusted):
    // trusting one project doesn't extend to others, and any rule-pack edit
    // invalidates trust so a silent post-trust modification is re-prompted.
    // Env-var `ANTS_AUDIT_TRUST_UNSAFE=1` remains an escape hatch for CI
    // that can't round-trip through the persisted store.
    // ANTS-2003 — reuse the live m_config so a rule-pack trusted mid-session
    // (in-memory, not yet flushed to disk) is honoured. A fresh Config would
    // re-read the on-disk store and wrongly deny the just-trusted pack.
    Config fallbackCfg;
    Config &cfg = m_config ? *m_config : fallbackCfg;
    const bool commandRulesTrusted =
        (qEnvironmentVariable("ANTS_AUDIT_TRUST_UNSAFE") == "1")
        || cfg.isAuditRulePackTrusted(m_projectPath, rulesBytes);

    int loaded = 0;
    for (const QJsonValue &v : rules) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        const QString id = o.value("id").toString();
        if (id.isEmpty()) continue;

        AuditCheck c;
        c.id          = id;
        c.name        = o.value("name").toString(id);
        c.description = o.value("description").toString();
        c.category    = o.value("category").toString("User");
        c.command     = o.value("command").toString();
        if (c.command.isEmpty()) continue;   // command is required

        // If the rule carries a `command` field and the user hasn't
        // trusted this rule-pack for this project, skip it rather than
        // execute an untrusted shell string. Counts get surfaced below.
        if (!commandRulesTrusted) {
            ++m_skippedUntrustedRules;
            continue;
        }
        c.type        = parseType(o.value("type").toString("info"));
        c.severity    = parseSeverity(o.value("severity").toString("minor"));
        c.autoSelect  = o.value("auto_select").toBool(false);
        c.available   = true;

        // Filter block.
        const QJsonArray drop = o.value("drop_if_contains").toArray();
        for (const QJsonValue &x : drop) c.filter.dropIfContains << x.toString();
        const QJsonArray keep = o.value("keep_only_if_contains").toArray();
        for (const QJsonValue &x : keep) c.filter.keepOnlyIfContains << x.toString();
        c.filter.dropIfMatches = o.value("drop_if_matches").toString();
        c.filter.maxLines = o.value("max_lines").toInt(100);

        // If a rule with this id was already added by populateChecks() we
        // replace it — user rules win. Otherwise append.
        bool replaced = false;
        for (int i = 0; i < m_checks.size(); ++i) {
            if (m_checks[i].id == id) {
                m_checks[i] = c;
                replaced = true;
                break;
            }
        }
        if (!replaced) m_checks.append(c);
        ++loaded;
    }

    if (m_skippedUntrustedRules > 0) {
        // Mirrored on the dialog's types badge (see AuditDialog ctor);
        // the stderr copy exists for headless / CI invocations where
        // the GUI badge isn't visible.
        qWarning("audit_rules.json: skipped %d rule(s) with `command` "
                 "fields — this project's rule pack is not trusted. "
                 "Export ANTS_AUDIT_TRUST_UNSAFE=1 or call "
                 "Config::trustAuditRulePack() to opt in; editing the "
                 "rule pack re-invalidates trust.",
                 m_skippedUntrustedRules);
    }

    return loaded;
}

// ---------------------------------------------------------------------------
// External-tool calibration — wire project-local suppression configs into
// the tool invocations so each scanner doesn't re-flag findings the project
// has already accepted. Origin: RetroDB audit-hygiene report 2026-04-21.
// ---------------------------------------------------------------------------

// Parse `.semgrep.yml`'s header block:
//
//     # Excluded upstream rules
//     # -----------------------
//     #   rule.id.one
//     #     Anchor: ...
//     #   rule.id.two
//     ...
//     # RetroDB-specific custom rules
//
// Rule IDs are the comment lines inside that block that begin with `#   `
// followed by a dotted identifier (two or more dot-separated segments). The
// heuristic matches the literal shell awk-based extractor RetroDB's own
// `.semgrep.yml` header documents.
QString AuditDialog::semgrepExcludeFlags() const {
    QFile f(m_projectPath + "/.semgrep.yml");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    // ANTS-2005 — cap the config read (1 MiB); a pathological file must not
    // be slurped whole into memory.
    const QString text = QString::fromUtf8(f.read(1 << 20));
    f.close();

    const QStringList rules = AuditHygiene::parseSemgrepExcludeRules(text);
    if (rules.isEmpty()) return {};
    QString flags;
    for (const QString &r : std::as_const(rules))
        flags += " --exclude-rule " + r;
    return flags;
}

// Parse `pyproject.toml`'s `[tool.ruff.lint.ignore]` array for `S<nnn>`
// codes and emit `--skip B<nnn>,B<mmm>,...` for bandit. Ruff's `S` family
// mirrors bandit's `B` family 1:1, so the mapping is lexical.
//
// Deliberately a lightweight text scan rather than a full TOML parse:
// (1) Qt has no built-in TOML reader and we'd otherwise pull in a dep;
// (2) the shape we care about is a flat list of string literals; (3) if the
// user's pyproject.toml is non-standard enough to defeat this, they can
// fall back to path rules or the allowlist.
QString AuditDialog::banditSkipFlags() const {
    QFile f(m_projectPath + "/pyproject.toml");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    // ANTS-2005 — cap the config read (1 MiB); a pathological file must not
    // be slurped whole into memory.
    const QString text = QString::fromUtf8(f.read(1 << 20));
    f.close();
    const QStringList bCodes = AuditHygiene::parseBanditSkipCodes(text);
    if (bCodes.isEmpty()) return {};
    return " --skip " + bCodes.join(",");
}

// ---------------------------------------------------------------------------
// Project-local grep-rule allowlist (.audit_allowlist.json)
// ---------------------------------------------------------------------------

// ANTS-3615 — loader + matcher moved to AuditEngine (Qt6::Core-only) so the
// headless `audit_run` path applies the same `.audit_allowlist.json` filter.
// These stay as the dialog's entry points; the logic has one home now.
void AuditDialog::loadAllowlist() {
    m_allowlist = AuditEngine::loadAllowlist(
        m_projectPath + "/.audit_allowlist.json");
}

bool AuditDialog::allowlisted(const Finding &f) const {
    return AuditEngine::allowlisted(m_allowlist, f.checkId, f.file, f.message);
}

// ANTS-1257 v2 — append one allowlist entry matching `f` and reload. The
// entry's triple is (rule = checkId, path_glob = exact file, line_regex =
// escaped message) so loadAllowlist()/allowlisted() drops exactly this
// finding (and others sharing all three) on the next run. INV-13.
bool AuditDialog::appendAllowlistEntry(const Finding &f, const QString &reason) {
    const QString path = m_projectPath + "/.audit_allowlist.json";

    QJsonObject rootObj;
    QJsonArray arr;
    QFile rf(path);
    if (rf.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll());
        rf.close();
        if (doc.isObject()) {
            rootObj = doc.object();
            arr = rootObj.value("allowlist").toArray();
        }
    }

    QJsonObject entry;
    entry["rule"] = f.checkId.isEmpty() ? QStringLiteral("unknown") : f.checkId;
    entry["path_glob"] = f.file.isEmpty() ? QStringLiteral("*") : f.file;
    // Escape the message so it matches literally; an empty message falls
    // back to ".*" (allow this rule on this file regardless of message).
    entry["line_regex"] = f.message.isEmpty()
        ? QStringLiteral(".*")
        : QRegularExpression::escape(f.message.left(200));
    entry["reason"] = reason;
    arr.append(entry);
    if (!rootObj.contains("version")) rootObj["version"] = 1;
    rootObj["allowlist"] = arr;

    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly)) return false;
    if (!setOwnerOnlyPerms(sf))  // 0600 — matches every other state writer here (indie-review-2026-05-21)
        warnNotOwnerOnly(path, "audit allowlist");
    const QByteArray body = QJsonDocument(rootObj).toJson(QJsonDocument::Indented);
    if (sf.write(body) != body.size()) return false;
    if (!sf.commit()) return false;
    if (!setOwnerOnlyPerms(path))
        warnNotOwnerOnly(path, "audit allowlist");

    loadAllowlist();   // refresh so the running session also hides it on re-render
    return true;
}

// ANTS-5084 — one file named with different leading directories: equal, or
// the longer path ends with "/" + the shorter. A bare suffix matched a
// changed "foo.cpp" to "src/barfoo.cpp".
static bool pathSuffixMatches(const QString &a, const QString &b) {
    if (a.isEmpty() || b.isEmpty()) return false;
    if (a.size() == b.size()) return a == b;
    const QString &longer  = a.size() > b.size() ? a : b;
    const QString &shorter = a.size() > b.size() ? b : a;
    return longer.endsWith(shorter)
        && longer.at(longer.size() - shorter.size() - 1) == QLatin1Char('/');
}

// ANTS-1257 v2 — INV-11 "Since baseline" predicate. Pure + static.
bool AuditDialog::visibleSinceBaseline(
    const Finding &f,
    const QHash<QString, QSet<int>> &recentLines,
    const QSet<QString> &baselineFingerprints) {
    // Baseline half: drop findings already present in the saved baseline.
    if (baselineFingerprints.contains(f.dedupKey)) return false;
    // Recent half: unfiled findings always pass; filed findings must sit on
    // a changed line. Match by exact path, then by path-suffix (scanner
    // output and git diff may disagree on the relative prefix).
    if (f.file.isEmpty() || f.line <= 0) return true;
    const auto exact = recentLines.constFind(f.file);
    if (exact != recentLines.constEnd()) return exact->contains(f.line);
    for (auto it = recentLines.constBegin(); it != recentLines.constEnd(); ++it)
        if (pathSuffixMatches(f.file, it.key()))
            return it->contains(f.line);
    return false;   // file not in the changed set
}

bool AuditDialog::sinceBaselineVisible(const Finding &f) const {
    if (!m_recentScopeError.isEmpty())
        return !m_baselineFingerprints.contains(f.dedupKey);
    return visibleSinceBaseline(f, m_recentLines, m_baselineFingerprints);
}

// ANTS-1257 v2 — currently-visible findings (same filter as the results
// pane) that are actionable: aiVerdict == TRUE_POSITIVE OR confidence >= 70.
QList<Finding> AuditDialog::actionableFindings() const {
    QList<Finding> out;
    for (const auto &r : m_completedResults) {
        if (r.warning) continue;
        for (const Finding &f : r.findings) {
            if (isSuppressed(f)) continue;
            if (m_sinceBaseline) {
                if (!sinceBaselineVisible(f))
                    continue;
            } else if (m_showNewOnly && m_hasBaseline
                       && m_baselineFingerprints.contains(f.dedupKey)) {
                continue;
            }
            if (!m_activeSeverities.contains(static_cast<int>(f.severity))) continue;
            if (!m_textFilter.isEmpty()) {
                const QString hay = (f.file + " " + f.message + " " +
                                     f.checkId + " " + f.blameAuthor).toLower();
                if (!hay.contains(m_textFilter)) continue;
            }
            const bool actionable =
                f.aiVerdict == QStringLiteral("TRUE_POSITIVE") || f.confidence >= 70;
            if (actionable) out.append(f);
        }
    }
    return out;
}

// ANTS-1257 v2 — fold orchestration. INV-14: allocateIds once, template
// once, insertBlock once. Returns false (no write) on empty set, id-alloc
// failure, or heading-not-found.
bool AuditDialog::foldFindingsIntoRoadmap(const QList<Finding> &actionable,
                                          const QString &releaseHeading) {
    if (actionable.isEmpty() || releaseHeading.isEmpty()) return false;

    // Pre-flight: confirm the heading exists before allocating IDs.
    // allocateIds bumps .roadmap-counter atomically (crash-safety), so a
    // doomed insert against a wrong heading would otherwise leak the
    // reserved IDs on every misclick.
    QString needle = releaseHeading;
    while (needle.endsWith(QChar('\n')) || needle.endsWith(QChar('\r')))
        needle.chop(1);
    if (!roadmapHeadingExists(needle)) return false;

    const QList<int> ids =
        RoadmapFoldIn::allocateIds(m_projectPath, actionable.size());
    if (ids.size() != actionable.size()) return false;   // alloc failed

    const QString block = AuditEngine::templateRoadmapFoldInBlock(
        actionable, ids,
        QDate::currentDate().toString(Qt::ISODate));

    return RoadmapFoldIn::insertBlock(m_projectPath, needle, block);
}

void AuditDialog::refreshFoldRoadmapButton() {
    if (!m_foldRoadmapBtn) return;
    const int n = actionableFindings().size();
    m_foldRoadmapBtn->setEnabled(n > 0);
    m_foldRoadmapBtn->setText(n > 0
        ? QString("🌳 Fold actionable (%1)").arg(n)
        : QStringLiteral("🌳 Fold actionable"));
}

void AuditDialog::onFoldRoadmapClicked() {
    const QList<Finding> actionable = actionableFindings();
    if (actionable.isEmpty()) {
        QMessageBox::information(this, "Fold into ROADMAP",
            "No actionable findings (none are AI-confirmed TRUE_POSITIVE or "
            "confidence ≥ 70).");
        return;
    }

    QString heading = RoadmapFoldIn::findActiveReleaseHeading(m_projectPath);

    bool ok = false;
    heading = QInputDialog::getText(this, "Fold actionable into ROADMAP",
        QString("Insert %1 finding%2 as a new\n"
                "  ### 🔍 Audit fold-in (%3)\n"
                "subsection immediately after this ROADMAP heading "
                "(edit if wrong):")
            .arg(actionable.size())
            .arg(actionable.size() == 1 ? "" : "s")
            .arg(QDate::currentDate().toString(Qt::ISODate)),
        QLineEdit::Normal, heading, &ok);
    if (!ok || heading.trimmed().isEmpty()) return;

    if (foldFindingsIntoRoadmap(actionable, heading.trimmed())) {
        if (m_statusLabel)
            m_statusLabel->setFullText(
                QString("Folded %1 finding%2 into ROADMAP.md")
                    .arg(actionable.size())
                    .arg(actionable.size() == 1 ? "" : "s"));
    } else {
        QMessageBox::warning(this, "Fold into ROADMAP",
            QString("Could not insert the fold-in block.\n\n"
                    "Check that the heading exists verbatim in ROADMAP.md:\n  %1")
                .arg(heading.trimmed()));
    }
}

void AuditDialog::onSinceBaselineToggled(bool on) {
    m_sinceBaseline = on;
    // Recompute the changed-line sets so the pill works even when the run
    // wasn't recent-scoped. Off → leave them (the predicate is gated on
    // m_sinceBaseline, so stale data is inert).
    const auto render = [this]() {
        if (!m_completedResults.isEmpty()) renderResults();
    };
    if (!on) {
        render();
        return;
    }
    // ANTS-5084 — git runs on a worker; render now (the filter stands down
    // while it reads) and again when the sets arrive.
    requestRecentChangeSets(/*includeLines=*/true, render);
    render();
}

// ---------------------------------------------------------------------------
// ANTS-1259 — Debt Sweep tab
// ---------------------------------------------------------------------------

bool AuditDialog::roadmapHeadingExists(const QString &heading) const {
    QString needle = heading;
    while (needle.endsWith(QChar('\n')) || needle.endsWith(QChar('\r')))
        needle.chop(1);
    QFile rf(m_projectPath + "/ROADMAP.md");
    if (!rf.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QStringList lines = QString::fromUtf8(rf.readAll()).split(QChar('\n'));
    rf.close();
    for (const QString &ln : lines)
        if (ln == needle) return true;
    return false;
}

// Collapse mypy "Library stubs not installed" findings into a single Info
// entry listing the packages to install. Bulk-installing is deterministic
// and the stub-install nag doesn't need to consume 20 finding slots.
void AuditDialog::consolidateMypyStubHints(CheckResult &r) {
    // ANTS-1343 — body lifted to AuditEngine so the consolidator can
    // run in non-GUI contexts (test bundles, future CLI) and so the
    // pre-collapse-count authoring is co-located with the data shape.
    AuditEngine::consolidateMypyStubHints(r);
}

void AuditDialog::loadSuppressions() {
    m_suppressedKeys.clear();
    m_suppressionReasons.clear();

    // ANTS-1708 — drift-resilient learned-FP ledger, loaded alongside the
    // line-grain .audit_suppress. Keyed by content fingerprint so a learned
    // false positive stays hidden after edits shift its line number.
    m_learnedFpFingerprints = ants::auditfp::fingerprintSet(
        ants::auditfp::loadEntries(m_projectPath));

    QByteArray suppressBytes;
    if (!readAuditStateFile(suppressionPath(), &suppressBytes)) return;
    const QStringList lines = QString::fromUtf8(suppressBytes).split('\n', Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        // v2 (JSONL): `{"key": "...", "rule": "...", ...}`
        if (line.startsWith('{')) {
            const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
            if (doc.isObject()) {
                const QJsonObject o = doc.object();
                const QString key = o.value("key").toString();
                if (!key.isEmpty()) {
                    m_suppressedKeys.insert(key);
                    const QString reason = o.value("reason").toString();
                    if (!reason.isEmpty())
                        m_suppressionReasons.insert(key, reason);
                }
                continue;
            }
            // Malformed JSON — fall through to legacy parse as a last resort.
        }

        // v1 legacy: first whitespace-delimited token is the key.
        static const QRegularExpression reWs(R"(\s+)");  // ANTS-1647
        m_suppressedKeys.insert(line.section(reWs, 0, 0));
    }
}

// ANTS-4444 — checked LIVE, for the same reason the dedupKey lookup is:
// a false positive learned after this audit ran must take effect without a
// re-run, which the cached `f.suppressed` flag cannot express. The empty
// check keeps the common case free of a SHA-256 per finding per render.
bool AuditDialog::isSuppressed(const Finding &f) const {
    if (isSuppressed(f.dedupKey)) return true;
    if (m_learnedFpFingerprints.isEmpty()) return false;
    return m_learnedFpFingerprints.contains(
        ants::auditfp::computeFingerprint(f.file, f.checkId, f.message));
}

bool AuditDialog::isSuppressed(const QString &dedupKey) const {
    if (dedupKey.isEmpty()) return false;
    if (m_suppressedKeys.contains(dedupKey)) return true;
    // Legacy 0.7.28-and-earlier keys are 16 hex chars; new keys are 24.
    // Match the legacy prefix so existing user suppressions keep working
    // after the dedup-width upgrade.
    if (dedupKey.size() >= 16 && m_suppressedKeys.contains(dedupKey.left(16)))
        return true;
    return false;
}

// AuditDialog::isCatastrophicRegex / hardenUserRegex now forward to
// the AuditEngine implementations (ANTS-1123 indie-review C1/C2/C3
// unification — was: two divergent definitions; is: one). Callers
// that look up `AuditDialog::isCatastrophicRegex` for source-grep
// stability still resolve, but the body is in `auditengine.cpp`.
bool AuditDialog::isCatastrophicRegex(const QString &pattern) {
    return AuditEngine::isCatastrophicRegex(pattern);
}

QString AuditDialog::hardenUserRegex(const QString &pattern) {
    return AuditEngine::hardenUserRegex(pattern);
}

void AuditDialog::saveSuppression(const QString &dedupKey,
                                  const QString &ruleId,
                                  const QString &reason) {
    if (dedupKey.isEmpty()) return;
    if (m_suppressedKeys.contains(dedupKey)) return;

    // Build the new JSONL entry.
    QJsonObject entry;
    entry["key"]       = dedupKey;
    entry["rule"]      = ruleId;
    entry["reason"]    = reason;
    entry["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    const QString path = suppressionPath();
    // ANTS-5083 — lock the read and the rewrite against another instance
    // saving a suppression, as appendSnapshot does for trend.json.
    ConfigWriteLock lock(path);
    QFile f(path);

    // If the existing file is in v1 (plain-text keys) format, convert it to
    // JSONL on first write so the user sees a consistent format afterwards.
    // Detect by peeking at the first non-comment line.
    bool needsConvert = false;
    QStringList legacyKeys;
    QByteArray existingBytes;
    if (readAuditStateFile(path, &existingBytes)) {   // ANTS-5083
        const QStringList lines = QString::fromUtf8(existingBytes).split('\n', Qt::SkipEmptyParts);
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            if (!line.startsWith('{')) {
                needsConvert = true;
                static const QRegularExpression reWs(R"(\s+)");  // ANTS-1647
                legacyKeys << line.section(reWs, 0, 0);
            }
            // JSONL lines are already v2 — we'll append to them as-is.
        }
    }

    bool saved = false;
    if (needsConvert) {
        // Rewrite whole file: existing JSONL lines (if any) preserved +
        // legacy keys upgraded with a migration marker.
        QStringList rebuilt;
        QByteArray rebuildBytes;
        if (readAuditStateFile(path, &rebuildBytes)) {   // ANTS-5083
            const QStringList lines = QString::fromUtf8(rebuildBytes).split('\n', Qt::SkipEmptyParts);
            for (const QString &raw : lines) {
                const QString line = raw.trimmed();
                if (line.isEmpty()) continue;
                if (line.startsWith('#') || line.startsWith('{')) {
                    rebuilt << line;
                }
            }
        }
        const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
        for (const QString &k : std::as_const(legacyKeys)) {
            QJsonObject o;
            o["key"]       = k;
            o["rule"]      = "unknown";
            o["reason"]    = "migrated from v1 plain-text format";
            o["timestamp"] = now;
            rebuilt << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
        }
        rebuilt << QString::fromUtf8(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        // Full rewrite — atomic via QSaveFile so an interrupted migration
        // can't corrupt the v1 → v2 conversion mid-flight and leave the
        // suppression list unparseable on next load.
        QSaveFile sf(path);
        if (sf.open(QIODevice::WriteOnly)) {
            if (!setOwnerOnlyPerms(sf))
                warnNotOwnerOnly(path, "audit suppressions");
            const QByteArray body = rebuilt.join('\n').toUtf8() + '\n';
            saved = sf.write(body) == body.size() && sf.commit();
        }
    } else {
        // Simple append — stays non-atomic. Each append is one JSONL line;
        // a torn write at EOF is a single bad line, and loadSuppressions()
        // already skips non-parseable lines.
        if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            bool ok = true;
            if (f.size() == 0) {
                if (!setOwnerOnlyPerms(f))
                    warnNotOwnerOnly(f.fileName(), "audit suppressions");
                const QByteArray header =
                    "# ants-audit suppressions (JSONL). Entries hide the matching "
                    "finding by dedup key.\n";
                ok = f.write(header) == header.size();
            }
            const QByteArray line =
                QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
            ok = ok && f.write(line) == line.size() && f.flush();
            f.close();
            saved = ok;
        }
    }

    // ANTS-5083 — a suppression that never reached the file must not hide the
    // finding for the rest of the session as if it had been saved.
    if (!saved) {
        if (m_statusLabel)
            m_statusLabel->setFullText(
                QStringLiteral("Could not save the suppression to %1").arg(path));
        return;
    }
    m_suppressedKeys.insert(dedupKey);
    if (!reason.isEmpty())
        m_suppressionReasons.insert(dedupKey, reason);
}

void AuditDialog::onResultAnchorClicked(const QUrl &url) {
    const QString scheme = url.scheme();
    const QString key = url.host().isEmpty() ? url.path().mid(1) : url.host();
    if (key.isEmpty()) return;

    // Toggle the per-finding details expand state; cheap — no network.
    if (scheme == "ants-expand") {
        if (m_expandedKeys.contains(key)) m_expandedKeys.remove(key);
        else                              m_expandedKeys.insert(key);
        if (!m_completedResults.isEmpty()) renderResults();
        return;
    }

    // Fire an AI triage request; response lands async via requestAiTriage().
    if (scheme == "ants-triage") {
        requestAiTriage(key);
        return;
    }

    // ANTS-1257 — "Allow this finding" — write a project-local allowlist
    // entry (rule + file + message) so future runs drop this finding.
    if (scheme == "ants-allow") {
        const Finding fa = m_findingsByKey.value(key);
        const QString whereA = (!fa.file.isEmpty() && fa.line > 0)
            ? QString("%1:%2").arg(fa.file, QString::number(fa.line))
            : (fa.file.isEmpty() ? QStringLiteral("(no location)") : fa.file);
        bool okA = false;
        const QString reasonA = QInputDialog::getText(
            this, "Allow finding",
            QString("Permanently allow this finding (writes "
                    ".audit_allowlist.json)?\n\n"
                    "Rule:     %1\n"
                    "Location: %2\n"
                    "Message:  %3\n\n"
                    "Optional reason:")
                .arg(fa.checkName.isEmpty() ? key : fa.checkName,
                     whereA, fa.message.left(200)),
            QLineEdit::Normal, QString(), &okA);
        if (!okA) return;
        if (appendAllowlistEntry(fa, reasonA)) {
            if (m_statusLabel)
                m_statusLabel->setFullText(
                    QString("Allowlisted %1 (%2)")
                        .arg(key.left(8),
                             fa.checkId.isEmpty() ? QStringLiteral("unknown")
                                                  : fa.checkId));
            if (!m_completedResults.isEmpty()) renderResults();
        } else if (m_statusLabel) {
            m_statusLabel->setFullText(
                QStringLiteral("Could not write .audit_allowlist.json"));
        }
        return;
    }

    if (scheme != "ants-suppress") return;
    const Finding f = m_findingsByKey.value(key);
    const QString where = (!f.file.isEmpty() && f.line > 0)
        ? QString("%1:%2").arg(f.file, QString::number(f.line))
        : (f.file.isEmpty() ? QStringLiteral("(no location)") : f.file);

    const QString prompt = QString(
        "Suppress this finding from future audits?\n\n"
        "Rule:     %1\n"
        "Location: %2\n"
        "Message:  %3\n\n"
        "Optional reason (saved alongside the suppression):")
        .arg(f.checkName.isEmpty() ? key : f.checkName,
             where,
             f.message.left(200));

    bool ok = false;
    const QString reason = QInputDialog::getText(
        this, "Suppress finding", prompt,
        QLineEdit::Normal, QString(), &ok);
    if (!ok) return;

    const QString ruleId = f.checkId.isEmpty() ? QStringLiteral("unknown") : f.checkId;
    saveSuppression(key, ruleId, reason);

    // ANTS-1708 — also record a drift-resilient learned-FP entry keyed by the
    // line-independent content fingerprint, so the suppression survives later
    // edits that shift this finding's line (the .audit_suppress key would not).
    if (!f.file.isEmpty()) {
        ants::auditfp::Entry e;
        e.fingerprint = ants::auditfp::computeFingerprint(
            f.file, f.checkId, f.message);
        e.rule   = ruleId;
        e.reason = reason;
        if (ants::auditfp::appendEntry(m_projectPath, e))
            m_learnedFpFingerprints.insert(e.fingerprint);
    }

    // 0.6.31 self-learning — record the suppression in the rule-quality
    // tracker AND check whether the LCS suggester now has enough samples
    // to propose a `dropIfContains` tightening. Surface the suggestion
    // inline (status bar) so the user can act on it without leaving the
    // dialog. The proposal is informational; applying it requires the
    // user to edit `audit_rules.json` (the cross-platform user-rule pack).
    if (m_qualityTracker) {
        m_qualityTracker->recordSuppression(ruleId, key, f.message, reason);
        const QString suggestion = m_qualityTracker->suggestTightening(ruleId);
        if (!suggestion.isEmpty() && m_statusLabel) {
            m_statusLabel->setFullText(
                QString("Suppressed %1 (%2). 💡 %3 looks like a common "
                        "FP shape — consider adding it to %2's "
                        "dropIfContains in audit_rules.json.")
                    .arg(key.left(8), ruleId, suggestion));
        } else if (m_statusLabel) {
            m_statusLabel->setFullText(QString("Suppressed %1 (%2)")
                                            .arg(key.left(8), ruleId));
        }
    } else if (m_statusLabel) {
        m_statusLabel->setFullText(QString("Suppressed %1 (%2)")
                                        .arg(key.left(8), ruleId));
    }

    // Re-render results minus the suppressed finding.
    if (!m_completedResults.isEmpty()) renderResults();
}

// ---------------------------------------------------------------------------
// Trend tracking — severity-count snapshots across audit runs
// ---------------------------------------------------------------------------
//
// Stored as a JSON array of {timestamp, total, blocker, critical, major,
// minor, info} objects at <project>/.audit_cache/trend.json. Capped at
// kMaxTrendHistory entries (FIFO eviction).

QString AuditDialog::trendPath() const {
    return m_projectPath + "/.audit_cache/trend.json";
}

AuditDialog::TrendSnapshot AuditDialog::loadLastSnapshot() const {
    TrendSnapshot s;
    QByteArray trendBytes;
    if (!readAuditStateFile(trendPath(), &trendBytes)) return s;
    const QJsonDocument doc = QJsonDocument::fromJson(trendBytes);
    if (!doc.isArray()) return s;
    const QJsonArray arr = doc.array();
    if (arr.isEmpty()) return s;
    const QJsonObject last = arr.last().toObject();
    s.timestamp     = last.value("timestamp").toString();
    s.total         = last.value("total").toInt();
    s.bySev[0]      = last.value("info").toInt();
    s.bySev[1]      = last.value("minor").toInt();
    s.bySev[2]      = last.value("major").toInt();
    s.bySev[3]      = last.value("critical").toInt();
    s.bySev[4]      = last.value("blocker").toInt();
    return s;
}

void AuditDialog::appendSnapshot(const TrendSnapshot &s) {
    ensurePrivateDir(m_projectPath + "/.audit_cache");   // ANTS-1988 — 0700
    // ANTS-1989 — lock the trend.json read-modify-write against a concurrent
    // Ants instance appending its own snapshot (last-writer-wins drops one run).
    ConfigWriteLock lock(trendPath());
    QJsonArray arr;
    QByteArray trendBytes;
    if (readAuditStateFile(trendPath(), &trendBytes))
        arr = QJsonDocument::fromJson(trendBytes).array();
    QJsonObject entry;
    entry["timestamp"] = s.timestamp;
    entry["total"]     = s.total;
    entry["info"]      = s.bySev[0];
    entry["minor"]     = s.bySev[1];
    entry["major"]     = s.bySev[2];
    entry["critical"]  = s.bySev[3];
    entry["blocker"]   = s.bySev[4];
    arr.append(entry);
    // Evict oldest if we've hit the cap.
    while (arr.size() > kMaxTrendHistory) arr.removeFirst();
    // Atomic write — trend history is cumulative, a torn write would
    // truncate the array and lose all prior runs.
    QSaveFile sf(trendPath());
    if (sf.open(QIODevice::WriteOnly)) {
        if (!setOwnerOnlyPerms(sf))
            warnNotOwnerOnly(trendPath(), "audit trend history");
        sf.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        sf.commit();
    }
}

// ---------------------------------------------------------------------------
// Baseline persistence
// ---------------------------------------------------------------------------

QString AuditDialog::baselinePath() const {
    return m_projectPath + "/.audit_cache/baseline.json";
}

void AuditDialog::loadBaseline() {
    m_baselineFingerprints.clear();
    m_hasBaseline = false;
    QByteArray baselineBytes;
    if (!readAuditStateFile(baselinePath(), &baselineBytes)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(baselineBytes);
    if (!doc.isObject()) return;
    const QJsonArray arr = doc.object().value("fingerprints").toArray();
    for (const QJsonValue &v : arr) m_baselineFingerprints.insert(v.toString());
    m_hasBaseline = !m_baselineFingerprints.isEmpty();
}

void AuditDialog::saveBaseline() {
    ensurePrivateDir(m_projectPath + "/.audit_cache");   // ANTS-1988 — 0700
    QJsonArray arr;
    for (const CheckResult &r : std::as_const(m_completedResults)) {
        // Per-finding fingerprints — stable across unrelated code changes,
        // because dedupKey is (file:line:checkId:title-hash).
        for (const Finding &f : r.findings)
            arr.append(f.dedupKey);
    }
    QJsonObject root;
    root["generated"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["version"] = 2;               // v2 = per-finding dedupKey
    root["fingerprints"] = arr;
    // Atomic write — baseline is the anchor for "new findings only" views;
    // a torn write leaves the filter off-by-many until re-saved.
    QSaveFile f(baselinePath());
    if (f.open(QIODevice::WriteOnly)) {
        if (!setOwnerOnlyPerms(f))
            warnNotOwnerOnly(baselinePath(), "audit baseline");
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        if (f.commit()) {
            loadBaseline();
            if (m_newOnlyBtn) m_newOnlyBtn->setEnabled(true);
            m_statusLabel->setFullText(QString("Baseline saved — %1 fingerprints").arg(arr.size()));
        }
    }
}

// ---------------------------------------------------------------------------
// ANTS-1719 — opt-in safe-list auto-fix. Plans behaviour-neutral repairs
// for the current findings, then applies them high-line-first per file so
// an earlier removal never shifts a later target line. Each applied repair
// is logged to .audit_cache/autofix-*.jsonl. Stale findings refresh on the
// next Run Audit — we don't auto-rerun the scan.
// ---------------------------------------------------------------------------
void AuditDialog::runAutoFix() {
    if (m_completedResults.isEmpty()) return;

    QHash<QString, QString> contents;   // absPath -> original file text
    QList<ants::autofix::Repair> repairs;
    // ANTS-5083 — planRepair needs a real line; no repairable file is this big.
    constexpr qint64 kMaxAutoFixFileBytes = 4 * 1024 * 1024;
    for (const CheckResult &r : std::as_const(m_completedResults)) {
        for (const Finding &f : r.findings) {
            // ANTS-5083 — ask now, not the flag cached when the run parsed the
            // finding: a suppression added after the run must stop the repair.
            if (isSuppressed(f)) continue;
            if (f.line < 1) continue;  // planRepair rejects it; don't read the file
            const QString abs = resolveProjectPath(f.file);
            if (abs.isEmpty()) continue;
            if (!contents.contains(abs)) {
                if (QFileInfo(abs).size() > kMaxAutoFixFileBytes) {
                    contents.insert(abs, QString());
                    continue;
                }
                QFile in(abs);
                contents.insert(abs, in.open(QIODevice::ReadOnly)
                    ? QString::fromUtf8(in.readAll()) : QString());
            }
            const QString &body = contents.value(abs);
            if (body.isEmpty()) continue;
            auto rep = ants::autofix::planRepair(
                f, abs, body, QStringLiteral(ANTS_VERSION));
            if (rep) repairs.append(*rep);
        }
    }

    // High-line-first within a file so a removal doesn't invalidate the
    // plan for a lower line still pending in this batch.
    std::sort(repairs.begin(), repairs.end(),
              [](const ants::autofix::Repair &a, const ants::autofix::Repair &b) {
                  if (a.file != b.file) return a.file < b.file;
                  return a.line > b.line;
              });

    const QString cacheDir = AuditCache::cacheDir(
        QFileInfo(m_projectPath).canonicalFilePath());
    int applied = 0;
    for (const ants::autofix::Repair &rep : std::as_const(repairs)) {
        if (ants::autofix::applyRepair(rep)) {
            ants::autofix::logRepair(cacheDir, rep);
            ++applied;
        }
    }

    if (m_statusLabel) {
        m_statusLabel->setFullText(repairs.isEmpty()
            ? QStringLiteral("Auto-fix: no safe repairs in the current findings")
            : QString("Auto-fix: applied %1 of %2 safe repairs — "
                      "re-run audit to refresh").arg(applied).arg(repairs.size()));
    }
}

// ---------------------------------------------------------------------------
// Rule Quality dialog (0.6.31 self-learning surface)
// ---------------------------------------------------------------------------
void AuditDialog::showRuleQualityDialog() {
    if (!m_qualityTracker) return;

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Rule Quality — last 30 days");
    dlg->resize(820, 540);

    auto *layout = new QVBoxLayout(dlg);
    auto *header = new QLabel(
        "Per-rule fire / suppression history from "
        "<code>audit_rule_quality.json</code>. Rules with a high "
        "suppression rate are noisy — consider tightening their "
        "regex or <code>dropIfContains</code> filter in "
        "<code>audit_rules.json</code>. The 💡 column proposes a "
        "common-substring tightening when ≥ 2 suppressions share a "
        "shape.", dlg);
    header->setWordWrap(true);
    header->setTextFormat(Qt::RichText);
    layout->addWidget(header);

    auto *view = new QTextBrowser(dlg);
    view->setOpenExternalLinks(false);
    layout->addWidget(view, 1);

    const auto rows = m_qualityTracker->report();
    QString html;
    html += "<style>"
            "table{border-collapse:collapse;width:100%;font-family:monospace;font-size:11px}"
            "th,td{border:1px solid #555;padding:4px 8px;text-align:left}"
            "th{background:#222}"
            ".hi{background:#3a1f1f}.med{background:#3a311f}"
            ".sug{font-style:italic;color:#7ec77a}"
            "</style>";
    if (rows.isEmpty()) {
        html += "<p><em>No audit runs recorded yet — run the audit at "
                "least once to populate the dashboard.</em></p>";
    } else {
        html += "<table><tr><th>Rule</th><th>Fires (30d)</th>"
                "<th>Suppressed (30d)</th><th>FP&nbsp;rate</th>"
                "<th>All-time fires</th><th>💡 Suggested tightening</th></tr>";
        for (const auto &s : rows) {
            QString rowClass;
            if (s.fpRate30d >= 50) rowClass = " class=\"hi\"";
            else if (s.fpRate30d >= 25) rowClass = " class=\"med\"";

            QString rateText;
            if (s.fpRate30d < 0) rateText = "—";
            else                  rateText = QString::number(s.fpRate30d) + "%";

            QString suggestion = m_qualityTracker->suggestTightening(s.ruleId);
            QString suggestionCell;
            if (suggestion.isEmpty()) {
                suggestionCell = "—";
            } else {
                // Show the candidate substring in monospace so the user can
                // see exact whitespace. Mark with the .sug class so it stands
                // out without forcing the user to read the full row.
                suggestionCell = QString("<code class=\"sug\">%1</code>")
                                    .arg(suggestion.toHtmlEscaped());
            }

            html += QString("<tr%1>"
                            "<td>%2</td><td>%3</td><td>%4</td><td>%5</td>"
                            "<td>%6</td><td>%7</td></tr>")
                       .arg(rowClass,
                            s.ruleId.toHtmlEscaped(),
                            QString::number(s.fires30d),
                            QString::number(s.suppressions30d),
                            rateText,
                            QString::number(s.firesAllTime),
                            suggestionCell);
        }
        html += "</table>";
    }
    view->setHtml(html);

    auto *btnBox = new QHBoxLayout;
    auto *closeBtn = new QPushButton("Close", dlg);
    btnBox->addStretch();
    btnBox->addWidget(closeBtn);
    layout->addLayout(btnBox);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);

    // Frameless-parent-friendly raise: same trick used by showDiffViewer
    // — a dialog spawned off a frameless QMainWindow can land behind on
    // KWin without an explicit raise + activate.
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void AuditDialog::buildUI() {
    // ANTS-1259 — the dialog hosts two tabs: the original single-pane
    // audit UI and a new Debt Sweep panel. `root` (below) lays out the
    // Audit tab exactly as before; only the outer container changed.
    auto *outer = new QVBoxLayout(m_contentArea);
    outer->setContentsMargins(0, 0, 0, 0);
    m_tabs = new QTabWidget(m_contentArea);
    outer->addWidget(m_tabs);

    auto *auditTab = new QWidget(m_tabs);
    auto *root = new QVBoxLayout(auditTab);
    root->setSpacing(8);

    m_pathLabel = new QLabel(this);
    m_pathLabel->setText("<b>Project:</b> " + m_projectPath);
    m_pathLabel->setTextFormat(Qt::RichText);
    root->addWidget(m_pathLabel);

    m_typesLabel = new QLabel(this);
    const QString types = m_detectedTypes.isEmpty() ? "Unknown" : m_detectedTypes.join(", ");
    QString typesText = "<b>Detected:</b> " + types +
                        "  ·  <span style='color:#888;'>ants-audit v" +
                        QString::fromLatin1(ANTS_VERSION) + "</span>";
    if (m_hasBaseline) typesText += "  ·  <i>Baseline loaded</i>";
    m_typesLabel->setText(typesText);
    m_typesLabel->setTextFormat(Qt::RichText);
    if (m_skippedUntrustedRules > 0) {
        // Tooltip explains *why* the "Untrusted rules" badge appears and
        // how to opt in. Mouse-hover reveals it; the qWarning on stderr
        // carries the same message for headless invocations.
        m_typesLabel->setToolTip(
            QString("This project's audit_rules.json contains %1 rule(s) with "
                    "a `command` field. Those fields are bash-exec'd verbatim "
                    "at audit time, so Ants requires explicit trust per "
                    "(project path, rule-pack hash). Editing the rule pack "
                    "invalidates trust. To opt in: export "
                    "ANTS_AUDIT_TRUST_UNSAFE=1, or call "
                    "Config::trustAuditRulePack() from a prior session.")
                .arg(m_skippedUntrustedRules));
    }
    root->addWidget(m_typesLabel);
    root->addSpacing(4);

    // Scrollable check list
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *scrollWidget = new QWidget();
    auto *scrollLayout = new QVBoxLayout(scrollWidget);
    scrollLayout->setSpacing(6);
    scrollLayout->setContentsMargins(0, 0, 8, 0);

    QStringList categories;
    for (const auto &c : std::as_const(m_checks))
        if (!categories.contains(c.category)) categories << c.category;

    for (const QString &cat : std::as_const(categories)) {
        auto *group = new QGroupBox(cat, scrollWidget);
        auto *gLayout = new QVBoxLayout(group);
        gLayout->setSpacing(4);
        gLayout->setContentsMargins(10, 14, 10, 8);

        for (int i = 0; i < m_checks.size(); ++i) {
            auto &check = m_checks[i];
            if (check.category != cat) continue;

            auto *row = new QHBoxLayout();
            row->setSpacing(8);

            auto *nameLabel = new QLabel(check.name, group);
            QFont f = nameLabel->font();
            f.setWeight(QFont::Medium);
            nameLabel->setFont(f);
            row->addWidget(nameLabel);

            auto *descLabel = new QLabel(check.description, group);
            descLabel->setStyleSheet("color: #888;");
            row->addWidget(descLabel);
            row->addStretch();

            auto *toggle = new ToggleSwitch(group);
            toggle->setChecked(check.autoSelect && check.available);
            toggle->setEnabled(check.available);
            check.toggle = toggle;
            row->addWidget(toggle);

            gLayout->addLayout(row);
        }

        scrollLayout->addWidget(group);
    }
    scrollLayout->addStretch();
    scroll->setWidget(scrollWidget);
    root->addWidget(scroll, 1);

    // Button row
    auto *btnRow = new QHBoxLayout();
    auto *allOn = new QPushButton("All On", this);
    auto *allOff = new QPushButton("All Off", this);
    allOn->setFixedWidth(70);
    allOff->setFixedWidth(70);
    connect(allOn, &QPushButton::clicked, this, [this]() {
        for (auto &c : m_checks)
            if (c.toggle && c.available) c.toggle->setChecked(true);
    });
    connect(allOff, &QPushButton::clicked, this, [this]() {
        for (auto &c : m_checks)
            if (c.toggle) c.toggle->setChecked(false);
    });
    btnRow->addWidget(allOn);
    btnRow->addWidget(allOff);
    btnRow->addStretch();

    m_newOnlyBtn = new QPushButton("New since baseline", this);
    m_newOnlyBtn->setCheckable(true);
    m_newOnlyBtn->setFixedHeight(32);
    m_newOnlyBtn->setEnabled(m_hasBaseline);
    m_newOnlyBtn->setToolTip(
        m_hasBaseline ? "Hide findings already present in the saved baseline"
                      : "No baseline saved yet — run an audit and click 'Save baseline'");
    // ANTS-1150 — connect FIRST then restore under a QSignalBlocker
    // so the blocker actually defends against the restore re-firing
    // the handler. Lazy-invalidate stickiness for the m_hasBaseline
    // gate: if baseline is later deleted, the persisted bool stays
    // through the gap; next saveBaseline re-honours the preference.
    connect(m_newOnlyBtn, &QPushButton::toggled, this, [this](bool on) {
        m_showNewOnly = on;
        if (m_config) m_config->setAuditShowNewOnly(on);
        if (!m_completedResults.isEmpty()) renderResults();
    });
    if (m_config && m_hasBaseline && m_config->auditShowNewOnly()) {
        QSignalBlocker block(m_newOnlyBtn);
        m_newOnlyBtn->setChecked(true);
        m_showNewOnly = true;
    }
    btnRow->addWidget(m_newOnlyBtn);

    // Recent-changes scope toggle (file-level).
    auto *recentBtn = new QPushButton("Recent changes only", this);
    recentBtn->setCheckable(true);
    recentBtn->setFixedHeight(32);
    recentBtn->setEnabled(m_detectedTypes.contains("Git"));
    recentBtn->setToolTip(
        m_detectedTypes.contains("Git")
        ? "Scope audit findings to files touched in the last 10 commits"
        : "Recent-changes mode requires a Git repository");

    // Stricter variant — filters by diff hunks, not just file membership.
    // Answers "what would a CI PR-review flag" without noise from pre-
    // existing issues in touched files.
    auto *linesBtn = new QPushButton("Changed lines only", this);
    linesBtn->setCheckable(true);
    linesBtn->setFixedHeight(32);
    linesBtn->setEnabled(m_detectedTypes.contains("Git"));
    linesBtn->setToolTip(
        m_detectedTypes.contains("Git")
        ? "Only findings on lines modified in the last 10 commits (vs. HEAD~N)"
        : "Changed-lines mode requires a Git repository");

    connect(recentBtn, &QPushButton::toggled, this, [this, linesBtn](bool on) {
        m_recentOnly = on;
        // Disabling file-level scope implicitly disables the stricter
        // line-level mode too.
        if (!on && linesBtn->isChecked()) linesBtn->setChecked(false);
    });
    connect(linesBtn, &QPushButton::toggled, this, [this, recentBtn](bool on) {
        m_recentLinesOnly = on;
        // Line-level is a stricter subset of file-level — keep the outer
        // toggle in sync so both behave predictably.
        if (on && !recentBtn->isChecked()) recentBtn->setChecked(true);
    });
    btnRow->addWidget(recentBtn);
    btnRow->addWidget(linesBtn);

    m_baselineBtn = new QPushButton("Save baseline", this);
    m_baselineBtn->setFixedHeight(32);
    m_baselineBtn->setVisible(false);
    m_baselineBtn->setToolTip("Record the current findings as a baseline. "
                              "Future runs will highlight only new findings.");
    connect(m_baselineBtn, &QPushButton::clicked, this, &AuditDialog::saveBaseline);
    btnRow->addWidget(m_baselineBtn);

    // 0.6.31 self-learning — Rule Quality dialog. Always visible (not
    // gated on a completed run) because the per-rule history persists
    // across runs and is informative even on a fresh dialog open.
    m_qualityBtn = new QPushButton("📊 Rule Quality", this);
    m_qualityBtn->setFixedHeight(32);
    m_qualityBtn->setToolTip("Show per-rule fire / suppression history "
                             "to surface noisy rules and propose tightenings");
    connect(m_qualityBtn, &QPushButton::clicked, this, &AuditDialog::showRuleQualityDialog);
    btnRow->addWidget(m_qualityBtn);

    m_runBtn = new QPushButton("Run Audit", this);
    m_runBtn->setFixedHeight(32);
    m_runBtn->setMinimumWidth(120);
    connect(m_runBtn, &QPushButton::clicked, this, &AuditDialog::runAudit);
    btnRow->addWidget(m_runBtn);

    // Cancel — appears only during a run. Placed immediately next to the
    // Run button so the user's eye is already there when they realize a
    // check is hanging (clazy can chew on compile_commands.json for
    // tens of seconds on a big project).
    m_cancelBtn = new QPushButton("Cancel", this);
    m_cancelBtn->setFixedHeight(32);
    m_cancelBtn->setMinimumWidth(100);
    m_cancelBtn->setVisible(false);
    m_cancelBtn->setToolTip("Stop the audit, kill the current tool, "
                             "and render whatever checks completed so far");
    connect(m_cancelBtn, &QPushButton::clicked, this, &AuditDialog::cancelAudit);
    btnRow->addWidget(m_cancelBtn);

    // SARIF export (industry-standard JSON, consumed by GitHub Code Scanning,
    // VSCode SARIF Viewer, SonarQube, CodeQL, etc.). Appears after a run.
    m_sarifBtn = new QPushButton("Export SARIF", this);
    m_sarifBtn->setFixedHeight(32);
    m_sarifBtn->setVisible(false);
    m_sarifBtn->setToolTip("Save findings as SARIF v2.1.0 JSON for CI / IDE viewers");
    connect(m_sarifBtn, &QPushButton::clicked, this, [this]() {
        if (m_completedResults.isEmpty()) return;
        ensurePrivateDir(m_projectPath + "/.audit_cache");  // ANTS-5085 — 0700 from the start
        const QString path = m_projectPath + "/.audit_cache/audit-"
                           + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")
                           + ".sarif";
        // 0.7.52 (2026-04-27 indie-review CRITICAL) — atomic write +
        // 0600 perms. The SARIF body may carry secrets surfaced by
        // gitleaks / secrets_scan rules (the whole point of those
        // checks is to find leaked tokens); persisting the report
        // world-readable would just relocate the leak. QSaveFile +
        // commit gives the same write-rename-fsync atomicity as the
        // Config code path; setOwnerOnlyPerms locks 0600.
        QSaveFile sf(path);
        if (!sf.open(QIODevice::WriteOnly)) {  // ANTS-5084 — say so
            m_statusLabel->setFullText("SARIF save failed: " + sf.errorString());
            return;
        }
        // ANTS-5151 — the report may carry leaked secrets: private or not saved.
        if (!setOwnerOnlyPerms(sf)) {
            m_statusLabel->setFullText("SARIF save failed: could not make the file private");
            return;  // uncommitted: QSaveFile discards its temp file
        }
        sf.write(exportSarif().toUtf8());
        if (sf.commit()) {
            if (setOwnerOnlyPerms(path)) {  // re-chmod final inode
                m_statusLabel->setFullText("SARIF saved: " + path);
            } else {
                QFile::remove(path);
                m_statusLabel->setFullText("SARIF save failed: could not make " + path + " private");
            }
        } else {
            m_statusLabel->setFullText("SARIF save failed: " + sf.errorString());
        }
    });
    btnRow->addWidget(m_sarifBtn);

    m_htmlBtn = new QPushButton("Export HTML", this);
    m_htmlBtn->setFixedHeight(32);
    m_htmlBtn->setVisible(false);
    m_htmlBtn->setToolTip("Save a single-file HTML report (browser-viewable, no external assets)");
    connect(m_htmlBtn, &QPushButton::clicked, this, [this]() {
        if (m_completedResults.isEmpty()) return;
        ensurePrivateDir(m_projectPath + "/.audit_cache");  // ANTS-5085 — 0700 from the start
        const QString path = m_projectPath + "/.audit_cache/audit-"
                           + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")
                           + ".html";
        // 0.7.52 — same atomic + 0600 treatment as SARIF (CRITICAL
        // from 2026-04-27 indie-review — the HTML body embeds the
        // same finding metadata, including any leaked-secret strings
        // surfaced by gitleaks rules).
        QSaveFile sf(path);
        if (!sf.open(QIODevice::WriteOnly)) {  // ANTS-5084 — say so
            m_statusLabel->setFullText("HTML save failed: " + sf.errorString());
            return;
        }
        // ANTS-5151 — same finding metadata as the SARIF: private or not saved.
        if (!setOwnerOnlyPerms(sf)) {
            m_statusLabel->setFullText("HTML save failed: could not make the file private");
            return;  // uncommitted: QSaveFile discards its temp file
        }
        sf.write(exportHtml().toUtf8());
        if (sf.commit()) {
            if (setOwnerOnlyPerms(path)) {
                m_statusLabel->setFullText("HTML report saved: " + path);
            } else {
                QFile::remove(path);
                m_statusLabel->setFullText("HTML save failed: could not make " + path + " private");
            }
        } else {
            m_statusLabel->setFullText("HTML save failed: " + sf.errorString());
        }
    });
    btnRow->addWidget(m_htmlBtn);

    m_reviewBtn = new QPushButton("Review with Claude", this);
    m_reviewBtn->setFixedHeight(32);
    m_reviewBtn->setMinimumWidth(160);
    m_reviewBtn->setToolTip("Save results and ask Claude Code to review and fix findings");
    m_reviewBtn->setVisible(false);
    connect(m_reviewBtn, &QPushButton::clicked, this, [this]() {
        QString text = plainTextResults();
        if (text.isEmpty()) return;

        auto *tmp = new QTemporaryFile(QDir::tempPath() + "/ants-audit-XXXXXX.txt");
        tmp->setAutoRemove(false);
        if (tmp->open()) {
            tmp->write(text.toUtf8());
            QString path = tmp->fileName();
            tmp->close();
            // ANTS-2119 — the report persists in /tmp (autoRemove off; the path is
            // handed to the external reviewer) and can contain file paths + code
            // context. Enforce owner-only perms explicitly rather than relying on
            // the platform's QTemporaryFile default.
            if (!setOwnerOnlyPerms(path)) {
                QFile::remove(path);
                delete tmp;
                m_statusLabel->setFullText("Review not started: could not make the report file private");
                return;
            }
            delete tmp;
            emit reviewRequested(path);
            close();
        } else {
            delete tmp;
        }
    });
    btnRow->addWidget(m_reviewBtn);

    // ANTS-1719 — opt-in safe-list auto-fix. Never runs implicitly on a
    // scan; the user clicks to apply behaviour-neutral repairs.
    m_autofixBtn = new QPushButton("Auto-fix safe", this);
    m_autofixBtn->setFixedHeight(32);
    m_autofixBtn->setMinimumWidth(140);
    m_autofixBtn->setToolTip("Apply mechanically-safe, behaviour-neutral "
                             "repairs (dead #include / Q_UNUSED, expired "
                             "version-TODO, comment spacing). Logged to "
                             ".audit_cache/autofix-*.jsonl.");
    m_autofixBtn->setVisible(false);
    connect(m_autofixBtn, &QPushButton::clicked, this, &AuditDialog::runAutoFix);
    btnRow->addWidget(m_autofixBtn);

    root->addLayout(btnRow);

    // Progress + results
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    m_statusLabel = new ElidedLabel(this);
    m_statusLabel->setElideMode(Qt::ElideRight);
    m_statusLabel->setVisible(false);
    root->addWidget(m_statusLabel);

    // Filter bar — severity pills + live text filter + confidence-sort toggle.
    // Hidden until the first audit run produces results. Wired to m_textFilter
    // / m_activeSeverities / m_sortByConfidence and re-renders on every change.
    m_filterBar = new QWidget(this);
    auto *filterRow = new QHBoxLayout(m_filterBar);
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(6);

    m_filterInput = new QLineEdit(m_filterBar);
    m_filterInput->setPlaceholderText("Filter by file, message, rule, author…");
    m_filterInput->setClearButtonEnabled(true);
    connect(m_filterInput, &QLineEdit::textChanged, this, [this](const QString &s) {
        m_textFilter = s.trimmed().toLower();
        if (!m_completedResults.isEmpty()) renderResults();
    });
    filterRow->addWidget(m_filterInput, 1);

    struct SevSpec { const char *label; Severity sev; const char *color; const char *jsonKey; };
    static const SevSpec kSevs[] = {
        {"BLK", Severity::Blocker,  "#8B0000", "blocker"},
        {"CRT", Severity::Critical, "#E74856", "critical"},
        {"MAJ", Severity::Major,    "#FFA500", "major"},
        {"MIN", Severity::Minor,    "#FFD700", "minor"},
        {"INF", Severity::Info,     "#4CAF50", "info"},
    };
    // ANTS-1150 — read persisted severity-filter pill states. Empty
    // object means "all 5 on" (matches first-launch behaviour).
    const QJsonObject persistedSev = m_config ? m_config->auditSeverityFilters()
                                              : QJsonObject{};
    for (const auto &sp : kSevs) {
        auto *pill = new QPushButton(sp.label, m_filterBar);
        pill->setCheckable(true);
        // Default checked == true (matches m_activeSeverities = {0..4}).
        // If the persisted object has an explicit key, honor it.
        const bool initial = persistedSev.isEmpty()
            ? true
            : persistedSev.value(QLatin1String(sp.jsonKey)).toBool(true);
        pill->setChecked(initial);
        pill->setFixedSize(44, 26);
        pill->setToolTip(QString("Show %1 findings").arg(sp.label));
        pill->setStyleSheet(QString(
            "QPushButton { border:1px solid %1; color:%1; background:transparent; "
            "  border-radius:12px; font-size:10px; font-weight:bold; }"
            "QPushButton:checked { background:%1; color:white; }"
            "QPushButton:!checked { color:#555; border-color:#555; }"
        ).arg(sp.color));
        const int sevIndex = static_cast<int>(sp.sev);
        // Sync m_activeSeverities to the restored pill state. The
        // member-default initialiser at auditdialog.h:472 is
        // {0,1,2,3,4} — i.e. all on; remove the index here if the
        // persisted state had this severity off.
        if (!initial) m_activeSeverities.remove(sevIndex);
        connect(pill, &QPushButton::toggled, this, [this, sevIndex](bool on) {
            if (on) m_activeSeverities.insert(sevIndex);
            else    m_activeSeverities.remove(sevIndex);
            // ANTS-1150 — persist the full 5-key severity-filter
            // object on every toggle. storeIfChanged short-circuits
            // when nothing actually changed.
            if (m_config) {
                QJsonObject sf;
                sf[QLatin1String("blocker")]  = m_activeSeverities.contains(static_cast<int>(Severity::Blocker));
                sf[QLatin1String("critical")] = m_activeSeverities.contains(static_cast<int>(Severity::Critical));
                sf[QLatin1String("major")]    = m_activeSeverities.contains(static_cast<int>(Severity::Major));
                sf[QLatin1String("minor")]    = m_activeSeverities.contains(static_cast<int>(Severity::Minor));
                sf[QLatin1String("info")]     = m_activeSeverities.contains(static_cast<int>(Severity::Info));
                m_config->setAuditSeverityFilters(sf);
            }
            if (!m_completedResults.isEmpty()) renderResults();
        });
        filterRow->addWidget(pill);
        m_sevPills.append(pill);
    }

    // ANTS-1257 — "Since baseline" pill. One toggle for the common
    // "what changed since I saved a baseline" view: only findings on
    // git-changed lines that aren't already in the saved baseline. The
    // underlying "New since baseline" / "Changed lines only" controls
    // stay available for using either mode independently.
    m_sinceBaselineBtn = new QPushButton("Since baseline", m_filterBar);
    m_sinceBaselineBtn->setCheckable(true);
    m_sinceBaselineBtn->setFixedHeight(26);
    m_sinceBaselineBtn->setEnabled(m_hasBaseline);
    m_sinceBaselineBtn->setToolTip(
        m_hasBaseline
        ? "Show only new findings on recently-changed lines (combines "
          "new-since-baseline with changed-lines-only)"
        : "Save a baseline first to use this filter");
    connect(m_sinceBaselineBtn, &QPushButton::toggled, this,
            &AuditDialog::onSinceBaselineToggled);
    filterRow->addWidget(m_sinceBaselineBtn);

    m_confidenceSortBtn = new QPushButton("Sort by confidence", m_filterBar);
    m_confidenceSortBtn->setCheckable(true);
    m_confidenceSortBtn->setFixedHeight(26);
    m_confidenceSortBtn->setToolTip("Sort findings within each check by confidence score (highest first)");
    connect(m_confidenceSortBtn, &QPushButton::toggled, this, [this](bool on) {
        m_sortByConfidence = on;
        if (!m_completedResults.isEmpty()) renderResults();
    });
    filterRow->addWidget(m_confidenceSortBtn);

    // Batch AI triage — sends every currently-visible, not-yet-triaged
    // finding to the LLM in one request (batches of ≤20). Label updates
    // with the visible count after every render; clicking it opens a
    // confirmation prompt so the user always sees how many tokens are
    // about to be spent before the POST goes out. Hidden when AI is not
    // configured — presence of the button would imply support that isn't
    // actually available.
    m_batchTriageBtn = new QPushButton(QStringLiteral("🧠 Triage visible"), m_filterBar);
    m_batchTriageBtn->setFixedHeight(26);
    m_batchTriageBtn->setToolTip(
        "Send every visible, not-yet-triaged finding to the configured "
        "AI endpoint in a single batch. Requires Settings → AI → enabled.");
    connect(m_batchTriageBtn, &QPushButton::clicked, this,
            &AuditDialog::onBatchTriageClicked);
    filterRow->addWidget(m_batchTriageBtn);

    // ANTS-1257 — "Fold actionable into ROADMAP". Allocates stable IDs and
    // inserts a templated `### 🔍 Audit fold-in (DATE)` block after the
    // active release heading. Enabled only when ≥1 visible finding is
    // actionable (AI TRUE_POSITIVE or confidence ≥ 70).
    m_foldRoadmapBtn = new QPushButton(QStringLiteral("🌳 Fold actionable"),
                                       m_filterBar);
    m_foldRoadmapBtn->setFixedHeight(26);
    m_foldRoadmapBtn->setEnabled(false);
    m_foldRoadmapBtn->setToolTip(
        "Fold the actionable findings (AI-confirmed or confidence ≥ 70) into "
        "ROADMAP.md as a dated audit fold-in block with freshly-allocated IDs.");
    connect(m_foldRoadmapBtn, &QPushButton::clicked, this,
            &AuditDialog::onFoldRoadmapClicked);
    filterRow->addWidget(m_foldRoadmapBtn);

    m_filterBar->setVisible(false);
    root->addWidget(m_filterBar);

    m_results = new QTextBrowser(this);
    m_results->setReadOnly(true);
    m_results->setFont(QFont("monospace", 9));
    m_results->setVisible(false);
    // Custom ants-suppress:// scheme is handled internally; tell QTextBrowser
    // not to try to open any clicked link as a navigable URL.
    m_results->setOpenLinks(false);
    m_results->setOpenExternalLinks(false);
    connect(m_results, &QTextBrowser::anchorClicked,
            this, &AuditDialog::onResultAnchorClicked);
    root->addWidget(m_results, 2);

    m_tabs->addTab(auditTab, tr("Audit"));

    // ANTS-1259 — Debt Sweep tab.
    buildDebtSweepTab();
}

// ---------------------------------------------------------------------------
// Project documentation helpers — used by the Claude-review handoff to
// attach documented standards so Claude can weigh findings against project
// rules when suggesting fixes.
// ---------------------------------------------------------------------------

QString AuditDialog::readProjectDoc(const QString &name) const {
    QFile f(m_projectPath + "/" + name);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.read(64 * 1024);  // Cap at 64 KB per doc
    f.close();
    return QString::fromUtf8(bytes).trimmed();
}

// ---------------------------------------------------------------------------
// Audit execution
// ---------------------------------------------------------------------------

AuditDialog::RecentChangeSets AuditDialog::readRecentChangeSets(
    const QString &projectPath, int commits, bool includeLines) {
    RecentChangeSets out;

    // ANTS-5084 — a git run that failed or timed out left both sets empty,
    // which the filters read as "nothing changed" and hid every filed
    // finding. The reason is kept instead, and the filters stand down.
    const auto runGit = [&projectPath](const QStringList &args, int timeoutMs,
                                       QByteArray *output) -> QString {
        static constexpr qint64 kMaxGitOutputBytes = 64LL * 1024 * 1024;
        const std::unique_ptr<QProcess, void (*)(QProcess *)> p(
            new QProcess, auditdialogdetail::releaseProcess);   // ANTS-5083
        p->setWorkingDirectory(projectPath);
        p->start(QStringLiteral("git"), args);
        if (!p->waitForFinished(timeoutMs)) {
            if (p->error() == QProcess::FailedToStart)
                return QStringLiteral("git could not start");
            p->kill();
            p->waitForFinished(1000);
            return QStringLiteral("git %1 timed out").arg(args.first());
        }
        if (p->exitStatus() != QProcess::NormalExit || p->exitCode() != 0)
            return QStringLiteral("git %1 failed").arg(args.first());
        *output = p->readAllStandardOutput();
        if (output->size() > kMaxGitOutputBytes) {
            output->clear();
            return QStringLiteral("git %1 output too large").arg(args.first());
        }
        return QString();
    };

    QByteArray logOut;
    out.error = runGit({QStringLiteral("log"),
                        QString("-n%1").arg(commits),
                        QStringLiteral("--name-only"),
                        QStringLiteral("--format="),
                        QStringLiteral("--diff-filter=ACMR")},
                       5000, &logOut);
    if (!out.error.isEmpty()) return out;
    const QStringList lines =
        QString::fromUtf8(logOut).split('\n', Qt::SkipEmptyParts);
    QSet<QString> seen;
    for (const QString &raw : lines) {
        const QString p = raw.trimmed();
        if (p.isEmpty() || seen.contains(p)) continue;
        seen.insert(p);
        // Keep only files that still exist on disk.
        if (QFile::exists(projectPath + "/" + p))
            out.files << p;
    }
    if (!includeLines) return out;

    // Line-level scoping: ask git for the diff with zero context, parse
    // the hunk headers (`@@ -old,count +new,count @@`) and record the
    // destination line range for each file. Uses HEAD~N..HEAD so new
    // commits + uncommitted working-tree changes both land in the map.
    // A repository without HEAD~N diffs against the empty tree instead,
    // so every line counts as changed.
    QString base = QString("HEAD~%1").arg(commits);
    QByteArray probe;
    if (!runGit({QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                 QStringLiteral("--quiet"), base + QStringLiteral("^{commit}")},
                2000, &probe).isEmpty()) {
        QByteArray emptyTree;
        out.error = runGit({QStringLiteral("hash-object"), QStringLiteral("-t"),
                            QStringLiteral("tree"), QStringLiteral("/dev/null")},
                           2000, &emptyTree);
        if (!out.error.isEmpty()) return out;
        base = QString::fromLatin1(emptyTree).trimmed();
    }
    QByteArray diffOut;
    out.error = runGit({QStringLiteral("diff"), QStringLiteral("--unified=0"), base},
                       8000, &diffOut);
    if (!out.error.isEmpty()) return out;
    const QStringList dlines =
        QString::fromUtf8(diffOut).split('\n', Qt::KeepEmptyParts);
    // Local, not static: two overlapping requests can run this concurrently.
    const QRegularExpression reFileHdr(R"(^\+\+\+ b/(.+)$)");
    const QRegularExpression reHunk(R"(^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@)");
    QString curFile;
    for (const QString &dl : dlines) {
        auto mf = reFileHdr.match(dl);
        if (mf.hasMatch()) { curFile = mf.captured(1); continue; }
        auto mh = reHunk.match(dl);
        if (mh.hasMatch() && !curFile.isEmpty()) {
            const int start = mh.captured(1).toInt();
            const int count = mh.captured(2).isEmpty()
                              ? 1 : mh.captured(2).toInt();
            // count=0 means pure deletion — nothing to attribute
            // to an added line; skip.
            if (count <= 0) continue;
            auto &set = out.lines[curFile];
            for (int i = 0; i < count; ++i) set.insert(start + i);
        }
    }
    return out;
}

void AuditDialog::requestRecentChangeSets(bool includeLines,
                                          std::function<void()> then) {
    m_recentScopeWaiters.append(std::move(then));
    if (!m_detectedTypes.contains("Git")) {
        m_recentFiles.clear();
        m_recentLines.clear();
        m_recentScopeError.clear();
        for (const auto &waiter : std::exchange(m_recentScopeWaiters, {})) waiter();
        return;
    }
    // A request already reading what this one needs serves it too.
    if (m_recentScopeInFlight && (m_recentScopeInFlightLines || !includeLines)) return;

    // While git runs, the recent filters stand down, as they do when it fails.
    m_recentFiles.clear();
    m_recentLines.clear();
    m_recentScopeError = QStringLiteral("reading git history");
    m_recentScopeInFlight = true;
    m_recentScopeInFlightLines = includeLines;
    const quint64 request = ++m_recentScopeRequest;

    auto result = std::make_shared<RecentChangeSets>();
    const QString projectPath = m_projectPath;
    const int commits = m_recentCommits;
    QThread *worker = QThread::create([result, projectPath, commits, includeLines]() {
        *result = readRecentChangeSets(projectPath, commits, includeLines);
    });
    // Same shape as requestDebtScan: the worker deletes itself, and if the
    // dialog closes first the `this` context drops the delivery.
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this, [this, request, result]() {
        if (request != m_recentScopeRequest) return;   // a later request supersedes
        m_recentScopeInFlight = false;
        m_recentFiles = std::move(result->files);
        m_recentLines = std::move(result->lines);
        m_recentScopeError = std::move(result->error);
        for (const auto &waiter : std::exchange(m_recentScopeWaiters, {})) waiter();
    }, Qt::QueuedConnection);
    worker->start();
}

void AuditDialog::runAudit() {
    m_results->clear();
    m_results->setVisible(true);
    m_progress->setVisible(true);
    m_statusLabel->setVisible(true);
    m_completedResults.clear();
    // ANTS-5083 — inlineSuppressed() reads source lines through this cache
    // during the run; left over from the last run, it missed an inline
    // suppression added in between.
    m_fileLineCache.clear();
    m_cancelled = false;
    ++m_runGeneration;  // ANTS-5067 — a lane result from an earlier run is dropped
    // ANTS-5084 — reset before the git wait too: a Cancel during it must not
    // report the previous run's check or counts.
    m_currentCheck = -1;
    m_checksRun = 0;
    m_totalSelected = 0;
    m_snapshotPersisted = false;  // see m_snapshotPersisted in auditdialog.h
    // Cancel becomes the primary action while a run is in-flight; Run
    // button stays visible but disabled so the button-row geometry
    // doesn't jump.
    if (m_cancelBtn) m_cancelBtn->setVisible(true);

    // Compute the "recent files" list if the user opted into scoped audit.
    // ANTS-5084 — the Since baseline pill reads the changed-line sets too;
    // clearing them under it hid every filed finding and wrote that count
    // to the trend file.
    // ANTS-5084 — git runs on a worker thread; the checks start once the sets
    // arrive, unless this run was cancelled or replaced meanwhile.
    if (m_recentOnly || m_sinceBaseline) {
        const quint64 generation = m_runGeneration;
        m_statusLabel->setFullText(QStringLiteral("Reading recent changes from git…"));
        requestRecentChangeSets(m_recentLinesOnly || m_sinceBaseline,
                                [this, generation]() {
            if (generation == m_runGeneration) startSelectedChecks();
        });
        return;
    }
    m_recentFiles.clear();
    m_recentLines.clear();
    m_recentScopeError.clear();
    startSelectedChecks();
}

void AuditDialog::startSelectedChecks() {
    m_totalSelected = 0;
    for (const auto &c : std::as_const(m_checks))
        if (c.toggle && c.toggle->isChecked()) ++m_totalSelected;

    if (m_totalSelected == 0) {
        m_statusLabel->setFullText("No checks selected.");
        m_progress->setVisible(false);
        return;
    }

    m_progress->setRange(0, m_totalSelected);
    m_progress->setValue(0);
    m_checksRun = 0;
    m_currentCheck = -1;
    m_runBtn->setEnabled(false);

    for (auto &c : m_checks)
        if (c.toggle) c.toggle->setEnabled(false);

    runNextCheck();
}

void AuditDialog::connectProcessSignals() {
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &AuditDialog::onCheckFinished);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &AuditDialog::onCheckOutputReady);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &AuditDialog::onCheckErrorReady);
}

void AuditDialog::cancelAudit() {
    // Idempotent — double-click on Cancel (or a race between the button
    // click and the check finishing) is harmless.
    if (m_cancelled) return;
    m_cancelled = true;
    ++m_runGeneration;  // ANTS-5067 — a lane still running delivers nothing

    // Watchdog timer and QProcess both have to be quieted BEFORE we
    // render, otherwise their queued signals would re-enter the pipeline.
    m_timeout->stop();
    if (m_process && m_process->state() != QProcess::NotRunning) {
        // Same disconnect-then-kill dance the timeout path uses: prevent
        // finished() from firing with the partial output we're about to
        // discard.
        disconnect(m_process, nullptr, this, nullptr);
        // ANTS-5038 — the whole process group, TERM first (see the timeout).
        const qint64 pgid = m_process->processId();
        ProcessGroup::signalGroup(pgid, SIGTERM);
        m_process->waitForFinished(500);
        ProcessGroup::signalGroup(pgid, SIGKILL);
        m_process->kill();
        m_process->waitForFinished(1000);
        // Reconnect so a follow-up Run Audit works normally — full set
        // (finished + drain slots), not just finished.
        connectProcessSignals();
    }

    // Record a sentinel row so the report makes the truncation obvious —
    // without this, cancelling between checks produces an empty report
    // indistinguishable from "all checks passed", which is the opposite
    // of what happened.
    if (m_currentCheck >= 0 && m_currentCheck < m_checks.size()) {
        const auto &check = m_checks[m_currentCheck];
        CheckResult r;
        r.checkId   = check.id;
        r.checkName = check.name;
        r.category  = check.category;
        r.type      = CheckType::Info;
        r.severity  = Severity::Info;
        r.source    = "audit";
        r.output    = "Cancelled by user — partial results only";
        r.warning   = true;
        m_completedResults.append(r);
    }

    // ANTS-1136 — set the snapshot-persisted flag BEFORE
    // renderResults() so the cancelled-run partial picture
    // doesn't pollute trend.json. Pre-fix code let
    // renderResults append a snapshot for an audit that ran
    // 3 of 28 checks, producing a noise drop on the next
    // run's trend line — opposite of what trends are for.
    m_snapshotPersisted = true;
    // ANTS-1136 (revalidation fold-in): also flush rule-quality
    // fires here so a cancel-then-kill doesn't lose the records
    // accumulated by checks that completed before cancel. The
    // runNextCheck cycle-completion flush only covers clean
    // completion.
    if (m_qualityTracker) m_qualityTracker->save();
    // ANTS-5041 — the corroboration shift, once per run, over every check's
    // findings, before the first render. Never per render: it is not
    // idempotent.
    {
        QSet<QString> noisy;
        if (m_qualityTracker)
            for (const QString &id : m_qualityTracker->noisyRuleIds()) noisy.insert(id);
        AuditEngine::applyCorroborationShiftAcross(m_completedResults, noisy);
    }
    // Render whatever completed + restore the UI to the idle state.
    renderResults();
    m_runBtn->setEnabled(true);
    if (m_cancelBtn) m_cancelBtn->setVisible(false);
    m_reviewBtn->setVisible(true);
    if (m_autofixBtn) m_autofixBtn->setVisible(true);
    m_baselineBtn->setVisible(true);
    if (m_sarifBtn)  m_sarifBtn->setVisible(true);
    if (m_htmlBtn)   m_htmlBtn->setVisible(true);
    if (m_filterBar) m_filterBar->setVisible(true);
    for (auto &c : m_checks)
        if (c.toggle) c.toggle->setEnabled(c.available);
    m_statusLabel->setFullText(
        QString("Audit cancelled after %1/%2 check(s).")
            .arg(m_checksRun).arg(m_totalSelected));
}

void AuditDialog::runNextCheck() {
    // Cancellation short-circuit — user clicked Cancel while a check was
    // running. `cancelAudit()` already killed the process and rendered
    // whatever completed, so we just bail out of the chain.
    if (m_cancelled) return;

    while (++m_currentCheck < m_checks.size()) {
        if (m_checks[m_currentCheck].toggle &&
            m_checks[m_currentCheck].toggle->isChecked())
            break;
    }

    if (m_currentCheck >= m_checks.size()) {
        m_progress->setValue(m_totalSelected);
        // ANTS-1136 — flush rule-quality fires to disk at end of
        // every audit run so a SIGSEGV / SIGKILL / power loss
        // before clean shutdown doesn't lose 30 minutes of fire
        // records. recordSuppression already saves immediately
        // (user-initiated, rare); recordFire was deferred to
        // RAII-on-destruction, which only fires on clean
        // shutdown. One save() per run keeps disk traffic bounded
        // while making the data durable.
        if (m_qualityTracker) m_qualityTracker->save();
        // ANTS-5041 — once per run, before the first render (see cancelAudit).
        {
            QSet<QString> noisy;
            if (m_qualityTracker)
                for (const QString &id : m_qualityTracker->noisyRuleIds()) noisy.insert(id);
            AuditEngine::applyCorroborationShiftAcross(m_completedResults, noisy);
        }
        renderResults();
        m_runBtn->setEnabled(true);
        if (m_cancelBtn)   m_cancelBtn->setVisible(false);
        m_reviewBtn->setVisible(true);
        m_baselineBtn->setVisible(true);
        if (m_sarifBtn)    m_sarifBtn->setVisible(true);
        if (m_htmlBtn)     m_htmlBtn->setVisible(true);
        if (m_filterBar)   m_filterBar->setVisible(true);
        for (auto &c : m_checks)
            if (c.toggle) c.toggle->setEnabled(c.available);
        return;
    }

    const auto &check = m_checks[m_currentCheck];
    m_statusLabel->setFullText("Running: " + check.name + "…");
    m_progress->setValue(m_checksRun);

    // In-process runner path — sidesteps QProcess entirely. Used by the
    // feature-coverage lanes whose logic is awkward to express in a
    // bash/grep/awk pipeline. Output is piped through the same
    // handleCheckOutput() post-processing the QProcess path uses, so
    // suppressions / path rules / dedup behave identically.
    //
    // ANTS-5067 — the runner reads the whole project, so it runs on a worker
    // thread and the GUI keeps painting and draining terminals. A result from
    // an earlier run (cancelled, or superseded by a new Run) is dropped by
    // generation. The worker deletes itself; if the dialog closes first, the
    // `this` context drops the delivery. Pattern: requestDebtScan.
    if (check.inProcessRunner) {
        const QString projectPath = m_projectPath;
        const auto runner = check.inProcessRunner;
        const quint64 generation = m_runGeneration;
        auto output = std::make_shared<QString>();
        QThread *worker = QThread::create([runner, projectPath, output]() {
            *output = runner(projectPath);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        connect(worker, &QThread::finished, this, [this, generation, output]() {
            if (generation != m_runGeneration) return;
            handleCheckOutput(*output);
        }, Qt::QueuedConnection);
        worker->start();
        return;
    }

    // Reset per-check accumulators before launching. The drain slots
    // append to these as the tool runs; onCheckFinished reads from
    // them instead of asking the live process for everything at once.
    m_currentOutput.clear();
    m_currentError.clear();
    m_outputOverflowed = false;

    m_timeout->start(check.timeoutMs);
    ProcessGroup::startsOwnGroup(*m_process);
    m_process->start("/bin/bash", {"-c", check.command});
    if (!m_process->waitForStarted(5000)) {
        m_timeout->stop();
        CheckResult r;
        r.checkId = check.id;
        r.checkName = check.name;
        r.category = check.category;
        r.type = check.type;
        r.severity = check.severity;
        r.output = "Failed to start process";
        r.warning = true;
        m_completedResults.append(r);
        ++m_checksRun;
        runNextCheck();
    }
}

void AuditDialog::onCheckOutputReady() {
    if (m_outputOverflowed || !m_process) return;
    m_currentOutput.append(m_process->readAllStandardOutput());
    if (m_currentOutput.size() + m_currentError.size() > MAX_TOOL_OUTPUT_BYTES) {
        m_outputOverflowed = true;
        if (m_process->state() != QProcess::NotRunning) {
            ProcessGroup::signalGroup(*m_process, SIGKILL);
            m_process->kill();
        }
    }
}

void AuditDialog::onCheckErrorReady() {
    if (m_outputOverflowed || !m_process) return;
    m_currentError.append(m_process->readAllStandardError());
    if (m_currentOutput.size() + m_currentError.size() > MAX_TOOL_OUTPUT_BYTES) {
        m_outputOverflowed = true;
        if (m_process->state() != QProcess::NotRunning) {
            ProcessGroup::signalGroup(*m_process, SIGKILL);
            m_process->kill();
        }
    }
}

// Helper: build the standard tool-health warning shape (Info severity,
// warning flag, distinct prefix). Centralizes the four exit modes
// (timeout, overflow, crash, non-zero-with-stderr-only) so the
// renderer styles them identically.
static CheckResult makeToolHealthWarning(const AuditCheck &check,
                                         const QString &message) {
    CheckResult r;
    r.checkId   = check.id;
    r.checkName = check.name;
    r.category  = check.category;
    r.type      = CheckType::Info;
    r.severity  = Severity::Info;
    r.output    = message;
    r.warning   = true;
    return r;
}

void AuditDialog::onCheckFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    m_timeout->stop();
    // Signal may arrive after `cancelAudit()` killed the process — the
    // finished() slot is queued, so we can race here. Bail silently
    // rather than appending a half-baked CheckResult from partial
    // output.
    if (m_cancelled) return;
    if (m_currentCheck < 0 || m_currentCheck >= m_checks.size()) return;

    // Drain any tail data the readyRead slots haven't picked up yet.
    // Qt's docs allow data to remain readable on the process even
    // after finished() fires; without these calls we'd lose the last
    // few hundred bytes from fast-exiting tools.
    if (!m_outputOverflowed) {
        m_currentOutput.append(m_process->readAllStandardOutput());
        m_currentError.append(m_process->readAllStandardError());
    }

    const auto &check = m_checks[m_currentCheck];

    // Tool-health branch 1: output-cap breached. Tool was killed by the
    // drain slot when its accumulated bytes exceeded MAX_TOOL_OUTPUT_BYTES.
    if (m_outputOverflowed) {
        const qsizetype capMiB = MAX_TOOL_OUTPUT_BYTES / (1024 * 1024);
        m_completedResults.append(makeToolHealthWarning(check,
            QString("Output exceeded %1 MiB cap — tool-health issue, "
                    "not a finding").arg(capMiB)));
        ++m_checksRun;
        runNextCheck();
        return;
    }

    // Tool-health branch 2: signal exit (segfault, SIGABRT, OOM-killed).
    // Distinguishes a tool bug from "tool ran cleanly with no findings."
    if (exitStatus == QProcess::CrashExit) {
        const QString errOut = QString::fromUtf8(m_currentError).trimmed();
        QString msg = "Tool crashed (signal exit) — tool-health issue, "
                      "not a finding";
        if (!errOut.isEmpty())
            msg += "\n" + errOut;
        m_completedResults.append(makeToolHealthWarning(check, msg));
        ++m_checksRun;
        runNextCheck();
        return;
    }

    QString output = QString::fromUtf8(m_currentOutput).trimmed();
    const QString errOutput = QString::fromUtf8(m_currentError).trimmed();

    // Tool-health branch 3: non-zero exit, empty stdout, stderr text.
    // The tool errored out before producing findings (clang-tidy
    // missing compile_commands.json, semgrep failed to parse a rule,
    // etc.). Distinct from the "stderr is just diagnostic noise"
    // success case below.
    if (exitCode != 0 && output.isEmpty() && !errOutput.isEmpty()) {
        m_completedResults.append(makeToolHealthWarning(check,
            QString("Tool exited %1 with no findings on stdout — "
                    "tool-health issue, not a finding\n%2")
                .arg(exitCode).arg(errOutput)));
        ++m_checksRun;
        runNextCheck();
        return;
    }

    // Success path: fold stdout + stderr into one findings stream via the
    // shared engine helper so the headless runner (auditrunner.cpp) feeds
    // byte-identical input — ANTS-2118 closed the channel-merge divergence.
    output = AuditEngine::mergeToolChannels(output, errOutput);

    handleCheckOutput(output);
}

void AuditDialog::handleCheckOutput(const QString &output) {
    // Drop racy callbacks arriving after the user cancelled — otherwise
    // a late-firing in-process runner could append a CheckResult behind
    // the "cancelled" sentinel.
    if (m_cancelled) return;
    if (m_currentCheck < 0 || m_currentCheck >= m_checks.size()) return;
    const auto &check = m_checks[m_currentCheck];

    // Apply declarative post-filter (noise excludes, head caps, etc).
    const FilterResult filtered =
        AuditEngine::applyFilter(output, check.filter, m_projectPath);

    CheckResult r;
    r.checkId   = check.id;
    r.checkName = check.name;
    r.category  = check.category;
    r.type      = check.type;
    r.severity  = check.severity;
    r.source    = AuditEngine::sourceForCheck(check.id);
    r.output    = filtered.body;
    r.warning   = false;

    // Parse body into structured findings. Apply in order:
    //   1. Suppress any finding whose dedup hash is in .audit_suppress
    //   2. Drop generated / path-rule-skipped findings
    //   3. Drop findings matched by .audit_allowlist.json
    //   4. Drop findings suppressed by inline directive in the file
    //   5. In "recent-only" mode: drop findings whose file isn't in the
    //      recent-commits set (unfiled findings always pass)
    //   6. Dedup within this single check (exact-duplicate message lines)
    QSet<QString> seenKeys;
    QList<Finding> parsed = AuditEngine::parseFindings(filtered.body, check);
    QSet<QString> recent;
    if (m_recentOnly) for (const QString &p : std::as_const(m_recentFiles)) recent.insert(p);
    // Mark suppressed instead of dropping so the SARIF export can surface
    // them via result.suppressions[] (SARIF §3.34). All user-facing render
    // paths (results pane, HTML export, summary) continue to filter on
    // isSuppressed and never display them.
    for (Finding &f : parsed)
        f.suppressed = isSuppressed(f.dedupKey);
    // ANTS-1708 / ANTS-1820 — also honour the drift-resilient learned-FP
    // ledger, keyed by line-independent content fingerprint. Route through
    // the shared AuditEngine filter rather than inlining its body so the
    // GUI and the headless audit_run path stay in lockstep.
    AuditEngine::applyLearnedFpSuppressions(parsed, m_learnedFpFingerprints);
    for (Finding &f : parsed) {
        if (!applyPathRules(f)) continue;       // generated files + path rules
        if (allowlisted(f)) continue;           // project-local allowlist
        if (inlineSuppressed(f)) continue;      // inline // ants-audit: disable ...
        if (m_recentOnly && m_recentScopeError.isEmpty() && !f.file.isEmpty()) {
            // Match by either exact path or project-relative path suffix.
            bool isRecent = recent.contains(f.file);
            QString matchedFile = f.file;
            if (!isRecent) {
                for (const QString &rf : std::as_const(m_recentFiles)) {
                    if (pathSuffixMatches(f.file, rf)) {
                        isRecent = true;
                        matchedFile = rf;
                        break;
                    }
                }
            }
            if (!isRecent) continue;

            // Stricter: drop findings whose line isn't within a diff hunk.
            if (m_recentLinesOnly && f.line > 0) {
                const auto it = m_recentLines.constFind(matchedFile);
                if (it == m_recentLines.constEnd() || !it->contains(f.line))
                    continue;
            }
        }
        if (seenKeys.contains(f.dedupKey)) continue;
        seenKeys.insert(f.dedupKey);
        r.findings.append(f);
    }

    // Comment/string-aware filtering for source-pattern checks. External
    // static analyzers (cppcheck, clang-tidy, pylint, …) already understand
    // comments, so skip them — applying the filter could remove real
    // findings that point to comment-adjacent declarations.
    static const QSet<QString> kSourceScannedChecks = {
        "secrets_scan", "unsafe_c_funcs", "cmd_injection", "cmd_injection_dyn",
        "format_string", "insecure_http", "unsafe_deser", "hardcoded_ips",
        "weak_crypto", "memory_patterns", "debug_leftovers", "todo_scan",
        "qt_openurl_unchecked",
        // clazy is NOT in this set — it's already AST-aware and understands
        // comment/string contexts natively.
    };
    if (kSourceScannedChecks.contains(check.id))
        dropFindingsInCommentsOrStrings(r);

    // Fold mypy "Library stubs not installed" repeats into one Info hint so
    // a missing-types nag doesn't eat 20 finding slots. When the
    // consolidator collapses ≥2 entries it stamps r.findingCountAuthored
    // = true with the pre-collapse count so the post-cap arithmetic
    // below leaves that value alone (ANTS-1343).
    consolidateMypyStubHints(r);

    // ANTS-5083 — a lane left uncapped on purpose (filter.maxLines = 0, the
    // contract-doc drift lanes, ANTS-3600 INV-10) keeps every finding; the
    // per-check cap would drop the alphabetically later docs again.
    if (check.filter.maxLines > 0)
        AuditEngine::capFindings(r, kMaxFindingsPerCheck);
    if (!r.findingCountAuthored)
        r.findingCount = r.findings.size() + r.omittedCount;
    m_completedResults.append(r);

    // 0.6.31 self-learning — record one fire per finding so the per-rule
    // dashboard can compute fires-vs-suppressions over time. Skipped
    // for the omittedCount tail since those findings never had their
    // line text materialized; the LCS suggester needs the line.
    if (m_qualityTracker) {
        for (const Finding &f : std::as_const(r.findings)) {
            if (f.suppressed) continue;
            m_qualityTracker->recordFire(check.id, f.message);
        }
    }

    ++m_checksRun;
    runNextCheck();
}

// ---------------------------------------------------------------------------
// Result rendering — sort by severity, summary banner, new-since-baseline tag
// ---------------------------------------------------------------------------

namespace auditdialogdetail {

QString severityLabel(Severity s) {
    switch (s) {
        case Severity::Blocker:  return "BLOCKER";
        case Severity::Critical: return "CRITICAL";
        case Severity::Major:    return "MAJOR";
        case Severity::Minor:    return "MINOR";
        case Severity::Info:     return "INFO";
    }
    return "?";
}

static QString severityColor(Severity s) {
    switch (s) {
        case Severity::Blocker:  return "#8B0000";
        case Severity::Critical: return "#E74856";
        case Severity::Major:    return "#FFA500";
        case Severity::Minor:    return "#FFD700";
        case Severity::Info:     return "#4CAF50";
    }
    return "#888";
}

QString typeLabel(CheckType t) {
    switch (t) {
        case CheckType::Info:          return "info";
        case CheckType::CodeSmell:     return "smell";
        case CheckType::Bug:           return "bug";
        case CheckType::Hotspot:       return "hotspot";
        case CheckType::Vulnerability: return "vuln";
    }
    return "?";
}

} // namespace auditdialogdetail

void AuditDialog::renderResults() {
    m_results->clear();
    m_findingsByKey.clear();

    // Multi-tool correlation: group findings by {file, line}. When a pair is
    // flagged by two or more distinct tools, mark every finding on that line
    // as high-confidence. Elevates cross-validated findings above single-
    // tool noise and answers the user question "which of these should I
    // trust?" without changing the severity ordering.
    {
        struct Bucket { QSet<QString> sources; QList<Finding*> fs; };
        QHash<QString, Bucket> byLoc;
        for (auto &r : m_completedResults) {
            for (Finding &f : r.findings) {
                if (f.file.isEmpty() || f.line <= 0) continue;
                const QString k = f.file + ":" + QString::number(f.line);
                auto &b = byLoc[k];
                b.sources.insert(f.source);
                b.fs.append(&f);
            }
        }
        for (auto it = byLoc.begin(); it != byLoc.end(); ++it) {
            if (it.value().sources.size() >= 2)
                for (Finding *p : std::as_const(it.value().fs)) p->highConfidence = true;
        }
    }

    // Enrichment pass: snippet + blame + confidence score, in place. Expensive
    // operations are bounded per run (blame shells out once per unique file:
    // line; snippet reads are cached in m_fileLineCache). Skip when a finding
    // has no file:line — free-form findings don't benefit from enrichment.
    //
    // We also seed Finding::aiVerdict from the existing key→finding cache so
    // confidence reflects any prior AI triage from this session.
    m_fileLineCache.clear();   // reset on every render — files may have changed
    int enriched = 0;
    const int kSnippetBudget = 300;        // per-render cap (cheap but bounded)
    for (auto &r : m_completedResults) {
        for (Finding &f : r.findings) {
            if (f.file.isEmpty() || f.line <= 0) continue;
            if (enriched >= kSnippetBudget) break;
            const QString abs = resolveProjectPath(f.file);
            if (abs.isEmpty()) continue;  // traversal: skip enrichment entirely
            if (f.snippet.isEmpty())
                f.snippet = readSnippet(abs, f.line, 3, &f.snippetStart);
            if (f.blameSha.isEmpty() && !applyCachedBlame(f))
                m_blameQueue[f.file].insert(f.line);   // ANTS-5040
            ++enriched;
        }
    }
    startQueuedBlame();
    for (auto &r : m_completedResults)
        for (Finding &f : r.findings)
            f.confidence = computeConfidence(f);

    // ANTS-1111's corroboration shift is not applied here. This runs on
    // every filter keystroke and the shift is not idempotent, so ANTS-5041
    // moved it to the run-completion paths (cancelAudit, runNextCheck).

    // Populate key→finding lookup for the anchor click handler. Done after
    // correlation so the lookup reflects the highConfidence flag.
    for (const auto &r : std::as_const(m_completedResults))
        for (const Finding &f : r.findings)
            m_findingsByKey.insert(f.dedupKey, f);

    // Sort: highest severity first, then by category, then by name.
    std::vector<CheckResult> sorted(m_completedResults.begin(), m_completedResults.end());
    std::sort(sorted.begin(), sorted.end(), [](const CheckResult &a, const CheckResult &b) {
        if (a.severity != b.severity) return a.severity > b.severity;
        if (a.category != b.category) return a.category < b.category;
        return a.checkName < b.checkName;
    });

    // Baseline comparison is now per-finding (via dedupKey), not per-check.
    auto findingIsNew = [this](const Finding &f) {
        if (!m_hasBaseline) return true;
        return !m_baselineFingerprints.contains(f.dedupKey);
    };
    auto checkHasNew = [&](const CheckResult &r) {
        if (!m_hasBaseline) return true;
        for (const Finding &f : r.findings)
            if (findingIsNew(f)) return true;
        return false;
    };

    // Counts by severity.
    int bySev[5] = {0, 0, 0, 0, 0};
    int totalFindings = 0;
    int totalNew = 0;
    int totalSuppressed = 0;    // kept for display; not sure how many the file hid
    for (const auto &r : sorted) {
        if (r.warning) continue;
        for (const Finding &f : r.findings) {
            if (isSuppressed(f)) continue;
            if (m_sinceBaseline) {
                if (!sinceBaselineVisible(f))
                    continue;
            } else if (m_showNewOnly && !findingIsNew(f)) {
                continue;
            }
            ++bySev[static_cast<int>(f.severity)];
            ++totalFindings;
            if (findingIsNew(f)) ++totalNew;
        }
        if (r.omittedCount > 0 && !m_showNewOnly) {
            // overflow beyond per-check cap — count toward its severity
            bySev[static_cast<int>(r.severity)] += r.omittedCount;
            totalFindings += r.omittedCount;
        }
    }
    totalSuppressed = m_suppressedKeys.size();

    // Compute trend delta against the last saved snapshot BEFORE writing our
    // own, so the user sees "vs previous run" rather than "vs this run".
    // Persist the new snapshot afterward.
    const TrendSnapshot prev = loadLastSnapshot();
    TrendSnapshot curr;
    curr.timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    curr.total     = totalFindings;
    for (int i = 0; i < 5; ++i) curr.bySev[i] = bySev[i];

    auto delta = [](int cur, int was) {
        if (cur == was) return QString("±0");
        const int d = cur - was;
        return QString(d > 0 ? "+%1" : "%1").arg(d);
    };
    auto deltaColor = [](int cur, int was, bool higherIsWorse = true) {
        if (cur == was) return QStringLiteral("#888");
        const bool worse = higherIsWorse ? (cur > was) : (cur < was);
        return worse ? QStringLiteral("#E74856") : QStringLiteral("#4CAF50");
    };

    QString trendLine;
    if (!prev.timestamp.isEmpty() && !m_showNewOnly) {
        trendLine = QString(
            "<br><span style='font-size:10px;'>"
            "Trend vs %1:  "
            "Total <span style='color:%2;'>%3</span>  ·  "
            "<span style='color:#8B0000;'>BLK</span> <span style='color:%4;'>%5</span>  ·  "
            "<span style='color:#E74856;'>CRT</span> <span style='color:%6;'>%7</span>  ·  "
            "<span style='color:#FFA500;'>MAJ</span> <span style='color:%8;'>%9</span>"
            "</span>"
        ).arg(prev.timestamp.left(10),
              deltaColor(curr.total, prev.total),
              delta(curr.total, prev.total),
              deltaColor(curr.bySev[4], prev.bySev[4]),
              delta(curr.bySev[4], prev.bySev[4]),
              deltaColor(curr.bySev[3], prev.bySev[3]),
              delta(curr.bySev[3], prev.bySev[3]),
              deltaColor(curr.bySev[2], prev.bySev[2]),
              delta(curr.bySev[2], prev.bySev[2]));
    }

    // Noise-floor stat (0.6.44) — actionable% for this run plus a rolling
    // 30-day average pulled from RuleQualityTracker. Denominator is every
    // finding the pipeline surfaced BEFORE suppressions + AI-FALSE_POSITIVE
    // verdicts were subtracted; numerator is what's left. This is the "how
    // much of the output was signal?" single-number answer users keep
    // asking for during review. Anchor from the 10th audit: 0/55
    // actionable = 0% signal, which is the exact regime where deeper
    // scrutiny is a waste — the stat makes that visible at a glance.
    int aiFalsePositiveCount = 0;
    for (const auto &r : sorted) {
        if (r.warning) continue;
        for (const Finding &f : r.findings) {
            if (isSuppressed(f)) continue;
            if (f.aiVerdict == "FALSE_POSITIVE") ++aiFalsePositiveCount;
        }
    }
    const int denominator = totalFindings + totalSuppressed;
    const int noiseCount  = totalSuppressed + aiFalsePositiveCount;
    const int signalCount = std::max(0, totalFindings - aiFalsePositiveCount);
    int actionablePct = -1;  // sentinel: show "–" when there's no data
    if (denominator > 0)
        actionablePct = (100 * signalCount) / denominator;

    // 30-day rolling average across RuleQualityTracker — sums the last
    // 30 days of fires vs suppressions to give "how noisy has this
    // project been lately?" as a peer comparison point.
    int history30dPct = -1;
    int history30dRuns = 0;
    if (m_qualityTracker) {
        const auto stats = m_qualityTracker->report();
        int fires = 0, suppressed = 0;
        for (const auto &s : stats) {
            fires      += s.fires30d;
            suppressed += s.suppressions30d;
        }
        if (fires > 0) {
            history30dPct = (100 * std::max(0, fires - suppressed)) / fires;
            history30dRuns = fires;  // finding-fires, not run count — naming is user-facing
        }
    }

    QString noiseFloorLine;
    if (denominator > 0) {
        QString actionableColor =
            actionablePct >= 60 ? QStringLiteral("#4CAF50") :
            actionablePct >= 20 ? QStringLiteral("#FFA500") :
                                  QStringLiteral("#E74856");
        QString details;
        if (totalSuppressed > 0)
            details += QString(" · %1 suppressed").arg(totalSuppressed);
        if (aiFalsePositiveCount > 0)
            details += QString(" · %1 AI-flagged FP").arg(aiFalsePositiveCount);
        QString historySuffix;
        if (history30dPct >= 0) {
            historySuffix = QString(
                "  <span style='color:#888;'>· 30d avg %1% actionable "
                "(n=%2 fires)</span>"
            ).arg(history30dPct).arg(history30dRuns);
        }
        noiseFloorLine = QString(
            "<br><span style='font-size:10px;'>Signal: "
            "<span style='color:%1; font-weight:bold;'>%2% actionable</span> "
            "(%3 / %4%5)%6</span>"
        ).arg(actionableColor,
              QString::number(actionablePct))
         .arg(signalCount)
         .arg(denominator)
         .arg(details)
         .arg(historySuffix);
    }
    (void)noiseCount;  // retained for future use; computed for symmetry

    // Summary banner.
    QString banner = QString(
        "<div style='background:#222; color:#eee; padding:6px 10px; border-radius:4px;"
        " margin-bottom:10px; font-family:monospace;'>"
        "<b>Audit Summary</b> — %1 checks, %2 findings"
        "%3"
        "<br>"
        "<span style='color:#8B0000;'>BLOCKER: %4</span> · "
        "<span style='color:#E74856;'>CRITICAL: %5</span> · "
        "<span style='color:#FFA500;'>MAJOR: %6</span> · "
        "<span style='color:#FFD700;'>MINOR: %7</span> · "
        "<span style='color:#4CAF50;'>INFO: %8</span>"
        "%9"
        "</div>"
    ).arg(sorted.size())
     .arg(totalFindings)
     .arg(m_showNewOnly ? " (new since baseline only)" : "")
     .arg(bySev[(int)Severity::Blocker])
     .arg(bySev[(int)Severity::Critical])
     .arg(bySev[(int)Severity::Major])
     .arg(bySev[(int)Severity::Minor])
     .arg(bySev[(int)Severity::Info])
     .arg((totalSuppressed > 0
          ? QString("<br><span style='color:#888; font-size:10px;'>"
                    "%1 suppression(s) loaded from .audit_suppress · "
                    "%2 new since baseline</span>")
              .arg(totalSuppressed).arg(m_hasBaseline ? totalNew : 0)
          : (m_hasBaseline ? QString("<br><span style='color:#888; font-size:10px;'>"
                                     "%1 new since baseline</span>").arg(totalNew)
                           : QString()))
          + noiseFloorLine
          + trendLine);
    m_results->append(banner);

    // Persist the snapshot for next run's trend line. Skip if we're in
    // showNewOnly mode — that's a filtered view, not an authoritative count.
    //
    // 0.7.55 (2026-04-27 indie-review) — also gate on m_snapshotPersisted
    // so re-renders triggered by severity-pill toggles, baseline switches,
    // or other UI filter actions don't append duplicate snapshots. Was a
    // fresh appendSnapshot per filter click before — the .audit_history
    // file accumulated 10+ "snapshots" per audit run.
    // m_snapshotPersisted is reset to false in runAudit() at the start of
    // each new run, then flipped to true here on the first authoritative
    // render. UI-only re-renders find it true and skip.
    if (!m_showNewOnly && !m_snapshotPersisted) {
        appendSnapshot(curr);
        m_snapshotPersisted = true;
    }

    // Per-check sections.
    for (const auto &r : sorted) {
        if (m_showNewOnly && !checkHasNew(r)) continue;
        const bool clean = r.findings.isEmpty() && r.omittedCount == 0 && !r.warning;
        const QString color = r.warning ? "#E74856" : severityColor(r.severity);

        // Corroborated-finding count — same signal as the ★ badge, summarised
        // at the category level so the reader can prioritise at a glance.
        int corroborated = 0;
        for (const Finding &f : r.findings)
            if (f.highConfidence) ++corroborated;
        const QString corroSuffix = corroborated > 0
            ? QString(" <span title='Cross-validated by ≥2 tools' "
                      "style='color:#FFD700; font-size:10px;'>"
                      "(%1 corroborated)</span>").arg(corroborated)
            : QString();

        // Header
        m_results->append(QString(
            "<div style='margin-bottom:4px;'>"
            "<span style='color:%1; font-weight:bold;'>[%2] %3 — %4%5</span>%8"
            "<span style='color:#888; font-size:10px;'>  · %6 · %7</span>"
            "</div>"
        ).arg(color,
              severityLabel(r.severity),
              typeLabel(r.type).toUpper(),
              r.checkName.toHtmlEscaped(),
              r.warning ? " (tool issue)" : "",  // timeout or failed start (ANTS-5084)
              r.category.toHtmlEscaped(),
              r.source,
              corroSuffix));

        if (r.warning) {
            m_results->append(QString("<pre style='margin:2px 0 10px 10px; color:#c00;'>%1</pre>")
                              .arg(r.output.toHtmlEscaped()));
            continue;
        }
        if (clean) {
            m_results->append("<div style='margin:2px 0 10px 10px; color:#888;'>"
                              "No issues found.</div>");
            continue;
        }

        // Build findings body. Each finding row now carries:
        //   confidence pip + file:line · message                  (header)
        //   ★ cross-tool hit · NEW · blame tag                    (inline tags)
        //   [details] snippet (±3) + ai triage button             (collapsed)
        //
        // Sorting: within a check, higher confidence first when m_sortByConfidence.
        // Filtering: respects active severity pills and the text filter input.
        std::vector<const Finding*> sortedFindings;
        sortedFindings.reserve(r.findings.size());
        for (const Finding &f : r.findings) sortedFindings.push_back(&f);
        if (m_sortByConfidence) {
            std::stable_sort(sortedFindings.begin(), sortedFindings.end(),
                [](const Finding *a, const Finding *b) {
                    return a->confidence > b->confidence;
                });
        }

        QStringList rows;
        // Hoisted out of the loop below per clazy container-inside-loop:
        // reused across iterations via clear() to avoid per-finding alloc.
        QStringList parts;
        for (const Finding *pf : sortedFindings) {
            const Finding &f = *pf;
            if (isSuppressed(f)) continue;
            if (m_sinceBaseline) {
                if (!sinceBaselineVisible(f))
                    continue;
            } else if (m_showNewOnly && !findingIsNew(f)) {
                continue;
            }
            if (!m_activeSeverities.contains(static_cast<int>(f.severity))) continue;
            if (!m_textFilter.isEmpty()) {
                const QString hay = (f.file + " " + f.message + " " +
                                     f.checkId + " " + f.blameAuthor).toLower();
                if (!hay.contains(m_textFilter)) continue;
            }
            const bool isNew = m_hasBaseline && findingIsNew(f);
            const QString loc = f.file.isEmpty()
                              ? QString()
                              : (f.line >= 0
                                 ? QString("<span style='color:#89B4FA;'>%1:%2</span>  ")
                                     .arg(f.file.toHtmlEscaped()).arg(f.line)
                                 : QString("<span style='color:#89B4FA;'>%1</span>  ")
                                     .arg(f.file.toHtmlEscaped()));

            // Confidence pip — small colored dot with the numeric score on hover.
            QString pipColor = "#4CAF50";  // green ≥70
            if (f.confidence < 40)      pipColor = "#E74856";
            else if (f.confidence < 70) pipColor = "#FFA500";
            const QString pip = QString(
                "<span title='Confidence %1/100' style='color:%2; "
                "font-family:monospace; font-size:11px; font-weight:bold;'>"
                "●</span><span style='color:#666; font-size:9px;'>%1</span>  ")
                .arg(f.confidence).arg(pipColor);

            // Dedup-key suppress anchor.
            const QString keyAnchor = QString(
                "<a href='ants-suppress://%1' style='color:#666; "
                "font-size:9px; text-decoration:none;' "
                "title='Click to suppress this finding'>%1</a>")
                .arg(f.dedupKey);
            const QString confTag = f.highConfidence
                ? QStringLiteral(" <span style='color:#FFD700; font-weight:bold;'"
                                 " title='Flagged by 2+ tools'>★</span>")
                : QString();
            // Blame tag — "by <author> · <date> · <sha>"
            QString blameTag;
            if (!f.blameSha.isEmpty()) {
                parts.clear();
                if (!f.blameAuthor.isEmpty()) parts << f.blameAuthor;
                if (!f.blameDate.isEmpty())   parts << f.blameDate;
                parts << f.blameSha;
                blameTag = QString(" <span style='color:#888; font-size:9px;' "
                                   "title='git blame'>[%1]</span>")
                               .arg(parts.join(" · ").toHtmlEscaped());
            }
            // AI triage verdict badge (if previously triaged).
            QString verdictBadge;
            if (!f.aiVerdict.isEmpty()) {
                QString vc = "#888";
                if (f.aiVerdict == "TRUE_POSITIVE")  vc = "#E74856";
                if (f.aiVerdict == "FALSE_POSITIVE") vc = "#4CAF50";
                if (f.aiVerdict == "NEEDS_REVIEW")   vc = "#FFA500";
                QString verdictLabel = f.aiVerdict;
                verdictLabel.replace('_', ' ');
                // ANTS-1830 — double-quote the title attribute. toHtmlEscaped()
                // escapes " but NOT ', so the untrusted aiReasoning (from the AI
                // endpoint, reachable via prompt-injection from the audited
                // project's own source) could break out of a single-quoted
                // attribute. Double quotes + the existing escaping close that.
                verdictBadge = QString(
                    " <span style=\"color:%1; font-size:9px; font-weight:bold;\" "
                    "title=\"AI triage: %2/100 — %3\">%4</span>")
                    .arg(vc, QString::number(f.aiConfidence),
                         f.aiReasoning.left(160).toHtmlEscaped(),
                         verdictLabel);
            }

            // Clickable "details" toggle — shows snippet + AI-triage link.
            const bool expanded = m_expandedKeys.contains(f.dedupKey);
            const QString toggleAnchor = QString(
                " <a href='ants-expand://%1' style='color:#666; font-size:9px; "
                "text-decoration:none;' title='%2 context'>%3</a>")
                .arg(f.dedupKey,
                     expanded ? "Hide" : "Show",
                     expanded ? "[hide]" : "[details]");

            QString header = QString("%1%2%3%4%5%6  %7%8")
                                .arg(pip, loc,
                                     f.message.toHtmlEscaped(),
                                     confTag, blameTag, verdictBadge,
                                     keyAnchor, toggleAnchor);
            if (isNew)
                header += " <span style='color:#4CAF50; font-weight:bold;'>NEW</span>";

            if (expanded && (!f.snippet.isEmpty() || !f.aiVerdict.isEmpty())) {
                QString body;
                if (!f.snippet.isEmpty()) {
                    // Render snippet with line numbers; highlight the finding line.
                    QStringList lines = f.snippet.split('\n');
                    QStringList numbered;
                    for (int i = 0; i < lines.size(); ++i) {
                        const int ln = f.snippetStart + i;
                        const bool hit = (ln == f.line);
                        const QString marker = hit ? "▸" : " ";
                        const QString bg = hit
                            ? "background:#2a1a1a; color:#e0e0e0;"
                            : "color:#999;";
                        numbered << QString(
                            "<span style='%1 font-family:monospace; "
                            "font-size:11px;'>%2 %3 %4</span>")
                            .arg(bg,
                                 QString::number(ln).rightJustified(5),
                                 marker,
                                 lines[i].toHtmlEscaped());
                    }
                    body += "<div style='margin:4px 0 4px 24px; padding:6px; "
                            "background:#151515; border-left:2px solid #444;'>"
                            + numbered.join("<br>") + "</div>";
                }
                // AI triage action link (always shown when expanded) — sends
                // the finding to the configured AI endpoint on click.
                QString triageLink;
                if (f.aiVerdict.isEmpty()) {
                    triageLink = QString(
                        "<a href='ants-triage://%1' style='color:#89B4FA; "
                        "font-size:10px;'>🧠 Triage with AI</a>").arg(f.dedupKey);
                } else {
                    triageLink = QString(
                        "<span style='color:#888; font-size:10px;'>"
                        "AI triage: %1 (%2/100) — %3</span>")
                        .arg(f.aiVerdict, QString::number(f.aiConfidence),
                             f.aiReasoning.left(220).toHtmlEscaped());
                }
                // ANTS-1257 — "Allow this finding" — appends a matching
                // entry to .audit_allowlist.json so future runs drop it.
                const QString allowLink = QString(
                    "  <a href='ants-allow://%1' style='color:#FFA500; "
                    "font-size:10px;' title='Permanently allowlist this "
                    "finding (writes .audit_allowlist.json)'>📥 Allow "
                    "this finding</a>").arg(f.dedupKey);
                body += "<div style='margin:2px 0 6px 24px; font-size:10px;'>"
                      + triageLink + allowLink + "</div>";
                header += "<br>" + body;
            }

            rows << header;
        }
        if (r.omittedCount > 0 && !m_showNewOnly) {
            rows << QString("<span style='color:#FFA500;'>… and %1 more (capped at %2 per check)</span>")
                    .arg(r.omittedCount).arg(kMaxFindingsPerCheck);
        }
        if (rows.isEmpty()) continue;  // everything filtered out — hide the check
        m_results->append("<div style='margin:0 0 10px 10px;'><div style='white-space:pre-wrap; margin:0;'>"
                          + rows.join("<br>") + "</div></div>");
    }

    auto *sb = m_results->verticalScrollBar();
    if (sb) sb->setValue(0);

    m_statusLabel->setFullText(
        (m_recentScopeError.isEmpty()
             ? QString()
             : QStringLiteral("Changed-lines filter off (%1). ").arg(m_recentScopeError))
        + QString("Audit complete — %1 findings across %2 checks%3")
                           .arg(totalFindings).arg(sorted.size())
                           .arg(m_hasBaseline
                                ? QString(" (%1 new since baseline)").arg(totalNew)
                                : QString()));

    // Keep the batch-triage button label in sync with the visible set —
    // the count changes with every filter toggle and every triage verdict
    // applied. Cheap (one scan of findings) so unconditional is fine.
    refreshBatchTriageButton();
    refreshFoldRoadmapButton();
}

// ---------------------------------------------------------------------------
// Feature-coverage lane runners
// ---------------------------------------------------------------------------
//
// Thin shims over featurecoverage.cpp so the inProcessRunner callable
// has the expected signature. The actual logic (file walk, token
// extraction, fuzzy match) lives in featurecoverage.cpp where it can
// be exercised by the feature test without linking QtWidgets.

QString AuditDialog::runSpecDriftCheck(const QString &projectPath) {
    return FeatureCoverage::runSpecDriftCheck(projectPath);
}

// ANTS-3600 — thin forwarder to the pure runner in ants_audit_lib, mirroring
// runSpecDriftCheck. All logic lives in FeatureCoverage so it is headless-
// testable without QtWidgets.
QString AuditDialog::runContractDocDriftStandardsCheck(const QString &projectPath) {
    return FeatureCoverage::runContractDocDriftStandardsCheck(projectPath);
}

QString AuditDialog::runContractDocDriftSpecsCheck(const QString &projectPath) {
    return FeatureCoverage::runContractDocDriftSpecsCheck(projectPath);
}

QString AuditDialog::runChangelogCoverageCheck(const QString &projectPath) {
    return FeatureCoverage::runChangelogCoverageCheck(projectPath);
}

