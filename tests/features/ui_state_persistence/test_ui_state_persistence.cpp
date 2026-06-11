// Feature-conformance test for ANTS-1150 (Phase 1) — UI / chrome
// state persistence. Hybrid: round-trip Config setters (link
// src/config.cpp) + source-grep on the four dialog files.
//
// INV labels are qualified ANTS-1150-INV-N to avoid cross-spec
// collision (per cold-eyes MEDIUM #13).
//
// INV map (full text in spec.md):
//   1   setSettingsDialogLastTab round-trip (5 + 0)
//   2   setRoadmapActivePreset round-trip + unknown-string fallback
//   3   setRoadmapKindFilters round-trip (sorted on disk)
//   4   setRoadmapStatusFilters round-trip (5-key object)
//   5   setAuditSeverityFilters round-trip (5-key object, mixed)
//   6   setAuditShowNewOnly round-trip
//   7   storeIfChanged short-circuit (rawData byte-equal)
//   8   First-launch defaults (no config.json)
//   9   settingsdialog.cpp wiring (setter + currentChanged + QSignalBlocker)
//   10  roadmapdialog.cpp wiring (setRoadmapActivePreset in BOTH applyPreset
//       AND onCheckboxToggled)
//   11  roadmapdialog.cpp wiring (Kind + Status setter calls)
//   12  roadmapdialog.cpp ctor restore reads three getters; status read
//       gated on Custom preset
//   13  auditdialog.h ctor takes Config* (no nullptr default)
//   14  auditdialog.cpp wiring (severity + show-new-only setter/getter)
//   15  mainwindow.cpp passes &m_config to new AuditDialog (multi-line grep)

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "config.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <fstream>
#include <sstream>
#include <string>
#include <gtest/gtest.h>

// ANTS-1150 source-grep INVs anchor to bundle-supplied absolute paths so
// the test fails loudly if a future build wires the bundle without them,
// rather than silently passing because readFile() returned empty
// (gtest_discover_tests sets CWD to the build dir, where bare relative
// paths like "src/settingsdialog.cpp" don't resolve).
#ifndef SRC_SETTINGSDIALOG_CPP_PATH
#  error "SRC_SETTINGSDIALOG_CPP_PATH must be defined by the bundle's compile defs"
#endif
#ifndef SRC_ROADMAPDIALOG_CPP_PATH
#  error "SRC_ROADMAPDIALOG_CPP_PATH must be defined by the bundle's compile defs"
#endif
#ifndef SRC_AUDITDIALOG_H_PATH
#  error "SRC_AUDITDIALOG_H_PATH must be defined by the bundle's compile defs"
#endif
#ifndef SRC_AUDITDIALOG_CPP_PATH
#  error "SRC_AUDITDIALOG_CPP_PATH must be defined by the bundle's compile defs"
#endif
#ifndef SRC_MAINWINDOW_CPP_PATH
#  error "SRC_MAINWINDOW_CPP_PATH must be defined by the bundle's compile defs"
#endif

ANTS_TEST_SCOPE();

