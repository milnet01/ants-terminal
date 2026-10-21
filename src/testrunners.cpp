// ANTS-5503 — test runner detection, parsing and commands. See
// testrunners.h and docs/specs/ANTS-5503-multi-runner-tests.md.

#include "testrunners.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace TestRunners {

namespace {

using TestResCache::ParsedTests;

constexpr int kPytestWalkCap = 20'000;

struct NamedRunner {
    Runner      runner;
    const char *name;
};
constexpr NamedRunner kRunners[] = {
    {Runner::Ctest, "ctest"}, {Runner::Pytest, "pytest"}, {Runner::Jest, "jest"},
    {Runner::Vitest, "vitest"}, {Runner::Cargo, "cargo"}, {Runner::Go, "go"},
};

QByteArray readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

bool fileExists(const QString &root, const char *rel) {
    return QFileInfo(root + QLatin1Char('/') + QLatin1String(rel)).isFile();
}

// Lines without their trailing '\r', so CRLF output parses like LF.
QStringList splitLines(const QString &output) {
    QStringList lines = output.split(QLatin1Char('\n'));
    for (QString &l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    return lines;
}

QString joinLines(const QStringList &lines, qsizetype from, qsizetype to) {
    return lines.mid(from, to - from).join(QLatin1Char('\n'));
}

qint64 toMs(const QString &number, const QString &unit) {
    const double v = number.toDouble();
    return static_cast<qint64>(unit == QLatin1String("ms") ? v : v * 1000.0);
}

// "3 passed" -> {3, "passed"}; false for anything else.
bool countItem(const QString &item, int *n, QString *word) {
    static const QRegularExpression re(QStringLiteral(R"(^\s*(\d+)\s+([a-z]+)\s*$)"));
    const auto m = re.match(item);
    if (!m.hasMatch()) return false;
    *n = m.captured(1).toInt();
    *word = m.captured(2);
    return true;
}

// The counting words every runner but ctest shares (§ 2.3's mapping).
void addCount(ParsedTests &t, int n, const QString &word) {
    if (word == u"passed" || word == u"xpassed")
        t.passed += n;
    else if (word == u"failed" || word == u"error" || word == u"errors")
        t.failed += n;
    else if (word == u"skipped" || word == u"xfailed" || word == u"todo")
        t.skipped += n;
}

// ---- pytest ---------------------------------------------------------------

const QRegularExpression &pytestSummaryRe() {
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*(?:=+\s+)?((?:\d+ [a-z]+(?:, )?)+|no tests ran)\s+in\s+(\d+(?:\.\d+)?)s\b)"));
    return re;
}

// The FAILURES / ERRORS section header naming one test:
//   "____ test_fail ____", "____ ERROR at setup of test_error ____".
QString pytestHeaderName(const QString &line) {
    static const QRegularExpression re(QStringLiteral(R"(^_{3,} (.+?) _{3,}$)"));
    const auto m = re.match(line);
    if (!m.hasMatch()) return {};
    QString n = m.captured(1);
    static const QRegularExpression prefix(
        QStringLiteral(R"(^ERROR (?:at (?:setup|teardown) of|collecting) )"));
    n.remove(prefix);
    return n;
}

ParsedTests parsePytest(const QString &output) {
    ParsedTests t;
    const QStringList lines = splitLines(output);
    static const QRegularExpression collectedRe(QStringLiteral(R"(^collected (\d+) items?\b)"));
    static const QRegularExpression failedRe(QStringLiteral(R"(^(?:FAILED|ERROR) (\S+))"));
    static const QRegularExpression ruleRe(QStringLiteral(R"(^=+ .* =+$)"));

    int summary = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (pytestSummaryRe().match(lines[i]).hasMatch()) summary = int(i);
        const auto c = collectedRe.match(lines[i]);
        if (c.hasMatch()) t.collected = c.captured(1).toInt();
    }
    if (summary < 0) return t;
    t.recognised = true;
    const auto m = pytestSummaryRe().match(lines[summary]);
    if (m.captured(1) != u"no tests ran") {
        for (const QString &item : m.captured(1).split(QLatin1Char(','))) {
            int n = 0;
            QString word;
            if (countItem(item, &n, &word)) addCount(t, n, word);
        }
    }
    t.durationMs = toMs(m.captured(2), QStringLiteral("s"));

