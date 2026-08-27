// Source-grep harness for ANTS-1308 — locks the wiring contract for
// the `invariant_check` MCP tool. See spec.md.
//
// Exit 0 = all 8 invariants hold.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <string>

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include "../../_support/srcgrep.h"

#ifndef SRC_CLAUDE_INTEGRATION_CPP_PATH
#error "SRC_CLAUDE_INTEGRATION_CPP_PATH compile definition required"
#endif
#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef ANTS_RC_SOURCES
#error "ANTS_RC_SOURCES compile definition required"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#error "SRC_MAINWINDOW_CPP_PATH compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {


bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

std::string extractFunctionBody(const std::string &src,
                                const std::string &declarationStart) {
    const auto pos = src.find(declarationStart);
    if (pos == std::string::npos) return {};
    auto end = src.find("\nQJsonDocument RemoteControl::cmd",
                        pos + declarationStart.size());
    if (end == std::string::npos) end = src.size();
    return src.substr(pos, end - pos);
}

}  // namespace

TEST(McpInvariantCheck, WiringContract) {
    expect_reset();

    const std::string rcHdr = ants_test::slurpFile(SRC_RC_HEADER);
    const std::string rcCpp = ants_test::slurpRemoteControl();
    const std::string ciCpp = ants_test::slurpFile(SRC_CLAUDE_INTEGRATION_CPP_PATH);
    const std::string mwCpp = ants_test::slurpFile(SRC_MAINWINDOW_CPP_PATH);

    // INV-1 — declaration on RemoteControl.
    expect(contains(rcHdr, "cmdInvariantCheck(const QJsonObject &req)"),
           "INV-1",
           "cmdInvariantCheck decl missing from src/remotecontrol.h");

    // INV-2 — definition + ANTS-1308 anchor.
    const std::string body =
        extractFunctionBody(rcCpp,
            "QJsonDocument RemoteControl::cmdInvariantCheck(");
    expect(!body.empty(),
           "INV-2a",
           "cmdInvariantCheck body missing from the remotecontrol TUs");
    expect(contains(rcCpp, "ANTS-1308"),
           "INV-2b",
           "cmdInvariantCheck section must carry an ANTS-1308 anchor");

    // INV-3 — bad_files refusal.
    expect(contains(body, "bad_files"),
           "INV-3",
           "cmdInvariantCheck must refuse missing/empty files with "
           "code:bad_files");

    // INV-4 — directory walk via QDir + ANTS-*.md filter.
    expect(contains(body, "QDir"),
           "INV-4a",
           "cmdInvariantCheck must iterate via QDir");
    // ANTS-4376 — this assertion used to REQUIRE the `ANTS-*.md` glob, which
    // pinned the defect: the verb saw nothing on any project whose id prefix
    // is not ANTS (LottoTracker 0 specs scanned where spec_query lists 8,
    // DOOM 0 where it lists 19, OneUp 0). Inverted deliberately — a
    // project-prefix-specific glob must never come back.
    expect(!contains(body, "ANTS-*.md"),
           "INV-4b",
           "cmdInvariantCheck must NOT filter specs by a project-specific "
           "prefix glob — it saw nothing on every project but this one");
    expect(contains(body, "specsDir"),
           "INV-4c",
           "cmdInvariantCheck must honour .ants/project.json specs_dir "
           "rather than hard-coding docs/specs");

    // INV-5 — shared parser delegation.
    expect(contains(body, "parseSpecBody"),
           "INV-5",
           "cmdInvariantCheck must delegate parsing to the shared "
           "parseSpecBody helper (single-source the parser)");

    // INV-6 — mainwindow registration.
    expect(contains(mwCpp, "registerToolProvider(\"invariant_check\""),
           "INV-6a",
           "MainWindow must register \"invariant_check\" via "
           "registerToolProvider");
    expect(contains(mwCpp, "cmdInvariantCheck"),
           "INV-6b",
           "MainWindow registration must delegate to "
           "m_remoteControl->cmdInvariantCheck");

    // INV-7 — tools/list schema entry.
    expect(contains(ciCpp, "t[\"name\"] = \"invariant_check\""),
           "INV-7a",
           "tools/list block must register an \"invariant_check\" "
           "entry");
    {
        // ANTS-3720 — self-sizing descriptor block. This was a fixed 3000-byte
        // window from the ANTS-1308 anchor, which ANTS-3699's `mode` property
        // pushed `req.append("files")` straight past: a scrape that measures
        // the descriptor's length, not the wiring it claims to lock.
        const std::string region =
            ants_test::mcpToolDescriptor(ciCpp, "invariant_check");
        ASSERT_FALSE(region.empty())
            << "invariant_check descriptor block not found in "
               "src/claudeintegration.cpp";
        expect(contains(region, "req.append(\"files\")"),
               "INV-7b",
               "invariant_check schema must mark \"files\" as required");
        expect(contains(region, "req.append(\"caller_cwd\")"),
               "INV-7c",
               "invariant_check schema must mark \"caller_cwd\" as "
               "required");
        expect(contains(region, "minItems"),
               "INV-7d",
               "invariant_check schema's files array must declare a "
               "minItems constraint (non-empty)");
    }

    // INV-8 — Required contract.
    {
        const auto pos = ciCpp.find(
            "callerCwdContractFor(const QString &toolName)");
        ASSERT_NE(pos, std::string::npos);
        const auto end = ciCpp.find("\n}\n", pos);
        ASSERT_NE(end, std::string::npos);
        const std::string fn = ciCpp.substr(pos, end - pos);
        const auto branch = fn.find("\"invariant_check\"");
        ASSERT_NE(branch, std::string::npos)
            << "invariant_check must have an explicit branch in "
               "callerCwdContractFor";
        const auto eol = fn.find('\n', branch);
        ASSERT_NE(eol, std::string::npos);
        const std::string line = fn.substr(branch, eol - branch);
        expect(line.find("C::Required;") != std::string::npos,
               "INV-8",
               "invariant_check must be classified C::Required");
    }
}

