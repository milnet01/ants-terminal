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

#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const std::string &detail = {}) {
    std::fprintf(stderr, "[%-72s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 detail.empty() ? "" : " — ",
                 detail.c_str());
    if (!ok) ++g_failures;
}

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

// Brace-balanced extraction. Returns the slice from the signature
// line through the matching closing brace. Naive (string literals
// containing braces would confuse it) but our targets are real C++
// member-function bodies so it's fine.
std::string extractBody(const std::string &source, const std::string &sig) {
    auto sigPos = source.find(sig);
    if (sigPos == std::string::npos) return {};
    auto open = source.find('{', sigPos);
    if (open == std::string::npos) return {};
    int depth = 1;
    auto pos = open + 1;
    while (pos < source.size() && depth > 0) {
        if (source[pos] == '{') ++depth;
        else if (source[pos] == '}') --depth;
        ++pos;
    }
    return source.substr(sigPos, pos - sigPos);
}

// Sandboxed Config — sets XDG_CONFIG_HOME, mkpath's the ants-terminal
// dir, returns the temp path. Caller's Config instances see only this
// dir for the lifetime of the QTemporaryDir.
struct Sandbox {
    QTemporaryDir tmp;
    QString configDir;
    bool valid() const { return tmp.isValid(); }
    Sandbox() {
        if (!tmp.isValid()) return;
        qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
        configDir = tmp.path() + "/ants-terminal";
        QDir().mkpath(configDir);
    }
};

// ----- Round-trip lane -----

void testInv1_settingsDialogLastTab() {
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
}

void testInv2_roadmapActivePreset() {
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
    // Unknown-string fallback: persist a bogus value; reader returns
    // the documented "full" default.
    {
        Config c;
        c.setRoadmapActivePreset(QStringLiteral("garbage-not-a-preset"));
    }
    {
        Config d;
        expect(d.roadmapActivePreset() == QStringLiteral("full"),
               "ANTS-1150-INV-2: unknown preset string falls back to \"full\"");
    }
}

void testInv3_roadmapKindFilters() {
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
}

void testInv4_roadmapStatusFilters() {
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
}

void testInv5_auditSeverityFilters() {
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
}

void testInv6_auditShowNewOnly() {
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
}

// INV-7 — short-circuit on second same-value setter call. Asserts
// at the rawData() in-memory layer (not filesystem mtime — see
// cold-eyes HIGH #5). Sample one of each setter shape.
void testInv7_storeIfChangedShortCircuit() {
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
}

// INV-8 — first-launch defaults. Fresh Config, no config.json,
// every getter returns the documented default without throwing.
void testInv8_firstLaunchDefaults() {
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
}

// ----- Wiring lane (source-grep) -----

void testInv9_settingsDialogWiring() {
    const std::string sd = readFile("src/settingsdialog.cpp");
    if (sd.empty()) { expect(false, "ANTS-1150-INV-9: read settingsdialog.cpp"); return; }

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
}

void testInv10_roadmapDialogPresetWriteSites() {
    const std::string rd = readFile("src/roadmapdialog.cpp");
    if (rd.empty()) { expect(false, "ANTS-1150-INV-10: read roadmapdialog.cpp"); return; }

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
}

void testInv11_roadmapDialogKindStatusWiring() {
    const std::string rd = readFile("src/roadmapdialog.cpp");
    if (rd.empty()) { expect(false, "ANTS-1150-INV-11: read roadmapdialog.cpp"); return; }

    expect(contains(rd, "setRoadmapKindFilters("),
           "ANTS-1150-INV-11: roadmapdialog.cpp calls setRoadmapKindFilters(");
    expect(contains(rd, "setRoadmapStatusFilters("),
           "ANTS-1150-INV-11: roadmapdialog.cpp calls setRoadmapStatusFilters(");
}

void testInv12_roadmapDialogCtorRestore() {
    const std::string rd = readFile("src/roadmapdialog.cpp");
    if (rd.empty()) { expect(false, "ANTS-1150-INV-12: read roadmapdialog.cpp"); return; }

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
}

void testInv13_auditDialogCtorSignature() {
    const std::string ah = readFile("src/auditdialog.h");
    if (ah.empty()) { expect(false, "ANTS-1150-INV-13: read auditdialog.h"); return; }

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
}

void testInv14_auditDialogWiring() {
    const std::string ad = readFile("src/auditdialog.cpp");
    if (ad.empty()) { expect(false, "ANTS-1150-INV-14: read auditdialog.cpp"); return; }

    expect(contains(ad, "setAuditSeverityFilters("),
           "ANTS-1150-INV-14: auditdialog.cpp calls setAuditSeverityFilters(");
    expect(contains(ad, "setAuditShowNewOnly("),
           "ANTS-1150-INV-14: auditdialog.cpp calls setAuditShowNewOnly(");
    expect(contains(ad, "auditSeverityFilters()"),
           "ANTS-1150-INV-14: auditdialog.cpp ctor reads auditSeverityFilters()");
    expect(contains(ad, "auditShowNewOnly()"),
           "ANTS-1150-INV-14: auditdialog.cpp ctor reads auditShowNewOnly()");
}

void testInv15_mainWindowAuditDialogCallSite() {
    const std::string mw = readFile("src/mainwindow.cpp");
    if (mw.empty()) { expect(false, "ANTS-1150-INV-15: read mainwindow.cpp"); return; }

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
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    testInv1_settingsDialogLastTab();
    testInv2_roadmapActivePreset();
    testInv3_roadmapKindFilters();
    testInv4_roadmapStatusFilters();
    testInv5_auditSeverityFilters();
    testInv6_auditShowNewOnly();
    testInv7_storeIfChangedShortCircuit();
    testInv8_firstLaunchDefaults();

    testInv9_settingsDialogWiring();
    testInv10_roadmapDialogPresetWriteSites();
    testInv11_roadmapDialogKindStatusWiring();
    testInv12_roadmapDialogCtorRestore();
    testInv13_auditDialogCtorSignature();
    testInv14_auditDialogWiring();
    testInv15_mainWindowAuditDialogCallSite();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nAll INVs PASS.\n");
    return 0;
}
