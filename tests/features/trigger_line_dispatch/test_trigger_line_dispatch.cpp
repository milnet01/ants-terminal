// Feature-conformance test for spec.md (ANTS-2119 terminalwidget M1).
//
// A non-instant dispatch trigger must fire once per completed line, not once
// per raw batch. Feeding lines through grid()->processAction fires the widget's
// line-completion callback (onGridLineCompleted) — the path the fix routes
// non-instant dispatch triggers through.

#include <gtest/gtest.h>
#include "terminalwidget.h"
#include "vtparser.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <cstring>

namespace {

QJsonArray oneRule(const char *pattern, const char *actionType,
                   const char *actionValue, bool instant) {
    QJsonObject o;
    o["pattern"]      = QString::fromUtf8(pattern);
    o["action_type"]  = QString::fromUtf8(actionType);
    o["action_value"] = QString::fromUtf8(actionValue);
    o["instant"]      = instant;
    QJsonArray a;
    a.append(o);
    return a;
}

void feed(TerminalWidget &w, const char *s) {
    VtParser parser([&w](const VtAction &a) { w.grid()->processAction(a); });
    parser.feed(s, static_cast<int>(std::strlen(s)));
}

}  // namespace

// INV-1 — a non-instant dispatch rule fires once per completed line.
TEST(TriggerLineDispatch, Inv1FiresPerCompletedLine) {
    TerminalWidget w;
    w.setTriggerRules(oneRule("error", "notify", "beep", /*instant=*/false));

    QSignalSpy spy(&w, &TerminalWidget::triggerFired);
    // Two matching lines + a non-matching one, each newline-terminated so the
    // grid completes them. The newlines land wherever they fall — the per-line
    // callback doesn't depend on batch boundaries.
    feed(w, "error one\nok two\nerror three\n");

    EXPECT_EQ(spy.count(), 2)
        << "a non-instant dispatch trigger must fire once per matching "
           "completed line (two matches → two fires), not once per batch";
}

// INV-2 — the emitted signal carries the rule's pattern/type/value.
TEST(TriggerLineDispatch, Inv2SignalPayload) {
    TerminalWidget w;
    w.setTriggerRules(oneRule("build (failed)", "command", "rebuild.sh",
                              /*instant=*/false));

    QSignalSpy spy(&w, &TerminalWidget::triggerFired);
    feed(w, "build failed\n");

    ASSERT_EQ(spy.count(), 1);
    const QList<QVariant> args = spy.at(0);
    EXPECT_EQ(args.at(1).toString(), QStringLiteral("command"));
    EXPECT_EQ(args.at(2).toString(), QStringLiteral("rebuild.sh"));
}

// INV-3 — a non-matching line emits nothing.
TEST(TriggerLineDispatch, Inv3NoMatchNoFire) {
    TerminalWidget w;
    w.setTriggerRules(oneRule("PANIC", "notify", "x", /*instant=*/false));

    QSignalSpy spy(&w, &TerminalWidget::triggerFired);
    feed(w, "all good here\nstill fine\n");

    EXPECT_EQ(spy.count(), 0);
}