namespace {

std::string readFile(const char *path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

// ANTS-1468 — delegate to the shared string/comment-aware extractor;
// prepend the signature so the returned span matches the original
// (signature line through the matching closing brace).
std::string extractBody(const std::string &source, const std::string &sig) {
    const std::string body = ants_test::slurpFunctionBody(source, sig);
    if (body.empty()) return {};
    const auto sigPos = source.find(sig);
    return source.substr(sigPos, source.find('{', sigPos) - sigPos) + body;
}

// Sandboxed Config — sets XDG_CONFIG_HOME, mkpath's the ants-terminal
// dir, returns the temp path. Caller's Config instances see only this
// dir for the lifetime of the QTemporaryDir.
struct Sandbox {
    QTemporaryDir tmp;
    QString configDir;
    QByteArray priorXdg;
    bool hadPriorXdg = false;
    bool valid() const { return tmp.isValid(); }
    Sandbox() {
        if (!tmp.isValid()) return;
        // Capture so dtor can restore; otherwise XDG_CONFIG_HOME leaks
        // past the QTemporaryDir's lifetime and the next Sandbox or
        // sibling test in the bundle sees a deleted directory.
        hadPriorXdg = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        if (hadPriorXdg) priorXdg = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
        configDir = tmp.path() + "/ants-terminal";
        QDir().mkpath(configDir);
    }
    ~Sandbox() {
        if (hadPriorXdg) qputenv("XDG_CONFIG_HOME", priorXdg);
        else if (qEnvironmentVariableIsSet("XDG_CONFIG_HOME"))
            qunsetenv("XDG_CONFIG_HOME");
    }
};

// ----- Round-trip lane -----

TEST(UiStatePersistence, Inv1_settingsDialogLastTab) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-1 setup"); return; }

    {
        Config a;
        a.setSettingsDialogLastTab(5);
    }  // a destroyed — config.json on disk
    {
        Config b;
        expect(b.settingsDialogLastTab() == 5,
               "ANTS-1150-INV-1: setSettingsDialogLastTab(5) round-trip");
    }
    // Default-equal (0) round-trip — first call may no-op (default match)
    // but the read-back must still return 0.
    {
        Config c;
        c.setSettingsDialogLastTab(0);
    }
    {
        Config d;
        expect(d.settingsDialogLastTab() == 0,
               "ANTS-1150-INV-1: setSettingsDialogLastTab(0) round-trip");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv1_settingsDialogLastTab failed";
}

TEST(UiStatePersistence, Inv2_roadmapActivePreset) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-2 setup"); return; }

    {
        Config a;
        a.setRoadmapActivePreset(QStringLiteral("current"));
    }
    {
        Config b;
        expect(b.roadmapActivePreset() == QStringLiteral("current"),
               "ANTS-1150-INV-2: setRoadmapActivePreset(\"current\") round-trip");
    }
    // Unknown-string fallback: simulate an on-disk corruption (e.g.
    // a future preset enum being downgraded to). The setter has been
    // hardened in ANTS-1179 to reject unknown strings, so the only
    // realistic path to a bogus value on disk is a direct file edit
    // or a forward-version write being read by an older binary.
    // Whatever the source, the getter must still fall back to "full"
    // so the dialog opens in a known-good preset rather than empty.
    {
        // Write a garbage preset directly into config.json, bypassing
        // the setter (mimics a forward-version write or a hand edit).
        const QString cfgPath = QStandardPaths::writableLocation(
                                    QStandardPaths::ConfigLocation)
                                + "/ants-terminal/config.json";
        QFile f(cfgPath);
        QJsonObject obj;
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            obj = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
        }
        obj["roadmap_active_preset"] = QStringLiteral("garbage-not-a-preset");
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QJsonDocument(obj).toJson());
            f.close();
        }
    }
    {
        Config d;
        expect(d.roadmapActivePreset() == QStringLiteral("full"),
               "ANTS-1150-INV-2: unknown preset string falls back to \"full\"");
    }
    // ANTS-1179: setter now rejects unknown strings outright (defense
    // at write). Verify the rejection by setting a known value, then
    // attempting to overwrite with garbage, then reading back — the
    // known value should survive.
    {
        Config e;
        e.setRoadmapActivePreset(QStringLiteral("history"));
        e.setRoadmapActivePreset(QStringLiteral("garbage-not-a-preset"));
    }
    {
        Config f;
        expect(f.roadmapActivePreset() == QStringLiteral("history"),
               "ANTS-1179: setRoadmapActivePreset(unknown) is a no-op; "
               "prior known value preserved");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv2_roadmapActivePreset failed";
}