namespace {

// Seed <root>/docs/specs/<id>.md with one INV bullet whose body is long
// enough that its presence or absence is unmistakable in the envelope.
void seedSpec(const QString &root, const QString &id, const QString &mentions) {
    QDir(root).mkpath(QStringLiteral("docs/specs"));
    QFile f(root + QStringLiteral("/docs/specs/") + id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write((QStringLiteral("# ") + id + QStringLiteral(" — seeded\n\n"
            "**Status:** accepted.\n\n## 2. Surface\n\nTouches `") + mentions +
            QStringLiteral("` in anger.\n\n## 3. Invariants\n\n"
            "- **INV-1** — ") + QString(400, QLatin1Char('x')) +
            QStringLiteral(". *Test:* T1.\n"
            "- **INV-2** — ") + QString(400, QLatin1Char('y')) +
            QStringLiteral(". *Test:* T2.\n")).toUtf8());
}

QJsonObject runCheck(const QString &root, const QString &file,
                     const QString &mode) {
    RemoteControl rc(nullptr);
    QJsonObject req;
    req[QStringLiteral("caller_cwd")] = root;
    QJsonArray files;
    files.append(file);
    req[QStringLiteral("files")] = files;
    if (!mode.isEmpty()) req[QStringLiteral("mode")] = mode;
    return rc.cmdInvariantCheck(req).object();
}

}  // namespace

// INV-9 (ANTS-3699) — summary is the DEFAULT shape: the match list survives,
// the invariant BODIES do not, and the envelope says so. The whole point is
// that a caller who knows nothing about `mode` gets the cheap answer, so the
// default is what this pins.
TEST(McpInvariantCheck, Ants3699SummaryOmitsBodiesByDefault) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("ANTS-9001"),
             QStringLiteral("src/widget.cpp"));

    const QJsonObject env =
        runCheck(dir.path(), QStringLiteral("src/widget.cpp"), QString());
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    EXPECT_EQ(env.value("mode").toString(), "summary");
    EXPECT_FALSE(env.value("invariants_included").toBool());

    const QJsonObject spec =
        env.value("matched_specs").toArray().at(0).toObject();
    EXPECT_EQ(spec.value("id").toString(), "ANTS-9001");
    EXPECT_EQ(spec.value("path").toString(), "docs/specs/ANTS-9001.md");
    EXPECT_EQ(spec.value("matched_terms").toArray().size(), 1);
    // The count is the real one even though the bodies are gone — that is what
    // makes the summary a usable answer rather than a truncation.
    EXPECT_EQ(spec.value("invariants_count").toInt(), 2);
    EXPECT_FALSE(spec.contains("invariants"))
        << "summary mode must OMIT invariant bodies, not shorten them";
    EXPECT_TRUE(env.contains("hint"))
        << "a summary with matches must say how to get the bodies";
}

