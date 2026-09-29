// Feature-conformance tests for docs/specs/ANTS-5558-welcome-dialog.md —
// the welcome dialog (src/welcomedialog.{h,cpp}) and its wiring.
//
// INV-1  — opens automatically once (welcome::maybeAutoShow + MainWindow).
// INV-2  — Help carries `Show &Welcome...` after `About &Qt...`.
// INV-3  — DialogChrome::install resizable + key "WelcomeDialog", a
//          QScrollArea body, non-modal.
// INV-4  — the Claude Code section follows detection.
// INV-9  — the CLAUDE.md and shell buttons write nothing until the preview
//          is accepted.
// INV-10 — the three donation links, in the dialog and in the Donate menu.
//
// Why this exists: a first-run dialog that shows on every launch (or never),
// a modal one that drops clicks on KWin/Wayland, or a setup button that
// writes a user's dotfiles before they confirm, are each the defect this
// feature must not ship with.
//
// Assumption (INV-10): each donation QPushButton carries the URL it opens in
// a dynamic property named "url" (the implementation sets it).
// Everything that touches HOME runs under a temporary HOME (Sandbox); the
// real ~/.claude, ~/.profile and ~/.bashrc are never read or written.

#include "welcomedialog.h"

#include "claudesetup.h"
#include "config.h"

#include "../../_support/expect.h"
#include "../../_support/srcgrep.h"
#include "welcome_support.h"

#include <gtest/gtest.h>

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSizeGrip>

ANTS_TEST_SCOPE();

using namespace welcome_test;

namespace {

// Source with comments dropped (string literals kept), so a comment that
// explains "never exec()" cannot trip an absence check.
QString stripComments(const QString &s) {
    QString out;
    out.reserve(s.size());
    const int n = s.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const QChar q = c;
            out += c;
            for (++i; i < n && s.at(i) != q; ++i) {
                out += s.at(i);
                if (s.at(i) == QLatin1Char('\\') && i + 1 < n) out += s.at(++i);
            }
            if (i < n) out += s.at(i);
        } else if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('/')) {
            while (i < n && s.at(i) != QLatin1Char('\n')) ++i;
            out += QLatin1Char('\n');
        } else if (c == QLatin1Char('/') && i + 1 < n && s.at(i + 1) == QLatin1Char('*')) {
            i += 2;
            while (i + 1 < n && !(s.at(i) == QLatin1Char('*') && s.at(i + 1) == QLatin1Char('/'))) ++i;
            ++i;
        } else {
            out += c;
        }
    }
    return out;
}

QString mainWindowSource() {
    return QString::fromStdString(ants_test::slurpMainWindow());
}

QString functionBody(const QString &src, const char *signature) {
    return QString::fromStdString(
        ants_test::slurpFunctionBody(src.toStdString(), signature));
}

// The visible preview dialog a writing row opened, or nullptr.
QDialog *findVisiblePreview() {
    for (QWidget *w : QApplication::allWidgets()) {
        if (w->objectName() != QLatin1String("welcomePreview")) continue;
        if (auto *d = qobject_cast<QDialog *>(w))
            if (d->isVisible()) return d;
    }
    return nullptr;
}

void pump() { QApplication::processEvents(); }

// Config in a sandbox holds nothing until a test writes it.
QByteArray configWith(const QByteArray &json) {
    (void)writeFile(Config::configPath(), json);
    return json;
}

int autoShowCalls(Config &cfg, int times) {
    int n = 0;
    for (int i = 0; i < times; ++i)
        welcome::maybeAutoShow(cfg, [&n] { ++n; });
    return n;
}

// "ui.welcome_shown" as it sits on disk: present-and-bool first, then its value.
bool keyOnDisk(bool *present, bool *isBool) {
    const QJsonObject o = readJson(Config::configPath());
    *present = o.contains(QStringLiteral("ui.welcome_shown"));
    *isBool = o.value(QStringLiteral("ui.welcome_shown")).isBool();
    return o.value(QStringLiteral("ui.welcome_shown")).toBool(false);
}

}  // namespace

