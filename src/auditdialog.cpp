#include "auditdialog.h"
#include "toggleswitch.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QFont>
#include <QScrollBar>
#include <QApplication>

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
            // Reconnect for next check
            connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, &AuditDialog::onCheckFinished);
            if (m_currentCheck >= 0 && m_currentCheck < m_checks.size())
                appendResult(m_checks[m_currentCheck].name, "Timed out (30s)", true);
            ++m_checksRun;
            runNextCheck();
        }
    });

    detectProject();
    populateChecks();
    buildUI();
}

// ---------------------------------------------------------------------------
// Project detection
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

    // Scan top-level + one level deep for language indicators
    QDirIterator it(m_projectPath, QDir::Files, QDirIterator::NoIteratorFlags);
    bool hasPy = false, hasSh = false, hasLua = false, hasJava = false;
    bool hasCpp = false;
    int scanned = 0;
    while (it.hasNext() && scanned < 200) {
        it.next();
        ++scanned;
        QString suf = it.fileInfo().suffix().toLower();
        if (suf == "py" || suf == "pyw") hasPy = true;
        if (suf == "sh" || suf == "bash") hasSh = true;
        if (suf == "lua") hasLua = true;
        if (suf == "java") hasJava = true;
        if (suf == "cpp" || suf == "c" || suf == "cc" || suf == "cxx") hasCpp = true;
    }
    // Also check src/ subdirectory
    QDirIterator srcIt(m_projectPath + "/src", QDir::Files, QDirIterator::NoIteratorFlags);
    scanned = 0;
    while (srcIt.hasNext() && scanned < 200) {
        srcIt.next();
        ++scanned;
        QString suf = srcIt.fileInfo().suffix().toLower();
        if (suf == "py" || suf == "pyw") hasPy = true;
        if (suf == "sh" || suf == "bash") hasSh = true;
        if (suf == "lua") hasLua = true;
        if (suf == "java") hasJava = true;
        if (suf == "cpp" || suf == "c" || suf == "cc" || suf == "cxx") hasCpp = true;
    }

    if (hasCpp && !m_detectedTypes.contains("C/C++"))
        m_detectedTypes << "C/C++";
    if (hasPy) m_detectedTypes << "Python";
    if (hasSh) m_detectedTypes << "Shell";
    if (hasLua) m_detectedTypes << "Lua";
    if (hasJava && !m_detectedTypes.contains("Java"))
        m_detectedTypes << "Java";
}

// ---------------------------------------------------------------------------
// Check definitions
// ---------------------------------------------------------------------------

bool AuditDialog::toolExists(const QString &tool) {
    QProcess p;
    p.start("which", {tool});
    p.waitForFinished(3000);
    return p.exitCode() == 0;
}