// INV-10 (ANTS-3699) — mode:"full" restores the bodies verbatim, and the
// envelope's shape flags flip with it.
TEST(McpInvariantCheck, Ants3699FullRestoresBodies) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("ANTS-9002"),
             QStringLiteral("src/widget.cpp"));

    const QJsonObject env = runCheck(dir.path(),
                                     QStringLiteral("src/widget.cpp"),
                                     QStringLiteral("full"));
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    EXPECT_EQ(env.value("mode").toString(), "full");
    EXPECT_TRUE(env.value("invariants_included").toBool());
    EXPECT_FALSE(env.contains("hint"));

    const QJsonObject spec =
        env.value("matched_specs").toArray().at(0).toObject();
    const QJsonArray invs = spec.value("invariants").toArray();
    ASSERT_EQ(invs.size(), 2);
    EXPECT_EQ(spec.value("invariants_count").toInt(), invs.size());
    EXPECT_TRUE(invs.at(0).toObject().value("body").toString().contains(
        QString(400, QLatin1Char('x'))))
        << "full mode must carry the invariant body verbatim";
}

// INV-11 (ANTS-3699) — an unknown mode refuses rather than silently picking
// one. A typo'd "brief" that quietly returned summary would look identical to
// a spec with no invariants.
TEST(McpInvariantCheck, Ants3699UnknownModeRefuses) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("ANTS-9003"),
             QStringLiteral("src/widget.cpp"));

    const QJsonObject env = runCheck(dir.path(),
                                     QStringLiteral("src/widget.cpp"),
                                     QStringLiteral("brief"));
    EXPECT_FALSE(env.value("ok").toBool());
    EXPECT_EQ(env.value("code").toString(), "bad_mode");
}

// ANTS-4376 — behavioural: a project whose specs are NOT `ANTS-*` is seen.
// The guard above only proves the bad glob is gone; this proves the verb
// actually reads such a project's specs.
TEST(McpInvariantCheck, ScansSpecsOfAnyProjectPrefix) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    ASSERT_TRUE(QDir().mkpath(root + "/docs/specs"));

    const auto write = [&](const QString &rel, const QString &body) {
        QFile f(root + QLatin1Char('/') + rel);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(body.toUtf8());
    };
    // LottoTracker's real shape: a non-ANTS prefix AND a topic suffix.
    write(QStringLiteral("docs/specs/LOTTO-0001-ticket-tracker.md"),
          QStringLiteral("# LOTTO-0001 — tickets\n\n"
                         "## 3. Invariants\n\n"
                         "- **INV-1** — `check.py` holds. *Test:* a test.\n"));
    write(QStringLiteral("docs/specs/DOOM-0331-bloom.md"),
          QStringLiteral("# DOOM-0331 — bloom\n\n"
                         "## 3. Invariants\n\n"
                         "- **INV-1** — unrelated. *Test:* a test.\n"));

    QJsonObject req;
    req["caller_cwd"] = root;
    QJsonArray files; files.append(QStringLiteral("check.py"));
    req["files"] = files;

    RemoteControl rc(nullptr, nullptr);
    const QJsonObject out = rc.cmdInvariantCheck(req).object();

    EXPECT_TRUE(out.value("ok").toBool());
    EXPECT_EQ(out.value("specs_scanned").toInt(), 2)
        << "both specs must be READ, whatever their id prefix";
    EXPECT_EQ(out.value("matched_count").toInt(), 1)
        << "and only the one citing check.py matches";
}

// ANTS-4376 / ANTS-4374 — a scan that read NOTHING must be distinguishable
// from a scan that read specs and matched none. Those were the same envelope
// for three sessions, which is what let a blind verb read as an all-clear.
TEST(McpInvariantCheck, ScanningNothingIsDistinguishableFromMatchingNothing) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString root = QFileInfo(tmp.path()).canonicalFilePath();
    QJsonObject req;
    req["caller_cwd"] = root;
    QJsonArray files; files.append(QStringLiteral("src/x.cpp"));
    req["files"] = files;

    RemoteControl rc(nullptr, nullptr);
    // (a) no specs dir at all — scanned nothing.
    const QJsonObject none = rc.cmdInvariantCheck(req).object();
    EXPECT_TRUE(none.value("ok").toBool());
    EXPECT_EQ(none.value("matched_count").toInt(), 0);
    EXPECT_TRUE(none.value("scanned_nothing").toBool())
        << "matched_count:0 with nothing read must say so";
    EXPECT_FALSE(none.value("hint").toString().isEmpty());

    // (b) a real spec that simply does not mention the file — matched
    // nothing, but DID look. No scanned_nothing marker.
    ASSERT_TRUE(QDir().mkpath(root + "/docs/specs"));
    QFile f(root + "/docs/specs/PROJ-0001-thing.md");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("# PROJ-0001 — thing\n\n## 3. Invariants\n\n"
            "- **INV-1** — about something else. *Test:* a test.\n");
    f.close();
    const QJsonObject looked = rc.cmdInvariantCheck(req).object();
    EXPECT_EQ(looked.value("matched_count").toInt(), 0);
    EXPECT_EQ(looked.value("specs_scanned").toInt(), 1);
    EXPECT_FALSE(looked.contains("scanned_nothing"))
        << "it looked and found nothing — that is the legitimate case";
}