// ----------------------------------------------------------------- INV-1 ----

TEST(WelcomeFirstRun, AutoShowOnceThenLatched) {
    expect_reset();

    // --- key absent: shows once, latches.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        Config cfg;
        expect(!cfg.welcomeShown(), "INV1a/absent-key-reads-false",
               QStringLiteral("a config with no ui.welcome_shown must read "
                              "false; actual true"));
        const int n = autoShowCalls(cfg, 2);
        expect(n == 1, "INV1b/absent-shows-exactly-once",
               QStringLiteral("two maybeAutoShow calls over an absent key: "
                              "expected the callback counted 1 time, actual %1")
                   .arg(n));
        expect(cfg.welcomeShown(), "INV1c/key-true-after-show",
               QStringLiteral("after the first show the key must read true; "
                              "expected true, actual false"));
        bool present = false, isBool = false;
        const bool onDisk = keyOnDisk(&present, &isBool);
        expect(present && isBool, "INV1d/key-persisted-as-bool",
               QStringLiteral("config.json must hold ui.welcome_shown as a JSON "
                              "bool; present=%1 isBool=%2 (file: %3)")
                   .arg(present).arg(isBool)
                   .arg(clip(readFile(Config::configPath()))));
        expect(present && isBool && onDisk, "INV1e/persisted-value-true",
               QStringLiteral("expected ui.welcome_shown true on disk, actual %1")
                   .arg(onDisk));
        Config again;
        expect(again.welcomeShown(), "INV1f/latch-survives-reload",
               QStringLiteral("a fresh Config over the same file must read the "
                              "key true; actual false"));
    }

    // --- key explicitly false: same as absent.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        configWith("{\"ui.welcome_shown\": false}");
        Config cfg;
        const int n = autoShowCalls(cfg, 2);
        expect(n == 1, "INV1g/false-shows-exactly-once",
               QStringLiteral("key present and false: expected 1 callback over "
                              "two calls, actual %1").arg(n));
        bool present = false, isBool = false;
        const bool onDisk = keyOnDisk(&present, &isBool);
        expect(present && isBool && onDisk, "INV1h/false-becomes-true-on-disk",
               QStringLiteral("expected ui.welcome_shown true (bool) on disk; "
                              "present=%1 isBool=%2 value=%3")
                   .arg(present).arg(isBool).arg(onDisk));
    }

    // --- key true: never shows.
    {
        Sandbox sb;
        ASSERT_TRUE(sb.valid());
        configWith("{\"ui.welcome_shown\": true}");
        Config cfg;
        expect(cfg.welcomeShown(), "INV1i/true-key-reads-true",
               QStringLiteral("a config holding ui.welcome_shown=true must read "
                              "true; actual false"));
        const int n = autoShowCalls(cfg, 3);
        expect(n == 0, "INV1j/true-never-shows",
               QStringLiteral("key already true: expected the callback counted "
                              "0 times, actual %1").arg(n));
    }

    // --- MainWindow calls it from its first-show path (source check).
    {
        const QString src = mainWindowSource();
        expect(!src.isEmpty(), "INV1k/mainwindow-source-readable",
               QStringLiteral("ants_test::slurpMainWindow() returned nothing"));
        const QString body = functionBody(src, "void MainWindow::showEvent");
        expect(!body.isEmpty(), "INV1l/showEvent-located",
               QStringLiteral("could not locate MainWindow::showEvent's body"));
        expect(body.contains(QStringLiteral("maybeAutoShow")),
               "INV1m/first-show-calls-maybeAutoShow",
               QStringLiteral("MainWindow::showEvent (its m_firstShow path) must "
                              "call welcome::maybeAutoShow; the body does not "
                              "mention it"));
    }

    ASSERT_EQ(0, expect_finish());
}