void AuditDialog::populateChecks() {
    // Exclude patterns common to all find/grep commands
    const QString X = " -not -path './.git/*' -not -path './build/*' -not -path './build-*/*'"
                      " -not -path './node_modules/*' -not -path './.cache/*'"
                      " -not -path './dist/*' -not -path './vendor/*'";
    const QString GX = " --exclude-dir=.git --exclude-dir=build --exclude-dir='build-*'"
                       " --exclude-dir=node_modules --exclude-dir=.cache"
                       " --exclude-dir=dist --exclude-dir=vendor";
    // Security scans also exclude the audit dialog itself (its check descriptions
    // contain the very patterns being searched for, causing false positives)
    const QString SX = GX + " --exclude=auditdialog.cpp --exclude=auditdialog.h";
    const QString GI = " --include='*.cpp' --include='*.h' --include='*.c'"
                       " --include='*.py' --include='*.js' --include='*.ts'"
                       " --include='*.go' --include='*.rs' --include='*.sh'"
                       " --include='*.lua' --include='*.java'";

    // ---- General ----
    m_checks.append({
        "todo_scan", "TODO / FIXME Scanner",
        "Find TODO, FIXME, HACK, XXX annotations", "General",
        "grep -rnI" + GX + GI + " -E '(TODO|FIXME|HACK|XXX)(\\(|:|\\s)' . 2>/dev/null | head -100",
        true, true, {}
    });
    m_checks.append({
        "large_files", "Large File Finder",
        "Files larger than 500 KB", "General",
        "find ." + X + " -type f -size +500k -exec ls -lh {} \\; 2>/dev/null"
        " | awk '{print $5, $9}' | sort -rh | head -30",
        true, true, {}
    });
    m_checks.append({
        "line_stats", "Line Count Statistics",
        "Lines of code by file (top 25)", "General",
        "find ." + X + " -type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
        " -o -name '*.py' -o -name '*.js' -o -name '*.ts' -o -name '*.go'"
        " -o -name '*.rs' -o -name '*.sh' -o -name '*.lua' -o -name '*.java' \\)"
        " | xargs wc -l 2>/dev/null | sort -rn | head -25",
        false, true, {}
    });
    m_checks.append({
        "readme_check", "README & License Check",
        "Verify documentation files exist", "General",
        "echo '=== README ===' && { f=$(ls README* readme* 2>/dev/null); [ -n \"$f\" ] && echo \"$f\" || echo 'No README found'; }"
        " && echo '=== LICENSE ===' && { f=$(ls LICENSE* license* COPYING* 2>/dev/null); [ -n \"$f\" ] && echo \"$f\" || echo 'No LICENSE found'; }",
        false, true, {}
    });
    m_checks.append({
        "dup_files", "Duplicate File Detection",
        "Find files with identical content", "General",
        "find ." + X + " -type f -size +100c \\( -name '*.cpp' -o -name '*.h' -o -name '*.py'"
        " -o -name '*.js' -o -name '*.ts' \\) -exec md5sum {} + 2>/dev/null"
        " | sort | uniq -Dw32 | head -30",
        false, true, {}
    });
    m_checks.append({
        "dangling_symlinks", "Dangling Symlinks",
        "Symlinks pointing to non-existent targets", "General",
        "find ." + X + " -type l ! -exec test -e {} \\; -print 2>/dev/null | head -30",
        false, true, {}
    });
    m_checks.append({
        "conflict_markers", "Git Conflict Markers",
        "Unresolved merge conflicts in source", "General",
        "grep -rnI" + GX + GI + " -E '^(<{7}|>{7}|={7})' . 2>/dev/null | head -20",
        true, true, {}
    });
    m_checks.append({
        "binary_in_repo", "Binary Files in Source",
        "Non-text files tracked alongside source", "General",
        "find ." + X + " -type f \\( -name '*.exe' -o -name '*.dll' -o -name '*.dylib'"
        " -o -name '*.bin' -o -name '*.dat' -o -name '*.db' -o -name '*.sqlite'"
        " -o -name '*.class' -o -name '*.pyc' -o -name '*.pyo' \\) 2>/dev/null | head -30",
        false, true, {}
    });
    m_checks.append({
        "encoding_check", "Source Encoding Check",
        "Non-UTF-8 or BOM-prefixed source files", "General",
        "find ." + X + " -type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
        " -o -name '*.py' -o -name '*.js' -o -name '*.ts' \\)"
        " -exec file {} \\; 2>/dev/null | grep -viE '(UTF-8|ASCII|empty)' | head -20",
        false, true, {}
    });
    m_checks.append({
        "long_files", "Overly Long Source Files",
        "Source files exceeding 1000 lines", "General",
        "find ." + X + " -type f \\( -name '*.cpp' -o -name '*.h' -o -name '*.c'"
        " -o -name '*.py' -o -name '*.js' -o -name '*.ts' -o -name '*.go'"
        " -o -name '*.rs' -o -name '*.java' \\)"
        " -exec awk 'END{if(NR>1000)print NR\" \"FILENAME}' {} \\; 2>/dev/null | sort -rn | head -20",
        false, true, {}
    });
    m_checks.append({
        "debug_leftovers", "Debug / Temp Code",
        "console.log, print(), debug statements left in source", "General",
        "grep -rnI" + GX + " --include='*.js' --include='*.ts' --include='*.jsx' --include='*.tsx'"
        " -E '\\bconsole\\.(log|debug|trace)\\(' . 2>/dev/null | head -20;"
        " grep -rnI" + GX + " --include='*.py' -E '^\\s*(print\\(|pdb\\.|breakpoint\\()' . 2>/dev/null | head -20;"
        " grep -rnI" + GX + " --include='*.cpp' --include='*.c' --include='*.h'"
        " -E '\\b(qDebug|std::cerr|std::cout)\\s*(<{2}|\\()' . 2>/dev/null"
        " | grep -viE '(//|/\\*|error|warn|fatal)' | head -20",
        false, true, {}
    });

    // ---- Security ----
    m_checks.append({
        "secrets_scan", "Hardcoded Secrets Scan",
        "Grep for API keys, passwords, tokens in source", "Security",
        "grep -rnI" + SX + GI + " --include='*.json' --include='*.yaml' --include='*.yml'"
        " --include='*.toml' --include='*.cfg' --include='*.ini'"
        " -iE '(api[_-]?key|password|secret[_-]?key|auth[_-]?token|credentials)\\s*[:=]' . 2>/dev/null"
        " | grep -viE '(test|example|mock|dummy|placeholder|TODO|FIXME|template|sample|EchoMode|setPlaceholder)' | head -50",
        true, true, {}
    });
    m_checks.append({
        "file_perms", "File Permission Check",
        "World-writable or group-writable files", "Security",
        "find ." + X + " -type f \\( -perm -002 -o -perm -020 \\) 2>/dev/null | head -30",
        false, true, {}
    });
    m_checks.append({
        "config_perms", "Sensitive Config Permissions",
        "Config files that should be owner-only (0600)", "Security",
        "find ." + X + " -type f \\( -name '*.json' -o -name '*.yaml' -o -name '*.yml'"
        " -o -name '*.toml' -o -name '*.ini' -o -name '*.cfg' -o -name '*.env' \\)"
        " -perm /077 2>/dev/null | head -20",
        false, true, {}
    });
    m_checks.append({
        "unsafe_c_funcs", "Unsafe C/C++ Functions",
        "strcpy, sprintf, gets, strcat, mktemp, etc.", "Security",
        "grep -rnI" + SX + GI + " -E '\\b(strcpy|strcat|sprintf|vsprintf|gets|mktemp|tmpnam|strtok|scanf)\\s*\\(' . 2>/dev/null"
        " | grep -v '// *safe' | head -50",
        true, true, {}
    });
    m_checks.append({
        "cmd_injection", "Command Injection Patterns",
        "system(), popen() with dynamic arguments", "Security",
        "grep -rnI" + SX + GI + " -E '\\b(system|popen|exec[lv]?p?)\\s*\\(' . 2>/dev/null"
        " | grep -vE '(\\.exec\\(|app\\.exec|menu\\.exec|dialog\\.exec|QApplication)' | head -30;"
        " grep -rnI" + SX + " --include='*.py' -E '(subprocess\\.(call|run|Popen).*shell\\s*=\\s*True|os\\.system)' . 2>/dev/null | head -20;"
        " grep -rnI" + SX + " --include='*.js' --include='*.ts' -E 'child_process\\.exec[^F]' . 2>/dev/null | head -20",
        true, true, {}
    });
    m_checks.append({
        "format_string", "Format String Vulnerabilities",
        "printf/fprintf with non-literal format argument", "Security",
        "grep -rnI" + SX + GI + " -P '\\b[fs]?n?printf\\s*\\([^\"]*\\b\\w+\\s*\\)' . 2>/dev/null"
        " | grep -vE 'printf\\s*\\(\\s*\"' | head -30",
        false, true, {}
    });
    m_checks.append({
        "insecure_http", "Insecure HTTP URLs",
        "http:// URLs that should be https://", "Security",
        "grep -rnI" + SX + GI + " --include='*.json' --include='*.yaml' --include='*.yml'"
        " --include='*.toml' --include='*.xml' --include='*.cfg'"
        " -E 'http://[^l][^o][^c]' . 2>/dev/null"
        " | grep -viE '(localhost|127\\.0\\.0|example\\.com|//comment|0\\.0\\.0\\.0|placeholder)' | head -30",
        true, true, {}
    });
    m_checks.append({
        "unsafe_deser", "Unsafe Deserialization",
        "eval(), pickle.loads(), yaml.load() without SafeLoader", "Security",
        "grep -rnI" + SX + " --include='*.py' -E '\\b(pickle\\.loads?|yaml\\.load|marshal\\.loads?|eval)\\s*\\(' . 2>/dev/null"
        " | grep -viE '(SafeLoader|safe_load|ast\\.literal_eval)' | head -20;"
        " grep -rnI" + SX + " --include='*.js' --include='*.ts' -E '\\beval\\s*\\(' . 2>/dev/null | head -20;"
        " grep -rnI" + SX + " --include='*.php' -E '\\b(unserialize|eval)\\s*\\(' . 2>/dev/null | head -20",
        true, true, {}
    });
    m_checks.append({
        "sql_injection", "SQL Injection Patterns",
        "String concatenation/formatting in SQL queries", "Security",
        "grep -rnI" + SX + GI + " -iE '(execute|query|cursor\\.execute|raw_input).*(%s|%d|\\+.*\\+|\\.format|f\").*([Ss][Ee][Ll][Ee][Cc][Tt]|[Ii][Nn][Ss][Ee][Rr][Tt]|[Uu][Pp][Dd][Aa][Tt][Ee]|[Dd][Ee][Ll][Ee][Tt][Ee])' . 2>/dev/null | head -30;"
        " grep -rnI" + SX + GI + " -iE '([Ss][Ee][Ll][Ee][Cc][Tt]|[Ii][Nn][Ss][Ee][Rr][Tt]|[Uu][Pp][Dd][Aa][Tt][Ee]|[Dd][Ee][Ll][Ee][Tt][Ee]).*(%s|%d|\\+.*\\+|\\.format|f\")' . 2>/dev/null | head -30",
        false, true, {}
    });
    m_checks.append({
        "xss_patterns", "Cross-Site Scripting (XSS)",
        "innerHTML, document.write, dangerouslySetInnerHTML", "Security",
        "grep -rnI" + SX + " --include='*.js' --include='*.ts' --include='*.jsx' --include='*.tsx' --include='*.html'"
        " -E '\\b(innerHTML|outerHTML|document\\.write|dangerouslySetInnerHTML|v-html)\\b' . 2>/dev/null | head -30",
        false, true, {}
    });
    m_checks.append({
        "path_traversal", "Path Traversal Patterns",
        "Unsanitized path joins with user input", "Security",
        "grep -rnI" + SX + GI + " -E '\\.\\./' . 2>/dev/null"
        " | grep -viE '(node_modules|vendor|build|/\\*|//|#|test|spec|mock|example)' | head -20;"
        " grep -rnI" + SX + " --include='*.py' -E 'os\\.path\\.join.*request' . 2>/dev/null | head -10",
        false, true, {}
    });
    m_checks.append({
        "insecure_random", "Insecure Randomness",
        "rand(), srand(), Math.random() for security use", "Security",
        "grep -rnI" + SX + GI + " -E '\\b(srand|rand)\\s*\\(' . 2>/dev/null"
        " | grep -viE '(test|example|seed|arc4random|random_device)' | head -20;"
        " grep -rnI" + SX + " --include='*.py' -E '\\brandom\\.(random|randint|choice)' . 2>/dev/null"
        " | grep -viE '(test|mock|seed|secrets)' | head -20",
        false, true, {}
    });
    m_checks.append({
        "weak_crypto", "Weak Cryptography",
        "MD5, SHA1, DES, RC4, ECB mode usage", "Security",
        "grep -rnI" + SX + GI + " -iE '\\b(md5|sha1|des|rc4|ecb)\\b' . 2>/dev/null"
        " | grep -viE '(git|checksums?|hash.*file|integrity|content.hash|test|checksum|etag)' | head -30",
        false, true, {}
    });
    m_checks.append({
        "hardcoded_ips", "Hardcoded IPs & Ports",
        "IP addresses and port numbers in source", "Security",
        "grep -rnI" + SX + GI + " -E '\\b([0-9]{1,3}\\.){3}[0-9]{1,3}\\b' . 2>/dev/null"
        " | grep -viE '(127\\.0\\.0\\.1|0\\.0\\.0\\.0|255\\.255|example|test|mock|version|license)' | head -20",
        false, true, {}
    });
    m_checks.append({
        "env_files", "Exposed Environment Files",
        ".env, .env.local, credentials files in repo", "Security",
        "find ." + X + " -type f \\( -name '.env' -o -name '.env.*' -o -name 'credentials'"
        " -o -name '*.pem' -o -name '*.key' -o -name '*.p12' -o -name '*.pfx'"
        " -o -name 'id_rsa' -o -name 'id_ed25519' -o -name '*.keystore' \\) 2>/dev/null | head -20",
        true, true, {}
    });
    m_checks.append({
        "temp_files", "Temporary/Backup Files",
        "Editor backups, swap files, OS metadata in repo", "Security",
        "find ." + X + " -type f \\( -name '*.swp' -o -name '*.swo' -o -name '*~'"
        " -o -name '.DS_Store' -o -name 'Thumbs.db' -o -name '*.bak'"
        " -o -name '*.orig' -o -name '*.tmp' \\) 2>/dev/null | head -20",
        false, true, {}
    });
    m_checks.append({
        "resource_leaks", "Resource Leak Patterns",
        "fopen/open without close, unchecked returns", "Security",
        "grep -rnI" + SX + " --include='*.c' --include='*.cpp'"
        " -E '\\b(fopen|open|socket|accept)\\s*\\(' . 2>/dev/null"
        " | grep -viE '(close|fclose|RAII|unique_ptr|shared_ptr|QFile|QSaveFile|auto_ptr)' | head -30",
        false, true, {}
    });

    // ---- Git ----
    if (m_detectedTypes.contains("Git")) {
        m_checks.append({
            "git_status", "Uncommitted Changes",
            "Show working tree status", "Git",
            "git status --short 2>/dev/null",
            true, true, {}
        });
        m_checks.append({
            "git_stale", "Branch Overview",
            "Merged and unmerged branches", "Git",
            "echo '=== Unmerged ===' && git branch -v --no-merged 2>/dev/null"
            " && echo '=== Merged (can delete) ===' && git branch -v --merged 2>/dev/null | grep -v '^\\*'",
            false, true, {}
        });
        m_checks.append({
            "git_large_history", "Large Files in Git History",
            "Blobs > 1MB ever committed (may bloat repo)", "Git",
            "git rev-list --objects --all 2>/dev/null"
            " | git cat-file --batch-check='%(objecttype) %(objectsize) %(rest)' 2>/dev/null"
            " | awk '$1==\"blob\" && $2>1048576 {printf \"%.1fMB %s\\n\", $2/1048576, $3}'"
            " | sort -rn | head -20",
            false, true, {}
        });
        m_checks.append({
            "git_sensitive", "Sensitive Files in Git",
            "Private keys, secrets, credentials ever committed", "Git",
            "git ls-files 2>/dev/null | grep -iE '(id_rsa|id_ed25519|\\.pem|\\.key|\\.env|credentials|secret|password)'"
            " | grep -viE '(example|template|sample|test|mock|lock|CLAUDE\\.md)' | head -20",
            false, true, {}
        });
    }

    // ---- C/C++ ----
    if (m_detectedTypes.contains("C/C++")) {
        bool hasCppcheck = toolExists("cppcheck");
        m_checks.append({
            "cppcheck", "cppcheck Static Analysis",
            hasCppcheck ? "Warnings, performance, portability" : "(cppcheck not installed)",
            "C/C++",
            "cppcheck --enable=warning,performance,portability --quiet --inline-suppr"
            " --suppress=missingInclude --suppress=unmatchedSuppression"
            " -i build -i build-test -i build-release -i node_modules"
            " -j$(nproc) . 2>&1 | head -100",
            hasCppcheck, hasCppcheck, {}
        });
        m_checks.append({
            "cppcheck_unused", "Dead Code Detection",
            hasCppcheck ? "Unused functions (single-threaded scan)" : "(cppcheck not installed)",
            "C/C++",
            "cppcheck --enable=unusedFunction --quiet --inline-suppr"
            " --suppress=missingInclude --suppress=unmatchedSuppression"
            " -i build -i build-test -i build-release -i node_modules"
            " . 2>&1 | head -50",
            false, hasCppcheck, {}
        });
        bool hasClangTidy = toolExists("clang-tidy");
        m_checks.append({
            "clang_tidy", "clang-tidy Analysis",
            hasClangTidy ? "Modernize, readability, performance checks" : "(clang-tidy not installed)",
            "C/C++",
            "find . -name '*.cpp'" + X + " | head -15"
            " | xargs -I{} clang-tidy {} -- -std=c++20 -I src 2>&1 | head -100",
            false, hasClangTidy, {}
        });
        m_checks.append({
            "header_guards", "Missing Header Guards",
            "Headers without #pragma once or include guards", "C/C++",
            "find ." + X + " -name '*.h' -o -name '*.hpp' | while read f; do"
            " head -5 \"$f\" | grep -qE '(#pragma once|#ifndef .+_H)' || echo \"$f\";"
            " done 2>/dev/null | head -20",
            false, true, {}
        });
        m_checks.append({
            "compiler_warnings", "Compiler Warnings Check",
            "Build with -Wall -Wextra and capture warnings", "C/C++",
            "if [ -f CMakeLists.txt ]; then"
            " tmpdir=$(mktemp -d) && cd \"$tmpdir\""
            " && cmake -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wno-unused-parameter' \"$OLDPWD\" 2>/dev/null"
            " && make -j$(nproc) 2>&1 | grep -E '(warning:|error:)' | head -60;"
            " rm -rf \"$tmpdir\";"
            " fi",
            false, true, {}
        });
        m_checks.append({
            "memory_patterns", "Memory Management Patterns",
            "new/malloc without smart pointers or free", "C/C++",
            "grep -rnI" + SX + " --include='*.cpp' --include='*.c'"
            " -E '\\b(new |malloc|calloc|realloc)\\b' . 2>/dev/null"
            " | grep -vE '(unique_ptr|shared_ptr|make_unique|make_shared|delete|free|RAII|nothrow|placement|override|Q_NEW|qMalloc)'"
            " | head -30",
            false, true, {}
        });
    }

    // ---- Python ----
    if (m_detectedTypes.contains("Python")) {
        bool hasPylint = toolExists("pylint");
        m_checks.append({
            "pylint", "Pylint Analysis",
            hasPylint ? "Error-level checks" : "(pylint not installed)",
            "Python",
            "find . -name '*.py'" + X + " | head -20 | xargs pylint --errors-only 2>&1 | head -100",
            hasPylint, hasPylint, {}
        });
        bool hasBandit = toolExists("bandit");
        m_checks.append({
            "bandit", "Bandit Security Scan",
            hasBandit ? "Python security issue detection" : "(bandit not installed)",
            "Python",
            "bandit -r . -q --exclude=./build,./build-test,./node_modules 2>&1 | head -100",
            hasBandit, hasBandit, {}
        });
        bool hasMypy = toolExists("mypy");
        m_checks.append({
            "mypy", "mypy Type Check",
            hasMypy ? "Static type analysis" : "(mypy not installed)",
            "Python",
            "mypy . --ignore-missing-imports 2>&1 | tail -20",
            false, hasMypy, {}
        });
        bool hasRuff = toolExists("ruff");
        m_checks.append({
            "ruff", "Ruff Linter",
            hasRuff ? "Fast Python linting (flake8 + more)" : "(ruff not installed)",
            "Python",
            "ruff check . 2>&1 | head -80",
            hasRuff, hasRuff, {}
        });
    }

    // ---- JavaScript / TypeScript ----
    if (m_detectedTypes.contains("JavaScript")) {
        bool hasNpm = toolExists("npm");
        m_checks.append({
            "npm_audit", "npm Dependency Audit",
            hasNpm ? "Check for known vulnerabilities" : "(npm not installed)",
            "JavaScript",
            "npm audit --production 2>&1 | tail -30",
            hasNpm, hasNpm, {}
        });
        m_checks.append({
            "eslint", "ESLint Analysis",
            hasNpm ? "Lint JavaScript/TypeScript" : "(npm not installed)",
            "JavaScript",
            "npx eslint . --max-warnings=50 2>&1 | head -100",
            false, hasNpm, {}
        });
        m_checks.append({
            "outdated_deps", "Outdated Dependencies",
            hasNpm ? "Check for outdated npm packages" : "(npm not installed)",
            "JavaScript",
            "npm outdated 2>&1 | head -30",
            false, hasNpm, {}
        });
    }

    // ---- Rust ----
    if (m_detectedTypes.contains("Rust")) {
        bool hasCargo = toolExists("cargo");
        m_checks.append({
            "cargo_clippy", "Cargo Clippy",
            hasCargo ? "Rust lint checks" : "(cargo not installed)",
            "Rust",
            "cargo clippy 2>&1 | head -100",
            hasCargo, hasCargo, {}
        });
        bool hasAudit = toolExists("cargo-audit");
        m_checks.append({
            "cargo_audit", "Cargo Audit",
            hasAudit ? "Dependency vulnerability scan" : "(cargo-audit not installed)",
            "Rust",
            "cargo audit 2>&1 | head -100",
            hasAudit, hasAudit, {}
        });
    }

    // ---- Go ----
    if (m_detectedTypes.contains("Go")) {
        bool hasGo = toolExists("go");
        m_checks.append({
            "go_vet", "Go Vet",
            hasGo ? "Report likely mistakes" : "(go not installed)",
            "Go",
            "go vet ./... 2>&1 | head -100",
            hasGo, hasGo, {}
        });
        bool hasVuln = toolExists("govulncheck");
        m_checks.append({
            "govulncheck", "Go Vulnerability Check",
            hasVuln ? "Scan dependencies for known vulnerabilities" : "(govulncheck not installed)",
            "Go",
            "govulncheck ./... 2>&1 | head -50",
            false, hasVuln, {}
        });
        bool hasLint = toolExists("golangci-lint");
        m_checks.append({
            "golangci_lint", "golangci-lint",
            hasLint ? "Comprehensive Go linting" : "(golangci-lint not installed)",
            "Go",
            "golangci-lint run ./... 2>&1 | head -100",
            false, hasLint, {}
        });
    }

    // ---- Shell ----
    if (m_detectedTypes.contains("Shell")) {
        bool hasSC = toolExists("shellcheck");
        m_checks.append({
            "shellcheck", "ShellCheck Analysis",
            hasSC ? "Static analysis for shell scripts" : "(shellcheck not installed)",
            "Shell",
            "find . -name '*.sh'" + X + " -exec shellcheck {} + 2>&1 | head -100",
            hasSC, hasSC, {}
        });
    }

    // ---- Lua ----
    if (m_detectedTypes.contains("Lua")) {
        bool hasLuacheck = toolExists("luacheck");
        m_checks.append({
            "luacheck", "Luacheck Analysis",
            hasLuacheck ? "Static analysis for Lua scripts" : "(luacheck not installed)",
            "Lua",
            "find . -name '*.lua'" + X + " -exec luacheck {} + 2>&1 | head -100",
            hasLuacheck, hasLuacheck, {}
        });
    }

    // ---- Java ----
    if (m_detectedTypes.contains("Java")) {
        bool hasSpotbugs = toolExists("spotbugs");
        m_checks.append({
            "spotbugs", "SpotBugs Analysis",
            hasSpotbugs ? "Find bug patterns in Java code" : "(spotbugs not installed)",
            "Java",
            "find . -name '*.class'" + X + " | head -1 >/dev/null 2>&1"
            " && spotbugs -textui -effort:max . 2>&1 | head -80 || echo 'No compiled .class files found'",
            false, hasSpotbugs, {}
        });
    }
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void AuditDialog::buildUI() {
    auto *root = new QVBoxLayout(this);
    root->setSpacing(8);

    // Header
    m_pathLabel = new QLabel(this);
    m_pathLabel->setText("<b>Project:</b> " + m_projectPath);
    m_pathLabel->setTextFormat(Qt::RichText);
    root->addWidget(m_pathLabel);

    m_typesLabel = new QLabel(this);
    QString types = m_detectedTypes.isEmpty() ? "Unknown" : m_detectedTypes.join(", ");
    m_typesLabel->setText("<b>Detected:</b> " + types);
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

    // Group checks by category
    QStringList categories;
    for (const auto &c : m_checks) {
        if (!categories.contains(c.category))
            categories << c.category;
    }

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

        // Write to a temp file that persists after dialog closes
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

    // Progress
    m_progress = new QProgressBar(this);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);
    root->addWidget(m_progress);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setVisible(false);
    root->addWidget(m_statusLabel);

    // Results
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

    // Disable toggles during run
    for (auto &c : m_checks)
        if (c.toggle) c.toggle->setEnabled(false);

    runNextCheck();
}

