// Why this exists: ANTS-6245 — a literal workspace_search that hits its wall
// budget must finish through the git index and return the SAME rows as a plain
// scan; a prefilter that drops a match does so without any error. Contract:
// tests/features/workspace_search_timeout_fallback/spec.md; design:
// docs/specs/ANTS-6245-search-timeout-git-fallback.md.
//
// Behavioural: each case builds a real git repository and a real ripgrep runs.
// RemoteControl::setSearchTimeoutForced(true) stands in for the wall budget
// stopping the first scan, so no case waits on a clock.

#include "../../_support/srcgrep.h"

#include "remotecontrol.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <string>

#ifndef ANTS_SOURCE_DIR
#error "this test reads src/ for INV-11 and needs ANTS_SOURCE_DIR"
#endif

namespace {

const QString kPat = QStringLiteral("zebrafoo");  // no k/K/s/S: needle is whole

QString canon(const QString &p) { return QFileInfo(p).canonicalFilePath(); }

bool writeFile(const QString &path, const QByteArray &body) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = (f.write(body) == body.size());
    f.close();
    return ok;
}

// Independent of the user's git config: no global/system files, no autocrlf.
bool git(const QString &dir, const QStringList &args, QByteArray *out = nullptr) {
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
    env.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
    p.setProcessEnvironment(env);
    p.setWorkingDirectory(dir);
    QStringList full{QStringLiteral("-c"), QStringLiteral("user.name=t"),
                     QStringLiteral("-c"), QStringLiteral("user.email=t@example.invalid"),
                     QStringLiteral("-c"), QStringLiteral("core.autocrlf=false"),
                     QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false")};
    full << args;
    p.start(QStringLiteral("git"), full);
    if (!p.waitForFinished(20000)) return false;
    if (out) *out = p.readAllStandardOutput();
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool initRepo(const QString &root) {
    return git(root, {QStringLiteral("init"), QStringLiteral("-q"),
                      QStringLiteral("-b"), QStringLiteral("main")});
}

bool commitAll(const QString &root, const QString &msg = QStringLiteral("c")) {
    return git(root, {QStringLiteral("add"), QStringLiteral("-A")})
        && git(root, {QStringLiteral("commit"), QStringLiteral("-q"),
                      QStringLiteral("-m"), msg});
}

// Tracked files that never hold any pattern, so the skip list is non-empty.
void addFillers(const QString &dir) {
    for (int i = 0; i < 6; ++i)
        writeFile(dir + QStringLiteral("/filler%1.txt").arg(i),
                  QByteArray("nothing to see here ") + QByteArray::number(i)
                      + "\n");
}

QJsonObject run(const QString &root, const QString &pattern, bool forced,
                QJsonObject req = QJsonObject()) {
    RemoteControl rc(nullptr);
    rc.setSearchTimeoutForced(forced);
    req[QStringLiteral("caller_cwd")] = root;
    req[QStringLiteral("pattern")] = pattern;
    return rc.cmdWorkspaceSearch(req).object();
}

std::string dump(const QJsonObject &o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

QSet<QString> rowsOf(const QJsonObject &resp) {
    QSet<QString> s;
    for (const auto &v : resp.value(QStringLiteral("matches")).toArray()) {
        const QJsonObject m = v.toObject();
        s.insert(m.value(QStringLiteral("file")).toString() + QLatin1Char('\t')
                 + QString::number(m.value(QStringLiteral("line")).toInt())
                 + QLatin1Char('\t') + m.value(QStringLiteral("text")).toString());
    }
    return s;
}

QSet<QString> filesOf(const QSet<QString> &rows) {
    QSet<QString> f;
    for (const QString &r : rows) f.insert(r.section(QLatin1Char('\t'), 0, 0));
    return f;
}

QString sorted(const QSet<QString> &s) {
    QStringList l(s.begin(), s.end());
    l.sort();
    return l.join(QLatin1String("\n  "));
}

// The one comparison every completed-fallback case makes. Returns the plain
// rows so a caller can add its own assertions.
QSet<QString> expectSameRows(const QString &root, const QString &pattern,
                             const QJsonObject &req = QJsonObject(),
                             const QStringList &mustFind = QStringList()) {
    const QJsonObject plain = run(root, pattern, false, req);
    const QJsonObject fb = run(root, pattern, true, req);
    const QSet<QString> want = rowsOf(plain);
    const QSet<QString> got = rowsOf(fb);

    EXPECT_TRUE(plain.value(QStringLiteral("ok")).toBool())
        << "plain scan failed: " << dump(plain);
    EXPECT_FALSE(want.isEmpty())
        << "fixture is vacuous: the plain scan found nothing; reply "
        << dump(plain);
    const QSet<QString> plainFiles = filesOf(want);
    for (const QString &f : mustFind)
        EXPECT_TRUE(plainFiles.contains(f))
            << "fixture check: plain scan should find " << f.toStdString()
            << "; found files:\n  " << sorted(plainFiles).toStdString();

    EXPECT_TRUE(fb.value(QStringLiteral("ok")).toBool())
        << "forced-timeout call: expected ok:true (fallback completed), "
           "actual reply " << dump(fb);
    EXPECT_EQ(fb.value(QStringLiteral("timeout_fallback")).toString(),
              QStringLiteral("git_index"))
        << "expected timeout_fallback:git_index; reply " << dump(fb);
    EXPECT_FALSE(fb.contains(QStringLiteral("timeout_fallback_skipped")))
        << "a completed fallback must carry no timeout_fallback_skipped; "
           "reply " << dump(fb);
    EXPECT_EQ(got, want)
        << "fallback rows differ from plain rows.\nplain:\n  "
        << sorted(want).toStdString() << "\nfallback:\n  "
        << sorted(got).toStdString();
    return want;
}

QJsonObject jreq(std::initializer_list<std::pair<QString, QJsonValue>> kv) {
    QJsonObject o;
    for (const auto &p : kv) o[p.first] = p.second;
    return o;
}

}  // namespace

// ---------------------------------------------------------------- INV-1 -----

TEST(workspace_search_timeout_fallback, sameRowsAsPlainScan) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    writeFile(root + "/a.txt", "one\nhas zebrafoo here\nthree\n");
    writeFile(root + "/sub/b.txt", "ZEBRAFOO upper\nzebrafoo twice zebrafoo\n");
    writeFile(root + "/c.cpp", "int x; // zebrafoo\n");
    ASSERT_TRUE(commitAll(root));
    expectSameRows(root, kPat, QJsonObject(),
                   {QStringLiteral("a.txt"), QStringLiteral("sub/b.txt"),
                    QStringLiteral("c.cpp")});
}

// ---------------------------------------------------------------- INV-2 -----

TEST(workspace_search_timeout_fallback, unicodeFoldKept) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    // U+212A KELVIN SIGN + "elvin"; "xyzte" + U+017F + "t". No ASCII
    // "kelvin" / "xyztest" anywhere. (Spec says "test"; its needle is "te",
    // under three bytes, so § 4.1 would skip the fallback. See spec.md.)
    writeFile(root + "/kelvin.txt", QByteArray("\xE2\x84\xAA" "elvin\n"));
    writeFile(root + "/long_s.txt", QByteArray("xyzte" "\xC5\xBF" "t\n"));
    ASSERT_TRUE(commitAll(root));
    expectSameRows(root, QStringLiteral("kelvin"), QJsonObject(),
                   {QStringLiteral("kelvin.txt")});
    expectSameRows(root, QStringLiteral("xyztest"), QJsonObject(),
                   {QStringLiteral("long_s.txt")});
}

