// ANTS-3636 — DocCitations::check: resolve a doc's citations, read the cited
// text. See tests/features/doc_citations/spec.md for the invariant map and
// docs/specs/ANTS-3636.md for the contract.
//
// Every fixture is a QTemporaryDir standing in for a project root plus a seeded
// basenameIndex — the engine is Qt6::Core-pure, so no CodebaseIndex on disk and
// no MainWindow is involved.

#include "../../_support/expect.h"
#include "fixture.h"
#include "doccitations.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <unistd.h>

ANTS_TEST_SCOPE();

namespace {

// The fixture and response accessors live in fixture.h — ANTS-3654 added a
// second test file against this engine and a divergent second copy of the
// canonical-root logic would fail silently. See that header.
using namespace doccit_test;

// The six-status fixture, shared by INV-41 (which statuses carry `file_lines`)
// and INV-26 (the tally identities). Both need every bucket non-empty, and
// building it twice would let the two drift apart.
struct SixStatus {
    Fixture fx;
    QString docPath;
    DocCitations::Options opts;

    SixStatus() {
        fx.write(QStringLiteral("src/a.cpp"), "line one\nline two\n");
        fx.write(QStringLiteral("src/nul.cpp"), QByteArray("abc\0def\n", 8));
        // `:5` first, so it has no antecedent to inherit → unresolved.
        docPath = fx.doc(
            "start `:5`\n"
            "ok src/a.cpp:1\n"
            "missing src/gone.cpp:1\n"
            "past src/a.cpp:9999\n"
            "unreadable src/nul.cpp:1\n"
            "ambiguous dup.cpp:1\n"
            "prose `6.2:1`\n");
        opts.basenameIndex.insert(QStringLiteral("dup.cpp"),
                                  {QStringLiteral("src/one/dup.cpp"),
                                   QStringLiteral("src/two/dup.cpp")});
    }
};

}  // namespace

