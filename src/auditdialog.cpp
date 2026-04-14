#include "auditdialog.h"
#include "toggleswitch.h"
#include "config.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QDir>
#include <QFile>
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
#include <QInputDialog>
#include <QMessageBox>
#include <QUrl>
#include <QUrlQuery>
#include <QTextBrowser>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>

#include <algorithm>

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------
//
// Centralised so individual check commands don't re-list the same excludes.
// Edit once, applies everywhere.
namespace {

// find(1) exclude expression (paths).
// `external/` and `third_party/` hold vendored code we don't maintain;
// findings there aren't actionable. Same rationale as `vendor/`.
const QString kFindExcl =
    " -not -path './.git/*' -not -path './build/*' -not -path './build-*/*'"
    " -not -path './node_modules/*' -not -path './.cache/*'"
    " -not -path './dist/*' -not -path './vendor/*' -not -path './.audit_cache/*'"
    " -not -path './external/*' -not -path './third_party/*'";

// grep(1) exclude-dir list (bare, no file-include filter).
const QString kGrepExcl =
    " --exclude-dir=.git --exclude-dir=build --exclude-dir='build-*'"
    " --exclude-dir=node_modules --exclude-dir=.cache"
    " --exclude-dir=dist --exclude-dir=vendor --exclude-dir=.audit_cache"
    " --exclude-dir=external --exclude-dir=third_party";

// Security scans also skip auditdialog.cpp/.h — its check descriptions
// contain the very patterns being searched for (unsafe function names,
// URL schemes, …), which would otherwise produce self-referential hits.
const QString kGrepExclSec =
    kGrepExcl + " --exclude=auditdialog.cpp --exclude=auditdialog.h";

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

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AuditDialog::AuditDialog(const QString &projectPath, QWidget *parent)
    : QDialog(parent), m_projectPath(projectPath) {
    setWindowTitle(QStringLiteral("Project Audit — ants-audit v") + ANTS_VERSION);
    setMinimumSize(900, 600);
    resize(1050, 750);

    m_process = new QProcess(this);
    m_process->setWorkingDirectory(m_projectPath);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &AuditDialog::onCheckFinished);

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this]() {
        if (m_process->state() != QProcess::NotRunning) {
            // Disconnect finished signal to prevent double-advance after kill
            disconnect(m_process, nullptr, this, nullptr);
            m_process->kill();
            m_process->waitForFinished(1000);
            connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, &AuditDialog::onCheckFinished);
            if (m_currentCheck >= 0 && m_currentCheck < m_checks.size()) {
                CheckResult r;
                r.checkId = m_checks[m_currentCheck].id;
                r.checkName = m_checks[m_currentCheck].name;
                r.category = m_checks[m_currentCheck].category;
                r.type = m_checks[m_currentCheck].type;
                r.severity = m_checks[m_currentCheck].severity;
                r.output = "Timed out (30s)";
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
        QProcess st;
        st.start("stat", {"-f", "-c", "%T", m_projectPath});
        if (st.waitForFinished(2000) && st.exitCode() == 0)
            m_projectFsType = QString::fromUtf8(st.readAllStandardOutput()).trimmed();
    }

    loadBaseline();
    loadSuppressions();
    populateChecks();
    const int userRules = loadUserRules();
    if (userRules > 0)
        m_detectedTypes << QString("User rules: %1").arg(userRules);
    if (!m_pathRules.isEmpty())
        m_detectedTypes << QString("Path rules: %1").arg(m_pathRules.size());
    buildUI();
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
    QFile cmake(m_projectPath + "/CMakeLists.txt");
    bool isQt = false;
    if (cmake.open(QIODevice::ReadOnly)) {
        const QByteArray content = cmake.read(4096);
        if (content.contains("find_package(Qt6") || content.contains("find_package(Qt5") ||
            content.contains("Qt6::")            || content.contains("Qt5::"))
            isQt = true;
        cmake.close();
    }
    // Fallback: scan for Q_OBJECT in src/
    if (!isQt) {
        QDirIterator it(m_projectPath + "/src", {"*.h", "*.hpp"},
                        QDir::Files, QDirIterator::NoIteratorFlags);
        int scanned = 0;
        while (it.hasNext() && scanned < 30) {
            QFile hf(it.next());
            ++scanned;
            if (!hf.open(QIODevice::ReadOnly)) continue;
            if (hf.read(2048).contains("Q_OBJECT")) { isQt = true; break; }
        }
    }
    if (isQt) m_detectedTypes << "Qt";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool AuditDialog::toolExists(const QString &tool) {
    QProcess p;
    p.start("which", {tool});
    p.waitForFinished(3000);
    return p.exitCode() == 0;
}

void AuditDialog::addGrepCheck(const QString &id, const QString &name,
                               const QString &desc, const QString &category,
                               const QString &pattern, CheckType type, Severity sev,
                               bool autoSelect, const OutputFilter &filter,
                               const QStringList &extraGrepArgs) {
    QString extra;
    for (const QString &a : extraGrepArgs) extra += " " + a;
    const QString cmd =
        "grep -rnI" + kGrepExclSec + kGrepIncludeSource + extra +
        " -E " + pattern + " . 2>/dev/null";
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

    addFindCheck("dup_files", "Duplicate File Detection",
                 "Files with identical content", "General",
                 "-type f -size +100c \\( -name '*.cpp' -o -name '*.h' -o -name '*.py'"
                 " -o -name '*.js' -o -name '*.ts' \\) -exec md5sum {} +"
                 " | sort | uniq -Dw32 | head -30",
                 CheckType::CodeSmell, Severity::Minor, false);

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

    addFindCheck("binary_in_repo", "Binary Files in Source",
                 "Non-text files tracked alongside source", "General",
                 "-type f \\( -name '*.exe' -o -name '*.dll' -o -name '*.dylib'"
                 " -o -name '*.bin' -o -name '*.dat' -o -name '*.db' -o -name '*.sqlite'"
                 " -o -name '*.class' -o -name '*.pyc' -o -name '*.pyo' \\)"
                 " | head -30",
                 CheckType::CodeSmell, Severity::Minor, false);

    addFindCheck("encoding_check", "Source Encoding Check",
                 "Non-UTF-8 or BOM-prefixed source files", "General",
                 "-type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
                 " -o -name '*.py' -o -name '*.js' -o -name '*.ts' \\)"
                 " -exec file {} \\;"
                 " | grep -viE '(UTF-8|ASCII|empty)' | head -20",
                 CheckType::Bug, Severity::Minor, false);

    addFindCheck("long_files", "Overly Long Source Files",
                 "Source files exceeding 1000 lines", "General",
                 "-type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
                 " -o -name '*.py' -o -name '*.js' -o -name '*.ts' -o -name '*.go'"
                 " -o -name '*.rs' -o -name '*.java' \\)"
                 " -exec awk 'END{if(NR>1000)print NR\" \"FILENAME}' {} \\;"
                 " | sort -rn | head -20",
                 CheckType::CodeSmell, Severity::Minor, false);

    // Debug leftovers: separate commands per language, joined with ';' in the shell.
    // Kept inline because the language-specific patterns can't collapse cleanly.
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
        { /*dropIfContains*/ {"//", "/*", "error", "warn", "fatal"}, "", {}, 80 },
        false, true, nullptr
    });

    // ========== Security ==========
    addGrepCheck("secrets_scan", "Hardcoded Secrets Scan",
                 "API keys, passwords, tokens in source", "Security",
                 "'(api[_-]?key|password|secret[_-]?key|auth[_-]?token|credentials)\\s*[:=]'",
                 CheckType::Hotspot, Severity::Critical, true,
                 { /*dropIfContains*/ {"EchoMode", "setPlaceholder",
                                       "// example", "# example",
                                       "Config::set", "setAiApiKey",
                                       "keybinding", "Keybinding",
                                       "const char",  // string literal pattern names
                                       "description", "Description"},
                   "", {}, 50 },
                 {"-i",
                  "--include='*.json' --include='*.yaml' --include='*.yml'",
                  "--include='*.toml' --include='*.cfg' --include='*.ini'"});

    if (isPosixFilesystem()) {
        addFindCheck("file_perms", "World-Writable Files",
                     "World-writable or group-writable files", "Security",
                     "-type f \\( -perm -002 -o -perm -020 \\) | head -30",
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
    addGrepCheck("cmd_injection", "Command Injection Patterns",
                 "system(), popen(), exec*() with dynamic arguments", "Security",
                 "'\\b(system|popen|execlp|execvp|execl|execv|execle)\\s*\\('",
                 CheckType::Hotspot, Severity::Critical, true,
                 { /*dropIfContains*/ {".exec(", "app.exec", "menu.exec",
                                       "dialog.exec", "QApplication",
                                       "QProcess", "forkpty", "setArguments"},
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

    addGrepCheck("insecure_http", "Insecure HTTP URLs",
                 "http:// in config / source (not schema / localhost)", "Security",
                 "'http://[^l][^o][^c]'",
                 CheckType::Hotspot, Severity::Minor, true,
                 { /*dropIfContains*/ {"localhost", "127.0.0.1", "0.0.0.0",
                                       "example.com", "// comment", "placeholder",
                                       // Schema / namespace URLs
                                       "json-schema.org", "www.w3.org",
                                       "schemas.xmlsoap.org", "tempuri.org",
                                       "xmlns", "namespace", "XSD"},
                   "", {}, 30 },
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

    // hardcoded_ips: drop version strings, license dates, checksums
    addGrepCheck("hardcoded_ips", "Hardcoded IPs & Ports",
                 "IPv4 literals in source", "Security",
                 "'\\b([0-9]{1,3}\\.){3}[0-9]{1,3}\\b'",
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
            " -i build -i build-test -i build-release -i node_modules -i .audit_cache"
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
            " -i build -i build-test -i build-release -i node_modules -i .audit_cache"
            " . 2>&1 | head -50",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 50 },
            false, toolExists("cppcheck"), nullptr
        });

        const bool hasClangTidy = toolExists("clang-tidy");
        m_checks.append({
            "clang_tidy", "clang-tidy Analysis",
            "Modernize, readability, performance checks", "C/C++",
            "find . -name '*.cpp'" + kFindExcl + " | head -15"
            " | xargs -I{} clang-tidy {} -- -std=c++20 -I src 2>&1 | head -100",
            CheckType::CodeSmell, Severity::Minor, { {}, "", {}, 100 },
            false, hasClangTidy, nullptr
        });

        addFindCheck("header_guards", "Missing Header Guards",
                     "Headers without #pragma once or ifndef guard", "C/C++",
                     "-name '*.h' -o -name '*.hpp' | while read f; do"
                     " head -5 \"$f\" | grep -qE '(#pragma once|#ifndef .+_H)' || echo \"$f\";"
                     " done | head -20",
                     CheckType::Bug, Severity::Major, false);

        m_checks.append({
            "compiler_warnings", "Compiler Warnings",
            "Build with -Wall -Wextra and capture warnings", "C/C++",
            "if [ -f CMakeLists.txt ]; then"
            " tmpdir=$(mktemp -d) && cd \"$tmpdir\""
            " && cmake -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wno-unused-parameter' \"$OLDPWD\" 2>/dev/null"
            " && make -j$(nproc) 2>&1 | grep -E '(warning:|error:)' | head -60;"
            " rm -rf \"$tmpdir\";"
            " fi",
            CheckType::Bug, Severity::Major, { {}, "", {}, 60 },
            false, true, nullptr
        });

        // Memory pattern check — Qt-aware. Filters out Qt parent-child
        // ownership (`new QSomething(this)` / `new X(parentArg)`) which
        // the previous audit iterations kept re-flagging falsely.
        if (isQt) {
            addGrepCheck("memory_patterns", "Memory Management (Qt-aware)",
                         "new/malloc without smart pointers (excl. Qt parent-child)",
                         "C/C++",
                         "'\\b(new |malloc|calloc|realloc)\\b'",
                         CheckType::CodeSmell, Severity::Minor, false,
                         { /*dropIfContains*/ {
                               "unique_ptr", "shared_ptr", "make_unique", "make_shared",
                               "delete", "free", "RAII", "nothrow", "placement",
                               "Q_NEW", "qMalloc",
                               // Qt parent-child — most common pattern
                               "(this)", "(this,", "(parent)", "(parent,",
                               "(group)", "(group,", "(scrollWidget)", "(scrollWidget,",
                               "(group);", "(scroll);", "(menu);", "(layout);",
                               "new QAction(", "new QMenu(", "new QLabel(",
                               "new QPushButton(", "new QHBoxLayout(", "new QVBoxLayout(",
                               "new QWidget(", "new QTimer(", "new QSplitter(",
                               // containers — hold by value
                               "new std::"
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
        QString clazyBuildDir;
        for (const char *cand : {"build", "build-release", "build-debug", "build-test"}) {
            if (QFile::exists(m_projectPath + "/" + cand + "/compile_commands.json")) {
                clazyBuildDir = QString::fromLatin1(cand);
                break;
            }
        }
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
        //   qt-keywords                — `signals` / `slots` as identifiers
        //   range-loop-detach          — `for (auto x : container)` on Qt containers
        //   qstring-arg                — QString::arg misuse
        //   qgetenv                    — prefer qEnvironmentVariable*
        //
        // clazy-standalone output shape:
        //   path/file.cpp:LINE:COL: warning: message [-Wclazy-check-name]
        // Matches our file:line:col: regex in parseFindings() cleanly.
        m_checks.append({
            "clazy", "clazy (Qt AST analysis)", clazyDesc, "Qt",
            QString("cd %1 && clazy-standalone -p . "
                    "--checks=connect-3arg-lambda,lambda-in-connect,"
                    "container-inside-loop,old-style-connect,qt-keywords,"
                    "range-loop-detach,qstring-arg,qgetenv "
                    "../src/*.cpp 2>&1 | head -100").arg(clazyBuildDir.isEmpty()
                                                         ? "build" : clazyBuildDir),
            CheckType::Bug, Severity::Major,
            // Filter out clang driver noise; keep warning lines with check tags.
            { /*dropIfContains*/ {"In file included from", "error generated",
                                  "warnings generated", "warning generated"},
              "", {}, 100 },
            hasClazy && !clazyBuildDir.isEmpty(),
            hasClazy && !clazyBuildDir.isEmpty(),
            nullptr
        });

        // OSC 8 / URL scheme allowlist — not covered by clazy; project-specific
        // invariant (we always front-load a scheme allowlist before openUrl).
        addGrepCheck("qt_openurl_unchecked", "Qt openUrl Without Scheme Check",
                     "QDesktopServices::openUrl called on unvalidated URIs", "Qt",
                     "'QDesktopServices::openUrl'",
                     CheckType::Vulnerability, Severity::Major, true,
                     { /*dropIfContains*/ {"scheme() ==", "validScheme",
                                           "allowScheme", "isLocal",
                                           "https://", "QUrl::TolerantMode"},
                       "", {}, 20 });
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
        QString cfg;
        for (const QString &p : packs) cfg += " --config=" + p;
        m_checks.append({
            "semgrep", "Semgrep (structural patterns)",
            "AST-aware pattern matching (" + packs.join(", ") + ")",
            "Security",
            "semgrep --timeout 30 --quiet --error --disable-version-check"
            + cfg +
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
        m_checks.append({
            "bandit", "Bandit Security Scan",
            hasBandit ? "Python security issue detection" : "(bandit not installed)", "Python",
            "bandit -r . -q --exclude=./build,./build-test,./node_modules,./.audit_cache 2>&1 | head -100",
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
}

// ---------------------------------------------------------------------------
// Filter application
// ---------------------------------------------------------------------------

AuditDialog::FilterResult AuditDialog::applyFilter(const QString &raw,
                                                    const OutputFilter &f) {
    if (raw.isEmpty()) return { QString(), 0 };

    QRegularExpression dropRe;
    const bool hasDropRe = !f.dropIfMatches.isEmpty();
    if (hasDropRe) {
        dropRe.setPattern(f.dropIfMatches);
        dropRe.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
    }

    QStringList out;
    const QStringList lines = raw.split('\n', Qt::KeepEmptyParts);
    int keptCount = 0;
    for (const QString &line : lines) {
        if (line.isEmpty()) continue;

        // dropIfContains (case-insensitive)
        bool drop = false;
        for (const QString &needle : f.dropIfContains) {
            if (line.contains(needle, Qt::CaseInsensitive)) { drop = true; break; }
        }
        if (drop) continue;

        // dropIfMatches (regex)
        if (hasDropRe && dropRe.match(line).hasMatch()) continue;

        // keepOnlyIfContains (AND — all must hit)
        if (!f.keepOnlyIfContains.isEmpty()) {
            bool allHit = true;
            for (const QString &needle : f.keepOnlyIfContains) {
                if (!line.contains(needle, Qt::CaseInsensitive)) { allHit = false; break; }
            }
            if (!allHit) continue;
        }

        out << line;
        ++keptCount;
        if (f.maxLines > 0 && keptCount >= f.maxLines) break;
    }
    return { out.join('\n'), keptCount };
}

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

static QString sourceForCheck(const QString &checkId) {
    if (checkId.startsWith("cppcheck"))  return "cppcheck";
    if (checkId == "clang_tidy")         return "clang-tidy";
    if (checkId == "clazy")              return "clazy";
    if (checkId == "semgrep")            return "semgrep";
    if (checkId == "pylint")             return "pylint";
    if (checkId == "bandit")             return "bandit";
    if (checkId == "ruff")               return "ruff";
    if (checkId == "mypy")               return "mypy";
    if (checkId == "shellcheck")         return "shellcheck";
    if (checkId == "luacheck")           return "luacheck";
    if (checkId == "cargo_clippy")       return "cargo-clippy";
    if (checkId == "cargo_audit")        return "cargo-audit";
    if (checkId == "go_vet")             return "go vet";
    if (checkId == "govulncheck")        return "govulncheck";
    if (checkId == "golangci_lint")      return "golangci-lint";
    if (checkId == "eslint")             return "eslint";
    if (checkId == "npm_audit")          return "npm audit";
    if (checkId.startsWith("git_"))      return "git";
    if (checkId == "compiler_warnings")  return "gcc";
    // find-based checks
    if (checkId == "large_files" || checkId == "dup_files" ||
        checkId == "dangling_symlinks" || checkId == "binary_in_repo" ||
        checkId == "env_files" || checkId == "temp_files" ||
        checkId == "file_perms" || checkId == "header_guards" ||
        checkId == "line_stats" || checkId == "long_files" ||
        checkId == "encoding_check")
        return "find";
    return "grep";
}

static QString computeDedup(const QString &file, int line,
                            const QString &checkId, const QString &title) {
    const QString raw = QString("%1:%2:%3:%4").arg(file).arg(line).arg(checkId, title);
    return QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256)
            .toHex().left(16));
}

QList<Finding> AuditDialog::parseFindings(const QString &body, const AuditCheck &check) {
    QList<Finding> out;
    if (body.isEmpty()) return out;

    // Patterns. Greedy on file segment, which can contain slashes but not
    // colons or whitespace (matches the conventions of every tool we wrap).
    static const QRegularExpression reFileLineCol(
        R"(^([^\s:]+):(\d+):(?:\d+:)?\s*(.*)$)");
    static const QRegularExpression reFileLine(
        R"(^([^\s:]+):(\d+):\s*(.*)$)");
    // A bare filename: path with at least one '/' OR an extension.
    static const QRegularExpression reJustFile(
        R"(^([^\s:]+\.[A-Za-z0-9_]+)$|^([^\s:]+/[^\s:]+)$)");

    const QString source = sourceForCheck(check.id);
    const QStringList lines = body.split('\n', Qt::SkipEmptyParts);
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) continue;

        Finding f;
        f.checkId   = check.id;
        f.checkName = check.name;
        f.category  = check.category;
        f.type      = check.type;
        f.severity  = check.severity;
        f.source    = source;
        f.message   = line;

        auto m1 = reFileLineCol.match(line);
        auto m2 = reFileLine.match(line);
        auto m3 = reJustFile.match(line);
        if (m1.hasMatch()) {
            f.file = m1.captured(1);
            f.line = m1.captured(2).toInt();
        } else if (m2.hasMatch()) {
            f.file = m2.captured(1);
            f.line = m2.captured(2).toInt();
        } else if (m3.hasMatch()) {
            f.file = m3.captured(1).isEmpty() ? m3.captured(2) : m3.captured(1);
        }

        // Title = first 80 chars of the (trimmed) body part — used in the
        // dedup hash so two tools flagging the same line still collide.
        const QString title = line.left(80);
        f.dedupKey = computeDedup(f.file, f.line, check.id, title);
        out.append(f);
    }
    return out;
}

void AuditDialog::capFindings(CheckResult &r, int cap) {
    if (cap <= 0 || r.findings.size() <= cap) return;
    r.omittedCount = r.findings.size() - cap;
    r.findings.erase(r.findings.begin() + cap, r.findings.end());
}

// ---------------------------------------------------------------------------
// Comment/string-aware line classification — the single biggest false-
// positive source for grep-style pattern checks. Runs a tiny state machine
// over the file up to the target line and reports whether that line's
// contents are actually code (vs. // comment, /* comment */, or "string").
// ---------------------------------------------------------------------------

bool AuditDialog::lineIsCode(const QString &absPath, int line) {
    if (line <= 0 || absPath.isEmpty()) return true;
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) return true;
    // Scanning arbitrary files on audit is I/O-heavy. Cap to 2 MB so a
    // runaway check doesn't stall the dialog; larger files fall through
    // as "code" (the safe default that preserves findings).
    if (f.size() > 2 * 1024 * 1024) return true;
    const QByteArray all = f.readAll();
    f.close();

    // State: 0=code, 1=line-comment (until \n), 2=block-comment, 3=string.
    int state = 0;
    int curLine = 1;
    bool hasCodeOnLine = false;
    QChar stringQuote;

    const QString src = QString::fromUtf8(all);
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src[i];
        const QChar n = (i + 1 < src.size()) ? src[i + 1] : QChar();

        if (curLine == line) {
            if (state == 0 && !c.isSpace()) hasCodeOnLine = true;
        } else if (curLine > line) {
            break;
        }

        if (c == '\n') {
            if (state == 1) state = 0;
            ++curLine;
            continue;
        }

        switch (state) {
        case 0:  // code
            if (c == '/' && n == '/') { state = 1; ++i; }
            else if (c == '/' && n == '*') { state = 2; ++i; }
            else if (c == '"' || c == '\'') { state = 3; stringQuote = c; }
            break;
        case 1:  // line comment — terminated only by newline (handled above)
            break;
        case 2:  // block comment
            if (c == '*' && n == '/') { state = 0; ++i; }
            break;
        case 3:  // string
            if (c == '\\') { ++i; }  // skip escape
            else if (c == stringQuote) state = 0;
            break;
        }
    }
    return hasCodeOnLine;
}

void AuditDialog::dropFindingsInCommentsOrStrings(CheckResult &r) const {
    // Only makes sense for checks that produce file:line findings.
    if (r.findings.isEmpty()) return;

    QList<Finding> kept;
    for (const Finding &f : r.findings) {
        if (f.file.isEmpty() || f.line <= 0) { kept.append(f); continue; }
        // Resolve relative path against the project root.
        const QString abs = QFileInfo(f.file).isAbsolute()
                          ? f.file
                          : (m_projectPath + "/" + f.file);
        if (lineIsCode(abs, f.line)) kept.append(f);
    }
    r.findings = kept;
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
//     // NOLINT / NOLINT(rule)                          — clang-tidy
//     // NOLINTNEXTLINE / NOLINTNEXTLINE(rule)          — clang-tidy (prev line)
//     // cppcheck-suppress[=id | [id1,id2]]             — cppcheck
//     # noqa / noqa: E501                               — flake8
//     # nosec / nosec B101                              — bandit
//     # nosemgrep / nosemgrep: rule-id                  — semgrep
//     # gitleaks:allow                                  — gitleaks
//     // eslint-disable-line / -next-line [rule]        — eslint
//     # pylint: disable=code                            — pylint
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
    if (pos >= 0) return s.mid(pos).replace(QRegularExpression(R"(^[/#\s-]+)"), "").trimmed();
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
    const QByteArray all = f.size() < 4 * 1024 * 1024 ? f.readAll() : QByteArray();
    f.close();
    const QStringList lines = QString::fromUtf8(all).split('\n', Qt::KeepEmptyParts);
    cache.insert(absPath, lines);
    return lines;
}

} // namespace

bool AuditDialog::commentSuppresses(const QString &commentText, const QString &ruleId) {
    if (commentText.isEmpty()) return false;

    // Lowercase for case-insensitive token matching — rule ids are already
    // lowercase-alnum so this doesn't lose info.
    const QString body = commentText.toLower();
    const QString rule = ruleId.toLower();

    // The "bare" forms (no rule list) — any of these suppresses everything.
    // Matched conservatively: must be a whole word with `\b` semantics.
    static const QRegularExpression reBare(
        R"(\b(?:ants-audit:\s*disable(?:-next-line|-file)?|nolint(?:nextline)?|cppcheck-suppress|noqa|nosec|nosemgrep|gitleaks:allow|eslint-disable(?:-line|-next-line)?|pylint:\s*disable)\b)",
        QRegularExpression::CaseInsensitiveOption);
    const auto bareMatch = reBare.match(body);
    if (!bareMatch.hasMatch()) return false;

    // Extract the rule-list payload, if any, following one of:
    //   `: rule1, rule2`    (noqa/nosec/nosemgrep/pylint:disable style)
    //   `=rule1,rule2`      (ants-audit, eslint, pylint variants)
    //   `(rule1, rule2)`    (clang-tidy, cppcheck block form)
    //   `[id1,id2]`         (cppcheck-suppress alt)
    //   ` rule1 rule2`      (eslint bare)
    // If none present → bare form → suppress everything.
    const int tokEnd = bareMatch.capturedEnd();
    const QString tail = body.mid(tokEnd).trimmed();

    // Nothing after the token = bare suppress.
    if (tail.isEmpty() || tail.startsWith("--")) return true;

    // Pull out anything that looks like a rule-id list.
    static const QRegularExpression reList(
        R"([:=(\[\s]\s*([a-z0-9_\-\*\s,\.]+?)(?:[)\]-]|$))",
        QRegularExpression::CaseInsensitiveOption);
    const auto listMatch = reList.match(QString(" ") + tail);  // leading space ensures capture
    if (!listMatch.hasMatch()) return true;   // token present but shape unknown → conservative suppress

    const QString listStr = listMatch.captured(1);
    const QStringList ids =
        listStr.split(QRegularExpression(R"([,\s]+)"), Qt::SkipEmptyParts);
    for (const QString &raw : ids) {
        const QString id = raw.trimmed();
        if (id.isEmpty()) continue;
        // Exact match or glob like `google-*`.
        if (id == rule) return true;
        if (id.contains('*')) {
            QString pat = QRegularExpression::escape(id);
            pat.replace("\\*", ".*");
            if (QRegularExpression("^" + pat + "$",
                    QRegularExpression::CaseInsensitiveOption)
                    .match(rule).hasMatch()) {
                return true;
            }
        }
    }
    return false;
}

bool AuditDialog::inlineSuppressed(const Finding &f) const {
    if (f.file.isEmpty() || f.line <= 0) return false;
    const QString abs = QFileInfo(f.file).isAbsolute()
                      ? f.file
                      : (m_projectPath + "/" + f.file);
    const QStringList lines = readFileLines(abs, m_fileLineCache);
    if (lines.isEmpty()) return false;

    // 1. file-scope: scan first 20 lines for disable-file markers.
    const int fileHeaderEnd = std::min<int>(20, lines.size());
    for (int i = 0; i < fileHeaderEnd; ++i) {
        const QString body = commentBody(lines[i]);
        if (body.contains("ants-audit:", Qt::CaseInsensitive) &&
            body.contains("disable-file", Qt::CaseInsensitive)) {
            if (commentSuppresses(body, f.checkId)) return true;
        }
    }

    // 2. same-line: a trailing comment on the finding's line.
    const int lineIdx = f.line - 1;
    if (lineIdx >= 0 && lineIdx < lines.size()) {
        const QString body = commentBody(lines[lineIdx]);
        if (!body.isEmpty() && commentSuppresses(body, f.checkId)) return true;
    }

    // 3. previous-line: disable-next-line / NOLINTNEXTLINE / eslint-disable-next-line.
    const int prevIdx = f.line - 2;
    if (prevIdx >= 0 && prevIdx < lines.size()) {
        const QString body = commentBody(lines[prevIdx]);
        if (!body.isEmpty() &&
            (body.contains("next-line", Qt::CaseInsensitive) ||
             body.contains("NOLINTNEXTLINE") ||
             body.contains("disable-next"))) {
            if (commentSuppresses(body, f.checkId)) return true;
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

    return false;
}

QRegularExpression AuditDialog::globToRegex(const QString &glob) {
    // Convert a minimal subset of glob to regex:
    //   **   — any path (including slashes)
    //   *    — any segment (no slashes)
    //   ?    — one char (no slash)
    // Everything else is escaped.
    QString re;
    re.reserve(glob.size() * 2);
    re.append('^');
    for (int i = 0; i < glob.size(); ++i) {
        const QChar c = glob[i];
        if (c == '*' && i + 1 < glob.size() && glob[i + 1] == '*') {
            re.append(".*");
            ++i;
        } else if (c == '*') {
            re.append("[^/]*");
        } else if (c == '?') {
            re.append("[^/]");
        } else {
            re.append(QRegularExpression::escape(QString(c)));
        }
    }
    re.append('$');
    return QRegularExpression(re);
}

bool AuditDialog::applyPathRules(Finding &f) const {
    if (f.file.isEmpty()) return true;

    // Generated-file skip is absolute — happens before user overrides.
    if (isGeneratedFile(f.file)) return false;

    for (const PathRule &rule : m_pathRules) {
        if (!rule.compiled.match(f.file).hasMatch()) continue;

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

void AuditDialog::enrichWithBlame(Finding &f) const {
    if (!m_blameEnabled) return;
    if (f.file.isEmpty() || f.line <= 0) return;
    const QString key = f.file + ":" + QString::number(f.line);
    auto it = m_blameCache.constFind(key);
    if (it != m_blameCache.constEnd()) {
        f.blameAuthor = it->author;
        f.blameDate   = it->date;
        f.blameSha    = it->sha;
        return;
    }

    QProcess git;
    git.setWorkingDirectory(m_projectPath);
    git.start("git", {"blame", "--porcelain",
                      "-L", QString("%1,%1").arg(f.line),
                      "HEAD", "--", f.file});
    if (!git.waitForFinished(2000)) {
        git.kill();
        m_blameCache.insert(key, {});
        return;
    }
    if (git.exitCode() != 0) {
        m_blameCache.insert(key, {});
        return;
    }
    BlameEntry b;
    const QStringList out =
        QString::fromUtf8(git.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    if (out.isEmpty()) { m_blameCache.insert(key, {}); return; }

    // First line: <sha> <old-line> <new-line> [group-size]
    const QStringList firstParts = out.first().split(' ');
    if (!firstParts.isEmpty()) b.sha = firstParts.first().left(8);

    for (const QString &ln : out) {
        if (ln.startsWith("author ")) b.author = ln.mid(7).trimmed();
        else if (ln.startsWith("author-time ")) {
            const qint64 t = ln.mid(12).trimmed().toLongLong();
            if (t > 0)
                b.date = QDateTime::fromSecsSinceEpoch(t).toString("yyyy-MM-dd");
        }
    }
    m_blameCache.insert(key, b);
    f.blameAuthor = b.author;
    f.blameDate   = b.date;
    f.blameSha    = b.sha;
}

// ---------------------------------------------------------------------------
// Confidence score — replaces the binary `highConfidence` ★
// ---------------------------------------------------------------------------
//
// Weighted sum keyed to signals reference tools surface publicly:
//   - severity is the biggest single factor (~60% of the ceiling)
//   - multi-tool agreement is strong positive (CodeQL / Snyk model)
//   - external AST tools (cppcheck/clang-tidy/clazy/bandit/…) rank above
//     raw regex grep because they already understand comments/strings/AST
//   - test paths and generated files get a penalty
//   - AI triage (when present) can raise or lower confidence explicitly
// Clamped to [0, 100].

int AuditDialog::computeConfidence(const Finding &f) {
    int score = 10;  // floor
    // Severity base: Info=0, Minor=15, Major=30, Critical=45, Blocker=60
    score += static_cast<int>(f.severity) * 15;

    if (f.highConfidence) score += 20;

    // External AST/semantic tools — hand-curated list that matches sources
    // produced by sourceForCheck().
    static const QSet<QString> kExternalTools = {
        "cppcheck", "clang-tidy", "clazy", "semgrep",
        "pylint", "bandit", "ruff", "mypy", "shellcheck", "luacheck",
        "cargo-clippy", "cargo-audit", "go vet", "govulncheck",
        "golangci-lint", "eslint", "npm audit", "gcc",
    };
    if (kExternalTools.contains(f.source)) score += 10;

    // Path-based penalties
    const QString lp = f.file.toLower();
    const bool inTests = lp.contains("/tests/") || lp.contains("/test/") ||
                         lp.contains("test_") || lp.endsWith("_test.py") ||
                         lp.endsWith("_test.go") || lp.endsWith(".test.js") ||
                         lp.endsWith(".spec.js");
    if (inTests) score -= 20;

    // Pure grep + message very short → likely noisy
    if (f.source == "grep" && f.message.size() < 30) score -= 5;

    // Explicit AI triage shifts confidence into its own band.
    if (f.aiVerdict == "FALSE_POSITIVE") score = std::min(score, 30);
    if (f.aiVerdict == "TRUE_POSITIVE")  score = std::max(score, 80);

    return std::clamp(score, 0, 100);
}

// ---------------------------------------------------------------------------
// Suppression file
// ---------------------------------------------------------------------------

QString AuditDialog::suppressionPath() const {
    return m_projectPath + "/.audit_suppress";
}

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
    QFile f(userRulesPath());
    if (!f.open(QIODevice::ReadOnly)) return 0;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    f.close();
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
    return loaded;
}

void AuditDialog::loadSuppressions() {
    m_suppressedKeys.clear();
    QFile f(suppressionPath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
    f.close();
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;

        // v2 (JSONL): `{"key": "...", "rule": "...", ...}`
        if (line.startsWith('{')) {
            const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
            if (doc.isObject()) {
                const QString key = doc.object().value("key").toString();
                if (!key.isEmpty()) m_suppressedKeys.insert(key);
                continue;
            }
            // Malformed JSON — fall through to legacy parse as a last resort.
        }

        // v1 legacy: first whitespace-delimited token is the key.
        m_suppressedKeys.insert(line.section(QRegularExpression(R"(\s+)"), 0, 0));
    }
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
    QFile f(path);

    // If the existing file is in v1 (plain-text keys) format, convert it to
    // JSONL on first write so the user sees a consistent format afterwards.
    // Detect by peeking at the first non-comment line.
    bool needsConvert = false;
    QStringList legacyKeys;
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        f.close();
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            if (!line.startsWith('{')) {
                needsConvert = true;
                legacyKeys << line.section(QRegularExpression(R"(\s+)"), 0, 0);
            }
            // JSONL lines are already v2 — we'll append to them as-is.
        }
    }

    if (needsConvert) {
        // Rewrite whole file: existing JSONL lines (if any) preserved +
        // legacy keys upgraded with a migration marker.
        QStringList rebuilt;
        QFile rf(path);
        if (rf.open(QIODevice::ReadOnly)) {
            const QStringList lines = QString::fromUtf8(rf.readAll()).split('\n', Qt::SkipEmptyParts);
            rf.close();
            for (const QString &raw : lines) {
                const QString line = raw.trimmed();
                if (line.isEmpty()) continue;
                if (line.startsWith('#') || line.startsWith('{')) {
                    rebuilt << line;
                }
            }
        }
        const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
        for (const QString &k : legacyKeys) {
            QJsonObject o;
            o["key"]       = k;
            o["rule"]      = "unknown";
            o["reason"]    = "migrated from v1 plain-text format";
            o["timestamp"] = now;
            rebuilt << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
        }
        rebuilt << QString::fromUtf8(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            f.write(rebuilt.join('\n').toUtf8());
            f.write("\n");
            f.close();
        }
    } else {
        // Simple append.
        if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            if (f.size() == 0) {
                f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                f.write("# ants-audit suppressions (JSONL). Entries hide the matching "
                        "finding by dedup key.\n");
            }
            f.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
            f.write("\n");
            f.close();
        }
    }

    m_suppressedKeys.insert(dedupKey);
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

    if (scheme != "ants-suppress") return;
    const Finding f = m_findingsByKey.value(key);
    const QString where = (!f.file.isEmpty() && f.line > 0)
        ? QString("%1:%2").arg(f.file).arg(f.line)
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

    saveSuppression(key, f.checkId.isEmpty() ? QStringLiteral("unknown") : f.checkId, reason);

    // Re-render results minus the suppressed finding.
    if (!m_completedResults.isEmpty()) renderResults();
    if (m_statusLabel)
        m_statusLabel->setText(QString("Suppressed %1 (%2)").arg(key.left(8), f.checkId));
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
    QFile f(trendPath());
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
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
    QDir().mkpath(m_projectPath + "/.audit_cache");
    QJsonArray arr;
    QFile f(trendPath());
    if (f.open(QIODevice::ReadOnly)) {
        arr = QJsonDocument::fromJson(f.readAll()).array();
        f.close();
    }
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
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
        f.close();
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
    QFile f(baselinePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return;
    const QJsonArray arr = doc.object().value("fingerprints").toArray();
    for (const QJsonValue &v : arr) m_baselineFingerprints.insert(v.toString());
    m_hasBaseline = !m_baselineFingerprints.isEmpty();
}

void AuditDialog::saveBaseline() {
    QDir().mkpath(m_projectPath + "/.audit_cache");
    QJsonArray arr;
    for (const CheckResult &r : m_completedResults) {
        // Per-finding fingerprints — stable across unrelated code changes,
        // because dedupKey is (file:line:checkId:title-hash).
        for (const Finding &f : r.findings)
            arr.append(f.dedupKey);
    }
    QJsonObject root;
    root["generated"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["version"] = 2;               // v2 = per-finding dedupKey
    root["fingerprints"] = arr;
    QFile f(baselinePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
        loadBaseline();
        if (m_newOnlyBtn) m_newOnlyBtn->setEnabled(true);
        m_statusLabel->setText(QString("Baseline saved — %1 fingerprints").arg(arr.size()));
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void AuditDialog::buildUI() {
    auto *root = new QVBoxLayout(this);
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
    for (const auto &c : m_checks)
        if (!categories.contains(c.category)) categories << c.category;

    for (const QString &cat : categories) {
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
    connect(m_newOnlyBtn, &QPushButton::toggled, this, [this](bool on) {
        m_showNewOnly = on;
        if (!m_completedResults.isEmpty()) renderResults();
    });
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

    m_runBtn = new QPushButton("Run Audit", this);
    m_runBtn->setFixedHeight(32);
    m_runBtn->setMinimumWidth(120);
    connect(m_runBtn, &QPushButton::clicked, this, &AuditDialog::runAudit);
    btnRow->addWidget(m_runBtn);

    // SARIF export (industry-standard JSON, consumed by GitHub Code Scanning,
    // VSCode SARIF Viewer, SonarQube, CodeQL, etc.). Appears after a run.
    m_sarifBtn = new QPushButton("Export SARIF", this);
    m_sarifBtn->setFixedHeight(32);
    m_sarifBtn->setVisible(false);
    m_sarifBtn->setToolTip("Save findings as SARIF v2.1.0 JSON for CI / IDE viewers");
    connect(m_sarifBtn, &QPushButton::clicked, this, [this]() {
        if (m_completedResults.isEmpty()) return;
        QDir().mkpath(m_projectPath + "/.audit_cache");
        const QString path = m_projectPath + "/.audit_cache/audit-"
                           + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")
                           + ".sarif";
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(exportSarif().toUtf8());
            f.close();
            m_statusLabel->setText("SARIF saved: " + path);
        }
    });
    btnRow->addWidget(m_sarifBtn);

    m_htmlBtn = new QPushButton("Export HTML", this);
    m_htmlBtn->setFixedHeight(32);
    m_htmlBtn->setVisible(false);
    m_htmlBtn->setToolTip("Save a single-file HTML report (browser-viewable, no external assets)");
    connect(m_htmlBtn, &QPushButton::clicked, this, [this]() {
        if (m_completedResults.isEmpty()) return;
        QDir().mkpath(m_projectPath + "/.audit_cache");
        const QString path = m_projectPath + "/.audit_cache/audit-"
                           + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")
                           + ".html";
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(exportHtml().toUtf8());
            f.close();
            m_statusLabel->setText("HTML report saved: " + path);
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
            delete tmp;
            emit reviewRequested(path);
            close();
        } else {
            delete tmp;
        }
    });
    btnRow->addWidget(m_reviewBtn);

    root->addLayout(btnRow);

    // Progress + results
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    m_statusLabel = new QLabel(this);
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

    struct SevSpec { const char *label; Severity sev; const char *color; };
    static const SevSpec kSevs[] = {
        {"BLK", Severity::Blocker,  "#8B0000"},
        {"CRT", Severity::Critical, "#E74856"},
        {"MAJ", Severity::Major,    "#FFA500"},
        {"MIN", Severity::Minor,    "#FFD700"},
        {"INF", Severity::Info,     "#4CAF50"},
    };
    for (const auto &sp : kSevs) {
        auto *pill = new QPushButton(sp.label, m_filterBar);
        pill->setCheckable(true);
        pill->setChecked(true);
        pill->setFixedSize(44, 26);
        pill->setToolTip(QString("Show %1 findings").arg(sp.label));
        pill->setStyleSheet(QString(
            "QPushButton { border:1px solid %1; color:%1; background:transparent; "
            "  border-radius:12px; font-size:10px; font-weight:bold; }"
            "QPushButton:checked { background:%1; color:white; }"
            "QPushButton:!checked { color:#555; border-color:#555; }"
        ).arg(sp.color));
        const int sevIndex = static_cast<int>(sp.sev);
        connect(pill, &QPushButton::toggled, this, [this, sevIndex](bool on) {
            if (on) m_activeSeverities.insert(sevIndex);
            else    m_activeSeverities.remove(sevIndex);
            if (!m_completedResults.isEmpty()) renderResults();
        });
        filterRow->addWidget(pill);
        m_sevPills.append(pill);
    }

    m_confidenceSortBtn = new QPushButton("Sort by confidence", m_filterBar);
    m_confidenceSortBtn->setCheckable(true);
    m_confidenceSortBtn->setFixedHeight(26);
    m_confidenceSortBtn->setToolTip("Sort findings within each check by confidence score (highest first)");
    connect(m_confidenceSortBtn, &QPushButton::toggled, this, [this](bool on) {
        m_sortByConfidence = on;
        if (!m_completedResults.isEmpty()) renderResults();
    });
    filterRow->addWidget(m_confidenceSortBtn);

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

void AuditDialog::runAudit() {
    m_results->clear();
    m_results->setVisible(true);
    m_progress->setVisible(true);
    m_statusLabel->setVisible(true);
    m_completedResults.clear();

    // Compute the "recent files" list if the user opted into scoped audit.
    m_recentFiles.clear();
    m_recentLines.clear();
    if (m_recentOnly && m_detectedTypes.contains("Git")) {
        QProcess git;
        git.setWorkingDirectory(m_projectPath);
        git.start("git", {"log", QString("-n%1").arg(m_recentCommits),
                          "--name-only", "--format=", "--diff-filter=ACMR"});
        if (git.waitForFinished(5000) && git.exitCode() == 0) {
            const QStringList lines =
                QString::fromUtf8(git.readAllStandardOutput())
                    .split('\n', Qt::SkipEmptyParts);
            QSet<QString> seen;
            for (const QString &raw : lines) {
                const QString p = raw.trimmed();
                if (p.isEmpty() || seen.contains(p)) continue;
                seen.insert(p);
                // Keep only files that still exist on disk.
                if (QFile::exists(m_projectPath + "/" + p))
                    m_recentFiles << p;
            }
        }

        // Line-level scoping: ask git for the diff with zero context, parse
        // the hunk headers (`@@ -old,count +new,count @@`) and record the
        // destination line range for each file. Uses HEAD~N..HEAD so new
        // commits + uncommitted working-tree changes both land in the map.
        if (m_recentLinesOnly) {
            QProcess gitDiff;
            gitDiff.setWorkingDirectory(m_projectPath);
            gitDiff.start("git", {"diff", "--unified=0",
                                  QString("HEAD~%1").arg(m_recentCommits)});
            if (gitDiff.waitForFinished(8000) && gitDiff.exitCode() == 0) {
                const QStringList dlines =
                    QString::fromUtf8(gitDiff.readAllStandardOutput())
                        .split('\n', Qt::KeepEmptyParts);
                static const QRegularExpression reFileHdr(R"(^\+\+\+ b/(.+)$)");
                static const QRegularExpression reHunk(
                    R"(^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@)");
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
                        auto &set = m_recentLines[curFile];
                        for (int i = 0; i < count; ++i) set.insert(start + i);
                    }
                }
            }
        }
    }

    m_totalSelected = 0;
    for (const auto &c : m_checks)
        if (c.toggle && c.toggle->isChecked()) ++m_totalSelected;

    if (m_totalSelected == 0) {
        m_statusLabel->setText("No checks selected.");
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

void AuditDialog::runNextCheck() {
    while (++m_currentCheck < m_checks.size()) {
        if (m_checks[m_currentCheck].toggle &&
            m_checks[m_currentCheck].toggle->isChecked())
            break;
    }

    if (m_currentCheck >= m_checks.size()) {
        m_progress->setValue(m_totalSelected);
        renderResults();
        m_runBtn->setEnabled(true);
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
    m_statusLabel->setText("Running: " + check.name + "…");
    m_progress->setValue(m_checksRun);

    m_timeout->start(30000);
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

void AuditDialog::onCheckFinished(int /*exitCode*/, QProcess::ExitStatus /*status*/) {
    m_timeout->stop();
    if (m_currentCheck < 0 || m_currentCheck >= m_checks.size()) return;

    const auto &check = m_checks[m_currentCheck];
    QString output = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
    const QString errOutput = QString::fromUtf8(m_process->readAllStandardError()).trimmed();

    if (!errOutput.isEmpty() && output.isEmpty())
        output = errOutput;
    else if (!errOutput.isEmpty())
        output += "\n" + errOutput;

    // Apply declarative post-filter (noise excludes, head caps, etc).
    const FilterResult filtered = applyFilter(output, check.filter);

    CheckResult r;
    r.checkId   = check.id;
    r.checkName = check.name;
    r.category  = check.category;
    r.type      = check.type;
    r.severity  = check.severity;
    r.source    = sourceForCheck(check.id);
    r.output    = filtered.body;
    r.warning   = false;

    // Parse body into structured findings. Apply in order:
    //   1. Suppress any finding whose dedup hash is in .audit_suppress
    //   2. Drop generated / path-rule-skipped findings
    //   3. Drop findings suppressed by inline directive in the file
    //   4. In "recent-only" mode: drop findings whose file isn't in the
    //      recent-commits set (unfiled findings always pass)
    //   5. Dedup within this single check (exact-duplicate message lines)
    QSet<QString> seenKeys;
    QList<Finding> parsed = parseFindings(filtered.body, check);
    QSet<QString> recent;
    if (m_recentOnly) for (const QString &p : m_recentFiles) recent.insert(p);
    for (Finding &f : parsed) {
        if (m_suppressedKeys.contains(f.dedupKey)) continue;
        if (!applyPathRules(f)) continue;       // generated files + path rules
        if (inlineSuppressed(f)) continue;      // inline // ants-audit: disable ...
        if (m_recentOnly && !f.file.isEmpty()) {
            // Match by either exact path or project-relative path suffix.
            bool isRecent = recent.contains(f.file);
            QString matchedFile = f.file;
            if (!isRecent) {
                for (const QString &rf : m_recentFiles) {
                    if (f.file.endsWith(rf) || rf.endsWith(f.file)) {
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

    capFindings(r, kMaxFindingsPerCheck);
    r.findingCount = r.findings.size() + r.omittedCount;
    m_completedResults.append(r);

    ++m_checksRun;
    runNextCheck();
}

// ---------------------------------------------------------------------------
// Result rendering — sort by severity, summary banner, new-since-baseline tag
// ---------------------------------------------------------------------------

static QString severityLabel(Severity s) {
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

static QString typeLabel(CheckType t) {
    switch (t) {
        case CheckType::Info:          return "info";
        case CheckType::CodeSmell:     return "smell";
        case CheckType::Bug:           return "bug";
        case CheckType::Hotspot:       return "hotspot";
        case CheckType::Vulnerability: return "vuln";
    }
    return "?";
}

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
                for (Finding *p : it.value().fs) p->highConfidence = true;
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
            const QString abs = QFileInfo(f.file).isAbsolute()
                              ? f.file
                              : (m_projectPath + "/" + f.file);
            if (f.snippet.isEmpty())
                f.snippet = readSnippet(abs, f.line, 3, &f.snippetStart);
            if (f.blameSha.isEmpty()) enrichWithBlame(f);
            ++enriched;
        }
    }
    for (auto &r : m_completedResults)
        for (Finding &f : r.findings)
            f.confidence = computeConfidence(f);

    // Populate key→finding lookup for the anchor click handler. Done after
    // correlation so the lookup reflects the highConfidence flag.
    for (const auto &r : m_completedResults)
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
            if (m_suppressedKeys.contains(f.dedupKey)) continue;
            if (m_showNewOnly && !findingIsNew(f)) continue;
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
          + trendLine);
    m_results->append(banner);

    // Persist the snapshot for next run's trend line. Skip if we're in
    // showNewOnly mode — that's a filtered view, not an authoritative count.
    if (!m_showNewOnly) appendSnapshot(curr);

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
              r.warning ? " (timeout)" : "",
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
        for (const Finding *pf : sortedFindings) {
            const Finding &f = *pf;
            if (m_suppressedKeys.contains(f.dedupKey)) continue;
            if (m_showNewOnly && !findingIsNew(f)) continue;
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
                QStringList parts;
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
                verdictBadge = QString(
                    " <span style='color:%1; font-size:9px; font-weight:bold;' "
                    "title='AI triage: %2/100 — %3'>%4</span>")
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
                            .arg(bg)
                            .arg(QString::number(ln).rightJustified(5))
                            .arg(marker, lines[i].toHtmlEscaped());
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
                body += "<div style='margin:2px 0 6px 24px; font-size:10px;'>"
                      + triageLink + "</div>";
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

    m_statusLabel->setText(QString("Audit complete — %1 findings across %2 checks%3")
                           .arg(totalFindings).arg(sorted.size())
                           .arg(m_hasBaseline
                                ? QString(" (%1 new since baseline)").arg(totalNew)
                                : QString()));
}

// ---------------------------------------------------------------------------
// AI triage — per-finding classification via OpenAI-compatible chat endpoint
// ---------------------------------------------------------------------------
//
// Shape: user clicks "🧠 Triage with AI" inside an expanded finding row.
// We POST a small prompt (rule + snippet + blame) to the project's configured
// /v1/chat/completions endpoint (same one powering AiDialog), expecting a
// JSON object {verdict, confidence, reasoning}. The response updates the
// Finding in place and re-renders so the verdict badge appears.
//
// Deliberately simple: one QNetworkAccessManager per call (cheap, short-
// lived), no streaming, 30s timeout matching STANDARDS.md. Failures surface
// in the status bar; Finding stays untouched so the user can retry.

void AuditDialog::requestAiTriage(const QString &dedupKey) {
    if (dedupKey.isEmpty()) return;
    Finding f = m_findingsByKey.value(dedupKey);
    if (f.checkId.isEmpty()) return;

    // Prefer a previously-computed snippet; fall back to a fresh read.
    if (f.snippet.isEmpty() && !f.file.isEmpty() && f.line > 0) {
        const QString abs = QFileInfo(f.file).isAbsolute()
                          ? f.file
                          : (m_projectPath + "/" + f.file);
        f.snippet = readSnippet(abs, f.line, 5, &f.snippetStart);
    }

    Config cfg;
    const QString endpoint = cfg.aiEndpoint();
    const QString apiKey   = cfg.aiApiKey();
    const QString model    = cfg.aiModel().isEmpty() ? QStringLiteral("gpt-4o-mini")
                                                     : cfg.aiModel();
    if (!cfg.aiEnabled() || endpoint.isEmpty()) {
        if (m_statusLabel)
            m_statusLabel->setText("AI triage: configure AI in Settings first");
        return;
    }

    // Compose prompt. Keep it concise — the model doesn't need our whole
    // rule catalogue, just the rule name + description + snippet.
    const QString sys =
        "You are a static analysis triage assistant. Classify a finding "
        "as TRUE_POSITIVE, FALSE_POSITIVE, or NEEDS_REVIEW. Respond ONLY "
        "with a compact JSON object of shape "
        "{\"verdict\":\"...\",\"confidence\":<0-100>,\"reasoning\":\"...\"}. "
        "Keep reasoning under 50 words.";
    QString userMsg;
    userMsg += "Rule: " + f.checkId + " — " + f.checkName + "\n";
    userMsg += "Severity: " + severityLabel(f.severity) + "\n";
    userMsg += "Source: " + f.source + "\n";
    if (!f.file.isEmpty())
        userMsg += QString("File: %1:%2\n").arg(f.file).arg(f.line);
    userMsg += "Message: " + f.message + "\n";
    if (!f.snippet.isEmpty()) {
        userMsg += "\nSnippet:\n```\n";
        userMsg += f.snippet;
        userMsg += "\n```\n";
    }
    if (!f.blameAuthor.isEmpty())
        userMsg += QString("\nLast modified: %1 by %2 (%3)\n")
                       .arg(f.blameDate, f.blameAuthor, f.blameSha);

    QJsonArray messages;
    messages.append(QJsonObject{{"role", "system"},  {"content", sys}});
    messages.append(QJsonObject{{"role", "user"},    {"content", userMsg}});

    QJsonObject body;
    body["model"]       = model;
    body["messages"]    = messages;
    body["temperature"] = 0.2;
    body["stream"]      = false;
    // Response-format JSON for providers that honor it; harmless otherwise.
    body["response_format"] = QJsonObject{{"type", "json_object"}};

    QUrl endpointUrl(endpoint);
    // Append /v1/chat/completions if the user gave a bare host.
    if (!endpointUrl.path().contains("chat/completions")) {
        const QString p = endpointUrl.path();
        endpointUrl.setPath((p.endsWith('/') ? p : p + "/") + "v1/chat/completions");
    }
    if (endpointUrl.scheme() != "https" && endpointUrl.scheme() != "http") {
        if (m_statusLabel) m_statusLabel->setText("AI triage: endpoint must be http(s)");
        return;
    }

    QNetworkRequest req(endpointUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!apiKey.isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
    req.setTransferTimeout(30000);

    auto *mgr = new QNetworkAccessManager(this);
    QNetworkReply *reply = mgr->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (m_statusLabel)
        m_statusLabel->setText("AI triage: querying " + endpointUrl.host() + "…");

    connect(reply, &QNetworkReply::finished, this, [this, reply, mgr, dedupKey]() {
        reply->deleteLater();
        mgr->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (m_statusLabel)
                m_statusLabel->setText("AI triage failed: " + reply->errorString());
            return;
        }
        const QByteArray data = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            if (m_statusLabel) m_statusLabel->setText("AI triage: invalid response");
            return;
        }
        // OpenAI shape: choices[0].message.content holds the assistant reply.
        const QJsonArray choices = doc.object().value("choices").toArray();
        if (choices.isEmpty()) {
            if (m_statusLabel) m_statusLabel->setText("AI triage: empty response");
            return;
        }
        QString content = choices.first().toObject()
                              .value("message").toObject()
                              .value("content").toString();
        // Some providers wrap the JSON in ```...```. Strip.
        content = content.trimmed();
        if (content.startsWith("```")) {
            const int nl = content.indexOf('\n');
            if (nl > 0) content = content.mid(nl + 1);
            if (content.endsWith("```"))
                content.chop(3);
            content = content.trimmed();
        }
        const QJsonDocument inner = QJsonDocument::fromJson(content.toUtf8());
        if (!inner.isObject()) {
            if (m_statusLabel) m_statusLabel->setText("AI triage: non-JSON verdict");
            return;
        }
        const QJsonObject o = inner.object();
        const QString verdict = o.value("verdict").toString("NEEDS_REVIEW").toUpper();
        const int conf = std::clamp(o.value("confidence").toInt(50), 0, 100);
        const QString reasoning = o.value("reasoning").toString().left(600);

        // Write back into every CheckResult that carries this key. Renamed
        // from `f` to `fi` so -Wshadow=local stays clean — an outer `f` at
        // line ~2938 is uncaptured here, but GCC warns anyway.
        for (auto &r : m_completedResults) {
            for (Finding &fi : r.findings) {
                if (fi.dedupKey != dedupKey) continue;
                fi.aiVerdict     = verdict;
                fi.aiConfidence  = conf;
                fi.aiReasoning   = reasoning;
                fi.confidence    = computeConfidence(fi);
            }
        }
        m_expandedKeys.insert(dedupKey);   // auto-expand to show the verdict
        renderResults();
        if (m_statusLabel)
            m_statusLabel->setText(QString("AI triage: %1 (%2/100)").arg(verdict).arg(conf));
    });
}

// ---------------------------------------------------------------------------
// Plain-text export for the Claude Review handoff
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SARIF v2.1.0 export (OASIS standard, consumed by GitHub code-scanning,
// VSCode SARIF viewer, SonarQube, etc.)
// ---------------------------------------------------------------------------
//
// Minimal compliant document: one run, one tool (ants-audit), a catalogue
// of rules we emitted, and one result per finding. SARIF level mapping:
//   Blocker / Critical → "error"
//   Major / Minor      → "warning"
//   Info               → "note"

QString AuditDialog::exportSarif() const {
    auto sarifLevel = [](Severity s) -> QString {
        switch (s) {
            case Severity::Blocker:
            case Severity::Critical: return "error";
            case Severity::Major:
            case Severity::Minor:    return "warning";
            case Severity::Info:     return "note";
        }
        return "none";
    };

    // Build the rule catalogue deduplicated by check id.
    QMap<QString, AuditCheck> ruleById;
    for (const auto &c : m_checks) ruleById.insert(c.id, c);

    QJsonArray rules;
    for (auto it = ruleById.constBegin(); it != ruleById.constEnd(); ++it) {
        const auto &c = it.value();
        QJsonObject r;
        r["id"] = c.id;
        r["name"] = c.name;
        QJsonObject shortDesc;  shortDesc["text"] = c.name;
        QJsonObject fullDesc;   fullDesc["text"]  = c.description;
        r["shortDescription"] = shortDesc;
        r["fullDescription"]  = fullDesc;
        QJsonObject defCfg; defCfg["level"] = sarifLevel(c.severity);
        r["defaultConfiguration"] = defCfg;
        r["properties"] = QJsonObject{
            {"category",  c.category},
            {"type",      typeLabel(c.type)},
            {"severity",  severityLabel(c.severity)},
        };
        rules.append(r);
    }

    QJsonArray results;
    for (const CheckResult &cr : m_completedResults) {
        if (cr.warning || cr.findings.isEmpty()) continue;
        for (const Finding &f : cr.findings) {
            QJsonObject res;
            res["ruleId"]   = f.checkId;
            res["level"]    = sarifLevel(f.severity);
            QJsonObject msg;
            msg["text"]     = f.message;
            res["message"]  = msg;

            if (!f.file.isEmpty()) {
                QJsonObject loc, physLoc, artLoc, region;
                artLoc["uri"] = f.file;
                physLoc["artifactLocation"] = artLoc;
                if (f.line > 0) {
                    region["startLine"] = f.line;
                    physLoc["region"]   = region;
                }
                // SARIF contextRegion — the ±3 lines around the finding.
                // Consumers (GitHub Code Scanning, SonarQube, VSCode viewer)
                // render this as the inline code preview.
                if (!f.snippet.isEmpty() && f.snippetStart > 0) {
                    QJsonObject ctxRegion;
                    ctxRegion["startLine"] = f.snippetStart;
                    ctxRegion["endLine"]   = f.snippetStart
                                           + f.snippet.count('\n');
                    ctxRegion["snippet"]   = QJsonObject{{"text", f.snippet}};
                    physLoc["contextRegion"] = ctxRegion;
                }
                loc["physicalLocation"] = physLoc;
                QJsonArray locs; locs.append(loc);
                res["locations"] = locs;
            }
            QJsonObject partialFp;
            partialFp["primaryLocationLineHash"] = f.dedupKey;
            res["partialFingerprints"] = partialFp;

            QJsonObject props{
                {"source", f.source},
                {"highConfidence", f.highConfidence},
                {"confidence", f.confidence},
            };
            // Git-blame bag — sarif-tools de-facto convention, not in the
            // formal schema but consumed by GitHub Code Scanning UI.
            if (!f.blameSha.isEmpty()) {
                QJsonObject blame{
                    {"author",      f.blameAuthor},
                    {"author-time", f.blameDate},
                    {"sha",         f.blameSha},
                };
                props["blame"] = blame;
            }
            // AI triage verdict (present only if user ran it).
            if (!f.aiVerdict.isEmpty()) {
                props["aiTriage"] = QJsonObject{
                    {"verdict",    f.aiVerdict},
                    {"confidence", f.aiConfidence},
                    {"reasoning",  f.aiReasoning},
                };
            }
            res["properties"] = props;
            results.append(res);
        }
    }

    QJsonObject driver;
    driver["name"] = "ants-audit";
    driver["version"] = QStringLiteral(ANTS_VERSION);
    driver["informationUri"] = "https://github.com/milnet01/ants-terminal";
    driver["rules"] = rules;

    QJsonObject tool;
    tool["driver"] = driver;

    QJsonObject run;
    run["tool"] = tool;
    run["results"] = results;
    QJsonObject invocation;
    invocation["workingDirectory"] = QJsonObject{{"uri", m_projectPath}};
    invocation["startTimeUtc"]     = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonArray invocations; invocations.append(invocation);
    run["invocations"] = invocations;

    QJsonArray runs; runs.append(run);

    QJsonObject root;
    root["$schema"] = "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/main/"
                      "Documents/CommitteeSpecifications/2.1.0/sarif-schema-2.1.0.json";
    root["version"] = "2.1.0";
    root["runs"] = runs;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// ---------------------------------------------------------------------------
// Single-file HTML report export
// ---------------------------------------------------------------------------
//
// Embeds findings as an inline JSON payload + ~120 lines of vanilla JS/CSS
// that render severity pills, a text filter, and per-check collapsible
// cards. No external assets — the file opens standalone in any browser.
QString AuditDialog::exportHtml() const {
    // Build a compact JSON payload that the page renders client-side.
    QJsonArray findingsJson;
    for (const CheckResult &cr : m_completedResults) {
        if (cr.warning) continue;
        for (const Finding &f : cr.findings) {
            if (m_suppressedKeys.contains(f.dedupKey)) continue;
            QJsonObject o;
            o["checkId"]   = f.checkId;
            o["checkName"] = f.checkName;
            o["category"]  = f.category;
            o["type"]      = typeLabel(f.type);
            o["severity"]  = severityLabel(f.severity);
            o["source"]    = f.source;
            o["file"]      = f.file;
            o["line"]      = f.line;
            o["message"]   = f.message;
            o["dedupKey"]  = f.dedupKey;
            o["highConf"]  = f.highConfidence;
            o["confidence"] = f.confidence;
            o["snippet"]   = f.snippet;
            o["snippetStart"] = f.snippetStart;
            if (!f.blameSha.isEmpty()) {
                o["blame"] = QJsonObject{
                    {"author", f.blameAuthor},
                    {"date",   f.blameDate},
                    {"sha",    f.blameSha},
                };
            }
            if (!f.aiVerdict.isEmpty()) {
                o["ai"] = QJsonObject{
                    {"verdict",    f.aiVerdict},
                    {"confidence", f.aiConfidence},
                    {"reasoning",  f.aiReasoning},
                };
            }
            findingsJson.append(o);
        }
    }

    QJsonObject meta;
    meta["project"]    = m_projectPath;
    meta["detected"]   = m_detectedTypes.join(", ");
    meta["generated"]  = QDateTime::currentDateTime().toString(Qt::ISODate);
    meta["version"]    = QString::fromLatin1(ANTS_VERSION);
    meta["findings"]   = findingsJson;

    QString payload = QString::fromUtf8(
        QJsonDocument(meta).toJson(QJsonDocument::Compact));
    // Defuse any </script> lookalikes inside finding messages. JSON parsers
    // treat "<\/" as equivalent to "</", so this is always safe.
    payload.replace("</", "<\\/");

    // clang-format off
    // Template is a raw string — escape the literal `)"` delimiter only.
    static const char *kTemplate = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Ants audit — {{PROJECT}}</title>
<style>
  :root {
    --bg:       #1e1e1e;
    --surface:  #2a2a2a;
    --border:   #3a3a3a;
    --text:     #e0e0e0;
    --muted:    #8a8a8a;
    --accent:   #89b4fa;
    --blocker:  #8b0000;
    --critical: #e74856;
    --major:    #ffa500;
    --minor:    #ffd700;
    --info:     #4caf50;
  }
  * { box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
    background: var(--bg); color: var(--text); margin: 0; padding: 24px;
    line-height: 1.5;
  }
  header { margin-bottom: 16px; }
  h1 { margin: 0 0 4px; font-size: 20px; }
  .meta { color: var(--muted); font-size: 12px; }
  .controls {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 6px; padding: 12px; margin-bottom: 16px;
    display: flex; gap: 12px; flex-wrap: wrap; align-items: center;
  }
  .pill {
    padding: 4px 10px; border-radius: 12px; font-size: 11px; cursor: pointer;
    border: 1px solid var(--border); user-select: none;
    background: var(--bg); color: var(--text);
  }
  .pill[data-active="false"] { opacity: 0.4; }
  .pill.BLOCKER  { color: var(--blocker);  border-color: var(--blocker); }
  .pill.CRITICAL { color: var(--critical); border-color: var(--critical); }
  .pill.MAJOR    { color: var(--major);    border-color: var(--major); }
  .pill.MINOR    { color: var(--minor);    border-color: var(--minor); }
  .pill.INFO     { color: var(--info);     border-color: var(--info); }
  #q {
    background: var(--bg); color: var(--text); border: 1px solid var(--border);
    border-radius: 4px; padding: 6px 10px; min-width: 280px; font-size: 13px;
  }
  .check {
    background: var(--surface); border: 1px solid var(--border);
    border-radius: 6px; margin-bottom: 10px; overflow: hidden;
  }
  .check summary {
    cursor: pointer; padding: 10px 14px; font-weight: 500;
    display: flex; align-items: center; gap: 10px;
  }
  .check summary::-webkit-details-marker { display: none; }
  .tag {
    font-size: 10px; padding: 2px 7px; border-radius: 3px;
    text-transform: uppercase; font-weight: 600; letter-spacing: 0.3px;
  }
  .tag.BLOCKER  { background: var(--blocker);  color: white; }
  .tag.CRITICAL { background: var(--critical); color: white; }
  .tag.MAJOR    { background: var(--major);    color: black; }
  .tag.MINOR    { background: var(--minor);    color: black; }
  .tag.INFO     { background: var(--info);     color: black; }
  .rows {
    padding: 0 14px 10px; font-family: 'SF Mono', Menlo, monospace;
    font-size: 12px;
  }
  .row {
    padding: 6px 0; border-top: 1px solid var(--border);
    white-space: pre-wrap; word-break: break-word;
  }
  .row:first-child { border-top: none; }
  .loc { color: var(--accent); }
  .key { color: var(--muted); font-size: 10px; margin-left: 8px; }
  .blame { color: var(--muted); font-size: 10px; margin-left: 6px; }
  .conf-pip { font-weight: bold; margin-right: 4px; }
  .conf-hi  { color: #4caf50; }
  .conf-md  { color: #ffa500; }
  .conf-lo  { color: #e74856; }
  .verdict { font-size: 10px; margin-left: 6px; font-weight: 600; }
  .verdict.TRUE_POSITIVE  { color: var(--critical); }
  .verdict.FALSE_POSITIVE { color: var(--info); }
  .verdict.NEEDS_REVIEW   { color: var(--major); }
  .snippet {
    margin: 6px 0 0 24px; padding: 6px 10px; background: var(--bg);
    border-left: 2px solid var(--border); border-radius: 3px;
    font-size: 11px; line-height: 1.35; overflow-x: auto;
  }
  .snippet .ln    { color: var(--muted); user-select: none; margin-right: 6px; }
  .snippet .hit   { background: rgba(231,72,86,0.15); }
  .snippet .hit .ln { color: var(--critical); }
  details.fx { margin-left: 24px; margin-top: 4px; }
  details.fx summary { cursor: pointer; color: var(--accent); font-size: 10px; }
  details.fx summary::-webkit-details-marker { display: none; }
  .reason { color: var(--muted); font-size: 10px; margin-top: 4px; }
  .empty { padding: 20px; color: var(--muted); font-style: italic; text-align: center; }
  footer { margin-top: 20px; color: var(--muted); font-size: 11px; text-align: center; }
</style>
</head>
<body>
<header>
  <h1>Ants audit report</h1>
  <div class="meta" id="meta"></div>
</header>
<div class="controls">
  <input type="search" id="q" placeholder="Filter by file, message, rule…">
  <span class="pill BLOCKER"  data-sev="BLOCKER"  data-active="true">Blocker</span>
  <span class="pill CRITICAL" data-sev="CRITICAL" data-active="true">Critical</span>
  <span class="pill MAJOR"    data-sev="MAJOR"    data-active="true">Major</span>
  <span class="pill MINOR"    data-sev="MINOR"    data-active="true">Minor</span>
  <span class="pill INFO"     data-sev="INFO"     data-active="true">Info</span>
</div>
<div id="results"></div>
<footer>Generated by ants-audit · single-file report · no external assets</footer>

<script id="data" type="application/json">{{PAYLOAD}}</script>
<script>
(function () {
  const DATA = JSON.parse(document.getElementById('data').textContent);
  const SEV_ORDER = { BLOCKER: 4, CRITICAL: 3, MAJOR: 2, MINOR: 1, INFO: 0 };
  const meta = document.getElementById('meta');
  meta.textContent = `Project: ${DATA.project} · Detected: ${DATA.detected} · ants-audit v${DATA.version || '?'} · ${DATA.generated}`;

  // Group by check.
  const byCheck = new Map();
  for (const f of DATA.findings) {
    if (!byCheck.has(f.checkId)) byCheck.set(f.checkId, { meta: f, findings: [] });
    byCheck.get(f.checkId).findings.push(f);
  }

  // Sort checks by highest-severity finding.
  const checks = Array.from(byCheck.values()).sort((a, b) =>
    SEV_ORDER[b.meta.severity] - SEV_ORDER[a.meta.severity]);

  const results = document.getElementById('results');
  const activeSevs = new Set(['BLOCKER', 'CRITICAL', 'MAJOR', 'MINOR', 'INFO']);
  let q = '';

  function escape(s) {
    return String(s).replace(/[&<>"']/g, c => ({
      '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
    })[c]);
  }

  function render() {
    results.innerHTML = '';
    let shown = 0;
    for (const c of checks) {
      const filtered = c.findings.filter(f => {
        if (!activeSevs.has(f.severity)) return false;
        if (!q) return true;
        const hay = (f.file + ' ' + f.message + ' ' + f.checkId).toLowerCase();
        return hay.includes(q);
      });
      if (filtered.length === 0) continue;
      shown += filtered.length;
      const rows = filtered.map(f => {
        const loc = f.file ? `<span class="loc">${escape(f.file)}${f.line > 0 ? ':' + f.line : ''}</span>  ` : '';
        const star = f.highConf ? ' <span title="Flagged by 2+ tools" style="color:#FFD700;">★</span>' : '';
        const confCls = (f.confidence >= 70) ? 'conf-hi'
                      : (f.confidence >= 40) ? 'conf-md' : 'conf-lo';
        const pip = (typeof f.confidence === 'number')
          ? `<span class="conf-pip ${confCls}" title="Confidence ${f.confidence}/100">●</span>`
          : '';
        const blame = f.blame
          ? `<span class="blame" title="git blame">[${escape(f.blame.author)} · ${escape(f.blame.date)} · ${escape(f.blame.sha)}]</span>`
          : '';
        const verdict = f.ai
          ? `<span class="verdict ${f.ai.verdict}" title="AI: ${f.ai.confidence}/100">${f.ai.verdict.replace('_',' ')}</span>`
          : '';
        let snippet = '';
        if (f.snippet) {
          const lines = f.snippet.split('\n').map((ln, idx) => {
            const num = (f.snippetStart || 0) + idx;
            const hit = (num === f.line);
            return `<div class="${hit ? 'hit' : ''}"><span class="ln">${String(num).padStart(5)}</span>${escape(ln)}</div>`;
          }).join('');
          snippet = `<details class="fx"><summary>[details]</summary>
            <div class="snippet">${lines}</div>
            ${f.ai ? `<div class="reason">${escape(f.ai.reasoning || '')}</div>` : ''}
          </details>`;
        }
        return `<div class="row">${pip}${loc}${escape(f.message)}${star}${blame}${verdict}<span class="key">${escape(f.dedupKey)}</span>${snippet}</div>`;
      }).join('');
      const det = document.createElement('details');
      det.className = 'check';
      det.open = SEV_ORDER[c.meta.severity] >= 2;  // open major+
      det.innerHTML = `
        <summary>
          <span class="tag ${c.meta.severity}">${c.meta.severity}</span>
          <strong>${escape(c.meta.checkName)}</strong>
          <span class="key">· ${escape(c.meta.category)} · ${escape(c.meta.source)} · ${filtered.length} finding${filtered.length === 1 ? '' : 's'}</span>
        </summary>
        <div class="rows">${rows}</div>`;
      results.appendChild(det);
    }
    if (shown === 0) {
      results.innerHTML = '<div class="empty">No findings match current filters.</div>';
    }
  }

  document.querySelectorAll('.pill').forEach(p => {
    p.addEventListener('click', () => {
      const sev = p.dataset.sev;
      if (activeSevs.has(sev)) { activeSevs.delete(sev); p.dataset.active = 'false'; }
      else                     { activeSevs.add(sev);    p.dataset.active = 'true'; }
      render();
    });
  });
  document.getElementById('q').addEventListener('input', e => {
    q = e.target.value.trim().toLowerCase(); render();
  });
  render();
})();
</script>
</body>
</html>
)HTML";
    // clang-format on

    QString html = QString::fromUtf8(kTemplate);
    html.replace("{{PROJECT}}", QFileInfo(m_projectPath).fileName().toHtmlEscaped());
    html.replace("{{PAYLOAD}}", payload);
    return html;
}

QString AuditDialog::plainTextResults() const {
    if (m_completedResults.isEmpty()) return {};

    QString header = "Project Audit Results\n"
                     "Generator: ants-audit v" + QString::fromLatin1(ANTS_VERSION) + "\n"
                     "Project: " + m_projectPath + "\n"
                     "Detected: " + m_detectedTypes.join(", ") + "\n";
    if (m_hasBaseline) header += "Baseline: loaded\n";
    if (!m_suppressedKeys.isEmpty())
        header += QString("Suppressions: %1 loaded from .audit_suppress\n")
                    .arg(m_suppressedKeys.size());
    if (m_recentOnly)
        header += QString("Scope: files touched in last %1 commits (%2 file(s))\n")
                    .arg(m_recentCommits).arg(m_recentFiles.size());
    header +=
        "Legend: `conf N` = 0-100 confidence score (higher = more tools\n"
        "        corroborate). `★` = flagged by ≥2 distinct tools on the\n"
        "        same file:line. `N corroborated` in a category header means\n"
        "        that many of the findings in that check carry the ★.\n";
    header += "---\n\n";

    // Attach the project's own standards docs when present. Gives the
    // Claude-review handoff enough context to weigh findings against
    // documented project rules rather than generic best practice.
    auto appendDocIfPresent = [this, &header](const QString &name, const QString &label) {
        const QString doc = readProjectDoc(name);
        if (doc.isEmpty()) return;
        header += QString("=== %1 (%2) ===\n").arg(label, name);
        header += doc;
        header += "\n\n";
    };
    appendDocIfPresent("CLAUDE.md",      "Project conventions");
    appendDocIfPresent("STANDARDS.md",   "Project coding standards");
    appendDocIfPresent("RULES.md",       "Project rules");
    appendDocIfPresent("CONTRIBUTING.md","Contributing guidelines");

    // 5-phase workflow scaffold for the downstream Claude session. Gives the
    // consumer a verification + approval gate rather than letting it plunge
    // straight into fixes — a common failure mode where regex false positives
    // get "fixed" into real bugs because nothing proved they were real first.
    header +=
        "=== How to process this report ===\n"
        "\n"
        "Follow these five phases in order. Don't jump to fixes.\n"
        "\n"
        "PHASE 1 — BASELINE\n"
        "  Run the existing test suite. Record pass/fail counts, build warnings,\n"
        "  lint output. If anything is broken before you start, surface that\n"
        "  first — don't proceed until the user acknowledges.\n"
        "\n"
        "PHASE 2 — VERIFY\n"
        "  For every BLOCKER, CRITICAL, and MAJOR finding, mark it VERIFIED,\n"
        "  UNCONFIRMED, or FALSE-POSITIVE with a one-line justification citing\n"
        "  file:line evidence. Criteria:\n"
        "    - Bug:       reproduce with a code trace or a failing test.\n"
        "    - Security:  confirm the pattern is exploitable in context\n"
        "                 (attacker-reachable data flow), not just a regex\n"
        "                 match on a keyword.\n"
        "    - Dead code: confirm no callers — including dynamic dispatch,\n"
        "                 registries, reflection, exported API, tests, and\n"
        "                 build scripts.\n"
        "  Confidence scores (0-100) are inline next to each finding: higher\n"
        "  means more tools corroborate it. Prefer triaging high-confidence\n"
        "  findings first.\n"
        "\n"
        "PHASE 3 — CITATIONS\n"
        "  For any dependency / CVE finding you intend to fix, cite an\n"
        "  authoritative URL (NVD, GitHub advisory, upstream changelog) and\n"
        "  cross-check the CVE's affected version range against the pinned\n"
        "  version in the lockfile. A CVE against a version lower than what's\n"
        "  pinned is FALSE-POSITIVE, not a fix target.\n"
        "\n"
        "PHASE 4 — APPROVAL GATE\n"
        "  Produce a findings list with file:line, severity, verification\n"
        "  status, proposed fix, and blast radius (files touched, public-API\n"
        "  impact). Wait for user approval before touching code for any\n"
        "  non-trivial finding. You may proceed directly on:\n"
        "    - unused imports / obviously dead local helpers with no callers\n"
        "    - typo'd conditionals that have a reproducing test\n"
        "    - formatting-only fixes for lint rules already enforced in CI\n"
        "\n"
        "PHASE 5 — IMPLEMENT + TEST\n"
        "  Fix root causes, not symptoms. No --no-verify, no swallowed\n"
        "  exceptions, no commented-out broken code, no capped loops that\n"
        "  hide a real divergence. If a workaround is genuinely unavoidable,\n"
        "  leave a comment documenting the constraint next to it.\n"
        "  Every behavioural fix gets a regression test — a fix without a\n"
        "  test will come back. Keep edits scoped to each finding; no\n"
        "  drive-by refactoring of surrounding code.\n"
        "  After fixes, re-run the full suite and include a pre/post diff of\n"
        "  tests passed/failed/skipped, build warnings, and finding counts\n"
        "  by severity.\n"
        "\n"
        "DELIVERABLE\n"
        "  (1) Findings list with VERIFIED / UNCONFIRMED / FALSE-POSITIVE tags\n"
        "  (2) Changes made — files touched, why, test evidence\n"
        "  (3) Deferred items — why deferred, what would unblock them\n"
        "  (4) Baseline comparison — tests / warnings / findings before vs after\n"
        "\n"
        "Be terse and concrete. Skip categories with no findings rather than\n"
        "writing \"none found\" filler for each.\n\n";

    header += "=== Findings ===\n\n";

    // Re-sort a copy for the plain-text report.
    std::vector<CheckResult> sorted(m_completedResults.begin(), m_completedResults.end());
    std::sort(sorted.begin(), sorted.end(), [](const CheckResult &a, const CheckResult &b) {
        if (a.severity != b.severity) return a.severity > b.severity;
        return a.checkName < b.checkName;
    });

    QString body;
    for (const auto &r : sorted) {
        if (r.warning) {
            body += QString("--- [%1] %2 (%3 / %4) [%5] ---\n(warning) %6\n\n")
                    .arg(severityLabel(r.severity), r.checkName,
                         typeLabel(r.type), r.category, r.source, r.output);
            continue;
        }
        if (r.findings.isEmpty() && r.omittedCount == 0) continue;

        // Count cross-tool-corroborated findings for the category header.
        // A finding is corroborated when ≥2 distinct tools flagged the same
        // file:line — the same signal that drives the ★ badge and boosts
        // computeConfidence(). Surfaced here so the downstream consumer can
        // prioritise triage without having to read every finding tag first.
        int corroborated = 0;
        for (const Finding &f : r.findings)
            if (f.highConfidence) ++corroborated;
        const QString corroTag = corroborated > 0
            ? QString(" (%1 corroborated)").arg(corroborated)
            : QString();

        body += QString("--- [%1] %2 (%3 / %4) [%5] — %6 finding(s)%7 ---\n")
                .arg(severityLabel(r.severity), r.checkName,
                     typeLabel(r.type), r.category, r.source)
                .arg(r.findings.size() + r.omittedCount)
                .arg(corroTag);
        for (const Finding &f : r.findings) {
            QString line;
            if (!f.file.isEmpty() && f.line >= 0)
                line = QString("%1:%2  %3").arg(f.file).arg(f.line).arg(f.message);
            else if (!f.file.isEmpty())
                line = QString("%1  %2").arg(f.file, f.message);
            else
                line = f.message;
            // Inline tags kept compact so Claude's context budget doesn't get
            // eaten by boilerplate — priority order: confidence, high-conf ★,
            // blame, triage verdict.
            QStringList tags{QString("conf %1").arg(f.confidence)};
            if (f.highConfidence) tags << "★";
            if (!f.blameSha.isEmpty())
                tags << QString("%1 @ %2").arg(f.blameAuthor, f.blameDate);
            if (!f.aiVerdict.isEmpty())
                tags << QString("AI: %1 (%2)").arg(f.aiVerdict).arg(f.aiConfidence);
            body += line + "  " + QString("[%1] [%2]").arg(tags.join(" · "), f.dedupKey) + "\n";
            // Include a 3-line snippet when we have one; expensive on token
            // count but essential for Claude to propose targeted fixes.
            if (!f.snippet.isEmpty()) {
                const QStringList lines = f.snippet.split('\n');
                for (int i = 0; i < lines.size(); ++i) {
                    const int ln = f.snippetStart + i;
                    const QChar marker = (ln == f.line) ? QChar('>') : QChar(' ');
                    body += QString("    %1 %2| %3\n")
                                .arg(marker).arg(ln, 5).arg(lines[i]);
                }
            }
        }
        if (r.omittedCount > 0)
            body += QString("… and %1 more (capped at %2 per check)\n")
                    .arg(r.omittedCount).arg(kMaxFindingsPerCheck);
        body += "\n";
    }
    if (body.isEmpty()) body = "No issues found.\n";

    return header + body;
}
