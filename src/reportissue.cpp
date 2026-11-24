// ANTS-6911 — see reportissue.h.

#include "reportissue.h"
#include "antsversion.h"
// cppcheck-suppress missingInclude  // ANTS-1682: generated at build time
#include "build_info.h"
#include "dialogchrome.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStringList>
#include <QSysInfo>
#include <QVBoxLayout>

namespace ReportIssue {

namespace {

const QString kRepoUrl = QStringLiteral("https://github.com/milnet01/ants-terminal");

}  // namespace

QString Environment::toText() const {
    QStringList lines{
        QStringLiteral("Ants Terminal: ") + appVersion,
        QStringLiteral("Qt: ") + qtVersion,
        QStringLiteral("System: ") + system,
        QStringLiteral("Desktop: ") + desktop,
    };
    if (!package.isEmpty()) lines << QStringLiteral("Package: ") + package;
    return lines.join(QLatin1Char('\n'));
}

Environment currentEnvironment() {
    Environment env;
    env.appVersion = QStringLiteral("%1 (commit %2)")
                         .arg(QString::fromLatin1(ANTS_VERSION),
                              QString::fromLatin1(ANTS_BUILD_COMMIT));
    env.qtVersion = QString::fromLatin1(qVersion());
    env.system = QStringLiteral("%1, kernel %2")
                     .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    const QString session = qEnvironmentVariable("XDG_SESSION_TYPE");
    env.desktop = desktop.isEmpty() ? QStringLiteral("unknown") : desktop;
    if (!session.isEmpty()) env.desktop += QStringLiteral(" (%1)").arg(session);
    // AppImage's runtime sets APPIMAGE to the image's own path; only the
    // fact is reported, never the path, which names the person's folders.
    if (qEnvironmentVariableIsSet("APPIMAGE")) env.package = QStringLiteral("AppImage");
    return env;
}

QUrl issueUrl(Kind kind, const QString &title, const Environment &env) {
    // Each value is percent-encoded by hand: QUrlQuery leaves '+' bare, and
    // GitHub reads a bare '+' as a space.
    QStringList items;
    const auto add = [&items](const char *key, const QString &value) {
        items << QLatin1String(key) + QLatin1Char('=')
                     + QString::fromLatin1(QUrl::toPercentEncoding(value));
    };
    add("template", kind == Kind::Bug ? QStringLiteral("bug_report.yml")
                                      : QStringLiteral("feature_request.yml"));
    if (!title.trimmed().isEmpty()) add("title", title.trimmed());
    add("environment", env.toText());
    QUrl url(kRepoUrl + QStringLiteral("/issues/new"));
    url.setQuery(items.join(QLatin1Char('&')), QUrl::StrictMode);
    return url;
}

QDialog *createDialog(QWidget *parent, const QString &logPath) {
    auto *dlg = new QDialog(parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QObject::tr("Report an Issue"));
    dlg->setObjectName(QStringLiteral("reportIssueDialog"));
    auto chrome = DialogChrome::install(dlg, QString(), /*resizable=*/true,
                                        QStringLiteral("ReportIssueDialog"));
    auto *layout = new QVBoxLayout(chrome.contentArea);

    auto *intro = new QLabel(QObject::tr(
        "This opens a new issue on GitHub in your browser, with your system "
        "details filled in. You send it from your own GitHub account; "
        "nothing is sent until you press Create there."), dlg);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *bug = new QRadioButton(QObject::tr("Something is &broken"), dlg);
    bug->setObjectName(QStringLiteral("reportIssueBug"));
    auto *idea = new QRadioButton(QObject::tr("An &idea or request"), dlg);
    idea->setObjectName(QStringLiteral("reportIssueIdea"));
    bug->setChecked(true);
    auto *kinds = new QButtonGroup(dlg);
    kinds->addButton(bug);
    kinds->addButton(idea);
    layout->addWidget(bug);
    layout->addWidget(idea);

    auto *title = new QLineEdit(dlg);
    title->setObjectName(QStringLiteral("reportIssueTitle"));
    title->setPlaceholderText(QObject::tr("In one line: what happened, or what you would like"));
    layout->addWidget(title);

    const Environment env = currentEnvironment();
    auto *details = new QLabel(
        QObject::tr("These details are filled in for you:") + QStringLiteral("\n")
            + env.toText(), dlg);
    details->setObjectName(QStringLiteral("reportIssueDetails"));
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setWordWrap(true);
    layout->addWidget(details);

    const bool hasLog = QFileInfo::exists(logPath)
                        || QFileInfo::exists(logPath + QStringLiteral(".1"));
    if (hasLog) {
        auto *warning = new QLabel(QObject::tr(
            "<b>⚠ About the debug log.</b> Ants Terminal has a debug log that "
            "may help. It can hold folder names, commands you ran and other "
            "private text, so it is never attached for you. If you want to "
            "share it, open its folder, read it, remove anything private, "
            "and attach it to the issue yourself."), dlg);
        warning->setObjectName(QStringLiteral("reportIssueLogWarning"));
        warning->setTextFormat(Qt::RichText);
        warning->setWordWrap(true);
        layout->addWidget(warning);
        auto *logRow = new QHBoxLayout;
        auto *openLogs = new QPushButton(QObject::tr("Open &Log Folder"), dlg);
        openLogs->setObjectName(QStringLiteral("reportIssueOpenLogFolder"));
        const QString folder = QFileInfo(logPath).absolutePath();
        QObject::connect(openLogs, &QPushButton::clicked, dlg, [folder]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        });
        logRow->addWidget(openLogs);
        logRow->addStretch();
        layout->addLayout(logRow);
    }

    auto *security = new QLabel(QObject::tr(
        "Found a security problem? Please do not post it publicly. "
        "<a href=\"%1/security/advisories/new\">Report it privately</a>.")
        .arg(kRepoUrl), dlg);
    security->setObjectName(QStringLiteral("reportIssueSecurity"));
    security->setTextFormat(Qt::RichText);
    security->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                      | Qt::LinksAccessibleByKeyboard);
    security->setOpenExternalLinks(true);
    security->setWordWrap(true);
    layout->addWidget(security);

    // Plain QPushButtons, no QDialogButtonBox: the Wayland click-delivery
    // pattern aboutdialogs.cpp documents.
    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    auto *cancel = new QPushButton(QObject::tr("Cancel"), dlg);
    QObject::connect(cancel, &QPushButton::clicked, dlg, &QDialog::close);
    auto *open = new QPushButton(QObject::tr("&Open in Browser"), dlg);
    open->setObjectName(QStringLiteral("reportIssueOpen"));
    open->setDefault(true);
    QObject::connect(open, &QPushButton::clicked, dlg, [dlg, bug, title, env]() {
        QDesktopServices::openUrl(
            issueUrl(bug->isChecked() ? Kind::Bug : Kind::Idea, title->text(), env));
        dlg->close();
    });
    btnRow->addWidget(cancel);
    btnRow->addWidget(open);
    layout->addLayout(btnRow);

    constexpr int kMinWidth = 520;
    dlg->setMinimumWidth(kMinWidth);
    if (QLayout *top = dlg->layout(); top && top->hasHeightForWidth())
        dlg->setMinimumHeight(top->totalHeightForWidth(kMinWidth));
    dlg->resize(dlg->minimumSize());
    title->setFocus();
    return dlg;
}

void show(QWidget *parent, const QString &logPath) {
    QDialog *dlg = createDialog(parent, logPath);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

}  // namespace ReportIssue