    // Failing ids come from the short test summary only, so a captured log
    // line cannot pose as one.
    qsizetype shortSummary = 0;
    for (qsizetype i = 0; i < summary; ++i)
        if (lines[i].contains(QLatin1String(" short test summary info "))) shortSummary = i;
    for (qsizetype i = shortSummary; i < summary; ++i) {
        const auto f = failedRe.match(lines[i]);
        if (!f.hasMatch()) continue;
        const QString id = f.captured(1);
        // The section header carries the id after the file, "::" as ".".
        const qsizetype sep = id.indexOf(QStringLiteral("::"));
        const QString key = sep < 0 ? id : id.mid(sep + 2).replace(QStringLiteral("::"),
                                                                   QStringLiteral("."));
        QString block = lines[i];
        for (qsizetype h = 0; h < i; ++h) {
            if (pytestHeaderName(lines[h]) != key) continue;
            qsizetype end = h + 1;
            while (end < i && pytestHeaderName(lines[end]).isEmpty() &&
                   !ruleRe.match(lines[end]).hasMatch())
                ++end;
            block = joinLines(lines, h, end);
            break;
        }
        if (!TestResCache::appendFailure(t, id, block)) break;
    }
    return t;
}

// ---- jest -----------------------------------------------------------------

const QRegularExpression &jestTestsRe() {
    static const QRegularExpression re(QStringLiteral(R"(^\s*Tests:\s+(.*?)\d+ total\s*$)"));
    return re;
}

bool isJestNoTests(const QString &line) {
    return line.trimmed().startsWith(QLatin1String("No tests found"));
}

ParsedTests parseJest(const QString &output) {
    ParsedTests t;
    const QStringList lines = splitLines(output);
    static const QRegularExpression timeRe(
        QStringLiteral(R"(^\s*Time:\s+(\d+(?:\.\d+)?)\s*(ms|s)\b)"));
    static const QRegularExpression fileRe(QStringLiteral(R"(^(?:FAIL|PASS) (\S+))"));
    const QString bullet = QStringLiteral("● ");

    int summary = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (jestTestsRe().match(lines[i]).hasMatch()) summary = int(i);
        if (isJestNoTests(lines[i])) t.recognised = true;
        const auto tm = timeRe.match(lines[i]);
        if (tm.hasMatch()) t.durationMs = toMs(tm.captured(1), tm.captured(2));
    }
    if (summary >= 0) {
        t.recognised = true;
        const QString counts = jestTestsRe().match(lines[summary]).captured(1);
        for (const QString &item : counts.split(QLatin1Char(','))) {
            int n = 0;
            QString word;
            if (countItem(item, &n, &word)) addCount(t, n, word);
        }
    }
    if (!t.recognised) return t;

    // "● suite › name" headers; a suite that failed to run has no "›", so
    // its file names it.
    const qsizetype end = summary >= 0 ? summary : lines.size();
    QString file;
    for (qsizetype i = 0; i < end; ++i) {
        const auto fm = fileRe.match(lines[i]);
        if (fm.hasMatch()) file = fm.captured(1);
        const QString trimmed = lines[i].trimmed();
        if (!trimmed.startsWith(bullet)) continue;
        QString name = trimmed.mid(bullet.size());
        if (!name.contains(QStringLiteral(" › ")) && !file.isEmpty())
            name = file + QStringLiteral(" › ") + name;
        qsizetype stop = i + 1;
        while (stop < end && !lines[stop].trimmed().startsWith(bullet) &&
               !fileRe.match(lines[stop]).hasMatch() &&
               !lines[stop].startsWith(QLatin1String("Test Suites:")))
            ++stop;
        if (!TestResCache::appendFailure(t, name, joinLines(lines, i, stop))) break;
    }
    return t;
}

// ---- vitest ---------------------------------------------------------------

const QRegularExpression &vitestTestsRe() {
    static const QRegularExpression re(QStringLiteral(R"(^\s*Tests\s+(.*)\((\d+)\)\s*$)"));
    return re;
}

bool isVitestNoTests(const QString &line) {
    return line.trimmed().startsWith(QLatin1String("No test files found"));
}

