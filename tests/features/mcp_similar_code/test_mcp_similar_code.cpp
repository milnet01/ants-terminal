// ANTS-1305 — feature-conformance test for similar_code. Live
// SimilarCode behaviour against a synthetic source tree + source-grep
// wiring contract. See spec.md.

#include "../../_support/expect.h"

#include "similarcode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_REMOTECONTROL_CPP_PATH
#error "SRC_REMOTECONTROL_CPP_PATH compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif
#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

void writeFile(const QString &root, const QString &rel, const QString &body) {
    const QString full = root + QLatin1Char('/') + rel;
    QDir().mkpath(QFileInfo(full).absolutePath());
    QFile f(full);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "setup-fail: cannot write %s\n",
                     full.toUtf8().constData());
        std::exit(2);
    }
    f.write(body.toUtf8());
    f.close();
}

const SimilarCode::Match *findByFile(const SimilarCode::Result &r,
                                     const QString &file) {
    for (const auto &m : r.matches)
        if (m.file == file) return &m;
    return nullptr;
}

bool tokensEqual(const QStringList &got, std::initializer_list<const char *> want) {
    if (got.size() != static_cast<int>(want.size())) return false;
    int i = 0;
    for (const char *w : want)
        if (got.at(i++) != QLatin1String(w)) return false;
    return true;
}

}  // namespace

