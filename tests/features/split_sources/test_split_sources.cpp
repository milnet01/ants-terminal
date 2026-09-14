// ANTS-1677 INV-2, INV-9, INV-11 — a split class's source list stays true.
//
// ANTS-1044, ANTS-1043 and ANTS-4919 split auditdialog.cpp, mainwindow.cpp and
// claudeintegration.cpp into pieces. Each class's pieces are named once, in
// CMakeLists.txt's ANTS_<STEM>_SOURCES_REL list; scrapes read that list, and
// shell and Python readers read the glob src/<stem>.cpp src/<stem>_*.cpp.
// Every case here defends one way the list, the glob and the build can drift
// apart without the build or the suite going red. See spec.md beside this file
// and docs/specs/ANTS-1677-large-file-decomposition.md § 3.
//
// The checks take a root directory, so DetectsEachDefectInAFixtureTree can run
// them over a tree built to break each one. The standing cases run them over
// the real tree, where a class with no list and no pieces has nothing to check.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

struct SplitClass {
    QString stem;
    QString library;   // the library that consumes the list
};

const QList<SplitClass> kClasses = {
    {QStringLiteral("auditdialog"),       QStringLiteral("ants_audit_dialog_lib")},
    {QStringLiteral("mainwindow"),        QStringLiteral("ants_chrome_lib")},
    {QStringLiteral("claudeintegration"), QStringLiteral("ants_claude_lib")},
};

// INV-9's classes. The last cut commit of a class's last item adds its stem.
const QStringList kCappedClasses = {};
constexpr qsizetype kLineCap = 4000;

// The ANTS_<STEM>_SOURCES definitions this bundle was compiled with, by stem.
// A class with a list must have one: without it the separator check below
// cannot run, and a skipped check reads as a passing one.
QMap<QString, QString> compiledLists() {
    QMap<QString, QString> m;
#if defined(ANTS_AUDITDIALOG_SOURCES)
    m.insert(QStringLiteral("auditdialog"), QString::fromUtf8(ANTS_AUDITDIALOG_SOURCES));
#endif
#if defined(ANTS_MAINWINDOW_SOURCES)
    m.insert(QStringLiteral("mainwindow"), QString::fromUtf8(ANTS_MAINWINDOW_SOURCES));
#endif
#if defined(ANTS_CLAUDEINTEGRATION_SOURCES)
    m.insert(QStringLiteral("claudeintegration"),
             QString::fromUtf8(ANTS_CLAUDEINTEGRATION_SOURCES));
#endif
    return m;
}

QByteArray readBytes(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

// CMakeLists.txt with every `#` comment removed, so a `)` inside a comment
// cannot end a block early.
QString cmakeWithoutComments(const QString &root) {
    QStringList out;
    const QString text = QString::fromUtf8(readBytes(root + QStringLiteral("/CMakeLists.txt")));
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const qsizetype hash = line.indexOf(QLatin1Char('#'));
        out << (hash < 0 ? line : line.left(hash));
    }
    return out.join(QLatin1Char('\n'));
}

// The words of the call opening with `head`, up to its closing `)`, or
// nothing when there is no such call.
QStringList callArguments(const QString &cmake, const QString &head) {
    const qsizetype at = cmake.indexOf(head);
    if (at < 0) return {};
    const qsizetype end = cmake.indexOf(QLatin1Char(')'), at);
    if (end < 0) return {};
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    return cmake.mid(at + head.size(), end - at - head.size())
        .split(ws, Qt::SkipEmptyParts);
}

// src/<stem>.cpp if present, then src/<stem>_*.cpp by name — the glob § 2.3
// gives shell and Python readers.
QStringList globbedPieces(const QString &root, const QString &stem) {
    const QDir src(root + QStringLiteral("/src"));
    QStringList out;
    if (src.exists(stem + QStringLiteral(".cpp")))
        out << QStringLiteral("src/") + stem + QStringLiteral(".cpp");
    for (const QString &name : src.entryList({stem + QStringLiteral("_*.cpp")},
                                             QDir::Files, QDir::Name))
        out << QStringLiteral("src/") + name;
    return out;
}

qsizetype lineCount(const QByteArray &body) {
    if (body.isEmpty()) return 0;
    return body.count('\n') + (body.endsWith('\n') ? 0 : 1);
}

// Every regular file under `dir`, skipping this case's own directory, whose
// spec.md names the include the INV-11 scan looks for.
QStringList filesUnder(const QString &root, const QString &dir) {
    const QString selfDir = root + QStringLiteral("/tests/features/split_sources/");
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!path.startsWith(selfDir)) out << path;
    }
    out.sort();
    return out;
}

struct Violations {
    QStringList inv2;
    QStringList inv9;
    QStringList inv11;
};

