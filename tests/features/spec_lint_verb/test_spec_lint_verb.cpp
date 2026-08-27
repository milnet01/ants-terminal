// ANTS-3662 — spec_lint VERB conformance test (INV-4, INV-7). The handler needs
// a live MainWindow, so behavioural rows drive the pure helpers and wiring rows
// source-scrape the registration sites (the rc_get_text_byte_cap pattern, as
// ANTS-3601's and ANTS-3661's verb tests do).

#include "remotecontrol.h"
#include "speclint.h"

#include <string>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <gtest/gtest.h>
#include "../../_support/srcgrep.h"

#if !defined(SRC_MAINWINDOW_CPP_PATH) || !defined(ANTS_RC_SOURCES) || \
    !defined(SRC_CLAUDE_INTEGRATION_CPP_PATH) || !defined(SRC_SPECLINT_CPP_PATH)
#error "spec_lint_verb test needs the test_claude source-path compile defs"
#endif
#if !defined(ANTS_SOURCE_DIR)
#error "spec_lint_verb test needs ANTS_SOURCE_DIR for the live gathering check"
#endif

namespace {

QString slurp(const char *path) {
    QFile f(QString::fromUtf8(path));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

}  // namespace

// INV-4 — command_test_no_expectation is a CANDIDATE: never auto-fixable, and
// the verb runs no subprocess. The scrape is the half a behavioural test cannot
// hold: "this engine never executes a clause" is a claim about code that does
// not exist, and only a source scan can assert an absence.
TEST(SpecLintVerb, Inv4CommandClauseIsACandidate) {
    const QString doc = QStringLiteral(
        "# ANTS-1 — clauses\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a. *Test:* `grep -c foo src/`\n"
        "- **INV-2** — b. *Test:* `grep -c foo src/` → 3\n"
        "- **INV-3** — c. *Test:* `tests/features/spec_lint/` covers it\n"
        "- **INV-4** — d. *Test:* run `ctest -R spec_lint`.\n"
        "- **INV-5** — e. *Test:* `tests/features/spec_lint/`\n");
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("c.md"), {});

    int candidates = 0;
    for (const auto &f : r.findings) {
        if (f.kind != QLatin1String("command_test_no_expectation")) continue;
        ++candidates;
        EXPECT_FALSE(f.autoFixable) << "a candidate is never auto-fixable";
        EXPECT_TRUE(f.message.contains(QStringLiteral("candidate")));
    }
    // INV-1 and INV-4 fire: a bare command, and a command whose only trailing
    // text is punctuation. Three do not, and each is excluded by a DIFFERENT
    // half of the heuristic — which is the point of carrying all three:
    //   INV-2  `→ 3` is an expectation (the trailing-alphanumeric rule).
    //   INV-3  trailing prose, so the expectation rule excludes it whatever the
    //          vocabulary says — it does NOT test the vocabulary.
    //   INV-5  a BARE path span with nothing after it. Only the fixed
    //          vocabulary keeps this quiet; a shape test ("contains a slash")
    //          fires here. Without this clause the vocabulary is untested.
    EXPECT_EQ(candidates, 2);

    // The wire form: ANTS-3664 INV-1 OMITS auto_fixable when false, so the
    // assertion is that the key is ABSENT — a test written against
    // `auto_fixable:false` fails on a conforming serialiser.
    DocFinding::Finding f;
    f.verb = QStringLiteral("spec_lint");
    f.kind = QStringLiteral("command_test_no_expectation");
    f.file = QStringLiteral("c.md");
    f.line = 5;
    const QJsonObject wire = DocFinding::toJson(f);
    EXPECT_FALSE(wire.contains(QStringLiteral("auto_fixable")));

    // No subprocess, ever. /write-spec Step 3 owns running a clause, at write
    // time, where a failure is free.
    //
    // COMMENTS ARE STRIPPED FIRST. speclint.cpp cites `QProcess` as an example
    // of a code span that is NOT a command — a comment explaining this very
    // rule — and a raw scrape reads that as the violation. Renaming the example
    // would leave the trap armed for the next comment; the scan is over code.
    const std::string eng =
        ants_test::stripComments(ants_test::slurpFile(SRC_SPECLINT_CPP_PATH));
    ASSERT_FALSE(eng.empty());
    for (const char *banned : {"QProcess", "std::system", "popen", "execve"})
        EXPECT_EQ(eng.find(banned), std::string::npos)
            << "speclint.cpp must never execute a test clause: " << banned;
}

