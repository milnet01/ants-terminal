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
    o.claudeDetected  = cs::claudeCodeDetected();
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
        "project.</p>"), body));

    // Each row reads its state now and again after its action.
    auto addRow = [this](QVBoxLayout *into, QWidget *owner, const QString &title,
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
    // Shows the exact change first; the writer runs only on accept (INV-9).
    auto runAfterPreview = [this, themeName](std::function<QString()> preview,
                                             std::function<cs::Outcome()> action) {
        return [this, themeName, preview = std::move(preview),
                action = std::move(action)](const Row &row) {
            auto *dlg = new QDialog(this);
            dlg->setObjectName(QStringLiteral("welcomePreview"));
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->setWindowTitle(tr("Before anything is written"));
            auto c = DialogChrome::install(dlg, themeName, /*resizable=*/true,
                                           QStringLiteral("WelcomePreview"));
            auto *lay = new QVBoxLayout(c.contentArea);
            lay->addWidget(wrapped(tr("This is exactly what will be written:"), dlg));
            auto *text = new QPlainTextEdit(preview(), dlg);
            text->setReadOnly(true);
            lay->addWidget(text, 1);
            auto *buttons = new QHBoxLayout;
            buttons->addStretch();
            auto *cancel = new QPushButton(tr("Cancel"), dlg);
            auto *write = new QPushButton(tr("Write it"), dlg);
            buttons->addWidget(cancel);
            buttons->addWidget(write);
            lay->addLayout(buttons);
            connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
            connect(write, &QPushButton::clicked, dlg, &QDialog::accept);
            connect(dlg, &QDialog::accepted, this, [row, action] {
                const cs::Outcome o = action();
                row.refresh();
                if (!o.message.isEmpty()) row.state->setText(o.message);
            });
            dlg->show();
            dlg->raise();
            dlg->activateWindow();
        };
    };

    // 3. Claude Code, only where it is installed (spec § 2.4).
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
    auto *hint = wrapped(tr("Using Claude Code? Install it, then reopen this "
                            "from Help → Show Welcome."), body);
    hint->setObjectName(QStringLiteral("welcomeClaudeMissingHint"));
    v->addWidget(hint);
    claude->setVisible(opts.claudeDetected);
    hint->setVisible(!opts.claudeDetected);

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

    setMinimumSize(560, 420);
    resize(680, 640);
}

namespace welcome {

void maybeAutoShow(Config &cfg, const std::function<void()> &show) {
    if (cfg.welcomeShown()) return;
    show();
    cfg.setWelcomeShown(true);
}

}  // namespace welcome