TEST(UiStatePersistence, Inv3_roadmapKindFilters) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-3 setup"); return; }

    {
        Config a;
        a.setRoadmapKindFilters({QStringLiteral("fix"),
                                 QStringLiteral("audit-fix")});
    }
    {
        Config b;
        const QStringList got = b.roadmapKindFilters();
        QSet<QString> want{QStringLiteral("fix"), QStringLiteral("audit-fix")};
        QSet<QString> have(got.begin(), got.end());
        expect(have == want,
               "ANTS-1150-INV-3: roadmapKindFilters round-trip preserves "
               "set membership");
    }
    // Bonus: stable on-disk ordering — read raw config.json, check
    // the array is sorted ASCII-wise. Only meaningful if the spec's
    // write-side `.sort()` actually fires.
    {
        const QString cfgPath = sb.configDir + "/config.json";
        const std::string raw = readFile(cfgPath.toLocal8Bit().constData());
        // Sorted ASCII: "audit-fix" < "fix" (lex), so the array on
        // disk must show "audit-fix" first.
        const auto a = raw.find("\"audit-fix\"");
        const auto f = raw.find("\"fix\"");
        expect(a != std::string::npos && f != std::string::npos && a < f,
               "ANTS-1150-INV-3: kinds sorted ASCII on disk "
               "(audit-fix before fix)");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv3_roadmapKindFilters failed";
}

TEST(UiStatePersistence, Inv4_roadmapStatusFilters) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-4 setup"); return; }

    QJsonObject sf;
    sf["done"]        = true;
    sf["planned"]     = false;
    sf["in_progress"] = true;
    sf["considered"]  = false;
    sf["current"]     = true;

    {
        Config a;
        a.setRoadmapStatusFilters(sf);
    }
    {
        Config b;
        const QJsonObject got = b.roadmapStatusFilters();
        const bool ok =
            got.value("done").toBool(false) == true   &&
            got.value("planned").toBool(true) == false &&
            got.value("in_progress").toBool(false) == true &&
            got.value("considered").toBool(true) == false &&
            got.value("current").toBool(false) == true;
        expect(ok, "ANTS-1150-INV-4: roadmapStatusFilters round-trip "
                   "preserves five-key boolean object");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv4_roadmapStatusFilters failed";
}

TEST(UiStatePersistence, Inv5_auditSeverityFilters) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-5 setup"); return; }

    QJsonObject sev;
    sev["blocker"]  = true;
    sev["critical"] = true;
    sev["major"]    = true;
    sev["minor"]    = false;
    sev["info"]     = false;

    {
        Config a;
        a.setAuditSeverityFilters(sev);
    }
    {
        Config b;
        const QJsonObject got = b.auditSeverityFilters();
        const bool ok =
            got.value("blocker").toBool(false)  == true &&
            got.value("critical").toBool(false) == true &&
            got.value("major").toBool(false)    == true &&
            got.value("minor").toBool(true)     == false &&
            got.value("info").toBool(true)      == false;
        expect(ok, "ANTS-1150-INV-5: auditSeverityFilters round-trip "
                   "preserves five-key boolean object");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv5_auditSeverityFilters failed";
}

TEST(UiStatePersistence, Inv6_auditShowNewOnly) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-6 setup"); return; }

    {
        Config a;
        a.setAuditShowNewOnly(true);
    }
    {
        Config b;
        expect(b.auditShowNewOnly() == true,
               "ANTS-1150-INV-6: setAuditShowNewOnly(true) round-trip");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv6_auditShowNewOnly failed";
}

// INV-7 — short-circuit on second same-value setter call. Asserts
// at the rawData() in-memory layer (not filesystem mtime — see
// cold-eyes HIGH #5). Sample one of each setter shape.
TEST(UiStatePersistence, Inv7_storeIfChangedShortCircuit) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-7 setup"); return; }

    Config c;

    // Prime each key with a non-default value.
    c.setSettingsDialogLastTab(3);
    c.setRoadmapActivePreset(QStringLiteral("history"));
    c.setRoadmapKindFilters({QStringLiteral("fix")});
    QJsonObject sf;
    sf["done"] = false;
    c.setRoadmapStatusFilters(sf);
    QJsonObject sev;
    sev["blocker"] = false;
    c.setAuditSeverityFilters(sev);
    c.setAuditShowNewOnly(true);

    // Snapshot, repeat each setter with the SAME value, assert
    // rawData() byte-identical.
    const QJsonObject snap = c.rawData();

    c.setSettingsDialogLastTab(3);
    c.setRoadmapActivePreset(QStringLiteral("history"));
    c.setRoadmapKindFilters({QStringLiteral("fix")});
    c.setRoadmapStatusFilters(sf);
    c.setAuditSeverityFilters(sev);
    c.setAuditShowNewOnly(true);

    expect(c.rawData() == snap,
           "ANTS-1150-INV-7: same-value re-set leaves rawData() "
           "byte-identical (storeIfChanged short-circuit)");
    EXPECT_EQ(0, expect_failures()) << "Inv7_storeIfChangedShortCircuit failed";
}