// ----------------------------------------------------------------- INV-2 ----

TEST(WelcomeDialog, HelpMenuActionOpensIt) {
    expect_reset();
    const QString src = mainWindowSource();
    ASSERT_FALSE(src.isEmpty());

    const QString help = functionBody(src, "void MainWindow::setupHelpMenu()");
    expect(!help.isEmpty(), "INV2a/help-block-located",
           QStringLiteral("could not locate setupHelpMenu()'s body"));

    const int aboutQt = help.indexOf(QStringLiteral("About &Qt..."));
    const int welcomeIdx = help.indexOf(QStringLiteral("Show &Welcome..."));
    expect(aboutQt >= 0, "INV2b/about-qt-present-in-help-block",
           QStringLiteral("`About &Qt...` not found in the Help block"));
    expect(welcomeIdx >= 0, "INV2c/show-welcome-in-help-block",
           QStringLiteral("`Show &Welcome...` must be an action of the Help "
                          "block (setupHelpMenu); not found"));
    expect(welcomeIdx >= 0 && aboutQt >= 0 && welcomeIdx > aboutQt,
           "INV2d/show-welcome-after-about-qt",
           QStringLiteral("`Show &Welcome...` must come after `About &Qt...`; "
                          "welcomeIdx=%1 aboutQtIdx=%2").arg(welcomeIdx).arg(aboutQt));

    // Opens the dialog however the key is set: the Help block must not read it.
    expect(!help.contains(QStringLiteral("welcomeShown")),
           "INV2e/help-action-ignores-the-key",
           QStringLiteral("the Help block must open the dialog whether or not "
                          "ui.welcome_shown is set; it mentions welcomeShown"));

    // Donate stays the last menu.
    const int helpIdx = src.indexOf(QStringLiteral("m_menuBar->addMenu(\"&Help\")"));
    const int donateIdx =
        src.indexOf(QStringLiteral("m_menuBar->addMenu(tr(\"&Donate\"))"));
    expect(helpIdx > 0 && donateIdx > helpIdx, "INV2f/donate-still-after-help",
           QStringLiteral("Donate must still be added after Help; helpIdx=%1 "
                          "donateIdx=%2").arg(helpIdx).arg(donateIdx));

    ASSERT_EQ(0, expect_finish());
}

// ----------------------------------------------------------------- INV-3 ----

TEST(WelcomeDialog, ChromeAndNonModal) {
    expect_reset();
    Sandbox sb;
    ASSERT_TRUE(sb.valid());

    // --- behaviour: constructed offscreen.
    {
        WelcomeDialog::Options opts;
        opts.claudeDetected = false;
        opts.claudeMdPath = sb.home + QStringLiteral("/.claude/CLAUDE.md");
        WelcomeDialog dlg(QStringLiteral("Dark"), opts);
        expect(dlg.findChild<QScrollArea *>() != nullptr,
               "INV3a/body-in-a-scroll-area",
               QStringLiteral("the dialog's body must sit in a QScrollArea; "
                              "found none among its children"));
        expect(!dlg.isModal(), "INV3b/not-modal",
               QStringLiteral("the dialog must be non-modal; isModal()=true"));
        expect(dlg.findChild<QSizeGrip *>() != nullptr, "INV3c/resizable-grip",
               QStringLiteral("DialogChrome::install(resizable=true) adds a "
                              "QSizeGrip; found none"));
    }

    // --- source: the install call and the absence of exec()/setModal(true).
    const QString raw = QString::fromStdString(ants_test::slurpFile(
        std::string(SRC_WELCOMEDIALOG_PATH)));
    expect(!raw.isEmpty(), "INV3d/source-readable",
           QStringLiteral("could not read %1")
               .arg(QString::fromUtf8(SRC_WELCOMEDIALOG_PATH)));
    const QString code = stripComments(raw);
    static const QRegularExpression installRe(QStringLiteral(
        "DialogChrome::install\\s*\\([^;]*\\btrue\\b[^;]*\"WelcomeDialog\""));
    expect(installRe.match(code).hasMatch(), "INV3e/install-call-shape",
           QStringLiteral("welcomedialog.cpp must call DialogChrome::install(this, "
                          "themeName, true, \"WelcomeDialog\"); no call with "
                          "`true` and the key \"WelcomeDialog\" found"));
    static const QRegularExpression execRe(QStringLiteral("\\bexec\\s*\\("));
    expect(!execRe.match(code).hasMatch(), "INV3f/no-exec",
           QStringLiteral("welcomedialog.cpp must never call exec() (a nested "
                          "event loop drops clicks on KWin/Wayland)"));
    static const QRegularExpression modalRe(
        QStringLiteral("\\bsetModal\\s*\\(\\s*true"));
    expect(!modalRe.match(code).hasMatch(), "INV3g/no-setModal-true",
           QStringLiteral("welcomedialog.cpp must never call setModal(true)"));

    ASSERT_EQ(0, expect_finish());
}