ParsedTests parseVitest(const QString &output) {
    ParsedTests t;
    const QStringList lines = splitLines(output);
    static const QRegularExpression durRe(
        QStringLiteral(R"(^\s*Duration\s+(\d+(?:\.\d+)?)(ms|s)\b)"));
    static const QRegularExpression failRe(QStringLiteral(R"(^\s*FAIL\s+(.+?)\s*$)"));
    const QChar rule(0x23AF);  // "⎯", vitest's section rule

    int summary = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        if (vitestTestsRe().match(lines[i]).hasMatch()) summary = int(i);
        if (isVitestNoTests(lines[i])) t.recognised = true;
        const auto dm = durRe.match(lines[i]);
        if (dm.hasMatch()) t.durationMs = toMs(dm.captured(1), dm.captured(2));
    }
    if (summary >= 0) {
        t.recognised = true;
        const QString counts = vitestTestsRe().match(lines[summary]).captured(1);
        for (const QString &item : counts.split(QLatin1Char('|'))) {
            int n = 0;
            QString word;
            if (countItem(item, &n, &word)) addCount(t, n, word);
        }
    }
    if (!t.recognised) return t;

    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto fm = failRe.match(lines[i]);
        if (!fm.hasMatch()) continue;
        qsizetype stop = i + 1;
        while (stop < lines.size() && !failRe.match(lines[stop]).hasMatch() &&
               !lines[stop].trimmed().startsWith(rule))
            ++stop;
        if (!TestResCache::appendFailure(t, fm.captured(1), joinLines(lines, i, stop)))
            break;
    }
    return t;
}

// ---- cargo ----------------------------------------------------------------

const QRegularExpression &cargoResultRe() {
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*test result: (?:ok|FAILED)\. (\d+) passed; (\d+) failed; (\d+) ignored;)"
        R"((?:.*finished in (\d+(?:\.\d+)?)s)?)"));
    return re;
}

ParsedTests parseCargo(const QString &output) {
    ParsedTests t;
    const QStringList lines = splitLines(output);
    static const QRegularExpression failRe(QStringLiteral(R"(^test (\S+) \.\.\. FAILED\s*$)"));
    QStringList failed;
    qint64 durationMs = 0;
    for (const QString &line : lines) {
        const auto m = cargoResultRe().match(line);
        if (m.hasMatch()) {
            t.recognised = true;
            t.passed  += m.captured(1).toInt();
            t.failed  += m.captured(2).toInt();
            t.skipped += m.captured(3).toInt();
            if (!m.captured(4).isEmpty()) durationMs += toMs(m.captured(4), QStringLiteral("s"));
        }
        const auto f = failRe.match(line);
        if (f.hasMatch()) failed << f.captured(1);
    }
    if (!t.recognised) return t;
    t.durationMs = durationMs;

    for (const QString &name : failed) {
        // "---- <name> stdout ----" up to the next block or "failures:".
        const QString head = QStringLiteral("---- %1 stdout ----").arg(name);
        QString block = QStringLiteral("test %1 ... FAILED").arg(name);
        const qsizetype h = lines.indexOf(head);
        if (h >= 0) {
            qsizetype stop = h + 1;
            while (stop < lines.size() && !lines[stop].startsWith(QLatin1String("---- ")) &&
                   lines[stop] != u"failures:")
                ++stop;
            block = joinLines(lines, h, stop);
        }
        if (!TestResCache::appendFailure(t, name, block)) break;
    }
    return t;
}

// ---- go -------------------------------------------------------------------

const QRegularExpression &goRunRe() {
    static const QRegularExpression re(QStringLiteral(R"(^=== RUN\s+(\S+))"));
    return re;
}

