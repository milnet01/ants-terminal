// Feature-conformance test for spec.md (ANTS-5079) — terminal output cannot
// flood trigger actions or command_finished.
//
// Feeds bytes through a VtParser into grid()->processAction, which fires the
// widget's line-completion and command-finished callbacks, and counts the
// signals MainWindow turns into shells, notifications and plugin events.

#include <gtest/gtest.h>
#include "terminalwidget.h"
#include "vtparser.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QThread>

namespace {

// Generous upper bound on the decided budget (spec.md § Out of scope).
constexpr int kUpperBound = 100;
constexpr int kFlood = 400;

QJsonObject rule(const char *pattern, const char *actionType,
                 const char *actionValue) {
    QJsonObject o;
    o["pattern"]      = QString::fromUtf8(pattern);
    o["action_type"]  = QString::fromUtf8(actionType);
    o["action_value"] = QString::fromUtf8(actionValue);
    o["instant"]      = false;
    return o;
}

void feed(TerminalWidget &w, const QByteArray &bytes) {
    VtParser parser([&w](const VtAction &a) { w.grid()->processAction(a); });
    parser.feed(bytes.constData(), static_cast<int>(bytes.size()));
}

QByteArray repeated(const QByteArray &unit, int n) {
    QByteArray out;
    out.reserve(unit.size() * n);
    for (int i = 0; i < n; ++i) out += unit;
    return out;
}

}  // namespace

// INV-1
TEST(TriggerEventRateLimit, Inv1LineTriggerFloodIsCapped) {
    TerminalWidget w;
    w.setTriggerRules(QJsonArray{rule("error", "notify", "x")});
    QSignalSpy spy(&w, &TerminalWidget::triggerFired);

    feed(w, repeated("error\r\n", kFlood));

    EXPECT_GE(spy.count(), 1);
    EXPECT_LE(spy.count(), kUpperBound)
        << "every matching line fired a trigger action";
}

// INV-2
TEST(TriggerEventRateLimit, Inv2RunScriptFloodIsCapped) {
    TerminalWidget w;
    w.setTriggerRules(QJsonArray{rule("error", "run_script", "act")});
    QSignalSpy spy(&w, &TerminalWidget::triggerRunScript);

    feed(w, repeated("error\r\n", kFlood));

    EXPECT_GE(spy.count(), 1);
    EXPECT_LE(spy.count(), kUpperBound)
        << "every matching line fired a plugin event";
}

// INV-3
TEST(TriggerEventRateLimit, Inv3TriggerKindsShareOneBudget) {
    TerminalWidget w;
    w.setTriggerRules(QJsonArray{rule("alpha", "notify", "x"),
                                 rule("beta", "run_script", "act")});
    QSignalSpy fired(&w, &TerminalWidget::triggerFired);
    QSignalSpy script(&w, &TerminalWidget::triggerRunScript);

    feed(w, repeated("alpha\r\nbeta\r\n", kFlood / 2));

    EXPECT_LE(fired.count() + script.count(), kUpperBound)
        << "each trigger kind has its own budget";
}

// INV-4
TEST(TriggerEventRateLimit, Inv4CommandFinishedFloodIsCapped) {
    TerminalWidget w;
    QSignalSpy spy(&w, &TerminalWidget::commandFinished);

    feed(w, repeated("\x1b]133;D;0\x07", kFlood));

    EXPECT_GE(spy.count(), 1);
    EXPECT_LE(spy.count(), kUpperBound)
        << "every OSC 133 D marker reached command_finished";
}

// INV-5
TEST(TriggerEventRateLimit, Inv5BudgetRefills) {
    TerminalWidget w;
    w.setTriggerRules(QJsonArray{rule("error", "notify", "x")});
    QSignalSpy spy(&w, &TerminalWidget::triggerFired);

    feed(w, repeated("error\r\n", kFlood));
    const int afterFlood = spy.count();
    QThread::msleep(1100);
    feed(w, "error\r\n");

    EXPECT_EQ(spy.count(), afterFlood + 1)
        << "the budget never refilled after its window passed";
}

// INV-6
TEST(TriggerEventRateLimit, Inv6OrdinaryUseIsUntouched) {
    TerminalWidget w;
    w.setTriggerRules(QJsonArray{rule("error", "notify", "x")});
    QSignalSpy spy(&w, &TerminalWidget::triggerFired);

    feed(w, "error one\r\nerror two\r\nerror three\r\n");

    EXPECT_EQ(spy.count(), 3);
}