// INV-4 — a bare basename with exactly one indexed path resolves; two or more is
// `ambiguous`, carrying the candidate set instead of a path, sorted by UTF-16
// code unit and capped.
TEST(DocCitations, Inv4BasenameIndexUniqueAmbiguousAndCapped) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/uniq.cpp"), "one\n");
    const QString doc = fx.doc("uniq.cpp:1 and dup.cpp:1 and many.cpp:1\n");

    DocCitations::Options opts;
    opts.basenameIndex.insert(QStringLiteral("uniq.cpp"), {QStringLiteral("src/uniq.cpp")});
    // Seeded in the order a locale collation would produce ("_a" first, since a
    // collating locale ignores the underscore), so a pass proves the code-unit
    // sort ran rather than that insertion order happened to be right.
    opts.basenameIndex.insert(QStringLiteral("dup.cpp"),
                              {QStringLiteral("tests/_a/dup.cpp"), QStringLiteral("tests/Z/dup.cpp")});
    QStringList many;
    for (int i = 0; i < 10; ++i) many << QStringLiteral("src/d%1/many.cpp").arg(i);
    opts.basenameIndex.insert(QStringLiteral("many.cpp"), many);

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    expect(cites(r).size() == 3, "INV-4: three citations", render(r));

    expect(status(r, 0) == QStringLiteral("ok"), "INV-4: unique index hit resolves", render(r));
    expect(cite(r, 0).value(QStringLiteral("path")).toString() == QStringLiteral("src/uniq.cpp"),
           "INV-4: the indexed path is the resolved one", render(r));

    const QJsonObject amb = cite(r, 1);
    expect(amb.value(QStringLiteral("status")).toString() == QStringLiteral("ambiguous"),
           "INV-4: two matches is ambiguous", render(r));
    expect(!amb.contains(QStringLiteral("path")), "INV-4: ambiguous carries no path", render(r));
    expect(!amb.contains(QStringLiteral("text")), "INV-4: ambiguous carries no text", render(r));
    const QJsonArray cand = amb.value(QStringLiteral("candidates")).toArray();
    expect(cand.size() == 2 && cand.at(0).toString() == QStringLiteral("tests/Z/dup.cpp")
               && cand.at(1).toString() == QStringLiteral("tests/_a/dup.cpp"),
           "INV-4: candidates sorted by UTF-16 code unit, not locale", render(r));
    expect(amb.value(QStringLiteral("candidates_total")).toInt() == 2,
           "INV-4: candidates_total is the true match count", render(r));
    expect(!amb.contains(QStringLiteral("truncated_candidates")),
           "INV-4: no truncation flag under the cap", render(r));

    const QJsonObject capped = cite(r, 2);
    expect(capped.value(QStringLiteral("candidates")).toArray().size() == opts.maxCandidates,
           "INV-4: candidates capped at maxCandidates", render(r));
    expect(capped.value(QStringLiteral("candidates_total")).toInt() == 10,
           "INV-4: candidates_total reports past the cap", render(r));
    expect(capped.value(QStringLiteral("truncated_candidates")).toBool(),
           "INV-4: truncated_candidates set past the cap", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-5 — step 3 (repo-root fallback) and step 4 (unresolvable), with an index
// that answers nothing.
TEST(DocCitations, Inv5RepoRootFallbackAndUnresolvable) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("CMakeLists.txt"), "project(x)\n");
    const QString doc = fx.doc("see CMakeLists.txt:1 and nowhere.cpp:1\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
    expect(cites(r).size() == 1, "INV-5: only the repo-root file resolves", render(r));
    expect(status(r, 0) == QStringLiteral("ok"), "INV-5: step 3 resolves a repo-root file", render(r));
    expect(unparsed(r).size() == 1, "INV-5: the unindexed basename is unparsed", render(r));
    expect(unparsed(r).at(0).toObject().value(QStringLiteral("reason")).toString()
               == QStringLiteral("unresolvable"),
           "INV-5: step 4's reason", render(r));
    expect(r.value(QStringLiteral("basename_index_size")).toInt() == 0,
           "INV-5: an empty index reports size 0", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-6 — step 0 is lexical: an out-of-root citation costs zero filesystem
// calls. Both shapes are directory-bearing on purpose; the grammar needs a dot
// in the final segment, so a dotless path would never reach the ladder at all.
TEST(DocCitations, Inv6OutOfRootRejectedWithoutStat) {
    expect_reset();
    Fixture fx;
    const QString doc = fx.doc("abs /etc/hosts.conf:1 and rel ../../outside.h:3\n");

    DocCitations::Probe probe;
    DocCitations::Options opts;
    opts.probe = &probe;

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    expect(cites(r).isEmpty(), "INV-6: no citations", render(r));
    expect(unparsed(r).size() == 2, "INV-6: both land in unparsed", render(r));
    for (const QJsonValue &v : unparsed(r))
        expect(v.toObject().value(QStringLiteral("reason")).toString()
                   == QStringLiteral("out_of_root"),
               "INV-6: reason is out_of_root", render(r));
    expect(probe.stats == 0, "INV-6: zero filesystem calls",
           QStringLiteral("stats=%1").arg(probe.stats));
    EXPECT_EQ(0, expect_failures());
}

// INV-7 — a continuation inherits the antecedent's resolved path and nothing
// else: it runs the read step itself and takes its own status.
TEST(DocCitations, Inv7ContinuationInheritsPathOnly) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\n");

    // An unparsed token between the antecedent and the continuation, to prove it
    // is not itself eligible to be inherited from.
    {
        const QString doc = fx.doc("src/a.cpp:1 then `6.2:9` then `:2`\n");
        const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
        expect(cites(r).size() == 2, "INV-7: two citations, the prose token unparsed", render(r));
        const QJsonObject cont = cite(r, 1);
        expect(cont.value(QStringLiteral("path")).toString() == QStringLiteral("src/a.cpp"),
               "INV-7: inherits the citation, not the unparsed token", render(r));
        expect(cont.value(QStringLiteral("inherited_path")).toBool(),
               "INV-7: inherited_path set", render(r));
        expect(cont.value(QStringLiteral("status")).toString() == QStringLiteral("ok"),
               "INV-7: continuation reads for itself", render(r));
        expect(textOf(cont) == QStringList{QStringLiteral("two")},
               "INV-7: its own locus, not the antecedent's", render(r));
    }
    // A missing antecedent yields a second missing_file, not an ok.
    {
        const QString doc = fx.doc("src/gone.cpp:1 then `:2`\n");
        const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
        expect(cites(r).size() == 2 && status(r, 1) == QStringLiteral("missing_file"),
               "INV-7: status is re-derived, not inherited", render(r));
        expect(cite(r, 1).value(QStringLiteral("inherited_path")).toBool(),
               "INV-7: still flagged as inherited", render(r));
    }
    // An ambiguous antecedent has no path to give: the candidate set propagates.
    {
        const QString doc = fx.doc("dup.cpp:1 then `:7`\n");
        DocCitations::Options opts;
        opts.basenameIndex.insert(QStringLiteral("dup.cpp"),
                                  {QStringLiteral("a/dup.cpp"), QStringLiteral("b/dup.cpp")});
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        const QJsonObject cont = cite(r, 1);
        expect(cont.value(QStringLiteral("status")).toString() == QStringLiteral("ambiguous"),
               "INV-7: ambiguity propagates", render(r));
        expect(cont.value(QStringLiteral("candidates")).toArray()
                   == cite(r, 0).value(QStringLiteral("candidates")).toArray(),
               "INV-7: the same candidate set", render(r));
        expect(!cont.contains(QStringLiteral("path")),
               "INV-7: an ambiguous continuation names no path", render(r));
        expect(cont.value(QStringLiteral("inherited_path")).toBool(),
               "INV-7: inherited_path set on the ambiguous chain too", render(r));
    }
    // Nothing to inherit.
    {
        const QString doc = fx.doc("leading `:5` with nothing before it\n");
        const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
        expect(cites(r).size() == 1 && status(r, 0) == QStringLiteral("unresolved"),
               "INV-7: no antecedent is unresolved", render(r));
        expect(!cite(r, 0).contains(QStringLiteral("inherited_path")),
               "INV-7: unresolved is not flagged inherited", render(r));
    }
    EXPECT_EQ(0, expect_failures());
}

// ANTS-3801 — an inherited path whose chain crossed an UNRESOLVABLE citation
// never reports out_of_range, because the range check would be running against
// a file the document never named at that point.
//
// DOOM Ants' shape: a shader citation the resolver cannot place, followed by a
// bare continuation. The continuation used to inherit whatever resolved
// EARLIER — a short unrelated header — and be range-checked against it,
// producing a real-looking out_of_range. Seven fabricated entries in one run.
// It matters more than an ordinary false positive: /cold-eyes runs this verb as
// a deterministic pre-pass and puts findings into the lane brief as
// already-logged fact, so a fabricated one invites an edit to a citation that
// was correct.
//
// The narrowness is the design. INV-7 above requires an unparsed token NOT to
// break the chain (its fixture uses `6.2:9`, prose noise), so clearing the
// antecedent outright is wrong and was measured to redden that test. Only
// out_of_range, only on an inherited path, only after a failed resolve.
TEST(DocCitations, Ants3801InheritedOutOfRangeAfterUnresolvedIsNotReported) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/short.h"), "one\ntwo\ntwo\n");   // 3 lines

    // short.h resolves; the shader does not; the continuation belongs to the
    // shader but would inherit short.h and be checked against its 3 lines.
    //
    // The shader is a BARE BASENAME, which is how DOOM cited it and is what
    // makes it Unresolvable: a path carrying a directory separator resolves to
    // missing_file instead, and missing_file legitimately DOES set the
    // antecedent. Continuations are backticked, the form the scanner reads.
    const QString doc = fx.doc(
        "see src/short.h:2 and then `pathtrace.comp:1080` and `:1209`\n");
    const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});

    const QJsonObject cont = cite(r, cites(r).size() - 1);
    expect(cont.value(QStringLiteral("status")).toString() != QStringLiteral("out_of_range"),
           "ANTS-3801: a continuation after an unresolvable citation must not be "
           "range-checked against the last file that happened to resolve",
           render(r));
    expect(cont.value(QStringLiteral("status")).toString() == QStringLiteral("unresolved"),
           "ANTS-3801: it reports unresolved — the citation genuinely cannot be "
           "checked, and silence would read as a pass",
           render(r));
    expect(!cont.contains(QStringLiteral("path")),
           "ANTS-3801: and it names no path, because the inherited one is not "
           "the file the document meant",
           render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-8 — a fence is a context break, so it clears the antecedent rather than
// merely being skipped.
TEST(DocCitations, Inv8FenceResetsAntecedent) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\n");
    const QString doc = fx.doc(
        "src/a.cpp:1\n"
        "```\n"
        "sample\n"
        "```\n"
        "after `:9`\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
    expect(cites(r).size() == 2, "INV-8: two citations", render(r));
    expect(status(r, 1) == QStringLiteral("unresolved"),
           "INV-8: the fence reset the tracker", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-11 — the three read-side failure statuses, none emitting text. The empty
// target is § 2.6's no-special-case claim.
TEST(DocCitations, Inv11StatusesWithoutText) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\n");
    fx.write(QStringLiteral("src/big.cpp"), QByteArray(400, 'x') + "\n");
    fx.write(QStringLiteral("src/nul.cpp"), QByteArray("abc\0def\n", 8));
    fx.write(QStringLiteral("src/empty.cpp"), QByteArray());
    const QString locked = fx.write(QStringLiteral("src/locked.cpp"), "secret\n");
    const bool canTestPerms = ::geteuid() != 0
                              && QFile::setPermissions(locked, QFile::Permissions{});

    const QString doc = fx.doc(
        "src/gone.cpp:1\n"
        "src/a.cpp:9999\n"
        "src/locked.cpp:1\n"
        "src/big.cpp:1\n"
        "src/nul.cpp:1\n"
        "src/empty.cpp:1\n");

    DocCitations::Options opts;
    opts.maxTargetBytes = 64;   // big.cpp is over it; every other target is under
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);

    expect(cites(r).size() == 6, "INV-11: six citations", render(r));
    expect(status(r, 0) == QStringLiteral("missing_file"), "INV-11: absent file", render(r));
    expect(status(r, 1) == QStringLiteral("out_of_range"), "INV-11: start past EOF", render(r));
    if (canTestPerms)
        expect(status(r, 2) == QStringLiteral("read_error"), "INV-11: unreadable file", render(r));
    expect(status(r, 3) == QStringLiteral("read_error"), "INV-11: over maxTargetBytes", render(r));
    expect(status(r, 4) == QStringLiteral("read_error"), "INV-11: NUL byte is not decodable", render(r));
    expect(status(r, 5) == QStringLiteral("out_of_range"),
           "INV-11: line 1 of an empty file points at nothing", render(r));
    expect(cite(r, 5).value(QStringLiteral("file_lines")).toInt() == 0,
           "INV-11: an empty target has zero lines", render(r));

    for (int i = 0; i < 6; ++i)
        expect(!cite(r, i).contains(QStringLiteral("text")),
               "INV-11: no text on any failing status", render(r));
    QFile::setPermissions(locked, QFile::ReadOwner | QFile::WriteOwner);
    EXPECT_EQ(0, expect_failures());
}

// INV-12 — max_range_lines bounds the emitted lines and nothing else.
TEST(DocCitations, Inv12RangeTruncated) {
    expect_reset();
    Fixture fx;
    QByteArray ten;
    for (int i = 1; i <= 10; ++i) ten += QByteArray::number(i) + "\n";
    fx.write(QStringLiteral("src/ten.cpp"), ten);
    const QString doc = fx.doc("src/ten.cpp:1-10\n");

    DocCitations::Options opts;
    opts.maxRangeLines = 3;
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonObject c = cite(r, 0);
    expect(textOf(c).size() == 3, "INV-12: max_range_lines lines emitted", render(r));
    expect(c.value(QStringLiteral("range_truncated")).toBool(), "INV-12: flagged", render(r));
    expect(c.value(QStringLiteral("end_line")).toInt() == 10,
           "INV-12: end_line still echoes what the doc wrote", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-15 — a shape-matching token that resolves nowhere is `unparsed[]`, and its
// `raw` is the grammar's capture without delimiters.
TEST(DocCitations, Inv15UnresolvableRawAndClip) {
    expect_reset();
    Fixture fx;
    const QString longName = QStringLiteral("z").repeated(200) + QStringLiteral(".cpp");
    const QString doc = fx.doc(
        QStringLiteral("prose `6.2:1` here\n%1:1\nsrc.old/README:12\n").arg(longName).toUtf8());

    DocCitations::Options opts;
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    expect(cites(r).isEmpty(), "INV-15: none of these is a citation", render(r));
    expect(unparsed(r).size() == 2,
           "INV-15: the dotless final segment is reported by nobody", render(r));

    const QJsonObject first = unparsed(r).at(0).toObject();
    expect(first.value(QStringLiteral("raw")).toString() == QStringLiteral("6.2:1"),
           "INV-15: raw is the capture, not the backticks", render(r));
    expect(!first.contains(QStringLiteral("raw_clipped")),
           "INV-15: a short raw is not flagged clipped", render(r));

    const QJsonObject clipped = unparsed(r).at(1).toObject();
    expect(clipped.value(QStringLiteral("raw")).toString().size() == opts.maxRawChars,
           "INV-15: raw clipped to maxRawChars", render(r));
    expect(clipped.value(QStringLiteral("raw_clipped")).toBool(),
           "INV-15: raw_clipped set when the cap bites", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-16 — no writes, no cross-call state.
TEST(DocCitations, Inv16NoWritesNoState) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\n");
    const QString doc = fx.doc("src/a.cpp:1 and src/gone.cpp:2 and `6.2:1`\n");

    const QStringList before = QDir(fx.root).entryList(QDir::AllEntries | QDir::Hidden
                                                       | QDir::NoDotAndDotDot);
    const QJsonObject one = DocCitations::check(fx.root, doc, DocCitations::Options{});
    const QJsonObject two = DocCitations::check(fx.root, doc, DocCitations::Options{});
    const QStringList after = QDir(fx.root).entryList(QDir::AllEntries | QDir::Hidden
                                                      | QDir::NoDotAndDotDot);

    expect(one == two, "INV-16: identical returns for an unchanged fixture",
           render(one) + QStringLiteral(" != ") + render(two));
    expect(before == after, "INV-16: no new files", after.join(QLatin1Char(',')));
    EXPECT_EQ(0, expect_failures());
}

// INV-20 — the line cache reads a cached target once; past its bounds a target
// is re-read per citation and still yields the right text.
TEST(DocCitations, Inv20LineCacheOpenCounts) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "alpha\nbeta\n");
    fx.write(QStringLiteral("src/b.cpp"), "gamma\ndelta\n");

    {
        const QString doc = fx.doc("src/a.cpp:1 src/a.cpp:1 src/a.cpp:2 src/a.cpp:1 src/a.cpp:2\n");
        DocCitations::Probe probe;
        DocCitations::Options opts;
        opts.probe = &probe;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(cites(r).size() == 5, "INV-20: five citations", render(r));
        expect(probe.opens == 1, "INV-20: one open for five citations of one file",
               QStringLiteral("opens=%1").arg(probe.opens));
    }
    // Below the cache's byte budget nothing is retained, so each citation re-reads
    // — and must still emit the correct text, which an implementation that
    // abandons the accumulated emit-lines would get wrong.
    {
        const QString doc = fx.doc("src/a.cpp:1 and src/a.cpp:2\n");
        DocCitations::Probe probe;
        DocCitations::Options opts;
        opts.probe = &probe;
        opts.maxCacheBytes = 1;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(probe.opens == 2, "INV-20: uncached targets re-read per citation",
               QStringLiteral("opens=%1").arg(probe.opens));
        expect(textOf(cite(r, 0)) == QStringList{QStringLiteral("alpha")},
               "INV-20: correct text on the first uncached read", render(r));
        expect(textOf(cite(r, 1)) == QStringList{QStringLiteral("beta")},
               "INV-20: correct text on the second", render(r));
    }
    // One cache slot, two targets alternating: the second never gets a slot.
    {
        const QString doc = fx.doc("src/a.cpp:1 src/b.cpp:1 src/a.cpp:2 src/b.cpp:2\n");
        DocCitations::Probe probe;
        DocCitations::Options opts;
        opts.probe = &probe;
        opts.maxCacheFiles = 1;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(probe.opens == 3, "INV-20: no eviction — the uncached target re-reads",
               QStringLiteral("opens=%1 %2").arg(probe.opens).arg(render(r)));
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-23 — a trailing \r never reaches the emitted text. (The other half of this
// invariant, the anchor comparison, lands with ANTS-3654.)
TEST(DocCitations, Inv23CarriageReturnStripped) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/crlf.cpp"), "int parseHeader\r\nnext\r\n");
    const QString doc = fx.doc("src/crlf.cpp:1\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
    const QStringList text = textOf(cite(r, 0));
    expect(text == QStringList{QStringLiteral("int parseHeader")},
           "INV-23: no trailing \\r in emitted text", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-25 — gate G canonicalises every component, so a symlinked *directory*
// cannot smuggle an out-of-root target into the response.
TEST(DocCitations, Inv25GateGCanonicalises) {
    expect_reset();
    Fixture fx;
    QTemporaryDir outside;
    const QString outsideRoot = QFileInfo(outside.path()).canonicalFilePath();
    {
        QFile f(outsideRoot + QStringLiteral("/foo.cpp"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("SECRET_OUTSIDE_CONTENT\n");
        f.close();
    }
    QDir().mkpath(fx.root + QStringLiteral("/src"));
    ASSERT_TRUE(QFile::link(outsideRoot, fx.root + QStringLiteral("/src/link")));
    fx.write(QStringLiteral("src/real.cpp"), "in root\n");
    ASSERT_TRUE(QFile::link(fx.root + QStringLiteral("/src/real.cpp"),
                            fx.root + QStringLiteral("/src/alias.cpp")));

    const QString doc = fx.doc("src/link/foo.cpp:1 and src/alias.cpp:1\n");
    const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});

    expect(cites(r).size() == 1, "INV-25: only the in-root symlink resolves", render(r));
    expect(status(r, 0) == QStringLiteral("ok"), "INV-25: a wholly in-root symlink is fine", render(r));
    expect(unparsed(r).size() == 1
               && unparsed(r).at(0).toObject().value(QStringLiteral("reason")).toString()
                      == QStringLiteral("out_of_root"),
           "INV-25: the escaping component is rejected", render(r));
    expect(!render(r).contains(QStringLiteral("SECRET_OUTSIDE_CONTENT")),
           "INV-25: the out-of-root target was never opened", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-27 — end_clamped and range_truncated answer different questions and never
// suppress each other; with a long line, text_clipped joins them.
TEST(DocCitations, Inv27EndClampedIndependentOfTruncation) {
    expect_reset();
    Fixture fx;
    QByteArray ten;
    for (int i = 1; i <= 10; ++i)
        ten += (i == 2 ? QByteArray(300, 'y') : QByteArray::number(i)) + "\n";
    fx.write(QStringLiteral("src/ten.cpp"), ten);
    const QString doc = fx.doc("src/ten.cpp:1-9999\n");

    {
        DocCitations::Options opts;
        opts.maxRangeLines = 20;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        const QJsonObject c = cite(r, 0);
        expect(c.value(QStringLiteral("status")).toString() == QStringLiteral("ok"),
               "INV-27: a range past EOF is not a failure", render(r));
        expect(textOf(c).size() == 10, "INV-27: text stops at EOF", render(r));
        expect(c.value(QStringLiteral("end_line")).toInt() == 9999,
               "INV-27: end_line echoes what the doc wrote", render(r));
        expect(c.value(QStringLiteral("end_clamped")).toBool(), "INV-27: end_clamped set", render(r));
        expect(!c.contains(QStringLiteral("range_truncated")),
               "INV-27: clamping alone does not set range_truncated", render(r));
    }
    {
        DocCitations::Options opts;
        opts.maxRangeLines = 3;
        opts.maxTextBytes  = 20;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        const QJsonObject c = cite(r, 0);
        expect(c.value(QStringLiteral("end_clamped")).toBool()
                   && c.value(QStringLiteral("range_truncated")).toBool()
                   && c.value(QStringLiteral("text_clipped")).toBool(),
               "INV-27: all three range flags may fire on one citation", render(r));
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-30 — doc_lines is the truth, scanned_lines is what was looked at.
TEST(DocCitations, Inv30DocLinesVersusScannedLines) {
    expect_reset();
    Fixture fx;
    QByteArray body;
    for (int i = 1; i <= 30; ++i) body += "line " + QByteArray::number(i) + "\n";
    const QString doc = fx.doc(body);

    DocCitations::Options opts;
    opts.maxDocLines = 10;   // below the handler's clamp floor: a direct engine call
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    expect(r.value(QStringLiteral("doc_lines")).toInt() == 30, "INV-30: true length", render(r));
    expect(r.value(QStringLiteral("scanned_lines")).toInt() == 10, "INV-30: scanned prefix", render(r));
    expect(r.value(QStringLiteral("truncated")).toBool(), "INV-30: truncated set", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-34 — the three branches of the directory-bearing / bare-basename
// asymmetry, including step 2's else, which is the one an implementation is
// likeliest to route to step 4.
TEST(DocCitations, Inv34DirectoryAndStaleIndexAsymmetry) {
    expect_reset();
    Fixture fx;
    QDir().mkpath(fx.root + QStringLiteral("/src/x.d"));
    const QString doc = fx.doc("src/x.d:1 and x.d:1 and stale.cpp:1\n");

    DocCitations::Options opts;
    opts.basenameIndex.insert(QStringLiteral("stale.cpp"), {QStringLiteral("src/stale.cpp")});
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);

    expect(cites(r).size() == 2, "INV-34: two citations, one unparsed", render(r));
    expect(status(r, 0) == QStringLiteral("missing_file"),
           "INV-34: a directory-bearing citation naming a directory is missing_file", render(r));
    expect(!cite(r, 0).contains(QStringLiteral("file_lines"))
               && !cite(r, 0).contains(QStringLiteral("text")),
           "INV-34: the directory was never opened", render(r));
    expect(status(r, 1) == QStringLiteral("missing_file"),
           "INV-34: step 2's else is a definite claim, not unresolvable", render(r));
    expect(unparsed(r).size() == 1
               && unparsed(r).at(0).toObject().value(QStringLiteral("reason")).toString()
                      == QStringLiteral("unresolvable"),
           "INV-34: the bare directory name falls through step 3 to step 4", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-37 — a truncated index looks healthy from the map alone, so the flag has
// to be passed in.
TEST(DocCitations, Inv37IndexTruncatedFlagIsPassedIn) {
    expect_reset();
    Fixture fx;
    const QString doc = fx.doc("nothing here\n");

    DocCitations::Options plain;
    plain.basenameIndex.insert(QStringLiteral("a.cpp"), {QStringLiteral("src/a.cpp")});
    DocCitations::Options flagged = plain;
    flagged.basenameIndexTruncated = true;

    const QJsonObject a = DocCitations::check(fx.root, doc, plain);
    const QJsonObject b = DocCitations::check(fx.root, doc, flagged);
    expect(a.value(QStringLiteral("basename_index_size"))
               == b.value(QStringLiteral("basename_index_size")),
           "INV-37: the same index reports the same size", render(a) + render(b));
    expect(!a.contains(QStringLiteral("basename_index_truncated")),
           "INV-37: absent when the index is whole", render(a));
    expect(b.value(QStringLiteral("basename_index_truncated")).toBool(),
           "INV-37: set from Options, never inferred from size", render(b));
    EXPECT_EQ(0, expect_failures());
}

// INV-41 — file_lines belongs to exactly the two statuses that established one.
TEST(DocCitations, Inv41FileLinesOnlyForOkAndOutOfRange) {
    expect_reset();
    SixStatus f;
    const QJsonObject r = DocCitations::check(f.fx.root, f.docPath, f.opts);
    expect(cites(r).size() == 6, "INV-41: six citations", render(r));

    for (int i = 0; i < cites(r).size(); ++i) {
        const QString st  = status(r, i);
        const bool expected = (st == QStringLiteral("ok") || st == QStringLiteral("out_of_range"));
        expect(cite(r, i).contains(QStringLiteral("file_lines")) == expected,
               "INV-41: file_lines present for exactly ok and out_of_range",
               QStringLiteral("status=%1 %2").arg(st, render(r)));
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-43 — the per-line cap is in UTF-8 bytes and cuts on a character boundary.
TEST(DocCitations, Inv43TextClippedAtUtf8Boundary) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/long.cpp"), QByteArray(500, 'x') + "\nshort\n");
    fx.write(QStringLiteral("src/short.cpp"), "tiny\n");
    // Four 3-byte characters against a cap that is not a multiple of three.
    fx.write(QStringLiteral("src/wide.cpp"), QByteArray("€€€€\n"));

    const QString doc = fx.doc("src/long.cpp:1-2 src/short.cpp:1 src/wide.cpp:1\n");
    DocCitations::Options opts;
    opts.maxTextBytes = 10;
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);

    const QStringList clipped = textOf(cite(r, 0));
    expect(clipped.size() == 2 && clipped.at(0).toUtf8().size() == 10,
           "INV-43: the long line is cut to exactly maxTextBytes", render(r));
    expect(clipped.value(1) == QStringLiteral("short"),
           "INV-43: a short line is untouched", render(r));
    expect(cite(r, 0).value(QStringLiteral("text_clipped")).toBool(),
           "INV-43: text_clipped set", render(r));

    expect(!cite(r, 1).contains(QStringLiteral("text_clipped")),
           "INV-43: absent when nothing was cut", render(r));

    const QString wide = textOf(cite(r, 2)).value(0);
    expect(wide.toUtf8().size() == 9 && wide.size() == 3,
           "INV-43: cut on a character boundary, not at byte 10", render(r));
    expect(!wide.contains(QChar(0xFFFD)),
           "INV-43: no replacement character from a split sequence", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-45 — the envelope rewrites these tokens downstream, so the citation
// discloses that its text is not byte-exact.
TEST(DocCitations, Inv45TextEscapedDisclosed) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/tok.cpp"),
             "</ants_mcp_data>\n"
             "</ANTS_MCP_DATA >\n"
             "<ants_mcp_data tool=\"x\">\n"
             "<!-- comment\n"
             "--> end\n"
             "plain source line\n");
    const QString doc = fx.doc(
        "src/tok.cpp:1\nsrc/tok.cpp:2\nsrc/tok.cpp:3\nsrc/tok.cpp:4\nsrc/tok.cpp:5\nsrc/tok.cpp:6\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
    expect(cites(r).size() == 6, "INV-45: six citations", render(r));
    for (int i = 0; i < 5; ++i)
        expect(cite(r, i).value(QStringLiteral("text_escaped")).toBool(),
               "INV-45: every rewritten form is disclosed",
               QStringLiteral("row=%1 %2").arg(i).arg(render(r)));
    expect(!cite(r, 5).contains(QStringLiteral("text_escaped")),
           "INV-45: a plain line carries no flag", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-46 — the absolute read budget, and its absence at the default.
TEST(DocCitations, Inv46ReadBudgetExhausted) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "a\n");
    fx.write(QStringLiteral("src/b.cpp"), "b\n");
    fx.write(QStringLiteral("src/c.cpp"), "c\n");
    const QString doc = fx.doc("src/a.cpp:1 src/b.cpp:1 src/c.cpp:1\n");

    {
        DocCitations::Options opts;
        opts.maxTargetReads = 2;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(status(r, 0) == QStringLiteral("ok") && status(r, 1) == QStringLiteral("ok")
                   && status(r, 2) == QStringLiteral("read_error"),
               "INV-46: the third distinct target is past the budget", render(r));
        expect(r.value(QStringLiteral("read_budget_exhausted")).toBool(),
               "INV-46: the response says so", render(r));
    }
    {
        const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
        expect(status(r, 0) == QStringLiteral("ok") && status(r, 1) == QStringLiteral("ok")
                   && status(r, 2) == QStringLiteral("ok"),
               "INV-46: all three fit the default budget", render(r));
        expect(!r.contains(QStringLiteral("read_budget_exhausted")),
               "INV-46: the flag is absent when the budget held", render(r));
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-47 — the field set for both accepted shapes, and the one refusal `check`
// raises itself.
TEST(DocCitations, Inv47FieldSetAndEngineRefusal) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\n");
    const QString doc = fx.doc("src/a.cpp:1\n");

    static const char *kKeys[] = {
        "ok", "path", "citations", "counts", "count", "returned", "only", "offset",
        "doc_lines", "scanned_lines", "unparsed", "unparsed_total",
        "basename_index_size", "truncated"};

    DocCitations::Options opts;
    opts.basenameIndex.insert(QStringLiteral("a.cpp"), {QStringLiteral("src/a.cpp")});

    const QJsonObject present = DocCitations::check(fx.root, doc, opts);
    for (const char *k : kKeys)
        expect(present.contains(QLatin1String(k)), "INV-47: key present on a real doc",
               QStringLiteral("%1 %2").arg(QLatin1String(k), render(present)));

    // A well-formed in-root path that does not exist: ok, and every value
    // predictable — presence alone would pass an implementation that invented
    // the numbers.
    const QJsonObject absent =
        DocCitations::check(fx.root, fx.root + QStringLiteral("/nope.md"), opts);
    for (const char *k : kKeys)
        expect(absent.contains(QLatin1String(k)), "INV-47: same key set for a missing doc",
               QStringLiteral("%1 %2").arg(QLatin1String(k), render(absent)));
    expect(absent.value(QStringLiteral("ok")).toBool(), "INV-47: a missing doc is not a refusal",
           render(absent));
    expect(absent.value(QStringLiteral("path")).toString() == QStringLiteral("nope.md"),
           "INV-47: path echoed", render(absent));
    expect(cites(absent).isEmpty() && unparsed(absent).isEmpty(),
           "INV-47: both arrays empty", render(absent));
    for (const char *k : {"count", "returned", "doc_lines", "scanned_lines", "unparsed_total"})
        expect(absent.value(QLatin1String(k)).toInt() == 0, "INV-47: zeroed",
               QStringLiteral("%1 %2").arg(QLatin1String(k), render(absent)));
    for (const QString &k : absent.value(QStringLiteral("counts")).toObject().keys())
        expect(absent.value(QStringLiteral("counts")).toObject().value(k).toInt() == 0,
               "INV-47: every count zeroed", QStringLiteral("%1 %2").arg(k, render(absent)));
    expect(absent.value(QStringLiteral("basename_index_size")).toInt() == 1,
           "INV-47: the index size is real, not zeroed with the rest", render(absent));
    expect(absent.value(QStringLiteral("truncated")).toBool() == false
               && absent.contains(QStringLiteral("truncated")),
           "INV-47: truncated present and false", render(absent));

    // The refusal the handler cannot raise without decoding the doc itself.
    const QString nulDoc = fx.write(QStringLiteral("nul.md"), QByteArray("a\0b\n", 4));
    const QJsonObject refused = DocCitations::check(fx.root, nulDoc, opts);
    expect(!refused.value(QStringLiteral("ok")).toBool()
               && refused.value(QStringLiteral("code")).toString() == QStringLiteral("read_failed"),
           "INV-47: an undecodable doc is the engine's own refusal", render(refused));
    EXPECT_EQ(0, expect_failures());
}

// INV-17 — the two resumable caps drop whole trailing entries and say where to
// resume; maxUnparsed drops entries with no way to fetch them, and says that
// instead. The first two are deliberately indistinguishable from each other.
TEST(DocCitations, Inv17ResumableAndNonResumableCaps) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\nthree\nfour\nfive\n");
    QByteArray body;
    for (int i = 1; i <= 5; ++i) body += "src/a.cpp:" + QByteArray::number(i) + "\n";
    for (int i = 0; i < 5; ++i) body += "prose `6." + QByteArray::number(i) + ":1`\n";
    const QString doc = fx.doc(body);

    // maxCitations
    {
        DocCitations::Options opts;
        opts.maxCitations = 2;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(r.value(QStringLiteral("returned")).toInt() == 2, "INV-17: page is the cap", render(r));
        expect(r.value(QStringLiteral("truncated")).toBool(), "INV-17: truncated set", render(r));
        expect(r.value(QStringLiteral("next_offset")).toInt() == 2,
               "INV-17: next_offset == offset + returned", render(r));
        for (int i = 0; i < 2; ++i)
            expect(cite(r, i).contains(QStringLiteral("status"))
                       && cite(r, i).contains(QStringLiteral("raw")),
                   "INV-17: no half-populated entry", render(r));

        opts.offset = r.value(QStringLiteral("next_offset")).toInt();
        const QJsonObject tail = DocCitations::check(fx.root, doc, opts);
        expect(tail.value(QStringLiteral("returned")).toInt() == 2
                   && cite(tail, 0).value(QStringLiteral("start_line")).toInt() == 3,
               "INV-17: the next page continues where the first stopped", render(tail));
    }
    // max_bytes
    {
        DocCitations::Options opts;
        opts.maxBytes = 700;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(r.value(QStringLiteral("truncated")).toBool()
                   && r.contains(QStringLiteral("next_offset")),
               "INV-17: a byte squeeze is resumable too", render(r));
        expect(r.value(QStringLiteral("next_offset")).toInt()
                   == r.value(QStringLiteral("returned")).toInt(),
               "INV-17: next_offset == offset + returned at offset 0", render(r));
        expect(unparsed(r).size() == 5,
               "INV-17: max_bytes never trims unparsed[]", render(r));
    }
    // maxUnparsed — truncated, but nothing to resume with
    {
        DocCitations::Options opts;
        opts.maxUnparsed = 2;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(unparsed(r).size() == 2, "INV-17: unparsed[] capped", render(r));
        expect(r.value(QStringLiteral("unparsed_total")).toInt() == 5,
               "INV-17: the true count survives the cap", render(r));
        expect(r.value(QStringLiteral("truncated")).toBool(), "INV-17: truncated set", render(r));
        expect(!r.contains(QStringLiteral("next_offset")),
               "INV-17: that array has no paging, so no next_offset", render(r));
        expect(r.value(QStringLiteral("unparsed_total")).toInt() > unparsed(r).size()
                   && !r.contains(QStringLiteral("next_offset")),
               "INV-17: distinguishable from the resumable caps by response fields alone",
               render(r));
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-26 — the three tally identities, with every bucket non-empty. Since
// ANTS-3654 the ok subset partitions THREE ways, and the third term has no
// counter of its own — it is derived from citations[] here. Asserting only
// anchor_missing + unchecked would pass on any fixture that happens to anchor
// nothing, which is exactly what this fixture does, so the identity would look
// verified while testing nothing.
TEST(DocCitations, Inv26TallyIdentities) {
    expect_reset();
    SixStatus f;
    const QJsonObject r = DocCitations::check(f.fx.root, f.docPath, f.opts);
    const QJsonObject counts = r.value(QStringLiteral("counts")).toObject();

    int sum = 0;
    for (const char *k : {"ok", "missing_file", "out_of_range", "read_error", "ambiguous",
                          "unresolved"}) {
        const int v = counts.value(QLatin1String(k)).toInt();
        expect(v > 0, "INV-26: every status bucket non-empty",
               QStringLiteral("%1 %2").arg(QLatin1String(k), render(r)));
        sum += v;
    }
    expect(sum == r.value(QStringLiteral("count")).toInt(),
           "INV-26: the six statuses partition count", render(r));
    int anchoredFound = 0;
    for (const QJsonValue &v : cites(r))
        if (v.toObject().value(QStringLiteral("anchor_found")).toBool()) ++anchoredFound;
    expect(counts.value(QStringLiteral("ok")).toInt()
               == counts.value(QStringLiteral("anchor_missing")).toInt()
                      + counts.value(QStringLiteral("unchecked")).toInt() + anchoredFound,
           "INV-26: the anchor overlays partition the ok subset", render(r));
    expect(counts.value(QStringLiteral("unparsed")).toInt()
               == r.value(QStringLiteral("unparsed_total")).toInt(),
           "INV-26: counts.unparsed mirrors unparsed_total", render(r));
    expect(counts.value(QStringLiteral("unparsed")).toInt() > 0,
           "INV-26: and that bucket is non-empty too", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-28 — omit-when-false, at all three levels, with the two exemptions.
TEST(DocCitations, Inv28FlagsOmittedWhenFalse) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\n");
    const QString doc = fx.doc("src/a.cpp:1 and prose `6.2:1`\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, DocCitations::Options{});
    const QJsonObject c = cite(r, 0);
    for (const char *k : {"range_truncated", "end_clamped", "inherited_path", "approximate",
                          "partial", "text_clipped", "text_escaped", "truncated_candidates"})
        expect(!c.contains(QLatin1String(k)), "INV-28: per-citation flag omitted when false",
               QStringLiteral("%1 %2").arg(QLatin1String(k), render(r)));
    expect(!unparsed(r).at(0).toObject().contains(QStringLiteral("raw_clipped")),
           "INV-28: per-entry flag omitted when false", render(r));
    for (const char *k : {"basename_index_truncated", "read_budget_exhausted",
                          "unterminated_fence", "next_offset"})
        expect(!r.contains(QLatin1String(k)), "INV-28: top-level flag omitted when false",
               QStringLiteral("%1 %2").arg(QLatin1String(k), render(r)));
    expect(r.contains(QStringLiteral("truncated"))
               && r.value(QStringLiteral("truncated")).toBool() == false,
           "INV-28: truncated is exempt and emitted false", render(r));
    expect(r.value(QStringLiteral("ok")).toBool(), "INV-28: ok always emitted", render(r));
    EXPECT_EQ(0, expect_failures());
}

// INV-31 — max_doc_lines is not resumable, but it does not suppress a
// resumable cap that also bound inside the prefix.
TEST(DocCitations, Inv31DocLineCapNotResumable) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\n");
    QByteArray body;
    for (int i = 1; i <= 30; ++i) {
        if (i == 5 || i == 25) body += "src/a.cpp:1\n";
        else body += "filler line " + QByteArray::number(i) + "\n";
    }
    const QString doc = fx.doc(body);

    DocCitations::Options opts;
    opts.maxDocLines = 10;
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    expect(r.value(QStringLiteral("count")).toInt() == 1,
           "INV-31: only the scanned prefix's citation is found", render(r));
    expect(!r.contains(QStringLiteral("next_offset")),
           "INV-31: the unscanned tail is not resumable", render(r));

    opts.offset = 1;
    const QJsonObject past = DocCitations::check(fx.root, doc, opts);
    expect(cites(past).isEmpty(), "INV-31: no offset reaches past scanned_lines", render(past));

    opts.offset = 0;
    opts.maxDocLines = 30;
    const QJsonObject whole = DocCitations::check(fx.root, doc, opts);
    expect(whole.value(QStringLiteral("count")).toInt() == 2,
           "INV-31: raising the cap finds both", render(whole));

    // Co-occurrence: a resumable cap binding inside a truncated prefix still
    // emits next_offset — suppressing it would strand the found citations.
    QByteArray dense;
    for (int i = 1; i <= 30; ++i)
        dense += (i <= 3 ? QByteArray("src/a.cpp:1\n") : QByteArray("filler\n"));
    const QString denseDoc = fx.write(QStringLiteral("dense.md"), dense);
    DocCitations::Options both;
    both.maxDocLines  = 10;
    both.maxCitations = 2;
    const QJsonObject r2 = DocCitations::check(fx.root, denseDoc, both);
    expect(r2.value(QStringLiteral("truncated")).toBool(), "INV-31: truncated", render(r2));
    expect(r2.value(QStringLiteral("scanned_lines")).toInt()
               < r2.value(QStringLiteral("doc_lines")).toInt(),
           "INV-31: the scan-level evidence stays set", render(r2));
    expect(r2.value(QStringLiteral("next_offset")).toInt() == 2,
           "INV-31: the resumable cause still emits next_offset", render(r2));
    EXPECT_EQ(0, expect_failures());
}

// INV-35 — max_bytes is a soft ceiling: one entry always ships, and
// next_offset strictly advances, or a paging caller loops forever.
TEST(DocCitations, Inv35AtLeastOneEntryAtTheFloor) {
    expect_reset();
    Fixture fx;
    QByteArray fat;
    for (int i = 0; i < 20; ++i) fat += QByteArray(2500, 'z') + "\n";
    for (const char *n : {"src/f1.cpp", "src/f2.cpp", "src/f3.cpp"})
        fx.write(QLatin1String(n), fat);

    // Ballast: enough unparsed entries to consume most of the byte budget, so
    // the first citation cannot fit in what remains.
    QByteArray body;
    for (int i = 0; i < 200; ++i)
        body += QByteArray(110, 'q') + QByteArray::number(i) + ".x:1\n";
    body += "src/f1.cpp:1-20\nsrc/f2.cpp:1-20\nsrc/f3.cpp:1-20\n";
    const QString doc = fx.doc(body);

    DocCitations::Options opts;
    opts.maxBytes      = 64 * 1024;   // the clamp floor
    opts.maxRangeLines = 20;          // the clamp ceiling
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);

    expect(r.value(QStringLiteral("returned")).toInt() == 1,
           "INV-35: one entry ships even when it does not fit", render(r).left(400));
    expect(r.value(QStringLiteral("truncated")).toBool(), "INV-35: truncated set",
           render(r).left(400));
    expect(r.value(QStringLiteral("next_offset")).toInt() == 1,
           "INV-35: next_offset strictly advances", render(r).left(400));
    expect(!unparsed(r).isEmpty(), "INV-35: unparsed[] still emitted intact",
           render(r).left(400));
    const int bytes = QJsonDocument(r).toJson(QJsonDocument::Compact).size();
    expect(bytes < opts.maxBytes + 200 * 1024,
           "INV-35: the overshoot is bounded by one entry, not unbounded",
           QStringLiteral("bytes=%1").arg(bytes));
    EXPECT_EQ(0, expect_failures());
}

// INV-38 — paging off the end is empty and terminal, not an error.
TEST(DocCitations, Inv38OffsetPastTheEnd) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\nthree\n");
    const QString doc = fx.doc("src/a.cpp:1\nsrc/a.cpp:2\nsrc/a.cpp:3\n");

    for (int off : {3, 99}) {
        DocCitations::Options opts;
        opts.offset = off;
        const QJsonObject r = DocCitations::check(fx.root, doc, opts);
        expect(cites(r).isEmpty(), "INV-38: empty page past the end",
               QStringLiteral("offset=%1 %2").arg(off).arg(render(r)));
        expect(r.value(QStringLiteral("count")).toInt() == 3,
               "INV-38: counts are unaffected by offset",
               QStringLiteral("offset=%1 %2").arg(off).arg(render(r)));
        expect(!r.contains(QStringLiteral("next_offset")),
               "INV-38: nothing to resume", QStringLiteral("offset=%1 %2").arg(off).arg(render(r)));
    }
    EXPECT_EQ(0, expect_failures());
}

// INV-42 — the emission caps bound what is emitted, never what is examined.
TEST(DocCitations, Inv42CountsAreWholeDoc) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "one\ntwo\nthree\nfour\nfive\nsix\nseven\n");
    QByteArray body;
    for (int i = 1; i <= 7; ++i) body += "src/a.cpp:" + QByteArray::number(i) + "\n";
    for (int i = 1; i <= 3; ++i) body += "src/gone" + QByteArray::number(i) + ".cpp:1\n";
    const QString doc = fx.doc(body);

    DocCitations::Options opts;
    opts.maxCitations = 2;
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    expect(r.value(QStringLiteral("returned")).toInt() == 2, "INV-42: two emitted", render(r));
    expect(r.value(QStringLiteral("count")).toInt() == 10,
           "INV-42: count is whole-doc", render(r));
    expect(r.value(QStringLiteral("counts")).toObject()
               .value(QStringLiteral("missing_file")).toInt() == 3,
           "INV-42: entries past the cap were still resolved", render(r));
    EXPECT_EQ(0, expect_failures());
}

// ANTS-4381 — a citation whose path is a real SUFFIX of a real file must not
// be reported as missing.
//
// `ui/import_wizard.py` cited where the file is
// `src/finbreak/ui/import_wizard.py` came back missing_file — with the
// basename index loaded in the same envelope. The map does not fire because
// the token has a directory component and so is not a bare basename, and
// step 1 was terminal both ways.
//
// The MIS-CLASSIFICATION is the defect, not the miss. `missing_file` asserts
// the file does not exist, so a reviewer triaging the stale list must open
// the tree to tell "deleted or moved" (a real finding) from "prefix short"
// (a smaller doc fix, sometimes a deliberate historical citation). It matters
// most inside review-contract Phase 1d, where a FINDING reaches cold lanes as
// settled fact — so a missing_file promoted on its label alone tells three
// reviewers a live file is gone.
TEST(DocCitations, Ants4381SuffixPathResolves) {
    Fixture fx;
    fx.write(QStringLiteral("src/pkg/ui/import_wizard.py"), "a\nb\nc\n");
    fx.write(QStringLiteral("src/pkg/core/util.py"), "x\n");
    fx.write(QStringLiteral("vendor/core/util.py"), "y\n");

    DocCitations::Options opts;
    opts.basenameIndex.insert(QStringLiteral("import_wizard.py"),
                              {QStringLiteral("src/pkg/ui/import_wizard.py")});
    opts.basenameIndex.insert(QStringLiteral("util.py"),
                              {QStringLiteral("src/pkg/core/util.py"),
                               QStringLiteral("vendor/core/util.py")});
    opts.basenameIndex.insert(QStringLiteral("nowhere.py"),
                              {QStringLiteral("src/pkg/nowhere.py")});

    const QString doc = fx.doc(
        "suffix `ui/import_wizard.py:2`\n"
        "two-way `core/util.py:1`\n"
        "genuinely absent `other/nowhere.py:1`\n");
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);

    EXPECT_EQ(status(r, 0), QStringLiteral("ok"))
        << "a suffix that matches exactly ONE real file resolves — reporting "
           "it missing_file asserts a live file was deleted";
    EXPECT_EQ(status(r, 1), QStringLiteral("ambiguous"))
        << "two files ending with the cited suffix stay ambiguous rather than "
           "being guessed at — the same answer the bare-basename rung gives";
    EXPECT_EQ(status(r, 2), QStringLiteral("missing_file"))
        << "a suffix matching nothing on disk is still genuinely missing, so "
           "the rung has not turned the check off";
}

// ANTS-4386 — quotation checking, and the whitespace rule is the whole thing.
//
// Nothing verified a fragment a document attributes to another document, and
// it is the highest-yield mechanical class in a cross-referencing corpus: a
// quote rots exactly when the quoted document is edited, which is when nobody
// re-reads the quoting one.
//
// The hand-rolled substitute is WORSE than not checking. A quotation in a
// hard-wrapped document does not survive a line-oriented search, so a grep
// reported "not found" for a phrase that was present and a reviewer nearly
// "fixed" a passage that was already correct — the collateral-generating move
// the whole review skill exists to prevent. Two of three lanes caught that
// false negative; one did not.
TEST(DocCitations, Ants4386QuotationsAreCheckedAcrossLineWraps) {
    Fixture fx;
    // The target is HARD-WRAPPED, which is the normal case and the one that
    // breaks a line-oriented search.
    fx.write(QStringLiteral("docs/standards/commits.md"),
             "# Commits\n"
             "\n"
             "Every commit message must name the reason the change\n"
             "exists, not merely what it changed.\n");
    fx.write(QStringLiteral("docs/a/dup.md"), "shared basename\n");
    fx.write(QStringLiteral("docs/b/dup.md"), "shared basename\n");

    DocCitations::Options opts;
    opts.quotes = true;
    opts.basenameIndex.insert(QStringLiteral("dup.md"),
                              {QStringLiteral("docs/a/dup.md"),
                               QStringLiteral("docs/b/dup.md")});

    // ANTS-4638 re-fixture: the five cases are independent, and attribution
    // is now PARAGRAPH-scoped — so they are separate paragraphs. Crammed
    // together they were one, and the no-attribution case inherited the
    // ambiguous basename from the case above it. Every assertion below is the
    // one this test shipped with; only the fixture says what it always meant.
    const QString doc = fx.doc(
        // Present in the target, but SPANNING its line break.
        "As `docs/standards/commits.md` puts it, \"name the reason the "
        "change exists, not merely what it changed\".\n"
        "\n"
        // Not present anywhere in the target.
        "It also says \"every commit shall be signed by two witnesses\" "
        "in `docs/standards/commits.md`.\n"
        "\n"
        // Attribution resolves to two files.
        "See `dup.md`: \"a quotation attributed to an ambiguous basename\".\n"
        "\n"
        // No document named anywhere in this paragraph.
        "Someone once said \"a quotation with nobody to attribute it to\".\n"
        "\n"
        // Below the floor — ordinary prose, must not be harvested.
        "The flag is called \"quotes\" and defaults to false.\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();

    auto statusOf = [&](const char *needle) -> QString {
        for (const auto &v : qs) {
            const QJsonObject q = v.toObject();
            if (q.value(QStringLiteral("text")).toString()
                    .contains(QString::fromUtf8(needle)))
                return q.value(QStringLiteral("status")).toString();
        }
        return QStringLiteral("<absent>");
    };

    EXPECT_EQ(statusOf("name the reason"), QStringLiteral("ok"))
        << "the quotation IS present in the target — it merely spans a line "
           "break there. Reporting not_found here is the false negative that "
           "makes the hand-rolled version unsafe";
    EXPECT_EQ(statusOf("two witnesses"), QStringLiteral("not_found"))
        << "…and a genuinely absent quotation must still be caught, or the "
           "whitespace fold has simply turned the check off";
    EXPECT_EQ(statusOf("ambiguous basename"), QStringLiteral("ambiguous"))
        << "an attribution matching several files reports the ambiguity "
           "rather than guessing which one it meant";
    EXPECT_EQ(statusOf("nobody to attribute"), QStringLiteral("no_target"))
        << "a quotation with no document named is not a finding — the verb "
           "cannot know what it is from";
    EXPECT_EQ(statusOf("quotes"), QStringLiteral("<absent>"))
        << "below the length floor is ordinary prose, not a quotation";

    // ANTS-4374's invariant: the zero has to say what was looked at.
    EXPECT_EQ(r.value(QStringLiteral("quotes_checked")).toInt(), 4);
    const QJsonObject qc = r.value(QStringLiteral("quote_counts")).toObject();
    EXPECT_EQ(qc.value(QStringLiteral("ok")).toInt(), 1);
    EXPECT_EQ(qc.value(QStringLiteral("not_found")).toInt(), 1);

    // Off by default: no existing caller's envelope moves.
    DocCitations::Options plain;
    const QJsonObject r2 = DocCitations::check(fx.root, doc, plain);
    EXPECT_FALSE(r2.contains(QStringLiteral("quotes")));
}

// ANTS-4085 — a quoted FOREIGN path is not a stale citation.
//
// `roadmap-data-model.md` § 5 quotes two sub-bullets from OTHER projects'
// roadmaps as the evidence its item-detail-vs-section-element distinction
// rests on. One is MAME Curator's and carries
// `tests/api/test_fp09_fixes.py:362`. doc_citations harvested it (a citation
// inside an inline code span IS harvested, deliberately — most real ones are
// written that way) and reported `missing_file`, because Ants has no
// `tests/api/` and never will: the path belongs to another repository.
//
// So it was a permanent finding on a frequently-linted file, unfixable at the
// document end — rewording the quote makes it no longer a quote. Option (c)
// of the three the item proposed: a distinct status that `only:"stale"`
// excludes. Cheapest, and it generalises to every project that quotes
// another's paths.
TEST(DocCitations, Ants4085ForeignPathIsNotStale) {
    Fixture fx;
    fx.write(QStringLiteral("src/pkg/real.py"), "a\nb\nc\n");

    DocCitations::Options opts;
    opts.basenameIndex.insert(QStringLiteral("real.py"),
                              {QStringLiteral("src/pkg/real.py")});
    // `ours.py` is a file of ours the index knows about but that is not on
    // disk — a genuine miss, and the discriminator against the foreign case.
    opts.basenameIndex.insert(QStringLiteral("ours.py"),
                              {QStringLiteral("src/pkg/ours.py")});

    const QString doc = fx.doc(
        "real `src/pkg/real.py:2`\n"
        "quoted from elsewhere `tests/api/test_fp09_fixes.py:362`\n"
        "ours and gone `src/pkg/ours.py:1`\n");
    const QJsonObject r = DocCitations::check(fx.root, doc, opts);

    EXPECT_EQ(status(r, 0), QStringLiteral("ok"));
    EXPECT_EQ(status(r, 1), QStringLiteral("foreign_path"))
        << "ANTS-4085: `tests/` names no directory here AND no file called "
           "test_fp09_fixes.py exists anywhere in the project — that pair "
           "means another repository's path, not one of ours that vanished";
    EXPECT_EQ(status(r, 2), QStringLiteral("missing_file"))
        << "ANTS-4085: the basename IS in this project's index, so the file "
           "is ours and genuinely missing — this must stay a real defect";

    // The point of the whole change: a foreign path drops out of only:"stale",
    // so a document that quotes another project can come back clean.
    DocCitations::Options stale = opts;
    stale.only = DocCitations::Only::Stale;
    const QJsonObject sr = DocCitations::check(fx.root, doc, stale);
    const QJsonArray rows = sr.value(QStringLiteral("citations")).toArray();
    for (const QJsonValue &v : rows) {
        EXPECT_NE(v.toObject().value(QStringLiteral("status")).toString(),
                  QStringLiteral("foreign_path"))
            << "ANTS-4085: only:\"stale\" must exclude a foreign path — a "
               "checker whose output is never empty stops being read";
    }
    // …but the genuinely-missing one is still there, so the filter did not
    // simply swallow everything.
    bool sawMissing = false;
    for (const QJsonValue &v : rows)
        if (v.toObject().value(QStringLiteral("status")).toString()
            == QLatin1String("missing_file")) sawMissing = true;
    EXPECT_TRUE(sawMissing)
        << "ANTS-4085: a real missing_file must still reach the stale list";

    // Counted like any other status, so `counts` still partitions the rows.
    EXPECT_EQ(r.value(QStringLiteral("counts")).toObject()
               .value(QStringLiteral("foreign_path")).toInt(), 1);
}

// ANTS-4664 — the quoted SPAN was detected per LINE while the matcher folded
// newlines (ANTS-4386), so a hard-wrapped quotation was never handed to the
// matcher at all: it entered NO bucket, and a document whose quotations all
// wrap returned quotes_checked:0 — byte-identical to one that quotes nothing.
// This corpus wraps at ~70 columns, so that is most quotations.
//
// Measured over this repo's 317 documents while fixing it: 745 quotations
// before, 1985 after, and `ok` (verified present in the target) 7 → 32. The
// surplus is not all new: the line-scoped pass was ALSO mis-pairing one
// quotation's CLOSING delimiter with the next one's OPENING one and emitting
// the prose between them as a quotation, and 18 such spans disappear. Hence
// the second paragraph below, which is that case.
TEST(DocCitations, Ants4664WrappedQuotationSpansAreDetected) {
    Fixture fx;
    fx.write(QStringLiteral("docs/standards/commits.md"),
             "# Commits\n"
             "\n"
             "Every commit message must name the reason the change\n"
             "exists, not merely what it changed. A second sentence\n"
             "lives here so a second quotation can be checked too.\n");

    DocCitations::Options opts;
    opts.quotes = true;

    const QString doc = fx.doc(
        // The defect itself: the quoted span straddles the line break.
        "As `docs/standards/commits.md` puts it, \"name the reason the change\n"
        "exists, not merely what it changed\", which is the whole rule.\n"
        "\n"
        // TWO wrapped quotations in ONE paragraph. The prose between them
        // (" and also ") must not become a third.
        "`docs/standards/commits.md` says \"must name the reason the\n"
        "change exists\" and also \"A second sentence lives here so a\n"
        "second quotation can be checked too\" in the same breath.\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();

    EXPECT_EQ(r.value(QStringLiteral("quotes_checked")).toInt(), 3)
        << "three wrapped quotations — and NOT a fourth manufactured out of "
           "the prose between the two that share a paragraph";

    for (const QJsonValue &v : qs) {
        const QJsonObject q = v.toObject();
        EXPECT_EQ(q.value(QStringLiteral("status")).toString(),
                  QStringLiteral("ok"))
            << "a wrapped quotation that IS present in its target must "
               "verify, not land in no bucket: "
            << q.value(QStringLiteral("text")).toString().toStdString();
        EXPECT_FALSE(q.value(QStringLiteral("text")).toString()
                         .contains(QLatin1Char('\n')))
            << "the emitted text is folded to one line";
    }
}

// ANTS-4637 — a quotation sitting in a loop-log row is historical BY DESIGN.
//
// A converged review document records each loop in a table row, and those rows
// quote the wording a fix REPLACED. `not_found` is the correct and permanent
// answer for every one of them, so the check cannot act on it. Measured over
// this machine's config corpus: 520 unfenced quotations of >= 30 characters,
// **271 of them in a loop-log row** — 52%, which is what makes the mode's
// output majority-false-stale and unreadable (the ANTS-4085 argument).
//
// It also actively harms: `CLAUDE.md` forbids back-filling a loop log, so a
// session acting on a `not_found` there destroys the audit trail the row is
// kept for.
//
// Reporting the skip matters as much as performing it — a suppressed
// quotation that is merely absent is indistinguishable from one nobody found.
TEST(DocCitations, Ants4637LoopLogRowsAreSkippedAndCounted) {
    Fixture fx;
    fx.write(QStringLiteral("standards/gate.md"),
             "# Gate\n\nthe wording this rule carries today\n");

    DocCitations::Options opts;
    opts.quotes = true;

    const QString doc = fx.write(QStringLiteral("standards/commits.md"),
        "# Commits\n"
        "\n"
        "The rule in `standards/gate.md` reads \"the wording this rule "
        "carries today\".\n"
        "\n"
        "| Loop | Date | Outcome |\n"
        "|------|------|---------|\n"
        "| 1 | 2026-08-14 | Fixed: `standards/gate.md` said \"the wording a "
        "fix has since replaced\". |\n"
        "| 2 | 2026-08-18 | Fixed: `standards/gate.md` said \"a second "
        "superseded phrasing nobody may restore\". |\n"
        "\n"
        "| Source | Note |\n"
        "|--------|------|\n"
        "| `standards/gate.md` | \"the wording this rule carries today\" |\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();
    const QJsonObject qc = r.value(QStringLiteral("quote_counts")).toObject();

    for (const QJsonValue &v : qs) {
        EXPECT_FALSE(v.toObject().value(QStringLiteral("text")).toString()
                         .contains(QStringLiteral("has since replaced")))
            << "ANTS-4637: a loop-log row quotes the wording a fix REPLACED — "
               "not_found is the permanent correct answer there, and acting "
               "on it destroys the audit trail: " << qPrintable(render(r));
    }
    EXPECT_EQ(qc.value(QStringLiteral("skipped")).toInt(), 2)
        << "ANTS-4637: the skip must be REPORTED — a zero has to be "
           "distinguishable from nothing-to-check: " << qPrintable(render(r));
    EXPECT_EQ(r.value(QStringLiteral("quotes_checked")).toInt(), 2)
        << "ANTS-4637: skipped quotations were not checked, so they are not "
           "in quotes_checked: " << qPrintable(render(r));

    // The trap: an ORDINARY table row is not a loop-log row. Suppressing every
    // table row would take the impact tables ANTS-4640 exists to serve with it.
    int okCount = 0;
    for (const QJsonValue &v : qs)
        if (v.toObject().value(QStringLiteral("status")).toString()
            == QLatin1String("ok")) ++okCount;
    EXPECT_EQ(okCount, 2)
        << "ANTS-4637: the prose quotation and the ordinary table row are "
           "still checked — the rule keys on the loop-number + ISO-date "
           "shape, not on being a table: " << qPrintable(render(r));

    // The partition is over what was CHECKED; `skipped` sits outside it, and
    // the envelope has to say so or the map reads as a broken tally.
    const QJsonArray overlay =
        r.value(QStringLiteral("quote_counts_overlay_keys")).toArray();
    EXPECT_TRUE(overlay.contains(QJsonValue(QStringLiteral("skipped"))))
        << "ANTS-4637: `skipped` is an overlay, not a status bucket: "
        << qPrintable(render(r));
    int partition = 0;
    for (const QString &k : {QStringLiteral("ok"), QStringLiteral("not_found"),
                             QStringLiteral("ambiguous"),
                             QStringLiteral("no_target"),
                             QStringLiteral("target_unresolved")})
        partition += qc.value(k).toInt();
    EXPECT_EQ(partition, r.value(QStringLiteral("quotes_checked")).toInt())
        << "ANTS-4637: the status buckets partition quotes_checked: "
        << qPrintable(render(r));
}

// ANTS-4638 — attribution is a PARAGRAPH-scoped question, not a line-scoped one.
//
// This corpus hard-wraps at ~70 columns, so the backticked path and the
// quotation it introduces routinely land on adjacent lines of one sentence.
// The mode's own strength — folding whitespace INCLUDING NEWLINES so a
// hard-wrapped quotation still matches — was defeated by the same wrapping on
// the attribution side. Measured, excluding ANTS-4637's loop-log rows: of 249
// quotations, 26 carry a path on their own line and 85 more carry one earlier
// in the same paragraph. Those 85 ARE attributed, in prose a human reads as
// attributed, and every one came back `no_target` — silent, and identical to
// a clean pass.
//
// Same-line stays the tie-break, so today's behaviour is a strict subset.
TEST(DocCitations, Ants4638AttributionWidensToTheParagraph) {
    Fixture fx;
    fx.write(QStringLiteral("standards/gate.md"),
             "# Gate\n\nthe attribution and its quotation wrapped apart\n");
    fx.write(QStringLiteral("standards/other.md"),
             "# Other\n\nnothing in here matches anything\n");

    DocCitations::Options opts;
    opts.quotes = true;

    const QString doc = fx.write(QStringLiteral("standards/commits.md"),
        "# Commits\n"
        "\n"
        "The rule lives in `standards/gate.md`, and is worth reading whole\n"
        "before changing it, because it reads \"the attribution and its "
        "quotation wrapped apart\".\n"
        "\n"
        "A paragraph that names `standards/other.md`.\n"
        "\n"
        "A separate paragraph entirely, quoting \"a phrase belonging to no "
        "named document at all\".\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();

    auto find = [&](const char *needle) -> QJsonObject {
        for (const QJsonValue &v : qs) {
            const QJsonObject q = v.toObject();
            if (q.value(QStringLiteral("text")).toString()
                    .contains(QString::fromUtf8(needle))) return q;
        }
        return QJsonObject{};
    };

    const QJsonObject wrapped = find("wrapped apart");
    EXPECT_EQ(wrapped.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"))
        << "ANTS-4638: the path sits one line up in the SAME sentence — "
           "reporting no_target here is the check silently not running: "
        << qPrintable(render(r));

    // The trap: a blank line ends the scope. Without it the rule reaches back
    // across the whole document and attributes a quotation to a path that
    // introduces something else entirely — a confident wrong answer, which is
    // worse than the silence it replaced.
    const QJsonObject far = find("no named document");
    EXPECT_EQ(far.value(QStringLiteral("status")).toString(),
              QStringLiteral("no_target"))
        << "ANTS-4638: a blank line ends the paragraph, so the previous "
           "paragraph's path must not be inherited: " << qPrintable(render(r));
}

// ANTS-4639 — three defects in one resolver, and they compound.
//
// (1) `standards/commits.md` attributes quotations to `releases.md` and
//     `coding.md`; both exist as siblings under `standards/`. Resolution never
//     tried the scanned document's OWN directory, which is the commonest form
//     a cross-reference takes inside a standards directory.
// (2) `core.hooksPath` — a git config key in backticks — became a `target`,
//     because the shape test asked only for a dot.
// (3) A target that PARSED but did not resolve reported `no_target`, which is
//     what a quotation with no attribution at all reports. Those mean opposite
//     things to a caller — the check did not run, versus the check ran and the
//     quotation is stale — and conflating them is what hid (1) and (2).
TEST(DocCitations, Ants4639SiblingResolutionAndHonestTargetStatuses) {
    Fixture fx;
    fx.write(QStringLiteral("standards/releases.md"),
             "# Releases\n\na sentence the sibling document really contains\n");

    DocCitations::Options opts;
    opts.quotes = true;

    const QString doc = fx.write(QStringLiteral("standards/commits.md"),
        "# Commits\n"
        "\n"
        "`releases.md` says \"a sentence the sibling document really "
        "contains\".\n"
        "\n"
        "The hook is installed by setting `core.hooksPath`, and the rule is "
        "\"a phrase attributed to a git config key, which is not a "
        "document\".\n"
        "\n"
        "`ghost.md` says \"a phrase attributed to a document that is not "
        "there\".\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();

    auto find = [&](const char *needle) -> QJsonObject {
        for (const QJsonValue &v : qs) {
            const QJsonObject q = v.toObject();
            if (q.value(QStringLiteral("text")).toString()
                    .contains(QString::fromUtf8(needle))) return q;
        }
        return QJsonObject{};
    };

    EXPECT_EQ(find("sibling document really").value(QStringLiteral("status"))
                  .toString(), QStringLiteral("ok"))
        << "ANTS-4639 (1): a bare basename resolves against the scanned "
           "document's own directory: " << qPrintable(render(r));

    const QJsonObject key = find("git config key");
    EXPECT_EQ(key.value(QStringLiteral("status")).toString(),
              QStringLiteral("no_target"))
        << "ANTS-4639 (2): a token needs a document extension before it "
           "becomes a target — a dot alone is every config key: "
        << qPrintable(render(r));
    EXPECT_FALSE(key.contains(QStringLiteral("target")))
        << "ANTS-4639 (2): …and it must not be reported AS a target: "
        << qPrintable(render(r));

    const QJsonObject ghost = find("document that is not there");
    EXPECT_EQ(ghost.value(QStringLiteral("status")).toString(),
              QStringLiteral("target_unresolved"))
        << "ANTS-4639 (3): the attribution parsed and did not resolve — that "
           "is not the same answer as no attribution at all: "
        << qPrintable(render(r));
    EXPECT_EQ(ghost.value(QStringLiteral("target")).toString(),
              QStringLiteral("ghost.md"))
        << "ANTS-4639 (3): …and the target it could not resolve is named: "
        << qPrintable(render(r));
}

// ANTS-4640 — in a table row the whole row is one line, and last-path-wins
// picks the wrong one by construction.
//
// A cross-document impact table names the SOURCE first and the affected module
// second, which is the natural sentence. Twice in one review loop a quotation
// was attributed to the affected module and came back `not_found` against text
// that is present and exact — which invites a session to "correct" a passage
// that was already right, the expensive direction.
//
// The row's first cell is its subject by convention; a path in the
// quotation's OWN cell is more local still and wins over it. Where one cell
// carries several, the envelope already has `ambiguous` — reporting the
// candidates beats guessing.
TEST(DocCitations, Ants4640TableRowAttributionIsCellScoped) {
    Fixture fx;
    fx.write(QStringLiteral("docs/specs/FIBR-0014.md"),
             "# FIBR-0014\n\na phrase the SOURCE spec really carries\n");
    // The affected column is a DOCUMENT too, so the extension gate ANTS-4639
    // adds cannot be what makes these rows pass — only the cell scope can.
    fx.write(QStringLiteral("docs/affected.md"), "# Affected\n\nnothing quotable\n");
    fx.write(QStringLiteral("docs/a/dup.md"), "shared basename\n");
    fx.write(QStringLiteral("docs/b/dup.md"), "shared basename\n");

    DocCitations::Options opts;
    opts.quotes = true;
    opts.basenameIndex.insert(QStringLiteral("dup.md"),
                              {QStringLiteral("docs/a/dup.md"),
                               QStringLiteral("docs/b/dup.md")});

    const QString doc = fx.doc(
        "| Source | Affected | Clause |\n"
        "|--------|----------|--------|\n"
        "| `docs/specs/FIBR-0014.md` | `docs/affected.md` | \"a phrase the "
        "SOURCE spec really carries\" |\n"
        "| `docs/affected.md` | `docs/specs/FIBR-0014.md` | \"a phrase whose "
        "own cell names nothing at all\" |\n"
        "| `docs/affected.md` | x | `docs/specs/FIBR-0014.md` says \"a phrase "
        "the SOURCE spec really carries\" |\n"
        "| `docs/a/dup.md` `docs/b/dup.md` | y | \"a phrase whose own cell "
        "names two documents\" |\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();
    ASSERT_EQ(qs.size(), 4) << qPrintable(render(r));

    EXPECT_EQ(qs.at(0).toObject().value(QStringLiteral("target")).toString(),
              QStringLiteral("docs/specs/FIBR-0014.md"))
        << "ANTS-4640: the quotation's own cell names no document, so the "
           "row's first cell — its subject — attributes it, NOT the affected "
           "module named later in the row: " << qPrintable(render(r));
    EXPECT_EQ(qs.at(0).toObject().value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"))
        << "ANTS-4640: …and the quotation is present and exact there, so a "
           "not_found here would invite correcting a correct passage: "
        << qPrintable(render(r));

    EXPECT_EQ(qs.at(1).toObject().value(QStringLiteral("target")).toString(),
              QStringLiteral("docs/affected.md"))
        << "ANTS-4640: the row's subject attributes it even when the phrase "
           "is NOT there — the rule picks the source, not the document that "
           "happens to make the check pass: " << qPrintable(render(r));
    EXPECT_EQ(qs.at(1).toObject().value(QStringLiteral("status")).toString(),
              QStringLiteral("not_found"))
        << "ANTS-4640: …so this stays a real finding. A rule that reached for "
           "whichever cell matched would turn the check off entirely: "
        << qPrintable(render(r));

    EXPECT_EQ(qs.at(2).toObject().value(QStringLiteral("target")).toString(),
              QStringLiteral("docs/specs/FIBR-0014.md"))
        << "ANTS-4640: a path in the quotation's OWN cell is more local than "
           "the row's subject and wins over it: " << qPrintable(render(r));

    EXPECT_EQ(qs.at(3).toObject().value(QStringLiteral("status")).toString(),
              QStringLiteral("ambiguous"))
        << "ANTS-4640: one cell naming two documents is reported with its "
           "candidates rather than guessed at: " << qPrintable(render(r));
    EXPECT_EQ(qs.at(3).toObject().value(QStringLiteral("candidates")).toArray()
                  .size(), 2)
        << "ANTS-4640: …and the candidate list is what makes it actionable: "
        << qPrintable(render(r));
}

// ANTS-4696 — a quotation whose nearest attribution is a path-shaped token
// that names no document extension must report no_target and NAME the
// rejected token, never fall through to an older document in the same
// paragraph. Falling through reports the quotation stale in a file that was
// never claimed as its source, and not_found is the actionable status: the
// natural repair is to edit a passage that was already correct.
TEST(DocCitations, Ants4696RejectedTargetDoesNotFallThrough) {
    Fixture fx;
    fx.write(QStringLiteral("docs/decisions/ADR-0003.md"),
             "# ADR-0003\n\nthe counter is a derived cache and an absent "
             "counter is normal\n");
    fx.write(QStringLiteral("docs/design.md"),
             "# Design\n\nnothing here is quoted by anyone\n");

    DocCitations::Options opts;
    opts.quotes = true;

    const QString doc = fx.write(QStringLiteral("docs/spec.md"),
        "# Spec\n"
        "\n"
        "This refines `docs/design.md` in one respect.\n"
        "See `docs/decisions/ADR-0003`, which says\n"
        "\"the counter is a derived cache and an absent counter is normal\".\n"
        "\n"
        "The hook is installed by setting `core.hooksPath`, and the rule is "
        "\"a phrase attributed to a git config key, which is not a "
        "document\".\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();

    auto find = [&](const char *needle) -> QJsonObject {
        for (const QJsonValue &v : qs) {
            const QJsonObject q = v.toObject();
            if (q.value(QStringLiteral("text")).toString()
                    .contains(QString::fromUtf8(needle))) return q;
        }
        return QJsonObject{};
    };

    const QJsonObject adr = find("derived cache");
    EXPECT_EQ(adr.value(QStringLiteral("status")).toString(),
              QStringLiteral("no_target"))
        << "the extensionless ADR is the attribution; walking past it to "
           "docs/design.md manufactures a stale-citation report: "
        << qPrintable(render(r));
    EXPECT_FALSE(adr.contains(QStringLiteral("target")))
        << "docs/design.md was never claimed as this quotation's source: "
        << qPrintable(render(r));
    EXPECT_EQ(adr.value(QStringLiteral("rejected_target")).toString(),
              QStringLiteral("docs/decisions/ADR-0003"))
        << "the rejected token must be named — the repair is one character: "
        << qPrintable(render(r));

    // The slash is what keeps this off ANTS-4639's motivating case. A config
    // key has a dot and no slash, so it is not path-shaped and must not be
    // reported as a rejected target.
    const QJsonObject key = find("git config key");
    EXPECT_EQ(key.value(QStringLiteral("status")).toString(),
              QStringLiteral("no_target"))
        << qPrintable(render(r));
    EXPECT_FALSE(key.contains(QStringLiteral("rejected_target")))
        << "ANTS-4639's config key is not path-shaped and must not be named "
           "as a rejected target: " << qPrintable(render(r));
}

// ANTS-4706 — a quotation inside a `> ` blockquote resolves. Found while
// probing ANTS-4697 against a throwaway project: a multi-line blockquoted
// quotation came back not_found against a document containing it verbatim,
// with the continuation markers embedded in the reported text
// ("...theta iota > kappa lambda..."). not_found is the ACTIONABLE status, so
// its natural repair is to edit a passage that was already correct — the same
// harm as ANTS-4696, by another route. A blockquote is how this corpus quotes
// another document at length.
TEST(DocCitations, Ants4706BlockquotedQuotationResolves) {
    Fixture fx;
    fx.write(QStringLiteral("docs/target.md"),
             "# Target\n\nthe counter is a derived cache and an absent counter "
             "is the normal fresh clone state rather than a desync\n");

    DocCitations::Options opts;
    opts.quotes = true;

    const QString doc = fx.write(QStringLiteral("docs/spec.md"),
        "# Spec\n"
        "\n"
        "> Per `docs/target.md`: \"the counter is a derived cache and an\n"
        "> absent counter is the normal fresh clone state rather than a\n"
        "> desync\"\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();
    ASSERT_EQ(qs.size(), 1) << qPrintable(render(r));
    const QJsonObject q = qs.at(0).toObject();

    EXPECT_EQ(q.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"))
        << "the quotation is present and exact in the target: "
        << qPrintable(render(r));
    // The reported text must be copy-pasteable into a search. Markers embedded
    // in it are why the first symptom was so hard to read.
    EXPECT_FALSE(q.value(QStringLiteral("text")).toString()
                     .contains(QLatin1Char('>')))
        << "got: " << q.value(QStringLiteral("text")).toString().toStdString();
}

// ANTS-4706 — and a `>` that is NOT a leading marker survives, because a
// comparison operator inside quoted prose is content. The strip is per line
// and anchored, which is the only place the marker is unambiguous.
TEST(DocCitations, Ants4706InlineAngleBracketIsContent) {
    Fixture fx;
    fx.write(QStringLiteral("docs/target.md"),
             "# Target\n\nrefuse the write when bytes > the configured ceiling "
             "and say which one it was\n");

    DocCitations::Options opts;
    opts.quotes = true;

    const QString doc = fx.write(QStringLiteral("docs/spec.md"),
        "# Spec\n"
        "\n"
        "`docs/target.md` says \"refuse the write when bytes > the configured\n"
        "ceiling and say which one it was\".\n");

    const QJsonObject r = DocCitations::check(fx.root, doc, opts);
    const QJsonArray qs = r.value(QStringLiteral("quotes")).toArray();
    ASSERT_EQ(qs.size(), 1) << qPrintable(render(r));
    EXPECT_EQ(qs.at(0).toObject().value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"))
        << "an inline `>` is prose and must not be stripped: "
        << qPrintable(render(r));
    EXPECT_TRUE(qs.at(0).toObject().value(QStringLiteral("text")).toString()
                    .contains(QLatin1Char('>')))
        << "…and must survive into the reported text";
}


// ANTS-4748 — a `path::symbol` span resolves to the symbol's line where the path
// resolves AND the symbol is unique in that file. The entry says where the line
// came from: `resolved_via` is what lets a reader tell a looked-up line from an
// authored one.
TEST(DocCitations, Ants4748UniqueSymbolResolvesToItsLine) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"),
             "// header\n"
             "void other() {\n"
             "}\n"
             "\n"
             "void doThing() {\n"
             "    body();\n"
             "}\n");
    const QString doc = fx.doc("see `src/a.cpp::doThing` for it\n");

    const QJsonObject r = DocCitations::check(fx.root, doc);
    expect(cites(r).size() == 1, "4748/one-citation", render(r));
    if (cites(r).size() == 1) {
        expect(status(r, 0) == QStringLiteral("ok"), "4748/ok", render(r));
        expect(cite(r, 0).value(QStringLiteral("start_line")).toInt() == 5,
               "4748/line-from-the-tree", render(r));
        expect(cite(r, 0).value(QStringLiteral("resolved_via")).toString()
                   == QStringLiteral("symbol"),
               "4748/resolved-via", render(r));
        expect(cite(r, 0).value(QStringLiteral("symbol")).toString()
                   == QStringLiteral("doThing"),
               "4748/symbol-echoed", render(r));
    }
    ASSERT_EQ(0, expect_finish());
}

// The guard that earns the widening. Two declarations of one name — an overload,
// the commonest case — resolve to no line at all. Reporting either one `ok` would
// be a FALSE ok, worse than the silence ANTS-4743 deliberately admitted, so the
// reference goes back to the unrecognised count.
TEST(DocCitations, Ants4748AmbiguousSymbolResolvesToNothing) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"),
             "void f(int x) {\n"
             "}\n"
             "void f(double x) {\n"
             "}\n");
    const QString doc = fx.doc("see `src/a.cpp::f`\n");

    const QJsonObject r = DocCitations::check(fx.root, doc);
    expect(cites(r).isEmpty(), "4748/ambiguous-invents-nothing", render(r));
    expect(r.value(QStringLiteral("unrecognised_candidates")).toInt() == 1,
           "4748/ambiguous-counted-unchecked", render(r));
    ASSERT_EQ(0, expect_finish());
}

// Same for a name the file does not carry — renamed, or moved away.
TEST(DocCitations, Ants4748AbsentSymbolResolvesToNothing) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/a.cpp"), "void present() {\n}\n");
    const QString doc = fx.doc("see `src/a.cpp::goneAway`\n");

    const QJsonObject r = DocCitations::check(fx.root, doc);
    expect(cites(r).isEmpty(), "4748/absent-invents-nothing", render(r));
    expect(r.value(QStringLiteral("unrecognised_candidates")).toInt() == 1,
           "4748/absent-counted-unchecked", render(r));
    ASSERT_EQ(0, expect_finish());
}

// An unresolvable PATH is the other half of the same guard: with no file to look
// in, there is nothing to make the reference unique against.
TEST(DocCitations, Ants4748UnresolvablePathResolvesToNothing) {
    expect_reset();
    Fixture fx;
    const QString doc = fx.doc("see `src/gone.cpp::doThing`\n");

    const QJsonObject r = DocCitations::check(fx.root, doc);
    expect(cites(r).isEmpty(), "4748/bad-path-invents-nothing", render(r));
    expect(r.value(QStringLiteral("unrecognised_candidates")).toInt() == 1,
           "4748/bad-path-counted-unchecked", render(r));
    ASSERT_EQ(0, expect_finish());
}

// The outline names an out-of-class definition by its QUALIFIED name, and the
// corpus writes the unqualified one — measured across this project's docs as the
// majority of the `path::symbol` spans. Without the suffix rung the widening
// reaches almost none of them, so it is pinned rather than left incidental.
TEST(DocCitations, Ants4748QualifiedDefinitionMatchesUnqualifiedReference) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/k.cpp"),
             "#include \"k.h\"\n"
             "\n"
             "void Klass::doThing() {\n"
             "    body();\n"
             "}\n");
    const QString doc = fx.doc("see `src/k.cpp::doThing`\n");

    const QJsonObject r = DocCitations::check(fx.root, doc);
    expect(cites(r).size() == 1, "4748/qualified-one-citation", render(r));
    if (cites(r).size() == 1) {
        expect(status(r, 0) == QStringLiteral("ok"), "4748/qualified-ok", render(r));
        expect(cite(r, 0).value(QStringLiteral("start_line")).toInt() == 3,
               "4748/qualified-line", render(r));
    }
    ASSERT_EQ(0, expect_finish());
}

// Uniqueness survives the suffix rung: two qualified names sharing one suffix
// are ambiguous, so the reference resolves to nothing rather than to whichever
// was seen first.
TEST(DocCitations, Ants4748SharedSuffixIsStillAmbiguous) {
    expect_reset();
    Fixture fx;
    fx.write(QStringLiteral("src/k.cpp"),
             "void Alpha::run() {\n"
             "}\n"
             "void Beta::run() {\n"
             "}\n");
    const QString doc = fx.doc("see `src/k.cpp::run`\n");

    const QJsonObject r = DocCitations::check(fx.root, doc);
    expect(cites(r).isEmpty(), "4748/shared-suffix-invents-nothing", render(r));
    expect(r.value(QStringLiteral("unrecognised_candidates")).toInt() == 1,
           "4748/shared-suffix-counted-unchecked", render(r));
    ASSERT_EQ(0, expect_finish());
}
