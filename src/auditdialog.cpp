#include "auditdialog.h"
#include "toggleswitch.h"

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

#include <algorithm>

// ---------------------------------------------------------------------------
// Shared constants
// ---------------------------------------------------------------------------
//
// Centralised so individual check commands don't re-list the same excludes.
// Edit once, applies everywhere.
namespace {

// find(1) exclude expression (paths)
const QString kFindExcl =
    " -not -path './.git/*' -not -path './build/*' -not -path './build-*/*'"
    " -not -path './node_modules/*' -not -path './.cache/*'"
    " -not -path './dist/*' -not -path './vendor/*' -not -path './.audit_cache/*'";

// grep(1) exclude-dir list (bare, no file-include filter)
const QString kGrepExcl =
    " --exclude-dir=.git --exclude-dir=build --exclude-dir='build-*'"
    " --exclude-dir=node_modules --exclude-dir=.cache"
    " --exclude-dir=dist --exclude-dir=vendor --exclude-dir=.audit_cache";

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

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AuditDialog::AuditDialog(const QString &projectPath, QWidget *parent)
    : QDialog(parent), m_projectPath(projectPath) {
    setWindowTitle("Project Audit");
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
    loadBaseline();
    populateChecks();
    buildUI();
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

    addGrepCheck("conflict_markers", "Git Conflict Markers",
                 "Unresolved merge conflicts in source", "General",
                 "'^(<{7}|>{7}|={7})'",
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

    addFindCheck("file_perms", "World-Writable Files",
                 "World-writable or group-writable files", "Security",
                 "-type f \\( -perm -002 -o -perm -020 \\) | head -30",
                 CheckType::Vulnerability, Severity::Major, false);

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
        // OSC 8 / URL scheme allowlist — detects QDesktopServices::openUrl calls
        // that don't front-load a scheme check. Matches fix H1 from audit v2.
        addGrepCheck("qt_openurl_unchecked", "Qt openUrl Without Scheme Check",
                     "QDesktopServices::openUrl called on unvalidated URIs", "Qt",
                     "'QDesktopServices::openUrl'",
                     CheckType::Vulnerability, Severity::Major, true,
                     { /*dropIfContains*/ {"scheme() ==", "validScheme",
                                           "allowScheme", "isLocal",
                                           "https://", "QUrl::TolerantMode"},
                       "", {}, 20 });

        // Hardcoded color values outside themes.cpp — RULES.md rule 1.
        addGrepCheck("qt_hardcoded_colors", "Hardcoded Colors",
                     "Colour literals outside the theme definition", "Qt",
                     "'(QColor\\s*\\([^)]*[0-9]|#[0-9A-Fa-f]{6})'",
                     CheckType::CodeSmell, Severity::Minor, false,
                     { /*dropIfContains*/ {"themes.cpp", "// theme", "/*theme",
                                           "glrenderer.cpp", // GL clear color
                                           "setStyleSheet",  // Qt stylesheet ok
                                           "QColor()"},       // default-constructed
                       "", {}, 30 });

        // Naked findChild<TerminalWidget*>() — misdirects to arbitrary first-match
        // in split-pane layouts. Matches fix from audit seventh (be261d9).
        addGrepCheck("qt_findchild_direct", "Direct findChild (split-pane risk)",
                     "findChild<T*> without the focused-descendant helper", "Qt",
                     "'findChild<TerminalWidget ?\\*>'",
                     CheckType::Bug, Severity::Minor, true,
                     { /*dropIfContains*/ {"activeTerminalInTab",
                                           "broadcast", "broadcastMode",
                                           "findChildren"},  // plural is a batch op
                       "", {}, 20 });

        // emit without the Qt emit macro / Q_EMIT — should be rare but catches
        // pattern-of-thinking issues.
        // (skipped — low signal, not useful)

        // Raw connect without disconnect in long-lived receivers that may
        // outlive the sender. Heuristic: flags connect(...) calls that
        // capture `this` without a `disconnect` in the same file. Inexact
        // but a useful code-review prompt.
        addGrepCheck("qt_connect_this_capture", "connect Capturing this",
                     "Lambda connect() capturing `this` — verify receiver lifetime", "Qt",
                     "'connect\\([^)]*,\\s*this,\\s*\\[this'",
                     CheckType::Hotspot, Severity::Minor, false,
                     { /*dropIfContains*/ {}, "", {}, 40 });
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
        if (r.output.isEmpty()) continue;
        // Fingerprint = checkId + first line of output. Coarse but stable
        // against re-orderings.
        const QString firstLine = r.output.section('\n', 0, 0).trimmed();
        if (!firstLine.isEmpty())
            arr.append(r.checkId + ":" + firstLine);
    }
    QJsonObject root;
    root["generated"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["fingerprints"] = arr;
    QFile f(baselinePath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.close();
        loadBaseline();
        m_statusLabel->setText("Baseline saved.");
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
    QString typesText = "<b>Detected:</b> " + types;
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

    m_results = new QTextEdit(this);
    m_results->setReadOnly(true);
    m_results->setFont(QFont("monospace", 9));
    m_results->setVisible(false);
    root->addWidget(m_results, 2);
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

    // Apply declarative post-filter
    const FilterResult filtered = applyFilter(output, check.filter);

    CheckResult r;
    r.checkId = check.id;
    r.checkName = check.name;
    r.category = check.category;
    r.type = check.type;
    r.severity = check.severity;
    r.output = filtered.body;
    r.findingCount = filtered.count;
    r.warning = false;
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

    // Sort: highest severity first, then by category then name.
    std::vector<CheckResult> sorted(m_completedResults.begin(), m_completedResults.end());
    std::sort(sorted.begin(), sorted.end(), [](const CheckResult &a, const CheckResult &b) {
        if (a.severity != b.severity) return a.severity > b.severity;
        if (a.category != b.category) return a.category < b.category;
        return a.checkName < b.checkName;
    });

    // Apply "new since baseline" filter if the user asked for it.
    auto isNew = [this](const CheckResult &r) {
        if (!m_hasBaseline) return true;
        const QString firstLine = r.output.section('\n', 0, 0).trimmed();
        if (firstLine.isEmpty()) return false;
        return !m_baselineFingerprints.contains(r.checkId + ":" + firstLine);
    };

    // Counts by severity (post-filter if showNewOnly is active).
    int bySev[5] = {0, 0, 0, 0, 0};
    int totalFindings = 0;
    for (const auto &r : sorted) {
        if (r.output.isEmpty() || r.warning) continue;
        if (m_showNewOnly && !isNew(r)) continue;
        bySev[static_cast<int>(r.severity)] += r.findingCount;
        totalFindings += r.findingCount;
    }

    // Summary banner
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
        "</div>"
    ).arg(sorted.size())
     .arg(totalFindings)
     .arg(m_showNewOnly ? " (new since baseline only)" : "")
     .arg(bySev[(int)Severity::Blocker])
     .arg(bySev[(int)Severity::Critical])
     .arg(bySev[(int)Severity::Major])
     .arg(bySev[(int)Severity::Minor])
     .arg(bySev[(int)Severity::Info]);
    m_results->append(banner);

    // Per-check sections
    for (const auto &r : sorted) {
        if (m_showNewOnly && !isNew(r)) continue;
        const bool clean = r.output.isEmpty() && !r.warning;
        const QString body = clean ? "No issues found." : r.output;
        const QString color = r.warning ? "#E74856" : severityColor(r.severity);
        const bool newTag = m_hasBaseline && !clean && !r.warning && isNew(r);

        m_results->append(QString(
            "<div style='margin-bottom:10px;'>"
            "<span style='color:%1; font-weight:bold;'>[%2] %3 %4 %5</span>"
            "<span style='color:#888; font-size:10px;'>  · %6</span>"
            "%7"
            "<pre style='margin:2px 0; white-space:pre-wrap;'>%8</pre>"
            "</div>"
        ).arg(color, severityLabel(r.severity), typeLabel(r.type).toUpper(),
              r.checkName.toHtmlEscaped(),
              r.warning ? "(timeout)" : "",
              r.category.toHtmlEscaped(),
              newTag ? " <span style='color:#4CAF50; font-weight:bold;'>NEW</span>" : "",
              body.toHtmlEscaped()));
    }

    auto *sb = m_results->verticalScrollBar();
    if (sb) sb->setValue(0);  // Reset to top to show summary

    m_statusLabel->setText(QString("Audit complete — %1 findings across %2 checks")
                           .arg(totalFindings).arg(sorted.size()));
}

// ---------------------------------------------------------------------------
// Plain-text export for the Claude Review handoff
// ---------------------------------------------------------------------------

QString AuditDialog::plainTextResults() const {
    if (m_completedResults.isEmpty()) return {};

    QString header = "Project Audit Results\n"
                     "Project: " + m_projectPath + "\n"
                     "Detected: " + m_detectedTypes.join(", ") + "\n";
    if (m_hasBaseline) header += "Baseline: loaded\n";
    header += "---\n\n";

    // Re-sort a copy for the plain-text report.
    std::vector<CheckResult> sorted(m_completedResults.begin(), m_completedResults.end());
    std::sort(sorted.begin(), sorted.end(), [](const CheckResult &a, const CheckResult &b) {
        if (a.severity != b.severity) return a.severity > b.severity;
        return a.checkName < b.checkName;
    });

    QString body;
    for (const auto &r : sorted) {
        if (r.output.isEmpty() && !r.warning) continue;
        body += QString("--- [%1] %2 (%3 / %4) ---\n%5\n\n")
                .arg(severityLabel(r.severity), r.checkName,
                     typeLabel(r.type), r.category,
                     r.warning ? QString("(%1) %2").arg("warning", r.output) : r.output);
    }
    if (body.isEmpty()) body = "No issues found.\n";

    return header + body;
}