// ---------------------------------------------------------------- INV-3 -----

TEST(workspace_search_timeout_fallback, utf16Kept) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    QByteArray u16("\xFF\xFE", 2);
    for (QChar c : QStringLiteral("zebrafoo in utf16\n")) {
        u16.append(char(c.unicode() & 0xFF));
        u16.append(char(c.unicode() >> 8));
    }
    writeFile(root + "/u16.txt", u16);
    ASSERT_TRUE(commitAll(root));
    expectSameRows(root, kPat, QJsonObject(), {QStringLiteral("u16.txt")});
}

// ---------------------------------------------------------------- INV-4 -----

TEST(workspace_search_timeout_fallback, dirtyStatesKept) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    const QByteArray base("base content\n");
    // Each line differs: the default reply folds identical lines into one row.
    const auto hit = [](const char *name) {
        return QByteArray("working copy of ") + name + " has zebrafoo\n";
    };
    for (const char *n : {"modified.txt", "assume.txt", "skipwt.txt",
                          "conflict.txt"})
        writeFile(root + "/" + n, base);
    ASSERT_TRUE(commitAll(root, QStringLiteral("base")));

    // Unmerged: a real conflict between two branches.
    ASSERT_TRUE(git(root, {"checkout", "-q", "-b", "side"}));
    writeFile(root + "/conflict.txt", "side\n");
    ASSERT_TRUE(commitAll(root, QStringLiteral("side")));
    ASSERT_TRUE(git(root, {"checkout", "-q", "main"}));
    writeFile(root + "/conflict.txt", "main\n");
    ASSERT_TRUE(commitAll(root, QStringLiteral("main")));
    // The merge must fail with a conflict; its exit code is 1.
    EXPECT_FALSE(git(root, {"merge", "side"}))
        << "fixture: merge was expected to conflict";
    QByteArray unmerged;
    ASSERT_TRUE(git(root, {"ls-files", "-u"}, &unmerged));
    ASSERT_FALSE(unmerged.isEmpty()) << "fixture: no unmerged entry was made";

    ASSERT_TRUE(git(root, {"update-index", "--assume-unchanged", "assume.txt"}));
    ASSERT_TRUE(git(root, {"update-index", "--skip-worktree", "skipwt.txt"}));
    // The pattern lives only in the working copy of each file.
    for (const char *n : {"modified.txt", "assume.txt", "skipwt.txt",
                          "untracked.txt", "conflict.txt"})
        writeFile(root + "/" + n, hit(n));

    expectSameRows(root, kPat, QJsonObject(),
                   {QStringLiteral("modified.txt"), QStringLiteral("assume.txt"),
                    QStringLiteral("skipwt.txt"), QStringLiteral("untracked.txt"),
                    QStringLiteral("conflict.txt")});
}