TEST(McpSimilarCode, TokenizeAndJaccard) {
    expect_reset();

    expect(tokensEqual(SimilarCode::tokenize(
               QStringLiteral("class FooDialog : public QDialog")),
               {"class", "foodialog", "public", "qdialog"}),
           "tokenize: class shape splits as expected");
    expect(tokensEqual(SimilarCode::tokenize(
               QStringLiteral("void cmdBar(const QJsonObject&)")),
               {"void", "cmdbar", "const", "qjsonobject"}),
           "tokenize: function shape splits on punctuation");
    expect(tokensEqual(SimilarCode::tokenize(QStringLiteral("cmd_bar")),
               {"cmd", "bar"}),
           "tokenize: underscore is a separator");
    expect(SimilarCode::tokenize(QStringLiteral("a + b")).isEmpty(),
           "tokenize: all-1-char tokens dropped");

    const QStringList a{QStringLiteral("a"), QStringLiteral("b"),
                        QStringLiteral("c")};
    const QStringList b{QStringLiteral("b"), QStringLiteral("c"),
                        QStringLiteral("d")};
    expect(std::abs(SimilarCode::jaccard(a, b) - 0.5) < 1e-9,
           "jaccard: 2/4 == 0.5");
    expect(SimilarCode::jaccard({QStringLiteral("x")},
                                {QStringLiteral("y")}) == 0.0,
           "jaccard: disjoint == 0");
    expect(SimilarCode::jaccard({}, {}) == 0.0,
           "jaccard: empty/empty == 0");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpSimilarCode, LiveBehaviour) {
    expect_reset();

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // Two QDialog subclasses + one unrelated class in one header.
    writeFile(root, QStringLiteral("src/dialogs.h"),
              QStringLiteral("class RoadmapDialog : public QDialog {\n"   // L1
                             "};\n"
                             "class AuditDialog : public QDialog {\n"     // L3
                             "};\n"
                             "class PlainThing {\n"                       // L5
                             "};\n"));
    // A C++ member definition (FileOutline kind "func").
    writeFile(root, QStringLiteral("src/dialogs.cpp"),
              QStringLiteral("void RoadmapDialog::doRefresh() {\n"
                             "}\n"));
    // A Python module with a TOP-LEVEL def. FileOutline's rxPy anchors
    // at column 0, so only top-level def/class are extracted (an
    // indented method is not). Kept top-level so it is a real candidate.
    writeFile(root, QStringLiteral("app.py"),
              QStringLiteral("def compute(value):\n"
                             "    return value\n"));
    // Skipped dirs — a QDialog subclass that must never surface.
    const QString skipBody =
        QStringLiteral("class SkipMe : public QDialog {\n};\n");
    writeFile(root, QStringLiteral("build/skip.h"), skipBody);
    writeFile(root, QStringLiteral("node_modules/skip.h"), skipBody);
    writeFile(root, QStringLiteral(".hidden/skip.h"), skipBody);

    SimilarCode::Options def;  // all defaults (auto, cap 3)

    // INV — ranking: QDialog subclasses rank above the plain class.
    const auto q = SimilarCode::findSimilar(
        root, QStringLiteral("class FooDialog : public QDialog"), def);
    expect(q.ok, "ranking: ok");
    const auto *road = findByFile(q, QStringLiteral("src/dialogs.h"));
    expect(road != nullptr, "ranking: dialogs.h surfaced");
    // First two matches are the QDialog subclasses (score 0.6), both
    // kind=class, lang=cpp; PlainThing (0.2) ranks below.
    expect(q.matches.size() == 3, "ranking: all 3 score>0 classes returned");
    expect(!q.matches.isEmpty() &&
               q.matches.first().score > q.matches.last().score,
           "ranking: descending score order");
    expect(!q.matches.isEmpty() &&
               q.matches.first().kind == QStringLiteral("class") &&
               q.matches.first().lang == QStringLiteral("cpp"),
           "ranking: kind/lang from FileOutline");
    bool topTwoAreDialogs = q.matches.size() >= 2 &&
        q.matches.at(0).signature.contains(QStringLiteral("QDialog")) &&
        q.matches.at(1).signature.contains(QStringLiteral("QDialog"));
    expect(topTwoAreDialogs, "ranking: top two matches are QDialog subclasses");
    // Tie-break: RoadmapDialog (line 1) before AuditDialog (line 3).
    expect(q.matches.size() >= 2 &&
               q.matches.at(0).line < q.matches.at(1).line,
           "ranking: equal-score tie broken by (file,line) ascending");

    // INV — project-relative paths.
    bool relOk = true;
    for (const auto &m : q.matches)
        if (m.file.startsWith(QLatin1Char('/')) || m.file.contains(root))
            relOk = false;
    expect(relOk, "paths: project-relative (no temp-root prefix)");

    // INV — skipped dirs never contribute.
    const auto skip = SimilarCode::findSimilar(
        root, QStringLiteral("class SkipMe : public QDialog"), def);
    bool noSkip = true;
    for (const auto &m : skip.matches)
        if (m.signature.contains(QStringLiteral("SkipMe"))) noSkip = false;
    expect(noSkip, "skip: build/node_modules/dot-dir classes not matched");

    // INV — C++ member def is captured as kind func.
    const auto fn = SimilarCode::findSimilar(
        root, QStringLiteral("void RoadmapDialog doRefresh"), def);
    bool sawFunc = false;
    for (const auto &m : fn.matches)
        if (m.kind == QStringLiteral("func") &&
            m.file == QStringLiteral("src/dialogs.cpp")) sawFunc = true;
    expect(sawFunc, "extract: C++ member definition surfaces as kind=func");

    // INV — lang filter: py-only finds the python def, no C++.
    SimilarCode::Options pyOnly;
    pyOnly.lang = SimilarCode::Lang::Py;
    const auto py = SimilarCode::findSimilar(
        root, QStringLiteral("def compute(self)"), pyOnly);
    bool pyHit = false, cppLeak = false;
    for (const auto &m : py.matches) {
        if (m.lang == QStringLiteral("py")) pyHit = true;
        if (m.lang == QStringLiteral("cpp")) cppLeak = true;
    }
    expect(pyHit, "lang=py: python signature found");
    expect(!cppLeak, "lang=py: no C++ signatures leak in");

    // INV — score>0 only: a no-overlap shape returns empty.
    const auto none = SimilarCode::findSimilar(
        root, QStringLiteral("zzzznomatchzzzz wwwwnopezzz"), def);
    expect(none.ok && none.matches.isEmpty() && none.matchesTotal == 0,
           "empty: no token overlap → ok with empty matches");

    // INV — maxResults caps; matches_count pre-cap; truncated flips.
    SimilarCode::Options cap1;
    cap1.maxResults = 1;
    const auto capped = SimilarCode::findSimilar(
        root, QStringLiteral("class FooDialog : public QDialog"), cap1);
    expect(capped.matches.size() == 1, "cap: matches capped to maxResults");
    expect(capped.matchesTotal == 3, "cap: matchesTotal is pre-cap (3)");
    expect(capped.truncated, "cap: truncated set when capped");
    expect(!capped.matches.isEmpty() &&
               capped.matches.first().signature.contains(QStringLiteral("QDialog")),
           "cap: the single kept match is the best-ranked one");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpSimilarCode, HardCapAndBadArgs) {
    expect_reset();

    QTemporaryDir tmp;
    expect(tmp.isValid(), "setup: QTemporaryDir valid");
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();

    // 25 QDialog subclasses, one per file — exceeds the hard cap of 20.
    for (int i = 0; i < 25; ++i) {
        writeFile(root, QStringLiteral("src/w%1.h").arg(i),
                  QStringLiteral("class Widget%1 : public QDialog {\n};\n").arg(i));
    }
    SimilarCode::Options big;
    big.maxResults = 100;  // above the hard cap
    const auto r = SimilarCode::findSimilar(
        root, QStringLiteral("class X : public QDialog"), big);
    expect(r.matches.size() == 20, "hardcap: matches clamped to 20");
    expect(r.matchesTotal == 25, "hardcap: matchesTotal is the pre-cap 25");
    expect(r.truncated, "hardcap: truncated set");

    // bad_args refusals from the lib.
    const auto empty = SimilarCode::findSimilar(root, QString(), big);
    expect(!empty.ok && empty.code == QStringLiteral("bad_args"),
           "bad_args: empty shape refused");
    const auto noTok = SimilarCode::findSimilar(
        root, QStringLiteral("a + b"), big);
    expect(!noTok.ok && noTok.code == QStringLiteral("bad_args"),
           "bad_args: token-empty shape refused");
    const auto tooLong = SimilarCode::findSimilar(
        root, QString(513, QLatin1Char('x')), big);
    expect(!tooLong.ok && tooLong.code == QStringLiteral("bad_args"),
           "bad_args: over-512-char shape refused");

    EXPECT_EQ(0, expect_failures());
}