Violations checkTree(const QString &root, const QMap<QString, QString> &compiled,
                     const QStringList &capped) {
    Violations v;
    const QString cmake = cmakeWithoutComments(root);

    for (const SplitClass &cls : kClasses) {
        const QString stem = cls.stem;
        const QString variable =
            QStringLiteral("ANTS_") + stem.toUpper() + QStringLiteral("_SOURCES_REL");
        const QString entryFile = QStringLiteral("src/") + stem + QStringLiteral(".cpp");

        const QStringList listed = callArguments(cmake, QStringLiteral("set(") + variable);
        const bool hasList = !listed.isEmpty();
        const QStringList globbed = globbedPieces(root, stem);
        const bool hasPieces = globbed.size() > (globbed.contains(entryFile) ? 1 : 0);

        // ---- INV-2 ----
        if (hasList || hasPieces) {
            if (!hasList) {
                v.inv2 << QStringLiteral("%1: pieces exist but CMakeLists.txt has no %2 list: %3")
                              .arg(stem, variable, globbed.join(QStringLiteral(", ")));
            } else {
                if (listed.first() != entryFile) {
                    v.inv2 << QStringLiteral("%1: %2 starts with %3, not %4")
                                  .arg(stem, variable, listed.first(), entryFile);
                }
                const QSet<QString> listSet(listed.begin(), listed.end());
                const QSet<QString> globSet(globbed.begin(), globbed.end());
                for (const QString &p : globbed) {
                    if (!listSet.contains(p))
                        v.inv2 << QStringLiteral("%1: %2 matches the glob but is not in %3")
                                      .arg(stem, p, variable);
                }
                for (const QString &p : listed) {
                    if (!globSet.contains(p))
                        v.inv2 << QStringLiteral("%1: %2 is in %3 but matches no file of the glob")
                                      .arg(stem, p, variable);
                }
                const QStringList libArgs =
                    callArguments(cmake, QStringLiteral("add_library(") + cls.library);
                if (!libArgs.contains(QStringLiteral("${") + variable + QStringLiteral("}"))) {
                    v.inv2 << QStringLiteral("%1: %2 does not consume ${%3}")
                                  .arg(stem, cls.library, variable);
                }
                if (!compiled.contains(stem)) {
                    v.inv2 << QStringLiteral("%1: this bundle has no ANTS_%2_SOURCES definition")
                                  .arg(stem, stem.toUpper());
                } else {
                    QStringList expected;
                    for (const QString &p : listed) expected << root + QLatin1Char('/') + p;
                    const QStringList entries =
                        compiled.value(stem).split(QLatin1Char(';'), Qt::SkipEmptyParts);
                    if (entries != expected) {
                        v.inv2 << QStringLiteral("%1: ANTS_%2_SOURCES holds [%3], the list is [%4] "
                                                 "— an unescaped ';' collapses it")
                                      .arg(stem, stem.toUpper(),
                                           entries.join(QStringLiteral(" | ")),
                                           expected.join(QStringLiteral(" | ")));
                    }
                }
            }
        }

        // ---- INV-9 ----
        if (capped.contains(stem)) {
            const QStringList files = hasList ? listed : globbed;
            for (const QString &p : files) {
                const qsizetype lines = lineCount(readBytes(root + QLatin1Char('/') + p));
                if (lines > kLineCap)
                    v.inv9 << QStringLiteral("%1 is %2 lines, over the %3 cap")
                                  .arg(p).arg(lines).arg(kLineCap);
            }
        }

        // ---- INV-11 ----
        QSet<QString> permitted;
        for (const QString &p : listed)
            permitted.insert(QFileInfo(root + QLatin1Char('/') + p).canonicalFilePath());
        const QByteArray include = QByteArrayLiteral("#include \"") + stem.toUtf8()
                                   + QByteArrayLiteral("_internal.h\"");
        for (const QString &dir : {root + QStringLiteral("/src"), root + QStringLiteral("/tests")}) {
            if (!QFileInfo::exists(dir)) continue;
            for (const QString &path : filesUnder(root, dir)) {
                if (!readBytes(path).contains(include)) continue;
                if (permitted.contains(QFileInfo(path).canonicalFilePath())) continue;
                v.inv11 << QStringLiteral("%1 includes %2_internal.h but is not in its class's list")
                               .arg(QDir(root).relativeFilePath(path), stem);
            }
        }
    }
    return v;
}

const QString kRoot = QStringLiteral(ANTS_SPLIT_SOURCES_ROOT_DIR);

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(body) == body.size();
}

}  // namespace

// INV-2 — the list, the glob, the library and the compiled definition agree.
TEST(SplitSources, ListMatchesTheGlob) {
    ASSERT_TRUE(QFileInfo::exists(kRoot + QStringLiteral("/CMakeLists.txt")));
    const Violations v = checkTree(kRoot, compiledLists(), kCappedClasses);
    EXPECT_TRUE(v.inv2.isEmpty()) << v.inv2.join(QLatin1Char('\n')).toStdString();
}