// ANTS-4644 — a spec cites a module the way a human writes it, so the
// project-relative form this verb's own description prescribes is the one that
// matches nothing. Reproduced in this repo: docs/specs/ANTS-2161.md, which
// governs op:detect, cites `projectsettings.cpp` and is invisible to a query
// for `src/projectsettings.cpp`.
TEST(McpInvariantCheck, Ants4644PathSuffixFallbackRescuesAConfidentZero) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("PROJ-0007"),
             QStringLiteral("services/auth.py"));

    const QJsonObject env =
        runCheck(dir.path(),
                 QStringLiteral("src/finbreak/services/auth.py"), QString());
    ASSERT_TRUE(env.value("ok").toBool());
    ASSERT_EQ(env.value("matched_count").toInt(), 1)
        << "the prescribed path form must not answer a confident zero";
    EXPECT_TRUE(env.value("fallback_match").toBool());
    EXPECT_EQ(env.value("fallback_kind").toString(), "path_suffix");
    // A rescued hit that looks like a direct hit is a different lie from the
    // one being fixed — say which form actually matched.
    EXPECT_EQ(env.value("matched_as").toObject()
                  .value("src/finbreak/services/auth.py").toString(),
              "services/auth.py");
    EXPECT_FALSE(env.value("hint").toString().isEmpty());
}

// ANTS-4566 — ANTS-4644's rescue fires only on a ZERO, so the worse case went
// unfixed: a query that matches ONE spec, on an incidental mention, never
// learns that the spec actually GOVERNING the file cites it by basename. An
// empty result prompts a second look; a confident single result does not — and
// write-code Phase 0 designs the edit against whatever came back.
TEST(McpInvariantCheck, Ants4566NearMissIsReportedBesideAConfidentHit) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    // The reporter's shape: a packaging spec mentions the full path once in a
    // list, and the spec that sets the file's contract names it by basename.
    seedSpec(dir.path(), QStringLiteral("PROJ-0025"),
             QStringLiteral("src/ui/card.cpp"));
    seedSpec(dir.path(), QStringLiteral("PROJ-0017"),
             QStringLiteral("card.cpp"));

    const QJsonObject env =
        runCheck(dir.path(), QStringLiteral("src/ui/card.cpp"), QString());
    ASSERT_TRUE(env.value("ok").toBool());

    // matched_specs keeps its exact meaning: the direct hit, and only it.
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    EXPECT_EQ(env.value("matched_specs").toArray().at(0).toObject()
                  .value("id").toString(), "PROJ-0025");
    EXPECT_FALSE(env.value("fallback_match").toBool())
        << "the rescue must NOT fire — this is a non-empty result, which is "
           "precisely why the near miss was invisible";

    // …and the governing spec is surfaced beside it rather than merged in.
    const QJsonArray near = env.value("basename_matches").toArray();
    ASSERT_EQ(near.size(), 1)
        << "the spec citing the file by basename must be reported";
    EXPECT_EQ(near.at(0).toObject().value("id").toString(), "PROJ-0017");
    EXPECT_EQ(near.at(0).toObject().value("matched_terms").toArray()
                  .at(0).toString(), "card.cpp")
        << "say WHICH shorter form matched, so a near miss never passes for "
           "a direct hit";
    EXPECT_EQ(env.value("basename_matches_count").toInt(), 1);
    EXPECT_FALSE(env.value("basename_matches_hint").toString().isEmpty());
}

// ANTS-4566 — the field is emitted only when it says something. A direct hit
// with nothing citing the file by a shorter form must not carry an empty array
// on every call.
TEST(McpInvariantCheck, Ants4566NoNearMissMeansNoField) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("PROJ-0026"),
             QStringLiteral("src/ui/card.cpp"));

    const QJsonObject env =
        runCheck(dir.path(), QStringLiteral("src/ui/card.cpp"), QString());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    EXPECT_FALSE(env.contains("basename_matches"))
        << "absent means no spec cites it by a shorter form, which is the "
           "same answer an empty array gives at a cost";
}

