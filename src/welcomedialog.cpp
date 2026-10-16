// ANTS-5558 — see welcomedialog.h. Built fresh on every open, so each row's
// state is read when the dialog appears (spec § 2.5). Non-modal throughout:
// the dialog and its previews use show(), never exec() or setModal(true) —
// help_about_menu Invariant 7's shape, for its Wayland reason (clicks lost
// on a modal dialog). Results land in the row's own status label for the
// same reason, never in a message box.

#include "welcomedialog.h"

#include "claudesetup.h"
#include "config.h"
#include "dialogchrome.h"

#include <QDesktopServices>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace cs = ants::claude_setup;

namespace {

QLabel *wrapped(const QString &text, QWidget *parent) {
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    return l;
}

// One setup row: a title, the current state, and an Install/Reinstall
// button. `status` reads the state; `onClick` runs the action.
struct Row {
    QPushButton *button = nullptr;
    QLabel      *state  = nullptr;
    std::function<cs::Status()> status;

    void refresh() const {
        const cs::Status s = status();
        state->setText(s.detail);
        button->setEnabled(s.state != cs::State::Unavailable);
        button->setText(s.state == cs::State::Installed
                            ? QObject::tr("Reinstall") : QObject::tr("Install"));
    }
};

}  // namespace

WelcomeDialog::Options WelcomeDialog::detect() {
    Options o;
    o.detectClaude    = &cs::claudeCodeDetected;
    o.shellScriptDirs = cs::defaultShellScriptDirs();
    o.claudeMdPath    = cs::defaultClaudeMdPath();
    return o;
}