// INV-9 — no file of a capped class exceeds the cap.
TEST(SplitSources, NoListedFileExceedsTheCap) {
    const Violations v = checkTree(kRoot, compiledLists(), kCappedClasses);
    EXPECT_TRUE(v.inv9.isEmpty()) << v.inv9.join(QLatin1Char('\n')).toStdString();
}

// INV-11 — src/<stem>_internal.h is included only from its class's list.
TEST(SplitSources, InternalHeaderStaysInternal) {
    const Violations v = checkTree(kRoot, compiledLists(), kCappedClasses);
    EXPECT_TRUE(v.inv11.isEmpty()) << v.inv11.join(QLatin1Char('\n')).toStdString();
}

// The checks above can only pass on today's tree while no class is split, so
// this case proves each one can fail. It builds a tree holding every defect the
// parent spec's must-fail list names, then a clean copy that must report none.
TEST(SplitSources, DetectsEachDefectInAFixtureTree) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    const QByteArray cmake =
        "# a comment with ) in it\n"
        "set(ANTS_AUDITDIALOG_SOURCES_REL src/auditdialog.cpp src/auditdialog_rules.cpp)\n"
        "add_library(ants_audit_dialog_lib STATIC ${ANTS_AUDITDIALOG_SOURCES_REL})\n";
    ASSERT_TRUE(writeFile(root + QStringLiteral("/CMakeLists.txt"), cmake));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/src/auditdialog.cpp"), "int a;\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/src/auditdialog_rules.cpp"),
                          "#include \"auditdialog_internal.h\"\n"));
    const QString compiledOk = root + QStringLiteral("/src/auditdialog.cpp;") + root
                               + QStringLiteral("/src/auditdialog_rules.cpp");

    // Clean: nothing to report.
    {
        const Violations v = checkTree(root, {{QStringLiteral("auditdialog"), compiledOk}},
                                       {QStringLiteral("auditdialog")});
        EXPECT_TRUE(v.inv2.isEmpty()) << v.inv2.join(QLatin1Char('\n')).toStdString();
        EXPECT_TRUE(v.inv9.isEmpty()) << v.inv9.join(QLatin1Char('\n')).toStdString();
        EXPECT_TRUE(v.inv11.isEmpty()) << v.inv11.join(QLatin1Char('\n')).toStdString();
    }

    // A piece the list misses, and a stray piece: both match the glob only.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/src/auditdialog_export.cpp"), "int b;\n"));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/src/auditdialog_x.cpp"), "int c;\n"));
    // A listed file one line over the cap.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/src/auditdialog.cpp"),
                          QByteArray("int a;\n").repeated(kLineCap + 1)));
    // A file outside the list including the internal header.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/tests/features/outsider/test_outsider.cpp"),
                          "#include \"auditdialog_internal.h\"\n"));
    // A compiled definition whose ';' was not escaped, so it holds one entry.
    const QString collapsed = root + QStringLiteral("/src/auditdialog.cpp src/auditdialog_rules.cpp");

    const Violations v = checkTree(root, {{QStringLiteral("auditdialog"), collapsed}},
                                   {QStringLiteral("auditdialog")});
    const QString inv2 = v.inv2.join(QLatin1Char('\n'));
    EXPECT_TRUE(inv2.contains(QStringLiteral("src/auditdialog_export.cpp matches the glob")))
        << inv2.toStdString();
    EXPECT_TRUE(inv2.contains(QStringLiteral("src/auditdialog_x.cpp matches the glob")))
        << inv2.toStdString();
    EXPECT_TRUE(inv2.contains(QStringLiteral("an unescaped ';'"))) << inv2.toStdString();
    EXPECT_EQ(v.inv9.size(), 1) << v.inv9.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(v.inv9.value(0).startsWith(QStringLiteral("src/auditdialog.cpp is 4001 lines")))
        << v.inv9.join(QLatin1Char('\n')).toStdString();
    EXPECT_EQ(v.inv11.size(), 1) << v.inv11.join(QLatin1Char('\n')).toStdString();

    // The library no longer consumes the list, and the list is gone while
    // pieces remain.
    ASSERT_TRUE(writeFile(root + QStringLiteral("/CMakeLists.txt"),
                          "set(ANTS_AUDITDIALOG_SOURCES_REL src/auditdialog.cpp src/auditdialog_rules.cpp)\n"
                          "add_library(ants_audit_dialog_lib STATIC src/auditdialog.cpp)\n"));
    EXPECT_TRUE(checkTree(root, {{QStringLiteral("auditdialog"), compiledOk}}, {})
                    .inv2.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("does not consume")));
    ASSERT_TRUE(writeFile(root + QStringLiteral("/CMakeLists.txt"), "project(x)\n"));
    EXPECT_TRUE(checkTree(root, {}, {})
                    .inv2.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("pieces exist but CMakeLists.txt has no")));
}