ParsedTests parseGo(const QString &output) {
    ParsedTests t;
    const QStringList lines = splitLines(output);
    // Un-indented only: an indented "--- " line is a subtest.
    static const QRegularExpression resultRe(QStringLiteral(R"(^--- (PASS|FAIL|SKIP): (\S+))"));
    static const QRegularExpression pkgRe(
        QStringLiteral(R"(^(ok|FAIL)\s+(\S+)(?:\s+(\d+(?:\.\d+)?)s|\s+\[[^\]]*\])?\s*$)"));

    for (const QString &line : lines)
        if (goRunRe().match(line).hasMatch()) t.recognised = true;
    if (!t.recognised) return t;

    // A block runs from the test's "=== RUN" line to the next line that
    // starts another top-level test or ends its package.
    auto blockEnd = [&](qsizetype from, const QString &name) {
        qsizetype i = from + 1;
        for (; i < lines.size(); ++i) {
            const auto r = goRunRe().match(lines[i]);
            if (r.hasMatch() && r.captured(1) != name &&
                !r.captured(1).startsWith(name + QLatin1Char('/')))
                break;
            if (pkgRe.match(lines[i]).hasMatch() || lines[i] == u"PASS" ||
                lines[i] == u"FAIL")
                break;
        }
        return i;
    };

    qint64 durationMs = 0;
    qsizetype pkgStart = 0;
    int failuresInPkg = 0;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const auto r = resultRe.match(lines[i]);
        if (r.hasMatch()) {
            const QString status = r.captured(1);
            if (status == u"PASS") {
                ++t.passed;
            } else if (status == u"SKIP") {
                ++t.skipped;
            } else {
                ++t.failed;
                ++failuresInPkg;
                const QString name = r.captured(2);
                qsizetype start = i;
                for (qsizetype j = pkgStart; j < i; ++j) {
                    const auto run = goRunRe().match(lines[j]);
                    if (run.hasMatch() && run.captured(1) == name) { start = j; break; }
                }
                TestResCache::appendFailure(t, name,
                                            joinLines(lines, start, blockEnd(start, name)));
            }
            continue;
        }
        const auto p = pkgRe.match(lines[i]);
        if (!p.hasMatch()) continue;
        if (!p.captured(3).isEmpty()) durationMs += toMs(p.captured(3), QStringLiteral("s"));
        // A package that failed with no failing test under it (a timeout, a
        // panic outside a test, a build failure) still failed.
        if (p.captured(1) == u"FAIL" && failuresInPkg == 0) {
            ++t.failed;
            TestResCache::appendFailure(t, p.captured(2), joinLines(lines, pkgStart, i + 1));
        }
        pkgStart = i + 1;
        failuresInPkg = 0;
    }
    t.durationMs = durationMs;
    return t;
}

// ---- focused_test commands -------------------------------------------------

// The changed file as a path under root, or empty when it leaves the root.
QString underRoot(const QString &root, const QString &entry) {
    if (entry.isEmpty() || QDir::isAbsolutePath(entry)) return {};
    const QString abs = QDir::cleanPath(root + QLatin1Char('/') + entry);
    if (!abs.startsWith(root + QLatin1Char('/'))) return {};
    return abs.mid(root.size() + 1);
}

QString dotSlash(const QString &rel) { return QStringLiteral("./") + rel; }

bool isPytestFile(const QString &fileName) {
    return fileName.endsWith(QLatin1String(".py")) &&
           (fileName.startsWith(QLatin1String("test_")) ||
            fileName.endsWith(QLatin1String("_test.py")));
}

// Every test_*.py / *_test.py under root, root-relative, skipping
// dot-directories, node_modules, build and virtualenvs. `capped` when the
// walk stopped at kPytestWalkCap files.
QStringList pytestFiles(const QString &root, bool *capped) {
    QStringList out;
    int seen = 0;
    QStringList dirs{root};
    while (!dirs.isEmpty()) {
        const QString dir = dirs.takeFirst();
        if (QFileInfo::exists(dir + QStringLiteral("/pyvenv.cfg"))) continue;
        const QFileInfoList entries = QDir(dir).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
        for (const QFileInfo &fi : entries) {
            if (fi.isDir()) {
                const QString n = fi.fileName();
                if (!n.startsWith(QLatin1Char('.')) && n != u"node_modules" &&
                    n != u"build" && !fi.isSymLink())
                    dirs << fi.filePath();
                continue;
            }
            if (++seen > kPytestWalkCap) { *capped = true; return out; }
            if (isPytestFile(fi.fileName()))
                out << fi.filePath().mid(root.size() + 1);
        }
    }
    *capped = false;
    return out;
}