void AuditDialog::runNextCheck() {
    // Find next selected check
    while (++m_currentCheck < m_checks.size()) {
        if (m_checks[m_currentCheck].toggle &&
            m_checks[m_currentCheck].toggle->isChecked())
            break;
    }

    if (m_currentCheck >= m_checks.size()) {
        // All done
        m_progress->setValue(m_totalSelected);
        m_statusLabel->setText(QString("Audit complete - %1 checks run").arg(m_totalSelected));
        m_runBtn->setEnabled(true);
        m_reviewBtn->setVisible(true);
        // Re-enable toggles
        for (auto &c : m_checks)
            if (c.toggle) c.toggle->setEnabled(c.available);
        return;
    }

    const auto &check = m_checks[m_currentCheck];
    m_statusLabel->setText("Running: " + check.name + "...");
    m_progress->setValue(m_checksRun);

    m_timeout->start(30000);
    m_process->start("/bin/bash", {"-c", check.command});
    if (!m_process->waitForStarted(5000)) {
        m_timeout->stop();
        appendResult(check.name, "Failed to start process", true);
        ++m_checksRun;
        runNextCheck();
    }
}

void AuditDialog::onCheckFinished(int /*exitCode*/, QProcess::ExitStatus /*status*/) {
    m_timeout->stop();
    if (m_currentCheck < 0 || m_currentCheck >= m_checks.size()) return;

    const auto &check = m_checks[m_currentCheck];
    QString output = QString::fromUtf8(m_process->readAllStandardOutput()).trimmed();
    QString errOutput = QString::fromUtf8(m_process->readAllStandardError()).trimmed();

    if (!errOutput.isEmpty() && output.isEmpty())
        output = errOutput;
    else if (!errOutput.isEmpty())
        output += "\n" + errOutput;

    if (output.isEmpty())
        output = "No issues found.";

    appendResult(check.name, output);
    ++m_checksRun;
    runNextCheck();
}

void AuditDialog::appendResult(const QString &title, const QString &output, bool isWarning) {
    QString color = isWarning ? "#e74856" : "#4CAF50";
    m_results->append(QString(
        "<div style='margin-bottom:8px;'>"
        "<span style='color:%1; font-weight:bold; font-size:11px;'>--- %2 ---</span><br>"
        "<pre style='margin:2px 0; white-space:pre-wrap;'>%3</pre>"
        "</div>"
    ).arg(color, title.toHtmlEscaped(), output.toHtmlEscaped()));

    // Auto-scroll to bottom
    auto *sb = m_results->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}

QString AuditDialog::plainTextResults() const {
    // Convert the rich-text results to plain text for Claude
    QString text = m_results->toPlainText().trimmed();
    if (text.isEmpty()) return {};

    QString header = "Project Audit Results\n"
                     "Project: " + m_projectPath + "\n"
                     "Detected: " + m_detectedTypes.join(", ") + "\n"
                     "---\n\n";
    return header + text;
}
