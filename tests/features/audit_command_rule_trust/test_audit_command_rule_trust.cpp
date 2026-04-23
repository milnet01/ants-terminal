// Feature-conformance test for spec.md — exercises the Config-layer
// trust API for <project>/audit_rules.json rule packs carrying a
// `command` field. Each invariant is a separate named assertion so a
// regression fingerprints to the exact rule that loosened.
//
// Exit 0 = all invariants hold. Non-zero = regression.

#include "config.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int g_failures = 0;

void expect(bool ok, const char *label, const char *detail = "") {
    std::fprintf(stderr, "[%-62s] %s%s%s\n",
                 label, ok ? "PASS" : "FAIL",
                 (detail && *detail) ? " — " : "",
                 detail ? detail : "");
    if (!ok) ++g_failures;
}

// Sandbox XDG_CONFIG_HOME inside tmp so Config writes never touch the
// real user config. Matches the pattern used by config_parse_failure_guard.
void setConfigSandbox(const QTemporaryDir &tmp) {
    qputenv("XDG_CONFIG_HOME", tmp.path().toLocal8Bit());
}

QString makeProject(const QTemporaryDir &tmp, const char *slug,
                    const QByteArray &rulesBytes) {
    const QString p = tmp.path() + "/" + slug;
    QDir().mkpath(p);
    QFile f(p + "/audit_rules.json");
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(rulesBytes);
    f.close();
    return p;
}

// Invariant 1, 2, 3, 6, 7
void testRoundTripAndHashInvalidation() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "roundTrip: tmp"); return; }
    setConfigSandbox(tmp);

    const QByteArray v1 = R"({"rules":[{"id":"r1","command":"echo a"}]})";
    const QByteArray v2 = R"({"rules":[{"id":"r1","command":"echo b"}]})";
    const QString proj = makeProject(tmp, "proj", v1);

    {
        Config cfg;
        expect(!cfg.isAuditRulePackTrusted(proj, v1),
               "inv1: default-untrusted");

        cfg.trustAuditRulePack(proj, v1);
        expect(cfg.isAuditRulePackTrusted(proj, v1),
               "inv2: trust record round-trips in-process");

        expect(!cfg.isAuditRulePackTrusted(proj, v2),
               "inv3: hash-bound — edited rule pack is no longer trusted");

        cfg.untrustAuditRulePack(proj);
        expect(!cfg.isAuditRulePackTrusted(proj, v1),
               "inv6: untrust drops the record");

        cfg.untrustAuditRulePack(proj);  // idempotent
        expect(!cfg.isAuditRulePackTrusted(proj, v1),
               "inv6: untrust is idempotent");

        cfg.trustAuditRulePack(proj, v1);
    }
    // New Config instance — round-trip through config.json.
    {
        Config cfg2;
        expect(cfg2.isAuditRulePackTrusted(proj, v1),
               "inv7: trust persists across Config instances");
    }
}

// Invariant 4
void testCrossProjectIsolation() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "crossProject: tmp"); return; }
    setConfigSandbox(tmp);

    // Byte-identical rule packs so the test can't pass by hash coincidence.
    const QByteArray bytes = R"({"rules":[{"id":"x","command":"whoami"}]})";
    const QString p1 = makeProject(tmp, "projA", bytes);
    const QString p2 = makeProject(tmp, "projB", bytes);

    Config cfg;
    cfg.trustAuditRulePack(p1, bytes);
    expect(cfg.isAuditRulePackTrusted(p1, bytes),
           "inv4: trusted project A reads back as trusted");
    expect(!cfg.isAuditRulePackTrusted(p2, bytes),
           "inv4: sibling project B does NOT inherit A's trust "
           "(even with byte-identical rule pack)");
}

// Invariant 5
void testPathCanonicalization() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) { expect(false, "canonical: tmp"); return; }
    setConfigSandbox(tmp);

    const QByteArray bytes = R"({"rules":[{"id":"q","command":"id"}]})";
    const QString proj = makeProject(tmp, "realproj", bytes);

    // Create a symlink pointing at the real project.
    const QString link = tmp.path() + "/linkproj";
    if (!QFile::link(proj, link)) {
        expect(false, "canonical: symlink creation failed "
                      "(test skipped on FS that can't symlink)");
        return;
    }

    Config cfg;
    cfg.trustAuditRulePack(proj, bytes);
    expect(cfg.isAuditRulePackTrusted(link, bytes),
           "inv5: trust lookup through symlink canonicalizes to the target");

    // Trailing slash on the same canonical path should also resolve.
    expect(cfg.isAuditRulePackTrusted(proj + "/", bytes),
           "inv5: trailing-slash path resolves to same trust record");
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    testRoundTripAndHashInvalidation();
    testCrossProjectIsolation();
    testPathCanonicalization();

    if (g_failures == 0) {
        std::fprintf(stderr, "\nAll invariants hold. Trust gate behavior "
                             "matches spec.md.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d invariant(s) failed — see above.\n",
                 g_failures);
    return 1;
}