// ---------------------------------------------------------------- INV-5 -----

TEST(workspace_search_timeout_fallback, attributedKept) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    writeFile(root + "/.gitattributes", "ident.txt ident\n");
    writeFile(root + "/ident.txt", "$Id$\nplain line\n");
    ASSERT_TRUE(commitAll(root));
    // Expansion happens on checkout, so the working copy gains "$Id: <sha> $".
    ASSERT_TRUE(QFile::remove(root + "/ident.txt"));
    ASSERT_TRUE(git(root, {"checkout", "--", "ident.txt"}));
    QFile expanded(root + "/ident.txt");
    ASSERT_TRUE(expanded.open(QIODevice::ReadOnly));
    ASSERT_TRUE(expanded.readAll().contains("$Id: "))
        << "fixture: git did not expand $Id$ in the working copy";
    expectSameRows(root, QStringLiteral("$Id: "), QJsonObject(),
                   {QStringLiteral("ident.txt")});
}

// ---------------------------------------------------------------- INV-6 -----

TEST(workspace_search_timeout_fallback, noExtraRows) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    writeFile(root + "/bin.dat",
              QByteArray("head\0zebrafoo in a binary file\0tail\n", 36));
    writeFile(root + "/notes.txt", "zebrafoo in a txt file\n");
    writeFile(root + "/code.cpp", "// zebrafoo in a cpp file\n");
    ASSERT_TRUE(commitAll(root));

    // Plain and fallback must agree, with and without a glob; naming files to
    // ripgrep would add bin.dat and ignore the glob.
    const QSet<QString> all = expectSameRows(root, kPat);
    EXPECT_FALSE(filesOf(all).contains(QStringLiteral("bin.dat")))
        << "fixture: a walk should skip the binary file";
    const QSet<QString> globbed = expectSameRows(
        root, kPat, jreq({{QStringLiteral("glob"), QStringLiteral("*.cpp")}}),
        {QStringLiteral("code.cpp")});
    EXPECT_EQ(filesOf(globbed), QSet<QString>{QStringLiteral("code.cpp")})
        << "glob *.cpp must keep notes.txt and bin.dat out";
}