TEST(McpSimilarCode, WiringContract) {
    expect_reset();

    const std::string rcHdr = slurp(SRC_RC_HEADER);
    const std::string rcCpp = slurp(SRC_REMOTECONTROL_CPP_PATH);
    const std::string mwCpp = slurp(SRC_MAINWINDOW_CPP_PATH);
    const std::string ciCpp = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);

    expect(contains(rcHdr, "cmdSimilarCode"),
           "wiring: handler declared in remotecontrol.h");
    expect(contains(rcCpp, "RemoteControl::cmdSimilarCode"),
           "wiring: cmdSimilarCode defined");
    expect(contains(rcCpp, "\"similar-code\""),
           "wiring: IPC dispatch verb wired");

    expect(contains(mwCpp, "registerToolProvider(\"similar_code\""),
           "wiring: tool registered in mainwindow.cpp");

    expect(contains(ciCpp, "t[\"name\"] = \"similar_code\""),
           "wiring: tool descriptor present");
    expect(contains(ciCpp, "\"similar_code\"),       {600,  2500}"),
           "wiring: similar_code token-cost entry");
    // kindForName branch: QLatin1String form is unique to the bucket
    // map (the contract uses QStringLiteral); "pattern" return is the
    // only QStringLiteral("pattern") in the file.
    expect(contains(ciCpp, "QLatin1String(\"similar_code\")"),
           "wiring: kindForName has a similar_code branch");
    expect(contains(ciCpp, "QStringLiteral(\"pattern\")"),
           "wiring: kindForName pattern bucket return present");
    expect(contains(ciCpp,
               "if (toolName == QStringLiteral(\"similar_code\"))        return C::Required;"),
           "wiring: similar_code contract Required");

    EXPECT_EQ(0, expect_failures());
}