// ----------------------------------------------------------------- INV-4 ----

TEST(WelcomeDialog, ClaudeSectionFollowsDetection) {
    expect_reset();
    Sandbox sb;
    ASSERT_TRUE(sb.valid());

    auto probe = [&](bool detected, QWidget **section, QWidget **hint,
                     bool *sectionVisible, bool *hintVisible,
                     WelcomeDialog *&dlgOut) {
        WelcomeDialog::Options opts;
        opts.claudeDetected = detected;
        opts.claudeMdPath = sb.home + QStringLiteral("/.claude/CLAUDE.md");
        dlgOut = new WelcomeDialog(QStringLiteral("Dark"), opts);
        *section = dlgOut->findChild<QWidget *>(QStringLiteral("welcomeClaudeSection"));
        *hint = dlgOut->findChild<QWidget *>(QStringLiteral("welcomeClaudeMissingHint"));
        *sectionVisible = *section && (*section)->isVisibleTo(dlgOut);
        *hintVisible = *hint && (*hint)->isVisibleTo(dlgOut);
    };

    // Detection injected false: the hint shows, the section does not.
    {
        QWidget *section = nullptr, *hint = nullptr;
        bool sectionVisible = false, hintVisible = false;
        WelcomeDialog *dlg = nullptr;
        probe(false, &section, &hint, &sectionVisible, &hintVisible, dlg);
        expect(hint != nullptr, "INV4a/hint-exists-when-not-detected",
               QStringLiteral("no widget named welcomeClaudeMissingHint"));
        expect(hintVisible, "INV4b/hint-visible-when-not-detected",
               QStringLiteral("detection false: expected the hint line visible, "
                              "actual %1").arg(hint ? QStringLiteral("hidden")
                                                    : QStringLiteral("missing")));
        expect(!sectionVisible, "INV4c/section-hidden-when-not-detected",
               QStringLiteral("detection false: the Claude section must not be "
                              "shown; it is visible"));
        delete dlg;
    }

    // Detection injected true: the section shows, the hint does not.
    {
        QWidget *section = nullptr, *hint = nullptr;
        bool sectionVisible = false, hintVisible = false;
        WelcomeDialog *dlg = nullptr;
        probe(true, &section, &hint, &sectionVisible, &hintVisible, dlg);
        expect(section != nullptr, "INV4d/section-exists-when-detected",
               QStringLiteral("no widget named welcomeClaudeSection"));
        expect(sectionVisible, "INV4e/section-visible-when-detected",
               QStringLiteral("detection true: expected the Claude section "
                              "visible, actual %1")
                   .arg(section ? QStringLiteral("hidden") : QStringLiteral("missing")));
        expect(!hintVisible, "INV4f/hint-hidden-when-detected",
               QStringLiteral("detection true: the hint line must not be shown; "
                              "it is visible"));
        delete dlg;
    }

    ASSERT_EQ(0, expect_finish());
}