// `[package] name` of the nearest Cargo.toml above `rel`, or empty.
QString cargoPackage(const QString &root, const QString &rel) {
    static const QRegularExpression nameRe(
        QStringLiteral(R"(^\s*name\s*=\s*"([^"]+)\")"));
    QString dir = QFileInfo(rel).path();
    while (true) {
        const QString toml = root + QLatin1Char('/') +
                             (dir == u"." ? QString() : dir + QLatin1Char('/')) +
                             QStringLiteral("Cargo.toml");
        if (QFileInfo(toml).isFile()) {
            bool inPackage = false;
            for (const QString &line : QString::fromUtf8(readFile(toml)).split('\n')) {
                const QString l = line.trimmed();
                if (l.startsWith(QLatin1Char('['))) inPackage = l == u"[package]";
                const auto m = nameRe.match(l);
                if (inPackage && m.hasMatch()) return m.captured(1);
            }
            return {};  // the nearest Cargo.toml has no package: a workspace root
        }
        if (dir == u"." || dir.isEmpty()) return {};
        dir = QFileInfo(dir).path();
    }
}

}  // namespace

QString name(Runner r) {
    for (const NamedRunner &n : kRunners)
        if (n.runner == r) return QString::fromLatin1(n.name);
    return {};
}

std::optional<Runner> fromName(const QString &s) {
    for (const NamedRunner &n : kRunners)
        if (s == QLatin1String(n.name)) return n.runner;
    return std::nullopt;
}

QString allNames() {
    QStringList out;
    for (const NamedRunner &n : kRunners) out << QString::fromLatin1(n.name);
    return out.join(QStringLiteral(", "));
}

Detection detect(const QString &root) {
    if (fileExists(root, "CMakeLists.txt")) return {Runner::Ctest, QStringLiteral("CMakeLists.txt")};
    if (fileExists(root, "Cargo.toml")) return {Runner::Cargo, QStringLiteral("Cargo.toml")};
    if (fileExists(root, "go.mod")) return {Runner::Go, QStringLiteral("go.mod")};
    if (fileExists(root, "package.json")) {
        const QJsonObject pkg =
            QJsonDocument::fromJson(readFile(root + QStringLiteral("/package.json"))).object();
        auto names = [&pkg](const char *dep) {
            const QString d = QLatin1String(dep);
            return pkg.value(QStringLiteral("dependencies")).toObject().contains(d) ||
                   pkg.value(QStringLiteral("devDependencies")).toObject().contains(d);
        };
        if (names("vitest")) return {Runner::Vitest, QStringLiteral("package.json")};
        if (names("jest") || pkg.contains(QStringLiteral("jest")))
            return {Runner::Jest, QStringLiteral("package.json")};
    }
    for (const char *marker : {"pytest.ini", "conftest.py"})
        if (fileExists(root, marker)) return {Runner::Pytest, QString::fromLatin1(marker)};
    if (readFile(root + QStringLiteral("/pyproject.toml")).contains("[tool.pytest"))
        return {Runner::Pytest, QStringLiteral("pyproject.toml")};
    if (readFile(root + QStringLiteral("/setup.cfg")).contains("[tool:pytest]"))
        return {Runner::Pytest, QStringLiteral("setup.cfg")};
    return {};
}

std::optional<Runner> sniff(const QString &output) {
    if (TestResCache::parseCtestOutput(output).recognised) return Runner::Ctest;
    const QStringList lines = splitLines(output);
    auto any = [&lines](auto &&pred) {
        return std::any_of(lines.begin(), lines.end(), pred);
    };
    if (any([](const QString &l) { return pytestSummaryRe().match(l).hasMatch(); }))
        return Runner::Pytest;
    if (any([](const QString &l) {
            return jestTestsRe().match(l).hasMatch() || isJestNoTests(l);
        }))
        return Runner::Jest;
    if (any([](const QString &l) {
            return vitestTestsRe().match(l).hasMatch() || isVitestNoTests(l);
        }))
        return Runner::Vitest;
    if (any([](const QString &l) { return cargoResultRe().match(l).hasMatch(); }))
        return Runner::Cargo;
    if (any([](const QString &l) { return goRunRe().match(l).hasMatch(); }))
        return Runner::Go;
    return std::nullopt;
}

ParsedTests parse(Runner r, const QString &output) {
    ParsedTests t;
    switch (r) {
        case Runner::Ctest:  return TestResCache::parseCtestOutput(output);
        case Runner::Pytest: t = parsePytest(output); break;
        case Runner::Jest:   t = parseJest(output); break;
        case Runner::Vitest: t = parseVitest(output); break;
        case Runner::Cargo:  t = parseCargo(output); break;
        case Runner::Go:     t = parseGo(output); break;
    }
    t.runner = name(r);
    t.total = t.passed + t.failed + t.skipped;
    t.outputByteSize = output.toUtf8().size();
    return t;
}

Command buildCommand(Runner r, const QString &root, const QStringList &changedFiles,
                     const FocusedTest::CoverageMap &map) {
    Command c;
    switch (r) {
        case Runner::Ctest:  return c;  // FocusedTest::resolve owns ctest
        case Runner::Pytest:
            c.program = QStringLiteral("python3");
            c.fullArgs = {QStringLiteral("-m"), QStringLiteral("pytest"), QStringLiteral("-rfE")};
            break;
        case Runner::Jest:
            c.program = QStringLiteral("npx");
            c.fullArgs = {QStringLiteral("--no-install"), QStringLiteral("jest"),
                          QStringLiteral("--ci")};
            break;
        case Runner::Vitest:
            c.program = QStringLiteral("npx");
            c.fullArgs = {QStringLiteral("--no-install"), QStringLiteral("vitest"),
                          QStringLiteral("run")};
            break;
        case Runner::Cargo:
            c.program = QStringLiteral("cargo");
            c.fullArgs = {QStringLiteral("test"), QStringLiteral("--no-fail-fast")};
            break;
        case Runner::Go:
            c.program = QStringLiteral("go");
            c.fullArgs = {QStringLiteral("test"), QStringLiteral("-v"), QStringLiteral("./...")};
            break;
    }

    // INV-10: only root-relative paths survive; then the map's ignore globs.
    QStringList files;
    for (const QString &entry : changedFiles) {
        const QString rel = underRoot(root, entry);
        if (rel.isEmpty() || (map.valid && FocusedTest::matchesIgnore(rel, map.ignoreGlobs)))
            c.ignoredFiles << entry;
        else if (!files.contains(rel))
            files << rel;
    }

    QString fullReason;
    QStringList units;  // paths, or cargo package names
    bool pytestCapped = false;
    QStringList pytestTree;
    bool pytestWalked = false;
    for (const QString &f : files) {
        QStringList picked;
        switch (r) {
            case Runner::Ctest: break;
            case Runner::Jest:
            case Runner::Vitest:
                picked << dotSlash(f);
                break;
            case Runner::Pytest: {
                if (!f.endsWith(QLatin1String(".py"))) break;
                const QString fileName = QFileInfo(f).fileName();
                if (isPytestFile(fileName)) { picked << dotSlash(f); break; }
                if (!pytestWalked) {
                    pytestTree = pytestFiles(root, &pytestCapped);
                    pytestWalked = true;
                }
                if (pytestCapped) break;
                const QString stem = QFileInfo(f).completeBaseName();
                const QString a = QStringLiteral("test_%1.py").arg(stem);
                const QString b = QStringLiteral("%1_test.py").arg(stem);
                for (const QString &t : pytestTree) {
                    const QString n = QFileInfo(t).fileName();
                    if (n == a || n == b) picked << dotSlash(t);
                }
                break;
            }
            case Runner::Cargo: {
                const QString pkg = cargoPackage(root, f);
                if (!pkg.isEmpty()) picked << pkg;
                break;
            }
            case Runner::Go:
                if (f.endsWith(QLatin1String(".go"))) {
                    const QString dir = QFileInfo(f).path();
                    picked << (dir == u"." ? QStringLiteral("./") : dotSlash(dir));
                }
                break;
        }
        if (picked.isEmpty()) {
            c.unmappedFiles << f;
            if (fullReason.isEmpty())
                fullReason = pytestCapped
                    ? QStringLiteral("the tree walk stopped at %1 files").arg(kPytestWalkCap)
                    : QStringLiteral("%1 has no test of its own").arg(f);
            continue;
        }
        c.mappedFiles << f;
        for (const QString &p : picked)
            if (!units.contains(p)) units << p;
    }

    if (files.isEmpty()) fullReason = QStringLiteral("no changed files to narrow by");
    if (!fullReason.isEmpty()) {
        c.selection = QStringLiteral("full");
        c.reason = std::move(fullReason);
        c.args = c.fullArgs;
        return c;
    }
    QStringList selector;
    for (const QString &u : units) {
        if (r == Runner::Cargo) selector << QStringLiteral("-p");
        selector << u;
    }
    c.selector = selector;
    c.selection = r == Runner::Pytest ? QStringLiteral("heuristic") : QStringLiteral("related");
    c.reason = QStringLiteral("%1 changed file(s) narrowed the run").arg(c.mappedFiles.size());
    switch (r) {
        case Runner::Jest:
            c.args = c.fullArgs;
            c.args << QStringLiteral("--findRelatedTests") << selector;
            break;
        case Runner::Vitest:
            c.args = {QStringLiteral("--no-install"), QStringLiteral("vitest"),
                      QStringLiteral("related"), QStringLiteral("--run")};
            c.args << selector;
            break;
        case Runner::Go:
            c.args = {QStringLiteral("test"), QStringLiteral("-v")};
            c.args << selector;
            break;
        default:
            c.args = c.fullArgs + selector;
            break;
    }
    return c;
}

}  // namespace TestRunners