// ---------------------------------------------------------------- INV-7 -----

// The argv cannot be inspected from outside, so this asserts the rows: an
// unescaped metacharacter in a skip glob either leaves a file in the scan
// (harmless) or removes one that holds the text (a row goes missing).
TEST(workspace_search_timeout_fallback, oddNamesRoundTrip) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    const QStringList stems{"x[1]", "x]", "x*", "x?", "x{a,b}", "x}",
                            "x\\", "x sp", "!x", "#x"};
    QStringList yes;
    for (const QString &s : stems) {
        // Each line differs: the default reply folds identical lines into one row.
        ASSERT_TRUE(writeFile(root + "/" + s + ".yes.txt",
                              "has zebrafoo " + QByteArray::number(yes.size()) + "\n"));
        ASSERT_TRUE(writeFile(root + "/" + s + ".no.txt", "nothing here\n"));
        yes << s + QStringLiteral(".yes.txt");
    }
    ASSERT_TRUE(commitAll(root));
    const QSet<QString> rows = expectSameRows(root, kPat, QJsonObject(), yes);
    EXPECT_EQ(filesOf(rows).size(), stems.size())
        << "every odd-named file holding the text must be a row";
}

// ---------------------------------------------------------------- INV-8 -----

TEST(workspace_search_timeout_fallback, skippedReasons) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    writeFile(root + "/a.txt", "zebrafoo here\nks short\nzebra\tfoo tab\n");
    ASSERT_TRUE(commitAll(root));

    QTemporaryDir plainDir;  // a folder that is not a git tree
    ASSERT_TRUE(plainDir.isValid());
    const QString noGit = canon(plainDir.path());
    writeFile(noGit + "/a.txt", "zebrafoo here\n");

    struct Row {
        const char *name;
        QString root;
        QString pattern;
        QJsonObject req;
        QString want;
    };
    const QList<Row> rows{
        {"regex", root, QStringLiteral("zebra.*foo"),
         jreq({{QStringLiteral("regex"), true}}), QStringLiteral("regex")},
        {"match_wrapped", root, kPat,
         jreq({{QStringLiteral("match_wrapped"), true}}),
         QStringLiteral("regex")},
        {"control_char", root, QStringLiteral("zebra\tfoo"), QJsonObject(),
         QStringLiteral("control_char")},
        {"budget", root, kPat, jreq({{QStringLiteral("timeout_sec"), 30}}),
         QStringLiteral("budget")},
        {"short_needle", root, QStringLiteral("ks"), QJsonObject(),
         QStringLiteral("short_needle")},
        {"git_failed", noGit, kPat, QJsonObject(),
         QStringLiteral("git_failed")},
    };
    for (const Row &r : rows) {
        const QJsonObject resp = run(r.root, r.pattern, true, r.req);
        SCOPED_TRACE(r.name);
        EXPECT_FALSE(resp.value(QStringLiteral("ok")).toBool(true))
            << "case " << r.name << ": expected the ok:false timeout "
            << "refusal, actual " << dump(resp);
        EXPECT_EQ(resp.value(QStringLiteral("code")).toString(),
                  QStringLiteral("rg_failed"))
            << "case " << r.name << ": reply " << dump(resp);
        const QJsonValue v = resp.value(QStringLiteral("timeout_fallback_skipped"));
        ASSERT_TRUE(v.isString())
            << "case " << r.name << ": timeout_fallback_skipped missing or "
            << "not a string; reply " << dump(resp);
        EXPECT_EQ(v.toString(), r.want) << "case " << r.name;
        EXPECT_FALSE(resp.contains(QStringLiteral("timeout_fallback")))
            << "case " << r.name << ": a skipped fallback is not a completed "
            << "one; reply " << dump(resp);
    }
}

