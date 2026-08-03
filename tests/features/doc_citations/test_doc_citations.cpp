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
