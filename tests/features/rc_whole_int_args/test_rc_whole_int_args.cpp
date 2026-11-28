// Terminal verbs refuse a number that is not a whole int — see spec.md.
// ANTS-6989.

#include "remotecontrol.h"
#include "../../_support/srcgrep.h"

#include <gtest/gtest.h>

#include <QJsonDocument>
#include <QJsonObject>

#include <string>

#ifndef ANTS_RC_SOURCES
#  error "ANTS_RC_SOURCES compile definition required"
#endif

namespace {

std::string verbBody(const std::string &verb) {
    return ants_test::slurpFunctionBody(
        ants_test::stripComments(ants_test::slurpRemoteControl()),
        "RemoteControl::" + verb);
}

bool readsWholeInts(const std::string &body) {
    return body.find("anyNotWholeInt(") != std::string::npos ||
           body.find("wholeIntArg(") != std::string::npos;
}

}  // namespace

// INV-1 — the three public verbs, driven on a bare RemoteControl. The check
// runs before any tab is looked up, so no MainWindow is needed. The scrape
// comes first: before the fix each verb calls MainWindow with a null pointer,
// and the ASSERT stops the test before that call can crash the bundle.
TEST(RcWholeIntArgs, Inv1FractionOrHugeNumberRefusesBadArgs) {
    using Handler = QJsonDocument (RemoteControl::*)(const QJsonObject &);
    struct Case { const char *verb; Handler fn; const char *arg; };
    const Case cases[] = {
        {"cmdGetText",       &RemoteControl::cmdGetText,       "tab"},
        {"cmdGetText",       &RemoteControl::cmdGetText,       "lines"},
        {"cmdGetText",       &RemoteControl::cmdGetText,       "max_bytes"},
        {"cmdRecentErrors",  &RemoteControl::cmdRecentErrors,  "tab"},
        {"cmdRecentErrors",  &RemoteControl::cmdRecentErrors,  "lines"},
        {"cmdRecentErrors",  &RemoteControl::cmdRecentErrors,  "max_results"},
        {"cmdLastSelection", &RemoteControl::cmdLastSelection, "tab"},
        {"cmdLastSelection", &RemoteControl::cmdLastSelection, "max_bytes"},
    };
    RemoteControl rc(nullptr);
    for (const Case &c : cases) {
        const std::string body = verbBody(c.verb);
        ASSERT_FALSE(body.empty()) << c.verb << " body not found";
        ASSERT_TRUE(readsWholeInts(body))
            << c.verb << " must check its numbers with wholeIntArg";
        for (const double bad : {1.5, 1e10}) {
            QJsonObject req;
            req[QString::fromLatin1(c.arg)] = bad;
            const QJsonObject r = (rc.*c.fn)(req).object();
            EXPECT_FALSE(r.value(QStringLiteral("ok")).toBool(true))
                << c.verb << " " << c.arg << "=" << bad;
            EXPECT_EQ(r.value(QStringLiteral("code")).toString(),
                      QStringLiteral("bad_args"))
                << c.verb << " " << c.arg << "=" << bad << ": "
                << QJsonDocument(r).toJson().toStdString();
        }
    }
}

// INV-2 — select-window and set-title are private, so their bodies are
// checked, as send-text's is (remote_control_opt_in, ANTS-6637).
TEST(RcWholeIntArgs, Inv2SelectWindowAndSetTitleCheckTheTab) {
    for (const char *verb : {"cmdSelectWindow", "cmdSetTitle"}) {
        const std::string body = verbBody(verb);
        ASSERT_FALSE(body.empty()) << verb << " body not found";
        EXPECT_TRUE(readsWholeInts(body))
            << verb << " must read `tab` through wholeIntArg";
    }
}