// ANTS-4644 — the bare basename is its own tier because it is the one that can
// collide, and it is also the tier the ANTS-2161 case needs.
TEST(McpInvariantCheck, Ants4644BasenameIsItsOwnTier) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("PROJ-0008"),
             QStringLiteral("projectsettings.cpp"));

    const QJsonObject env =
        runCheck(dir.path(), QStringLiteral("src/projectsettings.cpp"),
                 QString());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    EXPECT_TRUE(env.value("fallback_match").toBool());
    EXPECT_EQ(env.value("fallback_kind").toString(), "basename")
        << "a basename hit must be reported as one, not as a path match";
    EXPECT_EQ(env.value("matched_as").toObject()
                  .value("src/projectsettings.cpp").toString(),
              "projectsettings.cpp");
}

// ANTS-4644 — the basename tier is GATED behind the fuller forms, so an
// unrelated `auth.py` elsewhere in the corpus cannot dilute a good answer.
TEST(McpInvariantCheck, Ants4644BasenameTierGatedBehindFullerForms) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("PROJ-0009"),
             QStringLiteral("services/auth.py"));
    seedSpec(dir.path(), QStringLiteral("PROJ-0010"),
             QStringLiteral("auth.py"));   // a different module, same basename

    const QJsonObject env =
        runCheck(dir.path(),
                 QStringLiteral("src/finbreak/services/auth.py"), QString());
    ASSERT_EQ(env.value("matched_count").toInt(), 1)
        << "the colliding basename must not be admitted once a longer form hit";
    EXPECT_EQ(env.value("matched_specs").toArray().at(0).toObject()
                  .value("id").toString(), "PROJ-0009");
    EXPECT_EQ(env.value("fallback_kind").toString(), "path_suffix");
}

// ANTS-4644 — a direct hit must stay a direct hit, and say so. `fallback_match`
// is emitted on every reply: an absent flag is exactly how the defect above
// reads to a caller that cannot tell which build it is talking to.
TEST(McpInvariantCheck, Ants4644DirectHitNeverFallsBack) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("PROJ-0011"),
             QStringLiteral("src/widget.cpp"));

    const QJsonObject env =
        runCheck(dir.path(), QStringLiteral("src/widget.cpp"), QString());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    ASSERT_TRUE(env.contains("fallback_match"));
    EXPECT_FALSE(env.value("fallback_match").toBool());
    EXPECT_FALSE(env.contains("matched_as"));
    EXPECT_FALSE(env.contains("fallback_kind"));
}

// ANTS-4645 — the envelope is confident and complete-looking, and the ROADMAP
// was never in scope. The harm case is a NON-zero answer, so the scope note
// rides on a matching reply too, not only on an empty one.
TEST(McpInvariantCheck, Ants4645SaysTheRoadmapWasNotConsulted) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("PROJ-0012"),
             QStringLiteral("src/registry.py"));

    const QJsonObject env =
        runCheck(dir.path(), QStringLiteral("src/registry.py"), QString());
    ASSERT_EQ(env.value("matched_count").toInt(), 1);
    ASSERT_TRUE(env.contains("roadmap_scanned"))
        << "an absent flag is indistinguishable from a build that never checked";
    EXPECT_FALSE(env.value("roadmap_scanned").toBool());
    EXPECT_TRUE(env.value("scope_note").toString().contains("roadmap_query"))
        << "the note must name where roadmap coverage actually comes from";
}