// ----------------------------------------------------------------- INV-9 ----

TEST(WelcomeDialog, ConfirmBeforeWriting) {
    expect_reset();
    Sandbox sb;
    ASSERT_TRUE(sb.valid());

    const QString fx = sb.path(QStringLiteral("scripts"));
    const QString script = fx + QStringLiteral("/ants-osc133.bash");
    ASSERT_TRUE(writeFile(script, "# fixture bash hook\n"));

    const QString claudeMd = sb.home + QStringLiteral("/.claude/CLAUDE.md");
    const QString profile = sb.home + QStringLiteral("/.profile");
    const QString bashrc = sb.home + QStringLiteral("/.bashrc");
    const QByteArray mdSeed = "# mine\nkeep me\n";
    const QByteArray profSeed = "export FOO=1\n";
    const QByteArray rcSeed = "alias a=b\n";
    ASSERT_TRUE(writeFile(claudeMd, mdSeed));
    ASSERT_TRUE(writeFile(profile, profSeed));
    ASSERT_TRUE(writeFile(bashrc, rcSeed));

    WelcomeDialog::Options opts;
    opts.claudeDetected = true;
    opts.shellScriptDirs = {fx};
    opts.claudeMdPath = claudeMd;
    WelcomeDialog dlg(QStringLiteral("Dark"), opts);

    auto *mdButton =
        dlg.findChild<QAbstractButton *>(QStringLiteral("welcomeClaudeMdButton"));
    auto *shellButton =
        dlg.findChild<QAbstractButton *>(QStringLiteral("welcomeShellButton"));
    ASSERT_TRUE(mdButton != nullptr) << "no button named welcomeClaudeMdButton";
    ASSERT_TRUE(shellButton != nullptr) << "no button named welcomeShellButton";

    // --- CLAUDE.md note.
    mdButton->click();
    pump();
    QPointer<QDialog> preview = findVisiblePreview();
    expect(!preview.isNull(), "INV9a/md-click-opens-a-preview",
           QStringLiteral("clicking welcomeClaudeMdButton must open a visible "
                          "dialog named welcomePreview; none is open"));
    expect(readFile(claudeMd) == mdSeed, "INV9b/md-unchanged-while-preview-open",
           QStringLiteral("CLAUDE.md must not change before the user accepts; "
                          "expected %1, actual %2")
               .arg(clip(mdSeed), clip(readFile(claudeMd))));
    if (preview) preview->accept();
    pump();
    const QByteArray md = readFile(claudeMd);
    expect(md.contains("<!-- ants-terminal:begin -->") && md.contains("keep me"),
           "INV9c/md-written-after-accept",
           QStringLiteral("after accepting the preview CLAUDE.md must hold the "
                          "marked block and the user's text; actual: %1")
               .arg(clip(md)));

    // --- shell integration.
    shellButton->click();
    pump();
    preview = findVisiblePreview();
    expect(!preview.isNull(), "INV9d/shell-click-opens-a-preview",
           QStringLiteral("clicking welcomeShellButton must open a visible "
                          "dialog named welcomePreview; none is open"));
    expect(readFile(profile) == profSeed && readFile(bashrc) == rcSeed,
           "INV9e/shell-files-unchanged-while-preview-open",
           QStringLiteral("~/.profile and ~/.bashrc must not change before the "
                          "user accepts; profile expected %1 actual %2; bashrc "
                          "expected %3 actual %4")
               .arg(clip(profSeed), clip(readFile(profile)), clip(rcSeed),
                    clip(readFile(bashrc))));
    if (preview) preview->accept();
    pump();
    const QByteArray p = readFile(profile);
    const QByteArray r = readFile(bashrc);
    expect(p.contains("ANTS_OSC133_KEY") && p.startsWith(profSeed),
           "INV9f/profile-written-after-accept",
           QStringLiteral("after accepting, ~/.profile must export the key and "
                          "keep its text; actual: %1").arg(clip(p)));
    expect(r.contains("source") && r.contains(script.toUtf8()) &&
               r.startsWith(rcSeed),
           "INV9g/bashrc-written-after-accept",
           QStringLiteral("after accepting, ~/.bashrc must source %1 and keep "
                          "its text; actual: %2").arg(script, clip(r)));

    ASSERT_EQ(0, expect_finish());
}