// ---------------------------------------------------------------- INV-9 -----

TEST(workspace_search_timeout_fallback, subdirRootAndLane) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString top = canon(tmp.path());
    ASSERT_TRUE(initRepo(top));
    addFillers(top);
    addFillers(top + "/proj");
    addFillers(top + "/proj/lib");
    addFillers(top + "/proj/app");
    writeFile(top + "/outside.txt", "zebrafoo outside the project\n");
    writeFile(top + "/proj/root.txt", "zebrafoo at project root\n");
    writeFile(top + "/proj/lib/in_lib.txt", "zebrafoo in lib\n");
    writeFile(top + "/proj/app/in_app.txt", "zebrafoo in app\n");
    ASSERT_TRUE(commitAll(top));

    // Project root below the git top level.
    expectSameRows(top + "/proj", kPat, QJsonObject(),
                   {QStringLiteral("root.txt"), QStringLiteral("lib/in_lib.txt")});
    // A lane narrowing the search.
    const QSet<QString> laned = expectSameRows(
        top + "/proj", kPat, jreq({{QStringLiteral("lane"), QStringLiteral("lib")}}),
        {QStringLiteral("lib/in_lib.txt")});
    EXPECT_FALSE(filesOf(laned).contains(QStringLiteral("app/in_app.txt")))
        << "lane=lib must keep app/ out";
}

// --------------------------------------------------------------- INV-11 -----

TEST(workspace_search_timeout_fallback, patternNotInArgv) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = canon(tmp.path());
    ASSERT_TRUE(initRepo(root));
    addFillers(root);
    writeFile(root + "/dash.txt", "see --needle-x here\n");
    writeFile(root + "/quote.txt", "say \"zebrafoo\" now\n");
    ASSERT_TRUE(commitAll(root));
    expectSameRows(root, QStringLiteral("--needle-x"), QJsonObject(),
                   {QStringLiteral("dash.txt")});
    expectSameRows(root, QStringLiteral("say \"zebrafoo\""), QJsonObject(),
                   {QStringLiteral("quote.txt")});
}

TEST(workspace_search_timeout_fallback, patternNotInArgvSourceScrape) {
    const std::string dir = std::string(ANTS_SOURCE_DIR) + "/src/";
    const std::string pre = ants_test::slurpFile(dir + "searchprefilter.cpp");
    const std::string ws = ants_test::slurpFile(dir + "remotecontrol_workspace.cpp");
    ASSERT_FALSE(pre.empty()) << "src/searchprefilter.cpp not readable";
    ASSERT_FALSE(ws.empty()) << "src/remotecontrol_workspace.cpp not readable";
    for (const char *bad : {"QProcess", "QFile", "/bin/sh", "bash", "startCommand"})
        EXPECT_EQ(pre.find(bad), std::string::npos)
            << "searchprefilter.cpp must start no process and open no file: "
            << "found " << bad;
    const std::size_t at = ws.find("SearchPrefilter::");
    ASSERT_NE(at, std::string::npos)
        << "the fallback block in remotecontrol_workspace.cpp does not use "
           "SearchPrefilter yet";
    const std::string block = ws.substr(at, 12000);
    for (const char *bad : {"/bin/sh", "\"sh\"", "bash", "startCommand"})
        EXPECT_EQ(block.find(bad), std::string::npos)
            << "the fallback block must start git shell-less with an argv "
            << "list: found " << bad;
}