// ANTS-4742 — a zero that is NOT `scanned_nothing` must say WHY it can be
// wrong.
//
// Every match tier here keys on the PATH: as given, with leading components
// stripped, or by bare filename. A spec that cites the module by SYMBOL carries
// no path form at all, so no suffix of the path can reach it — and the reply
// then reads "no spec governs this file", which is the opposite of the truth.
//
// The direction is what makes it worth a field. write-code's Phase 0 makes this
// the first lookup, ahead of the first line of code: a false zero sends the
// session to write against no contract, while a false hit is visible and gets
// read. On the reporting session the missed spec was the one that DECIDED the
// fix — it named the remedy as the opposite of the one the finding implied, so
// a session trusting the zero would have shipped the wrong fix and it would
// have passed its tests.
TEST(McpInvariantCheck, Ants4742ZeroSaysMatchingIsByPathOnly) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    // The reporter's shape: the governing spec names the module by SYMBOL, so
    // no path form of it appears anywhere in the document.
    seedSpec(dir.path(), QStringLiteral("PROJ-0019"),
             QStringLiteral("vault_migration.resume"));

    const QJsonObject env = runCheck(
        dir.path(), QStringLiteral("services/vault_migration.py"), QString());
    ASSERT_EQ(env.value("matched_count").toInt(), 0);
    ASSERT_GT(env.value("total_scanned").toInt(), 0)
        << "specs WERE read — this is not the scanned_nothing case";
    EXPECT_FALSE(env.contains("scanned_nothing"))
        << "the two zeroes are different and must stay distinguishable";

    EXPECT_TRUE(env.value("path_match_only").toBool())
        << "a silent zero is indistinguishable from a genuine absence";
    const QString hint = env.value("hint").toString();
    EXPECT_TRUE(hint.contains(QStringLiteral("PATH substring")))
        << "the hint must say what was actually matched against";
    EXPECT_TRUE(hint.contains(QStringLiteral("workspace_search")))
        << "naming the cheap fallback is what makes the hint actionable";

    // It fires ONLY on the zero. A reply that matched something is not in
    // doubt, and a constant flag would be noise. Without this arm the test is
    // satisfied by an implementation that flags every call.
    seedSpec(dir.path(), QStringLiteral("PROJ-0020"),
             QStringLiteral("services/vault_migration.py"));
    const QJsonObject hit = runCheck(
        dir.path(), QStringLiteral("services/vault_migration.py"), QString());
    ASSERT_GT(hit.value("matched_count").toInt(), 0);
    EXPECT_FALSE(hit.contains("path_match_only"));
}

// ANTS-4744 — `paths` is an alias for `files`.
//
// The multi-path verbs disagreed on the name: read_regions takes `items` with
// three aliases, doc_integrity and file_outline take `paths`, and this one took
// `files` alone. A caller arriving from any of the other three paid a round trip
// on a refusal. There is no files-versus-directories distinction behind the
// name — every entry is substring-matched — so the asymmetry was accidental.
TEST(McpInvariantCheck, Ants4744PathsIsAnAliasForFiles) {
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    seedSpec(dir.path(), QStringLiteral("PROJ-0044"),
             QStringLiteral("src/store.py"));

    RemoteControl rc(nullptr);
    const auto call = [&](const char *key) {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = dir.path();
        QJsonArray a; a.append(QStringLiteral("src/store.py"));
        req[QString::fromUtf8(key)] = a;
        return rc.cmdInvariantCheck(req).object();
    };

    const QJsonObject viaFiles = call("files");
    ASSERT_TRUE(viaFiles.value("ok").toBool());
    ASSERT_EQ(viaFiles.value("matched_count").toInt(), 1);

    const QJsonObject viaPaths = call("paths");
    ASSERT_TRUE(viaPaths.value("ok").toBool())
        << "the alias must not refuse — a wasted round trip is the whole cost";
    EXPECT_EQ(viaPaths.value("matched_count").toInt(),
              viaFiles.value("matched_count").toInt())
        << "the alias must give the SAME answer, not merely an ok one";

    // `files` wins when both are sent — the house rule for every alias here.
    // Without this arm the alias could be implemented as "last key wins".
    {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = dir.path();
        QJsonArray good; good.append(QStringLiteral("src/store.py"));
        QJsonArray bad;  bad.append(QStringLiteral("src/nothing_cited.py"));
        req[QStringLiteral("files")] = good;
        req[QStringLiteral("paths")] = bad;
        const QJsonObject both = rc.cmdInvariantCheck(req).object();
        EXPECT_EQ(both.value("matched_count").toInt(), 1)
            << "`files` must win, so a caller sending both is not surprised";
    }

    // Neither key still refuses, and the refusal names both spellings — which
    // is what makes the correction cheap from the reply alone.
    {
        QJsonObject req;
        req[QStringLiteral("caller_cwd")] = dir.path();
        const QJsonObject env = rc.cmdInvariantCheck(req).object();
        EXPECT_FALSE(env.value("ok").toBool());
        EXPECT_EQ(env.value("code").toString().toStdString(),
                  std::string("bad_files"));
        EXPECT_TRUE(env.value("error").toString().contains(QStringLiteral("paths")))
            << "the refusal must name the alias it now accepts";
    }
}