// INV-8 — first-launch defaults. Fresh Config, no config.json,
// every getter returns the documented default without throwing.
TEST(UiStatePersistence, Inv8_firstLaunchDefaults) {
    expect_reset();
    Sandbox sb;
    if (!sb.valid()) { expect(false, "ANTS-1150-INV-8 setup"); return; }

    Config c;
    expect(c.settingsDialogLastTab() == 0,
           "ANTS-1150-INV-8: settingsDialogLastTab default = 0");
    expect(c.roadmapActivePreset() == QStringLiteral("full"),
           "ANTS-1150-INV-8: roadmapActivePreset default = \"full\"");
    expect(c.roadmapKindFilters().isEmpty(),
           "ANTS-1150-INV-8: roadmapKindFilters default = empty list");
    expect(c.roadmapStatusFilters().isEmpty(),
           "ANTS-1150-INV-8: roadmapStatusFilters default = empty object");
    expect(c.auditSeverityFilters().isEmpty(),
           "ANTS-1150-INV-8: auditSeverityFilters default = empty object");
    expect(c.auditShowNewOnly() == false,
           "ANTS-1150-INV-8: auditShowNewOnly default = false");
    EXPECT_EQ(0, expect_failures()) << "Inv8_firstLaunchDefaults failed";
}

// ----- Wiring lane (source-grep) -----

TEST(UiStatePersistence, Inv9_settingsDialogWiring) {
    expect_reset();
    const std::string sd = readFile(SRC_SETTINGSDIALOG_CPP_PATH);
    ASSERT_FALSE(sd.empty()) << "ANTS-1150-INV-9: read settingsdialog.cpp (" << SRC_SETTINGSDIALOG_CPP_PATH << ")";

    expect(contains(sd, "setSettingsDialogLastTab("),
           "ANTS-1150-INV-9: settingsdialog.cpp calls setSettingsDialogLastTab(");
    // currentChanged connect on m_tabs (any of the modern syntaxes).
    expect(contains(sd, "&QTabWidget::currentChanged") ||
           contains(sd, "QTabWidget::currentChanged") ||
           contains(sd, "currentChanged(int)"),
           "ANTS-1150-INV-9: settingsdialog.cpp connects QTabWidget::currentChanged");
    // QSignalBlocker on restore — guards against the restore writing
    // back through the just-installed handler (cold-eyes MEDIUM #9).
    expect(contains(sd, "QSignalBlocker"),
           "ANTS-1150-INV-9: settingsdialog.cpp uses QSignalBlocker on restore");
    EXPECT_EQ(0, expect_failures()) << "Inv9_settingsDialogWiring failed";
}