WelcomeDialog::WelcomeDialog(const QString &themeName, const Options &opts,
                             QWidget *parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("WelcomeDialog"));
    setWindowTitle(tr("Welcome to Ants Terminal"));
    auto chrome = DialogChrome::install(this, themeName, /*resizable=*/true,
                                        QStringLiteral("WelcomeDialog"));
    auto *outer = new QVBoxLayout(chrome.contentArea);

    // D5 — the body scrolls rather than compressing.
    auto *scroll = new QScrollArea(chrome.contentArea);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    auto *v = new QVBoxLayout(body);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    // 1. Thanks.
    v->addWidget(wrapped(tr(
        "<h2>Welcome to Ants Terminal</h2>"
        "<p>Thank you for using Ants Terminal. Here is what it can do, and a "
        "few one-click steps to get the most from it.</p>"), body));

    // 2. What it does.
    v->addWidget(wrapped(tr(
        "<h3>What it does</h3>"
        "<p><b>A capable terminal:</b> tabs and split panes, font ligatures and "
        "inline images, search, a command palette, themes, session restore, "
        "Lua plugins, and a built-in project audit.</p>"
        "<p><b>Built for Claude Code:</b> a live status bar showing what Claude "
        "is doing, session and permission tools, Review Changes, and the Ants "
        "MCP toolkit, which gives Claude cheaper ways to read and search your "
        "project.</p>"
        "<p>Ants Terminal is an independent project. It is not made by, "
        "affiliated with or endorsed by Anthropic, the maker of Claude "
        "Code.</p>"), body));

    // Each row reads its state now and again after its action. `rows` keeps
    // a re-read of each, for when detection turns true (INV-13).
    std::vector<std::function<void()>> rows;
    auto addRow = [this, &rows](QVBoxLayout *into, QWidget *owner, const QString &title,
                         const QString &buttonName,
                         std::function<cs::Status()> status,
                         std::function<void(const Row &)> onClick) {
        auto *line = new QHBoxLayout;
        auto *text = new QVBoxLayout;
        text->addWidget(wrapped(QStringLiteral("<b>%1</b>").arg(title), owner));
        auto *state = wrapped(QString(), owner);
        state->setTextFormat(Qt::PlainText);
        text->addWidget(state);
        line->addLayout(text, 1);
        auto *button = new QPushButton(owner);
        button->setObjectName(buttonName);
        line->addWidget(button, 0, Qt::AlignTop);
        into->addLayout(line);
        Row row{button, state, std::move(status)};
        row.refresh();
        rows.emplace_back([row] { row.refresh(); });
        connect(button, &QPushButton::clicked, this,
                [row, onClick = std::move(onClick)] { onClick(row); });
    };
    // Runs an action and shows its result in the row, then its fresh state.
    auto runNow = [](std::function<cs::Outcome()> action) {
        return [action = std::move(action)](const Row &row) {
            const cs::Outcome o = action();
            row.refresh();
            if (!o.ok || !o.message.isEmpty())
                row.state->setText(o.message.isEmpty() ? row.state->text() : o.message);
        };
    };
    // A non-modal preview owned by this dialog; `onAccept` runs only from
    // its accept handler (INV-9, INV-12).
    auto openPreview = [this, themeName](const QString &title, const QString &intro,
                                         const QString &text,
                                         const QString &acceptLabel,
                                         std::function<void()> onAccept) {
        auto *dlg = new QDialog(this);
        dlg->setObjectName(QStringLiteral("welcomePreview"));
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(title);
        auto c = DialogChrome::install(dlg, themeName, /*resizable=*/true,
                                       QStringLiteral("WelcomePreview"));
        auto *lay = new QVBoxLayout(c.contentArea);
        lay->addWidget(wrapped(intro, dlg));
        auto *view = new QPlainTextEdit(text, dlg);
        view->setReadOnly(true);
        lay->addWidget(view, 1);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = new QPushButton(tr("Cancel"), dlg);
        auto *accept = new QPushButton(acceptLabel, dlg);
        buttons->addWidget(cancel);
        buttons->addWidget(accept);
        lay->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
        connect(accept, &QPushButton::clicked, dlg, &QDialog::accept);
        connect(dlg, &QDialog::accepted, this, std::move(onAccept));
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    };
    // Shows the exact change first; the writer runs only on accept (INV-9).
    auto runAfterPreview = [openPreview](std::function<QString()> preview,
                                         std::function<cs::Outcome()> action) {
        return [openPreview, preview = std::move(preview),
                action = std::move(action)](const Row &row) {
            openPreview(tr("Before anything is written"),
                        tr("This is exactly what will be written:"), preview(),
                        tr("Write it"), [row, action] {
                            const cs::Outcome o = action();
                            row.refresh();
                            if (!o.message.isEmpty()) row.state->setText(o.message);
                        });
        };
    };

    // 3. Claude Code, only where it is installed (spec § 2.4); otherwise the
    // install offer (§ 2.8) stands in its place.
    auto *claude = new QGroupBox(tr("Set up with Claude Code"), body);
    claude->setObjectName(QStringLiteral("welcomeClaudeSection"));
    auto *cv = new QVBoxLayout(claude);
    addRow(cv, claude, tr("Live status bar hooks"),
           QStringLiteral("welcomeStatusHooksButton"), &cs::statusHooksStatus,
           runNow(&cs::installStatusHooks));
    addRow(cv, claude, tr("Connect the Ants MCP toolkit to Claude Code"),
           QStringLiteral("welcomeMcpButton"), &cs::mcpStatus,
           runNow(&cs::registerMcp));
    addRow(cv, claude, tr("Git context on every prompt"),
           QStringLiteral("welcomeGitContextButton"), &cs::gitContextStatus,
           runNow(&cs::installGitContextHook));
    const QString mdPath = opts.claudeMdPath;
    addRow(cv, claude, tr("Ants notes in your CLAUDE.md (optional)"),
           QStringLiteral("welcomeClaudeMdButton"),
           [mdPath] { return cs::claudeMdNoteStatus(mdPath); },
           runAfterPreview(
               [mdPath] {
                   return tr("File: %1\n\n<!-- ants-terminal:begin -->\n%2\n"
                             "<!-- ants-terminal:end -->")
                       .arg(mdPath, cs::claudeMdNoteText());
               },
               [mdPath] { return cs::installClaudeMdNote(mdPath); }));
    v->addWidget(claude);

    // The installer runs in a new tab the user watches, and only after they
    // accept a preview naming the URL and the whole command.
    auto *offer = new QGroupBox(tr("Claude Code"), body);
    auto *ov = new QVBoxLayout(offer);
    auto *note = wrapped(tr("Claude Code needs a paid Claude plan, such as Pro, "
                            "Max or Enterprise, or an Anthropic API account. It "
                            "does not work with the free plan."), offer);
    note->setObjectName(QStringLiteral("welcomeClaudePlanNote"));
    note->setTextFormat(Qt::PlainText);
    ov->addWidget(note);
    auto *installLine = new QHBoxLayout;
    auto *install = new QPushButton(tr("Install Claude Code"), offer);
    install->setObjectName(QStringLiteral("welcomeInstallClaudeButton"));
    install->setEnabled(static_cast<bool>(opts.openInstallTab));
    installLine->addWidget(install);
    installLine->addStretch();
    ov->addLayout(installLine);
    v->addWidget(offer);
    connect(install, &QPushButton::clicked, this,
            [openPreview, openTab = opts.openInstallTab] {
                const QString command = cs::claudeInstallCommand();
                openPreview(tr("Before anything runs"),
                            tr("This is what will run in a new tab:"),
                            tr("Anthropic's installer, from "
                               "https://claude.ai/install.sh, then Claude Code "
                               "for its one-time sign-in:\n\n%1").arg(command),
                            tr("Install"), [openTab, command] { openTab(command); });
            });
    m_detectClaude = opts.detectClaude;
    m_claudeSection = claude;
    m_installOffer = offer;

    // 4. Shell integration (spec § 2.6).
    auto *shell = new QGroupBox(tr("Shell integration"), body);
    auto *sv = new QVBoxLayout(shell);
    const QStringList dirs = opts.shellScriptDirs;
    addRow(sv, shell, tr("Mark where each command starts and ends"),
           QStringLiteral("welcomeShellButton"),
           [dirs] { return cs::shellIntegrationStatus(dirs); },
           runAfterPreview([dirs] { return cs::shellIntegrationPreview(dirs); },
                           [dirs] { return cs::installShellIntegration(dirs); }));
    v->addWidget(shell);

    // 5. Support.
    auto *support = new QGroupBox(tr("Support Ants Terminal"), body);
    auto *pv = new QVBoxLayout(support);
    pv->addWidget(wrapped(tr("Ants Terminal is made by one developer. Donations "
                             "keep it improving — thank you."), support));
    auto *links = new QHBoxLayout;
    const struct { const char *name; const char *label; const char *url; } donate[] = {
        {"welcomeDonateGitHub",  "Sponsor on GitHub",  "https://github.com/sponsors/milnet01"},
        {"welcomeDonatePatreon", "Support on Patreon", "https://www.patreon.com/c/AntsProjectsHub"},
        {"welcomeDonatePayBru",  "Tip via PayBru",     "https://paybru.co.za/tip/ants-projects-hub"},
    };
    for (const auto &d : donate) {
        auto *b = new QPushButton(tr(d.label), support);
        b->setObjectName(QLatin1String(d.name));
        b->setProperty("url", QString::fromLatin1(d.url));
        connect(b, &QPushButton::clicked, this,
                [url = QString::fromLatin1(d.url)] {
                    QDesktopServices::openUrl(QUrl(url));
                });
        links->addWidget(b);
    }
    links->addStretch();
    pv->addLayout(links);
    v->addWidget(support);
    v->addStretch();

    // 6. Close.
    auto *bottom = new QHBoxLayout;
    bottom->addStretch();
    auto *close = new QPushButton(tr("Close"), chrome.contentArea);
    close->setDefault(true);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    bottom->addWidget(close);
    outer->addLayout(bottom);

    // The rows were read as they were built, so this first call re-reads
    // none; later calls re-read them all when detection turns true.
    applyDetection();
    m_refreshRows = std::move(rows);

    setMinimumSize(560, 420);
    resize(680, 640);
}

bool WelcomeDialog::event(QEvent *e) {
    if (e->type() == QEvent::WindowActivate) applyDetection();
    return QDialog::event(e);
}

void WelcomeDialog::applyDetection() {
    const bool detected = m_detectClaude && m_detectClaude();
    if (detected && !m_detected)
        for (const auto &refresh : m_refreshRows) refresh();
    m_detected = detected;
    m_claudeSection->setVisible(detected);
    m_installOffer->setVisible(!detected);
}

namespace welcome {

void maybeAutoShow(Config &cfg, const std::function<void()> &show) {
    if (cfg.welcomeShown()) return;
    show();
    cfg.setWelcomeShown(true);
}

}  // namespace welcome