// ---------------------------------------------------------------- INV-10 ----

TEST(WelcomeDialog, DonationLinks) {
    expect_reset();
    Sandbox sb;
    ASSERT_TRUE(sb.valid());

    struct Link { const char *object; const char *url; };
    const Link links[] = {
        {"welcomeDonateGitHub",  "https://github.com/sponsors/milnet01"},
        {"welcomeDonatePatreon", "https://www.patreon.com/c/AntsProjectsHub"},
        {"welcomeDonatePayBru",  "https://paybru.co.za/tip/ants-projects-hub"},
    };

    // --- the dialog's three buttons carry their URLs in property "url".
    {
        WelcomeDialog::Options opts;
        opts.claudeMdPath = sb.home + QStringLiteral("/.claude/CLAUDE.md");
        WelcomeDialog dlg(QStringLiteral("Dark"), opts);
        for (const Link &l : links) {
            auto *b = dlg.findChild<QAbstractButton *>(QString::fromLatin1(l.object));
            expect(b != nullptr, "INV10a/dialog-button-exists",
                   QStringLiteral("no button named %1").arg(QString::fromLatin1(l.object)));
            if (!b) continue;
            const QString got = b->property("url").toString();
            expect(got == QString::fromLatin1(l.url), "INV10b/dialog-button-url",
                   QStringLiteral("%1: expected url %2, actual %3")
                       .arg(QString::fromLatin1(l.object),
                            QString::fromLatin1(l.url), got));
        }
    }

    // --- the Donate menu: three links, GitHub first, Patreon before PayBru.
    {
        const QString src = mainWindowSource();
        ASSERT_FALSE(src.isEmpty());
        const QString donate = functionBody(src, "void MainWindow::setupDonateMenu()");
        expect(!donate.isEmpty(), "INV10c/donate-block-located",
               QStringLiteral("could not locate setupDonateMenu()'s body"));
        int urlIdx[3];
        for (int i = 0; i < 3; ++i) {
            urlIdx[i] = donate.indexOf(QString::fromLatin1(links[i].url));
            expect(urlIdx[i] >= 0, "INV10d/menu-offers-url",
                   QStringLiteral("setupDonateMenu() must offer %1; not found")
                       .arg(QString::fromLatin1(links[i].url)));
        }
        const int gh = donate.indexOf(QStringLiteral("Sponsor on &GitHub..."));
        const int pat = donate.indexOf(QStringLiteral("Support on &Patreon..."));
        const int pay = donate.indexOf(QStringLiteral("Tip via &PayBru..."));
        expect(pay >= 0, "INV10e/paybru-action-present",
               QStringLiteral("the Donate menu must gain `Tip via &PayBru...`; "
                              "not found"));
        expect(gh >= 0 && pat >= 0 && pay >= 0 && gh < pat && gh < pay,
               "INV10f/github-action-first",
               QStringLiteral("`Sponsor on &GitHub...` must stay first; "
                              "githubIdx=%1 patreonIdx=%2 paybruIdx=%3")
                   .arg(gh).arg(pat).arg(pay));
        expect(pat >= 0 && pay > pat, "INV10g/paybru-after-patreon",
               QStringLiteral("`Tip via &PayBru...` goes after the Patreon "
                              "item; patreonIdx=%1 paybruIdx=%2").arg(pat).arg(pay));
    }

    ASSERT_EQ(0, expect_finish());
}