TEST(UiStatePersistence, Inv10_roadmapDialogPresetWriteSites) {
    expect_reset();
    const std::string rd = readFile(SRC_ROADMAPDIALOG_CPP_PATH);
    ASSERT_FALSE(rd.empty()) << "ANTS-1150-INV-10: read roadmapdialog.cpp";

    // Both call sites for setRoadmapActivePreset (via persistActivePreset
    // helper or direct). Either spelling counts — the test is "the
    // setter or its wrapper appears inside both function bodies."
    const std::string applyBody = extractBody(rd, "void RoadmapDialog::applyPreset");
    expect(!applyBody.empty(), "ANTS-1150-INV-10: applyPreset body extracted");

    const bool applyPersists =
        contains(applyBody, "setRoadmapActivePreset(") ||
        contains(applyBody, "persistActivePreset(");
    expect(applyPersists,
           "ANTS-1150-INV-10: applyPreset persists active preset "
           "(setRoadmapActivePreset or persistActivePreset call)");

    const std::string toggleBody = extractBody(rd, "void RoadmapDialog::onCheckboxToggled");
    expect(!toggleBody.empty(), "ANTS-1150-INV-10: onCheckboxToggled body extracted");

    const bool togglePersists =
        contains(toggleBody, "setRoadmapActivePreset(") ||
        contains(toggleBody, "persistActivePreset(");
    expect(togglePersists,
           "ANTS-1150-INV-10: onCheckboxToggled persists active preset "
           "on Custom-divergence (setRoadmapActivePreset or "
           "persistActivePreset call)");
    EXPECT_EQ(0, expect_failures()) << "Inv10_roadmapDialogPresetWriteSites failed";
}

TEST(UiStatePersistence, Inv11_roadmapDialogKindStatusWiring) {
    expect_reset();
    const std::string rd = readFile(SRC_ROADMAPDIALOG_CPP_PATH);
    ASSERT_FALSE(rd.empty()) << "ANTS-1150-INV-11: read roadmapdialog.cpp";

    expect(contains(rd, "setRoadmapKindFilters("),
           "ANTS-1150-INV-11: roadmapdialog.cpp calls setRoadmapKindFilters(");
    expect(contains(rd, "setRoadmapStatusFilters("),
           "ANTS-1150-INV-11: roadmapdialog.cpp calls setRoadmapStatusFilters(");
    EXPECT_EQ(0, expect_failures()) << "Inv11_roadmapDialogKindStatusWiring failed";
}