// INV-7 — the verb-contract minimum: caller_cwd Required; a root-escaping
// `path` refuses bad_path; a well-formed non-existent in-root `path` is ok:true
// with empty findings (ANTS-3601 INV-15's shape).
TEST(SpecLintVerb, Inv7RefusalMinimums) {
    // (1) caller_cwd Required — at the call site AND in the static table
    // registerToolProvider asserts against.
    const QString mw = slurp(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.isEmpty());
    const int reg = mw.indexOf(QStringLiteral("registerToolProvider(\"spec_lint\""));
    ASSERT_GE(reg, 0);
    EXPECT_TRUE(mw.mid(reg, 160).contains(QStringLiteral("CallerCwdContract::Required")));

    const QString ci = slurp(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    ASSERT_FALSE(ci.isEmpty());
    const int cc = ci.indexOf(QStringLiteral("toolName == QStringLiteral(\"spec_lint\")"));
    ASSERT_GE(cc, 0);
    EXPECT_TRUE(ci.mid(cc, 100).contains(QStringLiteral("C::Required")));
    EXPECT_TRUE(ci.contains(QStringLiteral("specLint[\"name\"] = \"spec_lint\"")));

    // (2) a supplied path is validated BEFORE any enumeration → bad_path.
    const QString rc = QString::fromStdString(ants_test::slurpRemoteControl());
    ASSERT_FALSE(rc.isEmpty());
    const int h = rc.indexOf(QStringLiteral("RemoteControl::cmdSpecLint"));
    ASSERT_GE(h, 0);
    const QString handler = rc.mid(h, 1600);
    EXPECT_TRUE(handler.contains(QStringLiteral("validatePath(")));
    EXPECT_TRUE(handler.contains(QStringLiteral("check.err")));

    // (3) nothing to scan → ok:true with EMPTY findings, not a refusal.
    const QJsonObject empty =
        RemoteControl::specLintBuildResponse({}, false, {}, false, {}, 0, false);
    EXPECT_TRUE(empty.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(empty.value(QStringLiteral("findings")).toArray().isEmpty());
    EXPECT_TRUE(empty.value(QStringLiteral("checked_docs")).toArray().isEmpty());

    // sections_checked is ALWAYS present, never omitted when false — it is the
    // one field distinguishing "every required section is there" from "nobody
    // checked", and false is this verb's shipping default.
    EXPECT_TRUE(empty.contains(QStringLiteral("sections_checked")));
    EXPECT_FALSE(empty.value(QStringLiteral("sections_checked")).toBool());
    EXPECT_TRUE(empty.contains(QStringLiteral("line_count")));
    // truncated is omitted when false (§ 2.1's envelope).
    EXPECT_FALSE(empty.contains(QStringLiteral("truncated")));
}

// ANTS-4127 INV-3..5, INV-7, INV-10 at the WIRE, plus the gathering step. The
// engine lane proves the classification; this lane proves it survives
// serialisation and that the verb actually gathers what the engine is handed —
// an engine that resolves perfectly against sets nobody fills reports a clean
// corpus forever.
TEST(SpecLintVerb, Ants4127SurfaceFieldsReachTheWire) {
    // (1) both envelope fields are ALWAYS emitted. `surfaces_checked` is the one
    // § 2.3 calls "always emitted and never inferred", and this is the only
    // layer where that is observable — a verb lane that never asserts it leaves
    // the claim untested.
    const QJsonObject empty =
        RemoteControl::specLintBuildResponse({}, false, {}, false, {}, 0, false);
    ASSERT_TRUE(empty.contains(QStringLiteral("surfaces_checked")));
    EXPECT_FALSE(empty.value(QStringLiteral("surfaces_checked")).toBool());
    ASSERT_TRUE(empty.contains(QStringLiteral("surfaces_resolved")));
    EXPECT_EQ(empty.value(QStringLiteral("surfaces_resolved")).toInt(), 0);

    // The falsifying pair: checked TRUE with the counter at ZERO. An envelope
    // built by inferring the flag from the count cannot produce this row.
    const QJsonObject checked =
        RemoteControl::specLintBuildResponse({}, false, {}, false, {}, 0, true);
    EXPECT_TRUE(checked.value(QStringLiteral("surfaces_checked")).toBool());
    EXPECT_EQ(checked.value(QStringLiteral("surfaces_resolved")).toInt(), 0);

    // (2) the three kinds, with their per-kind detail, through DocFinding's
    // serialiser — including `spec_status: null`, which must be a PRESENT key
    // holding null rather than an omitted one (§ 2.3).
    const QString doc = QStringLiteral(
        "# ANTS-1 — a spec\n"
        "\n"
        "## 3. Invariants\n"
        "\n"
        "- **INV-1** — a rule. *Test:* `tests/features/absent_one/` covers it.\n");
    SpecLint::Options opts;
    opts.existingTestDirs = {QStringLiteral("present_one")};
    const SpecLint::Result r = SpecLint::check(doc, QStringLiteral("s.md"), opts);
    ASSERT_EQ(r.findings.size(), 1);
    const QJsonObject wire = DocFinding::toJson(r.findings.first());
    EXPECT_EQ(wire.value(QStringLiteral("kind")).toString(),
              QStringLiteral("test_surface_unresolved"));
    EXPECT_EQ(wire.value(QStringLiteral("invariant")).toString(),
              QStringLiteral("INV-1"));
    EXPECT_EQ(wire.value(QStringLiteral("surface")).toString(),
              QStringLiteral("tests/features/absent_one"));
    ASSERT_TRUE(wire.contains(QStringLiteral("spec_status")));
    EXPECT_TRUE(wire.value(QStringLiteral("spec_status")).isNull());
    // The six base fields are the contract ANTS-3663 sorts on and `extra` must
    // never redefine one.
    EXPECT_EQ(wire.value(QStringLiteral("verb")).toString(),
              QStringLiteral("spec_lint"));
    EXPECT_EQ(wire.value(QStringLiteral("file")).toString(), QStringLiteral("s.md"));

    // (3) the gathering step is wired into the handler, once per run and before
    // the document walk — the shape specLintRequiredSections already uses.
    // The WHOLE body, not a fixed byte window: this handler is ~130 lines and
    // the accumulation sits at the end of the document loop, so a `mid(h, N)`
    // window silently stops testing as the body grows (the ANTS-1348 trap
    // `slurpFunctionBody` exists for).
    const std::string rcSrc = ants_test::slurpRemoteControl();
    ASSERT_FALSE(rcSrc.empty());
    const QString handler = QString::fromStdString(
        ants_test::slurpFunctionBody(rcSrc, "RemoteControl::cmdSpecLint"));
    ASSERT_FALSE(handler.isEmpty());
    EXPECT_TRUE(handler.contains(QStringLiteral("opts.existingTestDirs =")));
    EXPECT_TRUE(handler.contains(QStringLiteral("opts.wiredTestDirs    =")));
    EXPECT_TRUE(handler.contains(QStringLiteral("surfacesResolved += r.surfacesResolved")))
        << "a walk's total is the SUM over documents (§ 2.3)";

    // (4) and what it gathers against the LIVE tree is non-empty and a subset —
    // the two properties the engine's skip contract rests on. An empty scan is
    // not a failure to the engine, it is a silent skip, so nothing else would
    // notice this project's own `tests/features/` moving.
    QDir features(QString::fromUtf8(ANTS_SOURCE_DIR "/tests/features"));
    ASSERT_TRUE(features.exists());
    const QStringList dirs =
        features.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const QSet<QString> existing(dirs.begin(), dirs.end());
    EXPECT_GT(existing.size(), 100) << "an empty scan skips the check silently";

    QFile cml(QString::fromUtf8(ANTS_SOURCE_DIR "/CMakeLists.txt"));
    ASSERT_TRUE(cml.open(QIODevice::ReadOnly));
    const QString cmake = QString::fromUtf8(cml.readAll());
    static const QRegularExpression re(
        QStringLiteral(R"(tests/features/([a-z0-9_]+)/test_[a-z0-9_]+\.cpp)"));
    QSet<QString> wired;
    auto it = re.globalMatch(cmake);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (existing.contains(name)) wired.insert(name);
    }
    EXPECT_FALSE(wired.isEmpty());
    EXPECT_TRUE(wired.contains(QStringLiteral("spec_lint")))
        << "this very test's directory must be in a bundle, or it is not running";
}

// ANTS-4390 — the global standards repo must be able to check its own specs.
// `~/.claude` has NO `docs/standards/` because it IS the standards set, so
// every candidate path missed and `sections_checked` came back false on the
// one repository that owns the canonical block. The `standards/` entries are
// not a guess at a second convention; they are that repo's real layout.
//
// ANTS-4373 — and the shape around the skip is its own defect: `ok:true` with
// an empty findings[] is the envelope a genuinely clean run produces, so an
// unrun check reads as a pass. It launders downstream, because
// review-contract Phase 1d hands the mechanical results to cold lanes as
// settled facts they are forbidden to question.
TEST(SpecLintVerb, Ants4390And4373StandardResolutionAndSkipReporting) {
    const std::string rc = ants_test::slurpRemoteControl();

    // ANTS-4390 — both layouts are candidates.
    EXPECT_TRUE(rc.find("\"docs/standards/spec-format.md\"") != std::string::npos)
        << "the project layout must stay first in the resolution order";
    EXPECT_TRUE(rc.find("\"standards/spec-format.md\"") != std::string::npos)
        << "the global standards repo's own layout must resolve too — it has "
           "no docs/ prefix because it IS the standards set";

    // ANTS-4373 — an array is read where a `false` is not, and the caller is
    // told which path was consulted rather than re-deriving the cause.
    EXPECT_TRUE(rc.find("\"sections_source\"") != std::string::npos)
        << "say WHICH standard was consulted, or null when none was";
    EXPECT_TRUE(rc.find("\"skipped\"") != std::string::npos)
        << "a skipped check must be reported as a read field, not inferred "
           "from a boolean nobody branches on";
    EXPECT_TRUE(rc.find("\"skipped_hint\"") != std::string::npos)
        << "a boolean says a check did not run; it does not say what to fix";
}

// ANTS-4393 — `surfaces_checked` has a DIFFERENT cause from
// `sections_checked`, and nothing documented turns it on.
//
// ANTS-4390's bullet recorded the shared-cause hypothesis as unverified. A
// reporting project measured it: adding the in-project format standard flips
// sections_checked to TRUE and leaves surfaces_checked FALSE. Same run, same
// corpus, one input added, one flag moved. Their spec carries nine
// invariants, every one with a *Test:* clause in the documented form, and
// surfaces_resolved stayed 0.
//
// That retires a guess a later session would reasonably have acted on —
// fixing sections_checked and expecting the other to follow. And it leaves a
// sharper asymmetry: there is a documented input for one silent check and
// NONE for the other, so `ok:true, findings:[]` says nothing about test
// surfaces on every project, permanently.
TEST(SpecLintVerb, Ants4393BothSkippedChecksAreNamed) {
    const std::string rc = ants_test::slurpRemoteControl();

    // ANTS-4666 re-fixture. This asserted that skipped[] names
    // `invariant_no_test`, and that was wrong: that finding fires for any
    // INV-N with an empty *Test:* clause and is gated on NOTHING, so the
    // envelope could carry its findings and name it as skipped in one breath.
    // The gated checks are the three surface-resolution kinds. The invariant
    // ANTS-4373 was reaching for — name every gated check that did not run —
    // is unchanged; only the names were wrong.
    EXPECT_NE(rc.find("\"test_surface_absent\""), std::string::npos)
        << "the skip list must name the checks that are actually gated";
    EXPECT_NE(rc.find("\"test_surface_unresolved\""), std::string::npos);
    EXPECT_NE(rc.find("\"test_surface_unwired\""), std::string::npos);
    EXPECT_NE(rc.find("surfaces_skipped_hint"), std::string::npos)
        << "…and say what would turn them on, because the flag otherwise "
           "reads as a capability a caller cannot act on";

    // ANTS-4679 re-fixture. This required the hint to say "NO documented
    // input" turns the check on. That was false: surfacesChecked is
    // !existingTestDirs.isEmpty(), filled by scanning <root>/tests/features/,
    // and this very project comes back surfaces_checked:true because it has
    // one. A false cause stated for a true skip is the class ANTS-4373 exists
    // to close, so the hint must name the real input instead.
    const auto pos = rc.find("surfaces_skipped_hint");
    ASSERT_NE(pos, std::string::npos);
    const std::string body = rc.substr(pos, 900);
    EXPECT_NE(body.find("tests/features/<name>/"), std::string::npos)
        << "the hint must name the input that turns the check on";
    EXPECT_EQ(body.find("NO documented input"), std::string::npos)
        << "the old wording claimed no input exists, and one does";
}

// ANTS-4666 — the general invariant behind the rename, asserted rather than
// argued: a check cannot be BOTH reported as skipped and be the source of a
// finding in the same envelope. One run returned findings[] of kind
// invariant_no_test, counts{invariant_no_test:1}, and skipped:["invariant_no
// _test"] together, which leaves a caller to either publish a false disclosure
// or redo by hand the work the verb had just done.
//
// write-spec Step 4 is the consumer that makes it bite: it requires disclosing
// any check that did not run AND performing that check itself.
TEST(SpecLintVerb, Ants4666SkippedAndCountsAreDisjoint) {
    DocFinding::Finding f;
    f.verb    = QStringLiteral("spec_lint");
    f.kind    = QStringLiteral("invariant_no_test");
    f.file    = QStringLiteral("docs/specs/X.md");
    f.line    = 12;
    f.message = QStringLiteral("INV-1 carries no test-surface clause");

    // Surfaces gated OFF, yet the invariant_no_test check has still produced a
    // finding — which is the exact envelope that was reported.
    const QJsonObject o = RemoteControl::specLintBuildResponse(
        {f}, true, QJsonObject{}, false,
        QStringList{QStringLiteral("docs/specs/X.md")}, 0, false,
        QStringLiteral("docs/standards/specs.md"));

    const QJsonObject counts = o.value(QStringLiteral("counts")).toObject();
    ASSERT_EQ(counts.value(QStringLiteral("invariant_no_test")).toInt(), 1)
        << "the check ran and produced a finding";

    for (const QJsonValue &v : o.value(QStringLiteral("skipped")).toArray()) {
        const QString k = v.toString();
        EXPECT_FALSE(counts.contains(k))
            << "a check reported as skipped must not also appear in counts: "
            << k.toStdString();
    }
}

// ANTS-4676 — a skipped[] entry and the field explaining it shared no token:
// `invariant_no_test` is explained by `surfaces_skipped_hint`, and
// `missing_section` by the generically-named `skipped_hint`. A reporting
// project read the pair as two skips, matched the hint to the wrong entry, and
// filed the hinted skip as unexplained — then hand-checked what the envelope
// had already answered. The PAIRING was never broken; its labelling was, and
// tools/list taught the wrong model by promising one hint for the array.
TEST(SpecLintVerb, Ants4676EachHintNamesItsSkippedEntry) {
    // The reporter's exact envelope: surfaces skipped, sections checked.
    const QJsonObject o = RemoteControl::specLintBuildResponse(
        {}, true, QJsonObject{}, false,
        QStringList{QStringLiteral("docs/specs/X.md")}, 0, false,
        QStringLiteral("~global/standards/spec-format.md"));
    QStringList skipped;
    for (const QJsonValue &v :
         o.value(QStringLiteral("skipped")).toArray())
        skipped << v.toString();
    // ANTS-4666 re-fixture: the gated checks are the surface-resolution kinds.
    EXPECT_EQ(skipped, (QStringList{QStringLiteral("test_surface_absent"),
                                    QStringLiteral("test_surface_unresolved"),
                                    QStringLiteral("test_surface_unwired")}))
        << "sections_checked:true means only the surface checks skipped";
    EXPECT_FALSE(skipped.contains(QStringLiteral("invariant_no_test")))
        << "ANTS-4666: that check is gated on nothing and always runs, so "
           "naming it here contradicts its own findings in the same envelope";
    const QString sh =
        o.value(QStringLiteral("surfaces_skipped_hint")).toString();
    for (const QString &k : skipped)
        EXPECT_TRUE(sh.contains(k))
            << "the hint must name every skipped[] entry it explains, or a "
               "caller cannot tell which entry it belongs to: "
            << sh.toStdString();
    EXPECT_FALSE(o.contains(QStringLiteral("skipped_hint")))
        << "skipped_hint explains missing_section, which did NOT skip here — "
           "its absence is correct and is what the reporter read as a gap";

    // …and the sections hint names its own entry on BOTH of its arms.
    const QJsonObject none = RemoteControl::specLintBuildResponse(
        {}, false, QJsonObject{}, false, QStringList{}, 0, false, QString());
    EXPECT_TRUE(none.value(QStringLiteral("skipped_hint")).toString()
                    .contains(QStringLiteral("missing_section")))
        << "the empty-walk arm must name its entry too";
    const QJsonObject some = RemoteControl::specLintBuildResponse(
        {}, false, QJsonObject{}, false,
        QStringList{QStringLiteral("docs/specs/X.md")}, 0, false, QString());
    EXPECT_TRUE(some.value(QStringLiteral("skipped_hint")).toString()
                    .contains(QStringLiteral("missing_section")));
}

// ANTS-4080 — the global tier. `~/.claude/standards/spec-format.md` became the
// authoritative spec-format standard on 2026-08-08 with projects carrying
// deltas, so a project with no local copy is linted against NOTHING and the
// skip reads as a clean structural result one layer downstream.
//
// This is not the `_shared` fallback ANTS-3662 § 2.1 rejects. That one always
// exists, so the skip arm could never fire; this one resolves through
// `expandGlobalConfigSentinel`, whose `QDir::homePath()` follows `$HOME` — so
// `none` stays a reachable outcome and the skip arm stays testable.
//
// The hint rows are BEHAVIOURAL (the pure builder is a public static); the
// resolution rows are a source scrape, because the resolver is a file-static
// the handler needs a live MainWindow to reach.
TEST(SpecLintVerb, Ants4080GlobalTierAndTheTwoSkipCauses) {
    const std::string rc = ants_test::slurpRemoteControl();

    EXPECT_NE(rc.find("expandGlobalConfigSentinel(QStringLiteral(\"~global\"))"),
              std::string::npos)
        << "the global tier must re-root through the SAME sentinel ANTS-3719 "
           "gave doc_integrity, not widen this verb's bad_path contract";
    EXPECT_NE(rc.find("QStringLiteral(\"~global/\")"), std::string::npos)
        << "sections_source must distinguish a global hit from a project one; "
           "ANTS-4373 made that field the path, so the path is prefixed";

    // An EMPTY walk checked no document, so no check of any kind ran and the
    // format standard is irrelevant. Naming it there states a false cause for
    // a true skip — the class ANTS-4373 exists to close, reintroduced by the
    // hint that predates this row.
    const QJsonObject none = RemoteControl::specLintBuildResponse(
        {}, false, QJsonObject{}, false, QStringList{}, 0, false, QString());
    ASSERT_TRUE(none.contains(QStringLiteral("skipped_hint")));
    const QString h = none.value(QStringLiteral("skipped_hint")).toString();
    EXPECT_TRUE(h.contains(QStringLiteral("no document was checked")))
        << h.toStdString();
    EXPECT_FALSE(h.contains(QStringLiteral("required-sections")))
        << "an empty walk says nothing about the standard: " << h.toStdString();

    // A walk that DID read a document and still skipped names the paths that
    // were actually consulted — and there are now six, not four.
    const QJsonObject some = RemoteControl::specLintBuildResponse(
        {}, false, QJsonObject{}, false,
        QStringList{QStringLiteral("docs/specs/X.md")}, 0, false, QString());
    const QString h2 = some.value(QStringLiteral("skipped_hint")).toString();
    EXPECT_TRUE(h2.contains(QStringLiteral("docs/standards/spec-format.md")))
        << h2.toStdString();
    EXPECT_TRUE(h2.contains(QStringLiteral("~/.claude/")))
        << "the hint must name the global tier it now consults: "
        << h2.toStdString();
}

// ---------------------------------------------------------------------------
// ANTS-4737 — the cap must trim what is EMITTED and nothing else.
//
// Measured at three caps over one unchanged corpus: uncapped reported 81,
// max_findings:40 reported 39, max_findings:5 reported 5. `truncated` was set,
// so the run was not silent — but a caller reading `counts` to ask "how much is
// there" got a number that was a function of its own argument. It bit a real
// measurement: the reporter was sizing the effect of adopting a spec-format
// standard, a capped call reported 39 where the truth was 81, and the fix
// looked twice as effective as it was. Two runs taken at different caps cannot
// be compared at all.
//
// workspace_search's `count_only` already documents `count` as the TRUE total,
// uncapped by max_results. Two verbs disagreeing about what a count means is
// the defect underneath this one.
TEST(SpecLintVerb, Ants4737CountsAreUncappedAndOnlyFindingsAreTrimmed) {
    const auto mk = [](int line) {
        DocFinding::Finding f;
        f.verb    = QStringLiteral("spec_lint");
        f.kind    = QStringLiteral("invariant_no_test");
        f.file    = QStringLiteral("docs/specs/X.md");
        f.line    = line;
        f.message = QStringLiteral("INV carries no test-surface clause");
        return f;
    };
    QList<DocFinding::Finding> all;
    for (int i = 1; i <= 10; ++i) all.push_back(mk(i));

    const auto build = [&](int cap) {
        return RemoteControl::specLintBuildResponse(
            all, true, QJsonObject{}, false,
            QStringList{QStringLiteral("docs/specs/X.md")}, 0, false,
            QStringLiteral("docs/standards/specs.md"), cap);
    };

    const QJsonObject capped = build(3);
    EXPECT_EQ(capped.value(QStringLiteral("findings")).toArray().size(), 3)
        << "the cap bounds the payload, which is what it is for";
    EXPECT_EQ(capped.value(QStringLiteral("findings_total")).toInt(), 10)
        << "the total the reporter probed for, and could not find";
    EXPECT_EQ(capped.value(QStringLiteral("counts")).toObject()
                  .value(QStringLiteral("invariant_no_test")).toInt(), 10)
        << "the cap must not choose the count — this is the whole defect";
    EXPECT_TRUE(capped.value(QStringLiteral("truncated")).toBool())
        << "a trimmed payload must still say it was trimmed";

    // The same corpus at a different cap must report the SAME counts, which is
    // the property that makes a before/after comparison meaningful.
    const QJsonObject tighter = build(1);
    EXPECT_EQ(tighter.value(QStringLiteral("findings")).toArray().size(), 1);
    EXPECT_EQ(tighter.value(QStringLiteral("counts")).toObject(),
              capped.value(QStringLiteral("counts")).toObject())
        << "two runs over one unchanged corpus at two caps must agree";
    EXPECT_EQ(tighter.value(QStringLiteral("findings_total")).toInt(), 10);

    // An uncapped run is unchanged: nothing trimmed, and `truncated` absent
    // rather than set. Without this arm every assertion above is satisfied by
    // an implementation that marks every run truncated.
    const QJsonObject full = build(50);
    EXPECT_EQ(full.value(QStringLiteral("findings")).toArray().size(), 10);
    EXPECT_FALSE(full.value(QStringLiteral("truncated")).toBool());
    EXPECT_EQ(full.value(QStringLiteral("findings_total")).toInt(), 10);
}

// The builder is only correct if the WALK hands it the caller's cap. Without
// this, removing that argument leaves every run uncapped — max_findings stops
// bounding the payload it exists to bound, and the behavioural test above
// cannot see it because it calls the builder directly.
TEST(SpecLintVerb, Ants4737WalkHandsTheCallerCapToTheBuilder) {
    const std::string body = ants_test::slurpFunctionBody(
        ants_test::slurpRemoteControl(),
        "QJsonDocument RemoteControl::cmdSpecLint");
    ASSERT_FALSE(body.empty()) << "cmdSpecLint body not found";
    EXPECT_NE(body.find("sectionsSource, callerCap)"), std::string::npos)
        << "max_findings must reach specLintBuildResponse, or nothing trims";
}
