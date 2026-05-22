// Feature-conformance test for spec.md (canonical: docs/specs/ANTS-1293.md).
// Pins the server-side response byte cap on the structured read tools
// (file_outline `symbols[]`, workspace_search `matches[]`) via the pure
// RemoteControl::capJsonArrayToBytes helper, plus source-grep on the two
// handler bodies for the wiring.
//
// BC-1..BC-5 exercise the helper directly (no Qt event loop).
// WI-1..WI-2 grep remotecontrol.cpp for the wiring.

#include "../../_support/expect.h"
#include "remotecontrol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#ifndef SRC_RC_HEADER
#error "SRC_RC_HEADER compile definition required"
#endif
#ifndef SRC_RC_CPP
#error "SRC_RC_CPP compile definition required"
#endif

ANTS_TEST_SCOPE();

namespace {

std::string slurp(const char *path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Build an envelope {ok, truncated:false, <field>:[ N items ]}. Each item
// is a small object so total size is predictable. Item 0 is oldest; the
// last item is the one a tail-trim drops first.
QJsonObject makeEnv(const QString &field, int n) {
    QJsonObject env;
    env["ok"]        = true;
    env["truncated"] = false;
    QJsonArray arr;
    for (int i = 0; i < n; ++i) {
        QJsonObject o;
        o["file"] = QStringLiteral("src/file%1.cpp").arg(i);
        o["line"] = i;
        o["text"] = QStringLiteral("some matching line of source text #%1").arg(i);
        arr.append(o);
    }
    env[field] = arr;
    return env;
}

int serializedSize(const QJsonObject &env) {
    return QJsonDocument(env).toJson(QJsonDocument::Compact).size();
}

}  // namespace

TEST(RcReadToolByteCap, HelperContract) {
    expect_reset();

    // BC-1 — under cap: no trim, no truncated flip, all items survive.
    {
        QJsonObject env = makeEnv(QStringLiteral("matches"), 5);
        const int full = serializedSize(env);
        const auto r = RemoteControl::capJsonArrayToBytes(
            env, QStringLiteral("matches"),
            QStringLiteral("results_dropped"), full + 1000);
        expect(!r.truncated && r.itemsDropped == 0, "BC-1",
               "small payload under cap should not trim");
        expect(env.value("matches").toArray().size() == 5, "BC-1b",
               "all 5 items should survive under cap");
        expect(!env.contains("results_dropped"), "BC-1c",
               "results_dropped must be absent when nothing dropped");
    }

    // BC-2 — tiny cap: trims from tail, leading items survive, truncated
    // set, dropped count correct, final serialized size within cap.
    {
        QJsonObject env = makeEnv(QStringLiteral("matches"), 100);
        const int cap = 600;
        const auto r = RemoteControl::capJsonArrayToBytes(
            env, QStringLiteral("matches"),
            QStringLiteral("results_dropped"), cap);
        const auto kept = env.value("matches").toArray();
        expect(r.truncated && r.itemsDropped > 0, "BC-2",
               "oversized payload should trim and set truncated");
        expect(kept.size() + r.itemsDropped == 100, "BC-2b",
               "kept + dropped must equal the original count");
        expect(env.value("truncated").toBool(), "BC-2c",
               "truncated flag must be set true on trim");
        expect(env.value("results_dropped").toInt() == r.itemsDropped,
               "BC-2d", "results_dropped must equal itemsDropped");
        expect(serializedSize(env) <= cap, "BC-2e",
               "final serialized envelope must be within the cap");
        // Leading item must be item 0 (tail-trim, not head-trim).
        expect(!kept.isEmpty() &&
                   kept.at(0).toObject().value("line").toInt() == 0,
               "BC-2f", "tail-trim must keep the leading item (line 0)");
    }

    // BC-3 — over-ceiling cap clamps to 4 MiB and flags capClamped; small
    // payload still fits so nothing is dropped.
    {
        QJsonObject env = makeEnv(QStringLiteral("symbols"), 3);
        const auto r = RemoteControl::capJsonArrayToBytes(
            env, QStringLiteral("symbols"),
            QStringLiteral("symbols_dropped"),
            RemoteControl::kReadToolMaxBytesCeiling + 1);
        expect(r.capClamped, "BC-3",
               "requested cap above ceiling must set capClamped");
        expect(!r.truncated && env.value("symbols").toArray().size() == 3,
               "BC-3b", "tiny payload should not trim even when clamped");
    }

    // BC-4 — maxBytes <= 0 uses the default cap (512 KiB); a modest
    // payload is well under it, so nothing trims.
    {
        QJsonObject env = makeEnv(QStringLiteral("matches"), 200);
        const auto r = RemoteControl::capJsonArrayToBytes(
            env, QStringLiteral("matches"),
            QStringLiteral("results_dropped"), 0);
        expect(!r.truncated, "BC-4",
               "maxBytes<=0 falls back to the 512 KiB default; "
               "200 small items fit");
    }

    // BC-5 — pathologically small cap keeps zero items but does not crash;
    // truncated set, all dropped.
    {
        QJsonObject env = makeEnv(QStringLiteral("matches"), 10);
        const auto r = RemoteControl::capJsonArrayToBytes(
            env, QStringLiteral("matches"),
            QStringLiteral("results_dropped"), 1);
        expect(r.truncated && r.itemsDropped == 10, "BC-5",
               "cap below base size drops everything, sets truncated");
        expect(env.value("matches").toArray().isEmpty(), "BC-5b",
               "matches array empty when cap below base size");
    }

    // WI-1 — file_outline handler caps symbols[] with symbols_dropped.
    {
        const std::string rc = slurp(SRC_RC_CPP);
        const auto p = rc.find("RemoteControl::cmdFileOutline");
        expect(p != std::string::npos, "WI-1", "cmdFileOutline not found");
        const std::string body = rc.substr(p, 6000);
        expect(body.find("capJsonArrayToBytes") != std::string::npos &&
                   body.find("symbols_dropped") != std::string::npos,
               "WI-1b",
               "cmdFileOutline does not byte-cap symbols[] (ANTS-1293)");
    }

    // WI-2 — workspace_search handler caps matches[] with results_dropped.
    {
        const std::string rc = slurp(SRC_RC_CPP);
        const auto p = rc.find("RemoteControl::cmdWorkspaceSearch");
        expect(p != std::string::npos, "WI-2",
               "cmdWorkspaceSearch not found");
        // Search the whole body (handler is long); just require both tokens
        // appear after the handler start.
        const std::string body = rc.substr(p);
        expect(body.find("capJsonArrayToBytes") != std::string::npos &&
                   body.find("results_dropped") != std::string::npos,
               "WI-2b",
               "cmdWorkspaceSearch does not byte-cap matches[] (ANTS-1293)");
    }

    EXPECT_EQ(0, expect_failures())
        << expect_failures() << " ANTS-1293 invariant(s) failed";
}