TEST(UiStatePersistence, Inv12_roadmapDialogCtorRestore) {
    expect_reset();
    const std::string rd = readFile(SRC_ROADMAPDIALOG_CPP_PATH);
    ASSERT_FALSE(rd.empty()) << "ANTS-1150-INV-12: read roadmapdialog.cpp";

    expect(contains(rd, "roadmapKindFilters()"),
           "ANTS-1150-INV-12: ctor reads roadmapKindFilters()");
    expect(contains(rd, "roadmapStatusFilters()"),
           "ANTS-1150-INV-12: ctor reads roadmapStatusFilters()");
    expect(contains(rd, "roadmapActivePreset()"),
           "ANTS-1150-INV-12: ctor reads roadmapActivePreset()");

    // Status-filter read MUST be inside an `if (... Preset::Custom ...)`
    // guard — named-preset path skips it (cold-eyes CRITICAL #1).
    // Heuristic: find roadmapStatusFilters( and walk back ~400 chars
    // for "Preset::Custom".
    const auto sfPos = rd.find("roadmapStatusFilters()");
    if (sfPos == std::string::npos) {
        expect(false, "ANTS-1150-INV-12: roadmapStatusFilters() call site not found");
    } else {
        const std::size_t lookbackStart = sfPos > 600 ? sfPos - 600 : 0;
        const std::string preceding = rd.substr(lookbackStart, sfPos - lookbackStart);
        expect(contains(preceding, "Preset::Custom"),
               "ANTS-1150-INV-12: roadmapStatusFilters() read is gated "
               "by Preset::Custom check (cold-eyes CRITICAL #1 "
               "named-preset-skip path)");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv12_roadmapDialogCtorRestore failed";
}

TEST(UiStatePersistence, Inv13_auditDialogCtorSignature) {
    expect_reset();
    const std::string ah = readFile(SRC_AUDITDIALOG_H_PATH);
    ASSERT_FALSE(ah.empty()) << "ANTS-1150-INV-13: read auditdialog.h";

    // Ctor must take Config* with no nullptr default (cold-eyes HIGH #3).
    // Accept either `Config *config` or `Config* config` etc.
    const bool hasConfigParam =
        contains(ah, "Config *config") ||
        contains(ah, "Config* config") ||
        contains(ah, "Config *cfg")     ||
        contains(ah, "Config* cfg");
    expect(hasConfigParam,
           "ANTS-1150-INV-13: auditdialog.h ctor takes Config* third arg");

    // No `= nullptr` default for the Config arg. Heuristic: find the
    // ctor signature line and verify it doesn't end with `= nullptr)`
    // after the Config* token.
    const auto cfgPos = ah.find("Config *config");
    const auto altPos = ah.find("Config* config");
    const auto pos = (cfgPos != std::string::npos) ? cfgPos
                   : (altPos != std::string::npos) ? altPos
                   : std::string::npos;
    if (pos != std::string::npos) {
        // Look forward up to 80 chars for `= nullptr` before the next semicolon.
        const std::string tail = ah.substr(pos, 80);
        const auto eq = tail.find("= nullptr");
        const auto semi = tail.find(';');
        const bool defaulted = (eq != std::string::npos)
                            && (semi == std::string::npos || eq < semi);
        expect(!defaulted,
               "ANTS-1150-INV-13: Config* third arg has no `= nullptr` "
               "default (cold-eyes HIGH #3 — single call site, no need)");
    }
    EXPECT_EQ(0, expect_failures()) << "Inv13_auditDialogCtorSignature failed";
}

TEST(UiStatePersistence, Inv14_auditDialogWiring) {
    expect_reset();
    const std::string ad = readFile(SRC_AUDITDIALOG_CPP_PATH);
    ASSERT_FALSE(ad.empty()) << "ANTS-1150-INV-14: read auditdialog.cpp";

    expect(contains(ad, "setAuditSeverityFilters("),
           "ANTS-1150-INV-14: auditdialog.cpp calls setAuditSeverityFilters(");
    expect(contains(ad, "setAuditShowNewOnly("),
           "ANTS-1150-INV-14: auditdialog.cpp calls setAuditShowNewOnly(");
    expect(contains(ad, "auditSeverityFilters()"),
           "ANTS-1150-INV-14: auditdialog.cpp ctor reads auditSeverityFilters()");
    expect(contains(ad, "auditShowNewOnly()"),
           "ANTS-1150-INV-14: auditdialog.cpp ctor reads auditShowNewOnly()");
    EXPECT_EQ(0, expect_failures()) << "Inv14_auditDialogWiring failed";
}

TEST(UiStatePersistence, Inv15_mainWindowAuditDialogCallSite) {
    expect_reset();
    const std::string mw = readFile(SRC_MAINWINDOW_CPP_PATH);
    ASSERT_FALSE(mw.empty()) << "ANTS-1150-INV-15: read mainwindow.cpp";

    // Multi-line aware: find first `new AuditDialog(` and look up to
    // the matching `)` for `&m_config`. Cold-eyes MEDIUM #12: don't
    // rely on a single-line regex — future ctor line-wraps would
    // silently break the test.
    const auto pos = mw.find("new AuditDialog(");
    if (pos == std::string::npos) {
        expect(false, "ANTS-1150-INV-15: `new AuditDialog(` call site not found");
        return;
    }

    // Walk forward, paren-counted, until depth returns to 0.
    int depth = 0;
    std::size_t i = pos + std::string("new AuditDialog").size();
    std::size_t end = i;
    while (i < mw.size()) {
        if (mw[i] == '(') {
            ++depth;
        } else if (mw[i] == ')') {
            --depth;
            if (depth == 0) { end = i; break; }
        }
        ++i;
    }
    if (depth != 0 || end == pos) {
        expect(false, "ANTS-1150-INV-15: could not find matching `)` for "
                      "`new AuditDialog(`");
        return;
    }

    const std::string ctorArgs = mw.substr(pos, end - pos + 1);
    expect(contains(ctorArgs, "&m_config"),
           "ANTS-1150-INV-15: `new AuditDialog(...)` includes `&m_config` "
           "in its argument list");
    EXPECT_EQ(0, expect_failures()) << "Inv15_mainWindowAuditDialogCallSite failed";
}

}  // namespace

