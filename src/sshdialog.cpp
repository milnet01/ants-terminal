#include "sshdialog.h"

#include "shellutils.h"

#include <QDir>
#include <QFormLayout>
#include <QProcess>
#include <QRegularExpression>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>

// SshBookmark implementation
QJsonObject SshBookmark::toJson() const {
    QJsonObject obj;
    obj["name"] = name;
    obj["host"] = host;
    obj["user"] = user;
    obj["port"] = port;
    obj["identity_file"] = identityFile;
    obj["extra_args"] = extraArgs;
    return obj;
}

SshBookmark SshBookmark::fromJson(const QJsonObject &obj) {
    SshBookmark bm;
    bm.name = obj["name"].toString();
    bm.host = obj["host"].toString();
    bm.user = obj["user"].toString();
    bm.port = obj["port"].toInt(22);
    bm.identityFile = obj["identity_file"].toString();
    bm.extraArgs = obj["extra_args"].toString();
    return bm;
}

QString SshBookmark::toSshCommand(bool controlMaster) const {
    QStringList args;
    args << "ssh";
    if (port != 22)
        args << "-p" << QString::number(port);
    if (!identityFile.isEmpty())
        args << "-i" << shellQuote(identityFile);
    if (controlMaster) {
        // Resolve $HOME in C++ rather than relying on shell tilde
        // expansion — bash expands `foo=~/…` in command args, dash
        // doesn't, and OpenSSH never does its own tilde expansion.
        // %r/%h/%p are SSH ControlPath tokens, NOT shell specials.
        const QString ctlPath =
            QDir::homePath() + QStringLiteral("/.ssh/cm-%r@%h:%p");
        args << "-o" << "ControlMaster=auto"
             << "-o" << shellQuote("ControlPath=" + ctlPath)
             << "-o" << "ControlPersist=10m";
    }
    if (!extraArgs.isEmpty()) {
        // Reject -o ProxyCommand / -o LocalCommand etc. (arbitrary
        // shell execution via ssh option surface) before shell-
        // quoting. See sanitizeExtraArgs. 0.7.12 /indie-review fix.
        for (const QString &arg : sanitizeExtraArgs(extraArgs))
            args << shellQuote(arg);
    }
    // `--` before the host stops OpenSSH from parsing a bookmark whose host
    // begins with `-` as an option (CVE-2017-1000117 class — `-oProxyCommand=`
    // injects arbitrary command execution). Shell-quoting alone does NOT
    // defend against this: the shell passes the value correctly, but ssh
    // itself then interprets the leading dash. `--` is the portable POSIX
    // argv-terminator and has been supported by OpenSSH since forever.
    args << "--";
    if (!user.isEmpty())
        args << shellQuote(user + "@" + host);
    else
        args << shellQuote(host);
    return args.join(' ');
}

QStringList SshBookmark::sanitizeExtraArgs(const QString &extraArgs,
                                           QStringList *out_rejected) {
    // Options whose value is a shell command string — any of these
    // effectively hands the caller arbitrary RCE on the local machine
    // via ssh's invocation of /bin/sh -c. The attack surface is
    // bookmark content (which may arrive via contributed plugins,
    // synced dotfile repos, or an attacker with transient config
    // write access) rather than user-typed commands.
    //
    // Accepts both forms: `-oProxyCommand=...` (single token) and
    // `-o ProxyCommand=...` (two tokens after whitespace split).
    //
    // Case-insensitive comparison because ssh's option parser itself
    // is case-insensitive on key names (upstream ssh accepts
    // ProxyCommand, proxycommand, PROXYCOMMAND identically).
    static const QStringList kDangerous = {
        QStringLiteral("ProxyCommand"),
        QStringLiteral("LocalCommand"),
        QStringLiteral("PermitLocalCommand"),  // value=yes enables LocalCommand
        QStringLiteral("KnownHostsCommand"),   // OpenSSH 8.5+, runs via /bin/sh -c
        QStringLiteral("Match"),               // `Match exec "<cmd>"` shells out
    };

    auto optionKey = [](const QString &s) -> QString {
        // Extract "X" from "X=value"
        int eq = s.indexOf('=');
        return (eq < 0) ? s : s.left(eq);
    };

    auto isDangerousKey = [&](const QString &key) {
        for (const QString &d : kDangerous)
            if (QString::compare(key, d, Qt::CaseInsensitive) == 0) return true;
        return false;
    };

    // 0.7.52 (2026-04-27 indie-review HIGH) — quote-aware tokenisation.
    // The previous regex split on whitespace silently fractured quoted
    // arguments, which let `-o "ProxyCommand=…"` slip past the
    // dangerous-key check (the leading quote attaches to "ProxyCommand,
    // which doesn't match the case-insensitive keyword). QProcess::
    // splitCommand mirrors POSIX shell quoting: handles single + double
    // quotes, backslash escapes, and rejects unterminated quotes (returns
    // empty list — caller treats as nothing-to-add, safe-fail).
    const QStringList tokens = QProcess::splitCommand(extraArgs);

    QStringList safe;
    int i = 0;
    while (i < tokens.size()) {
        const QString &t = tokens[i];

        // Single-token form: -oKEY=VAL
        if (t.startsWith(QStringLiteral("-o")) && t.size() > 2) {
            const QString payload = t.mid(2);  // "KEY=VAL"
            if (isDangerousKey(optionKey(payload))) {
                if (out_rejected) out_rejected->append(t);
                ++i;
                continue;
            }
        }
        // Two-token form: -o KEY=VAL
        else if (t == QStringLiteral("-o") && i + 1 < tokens.size()) {
            if (isDangerousKey(optionKey(tokens[i + 1]))) {
                if (out_rejected) {
                    out_rejected->append(t);
                    out_rejected->append(tokens[i + 1]);
                }
                i += 2;
                continue;
            }
        }

        safe << t;
        ++i;
    }
    return safe;
}

// SshDialog implementation
SshDialog::SshDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("SSH Manager");
    setMinimumSize(600, 400);
    resize(700, 450);

    auto *mainLayout = new QVBoxLayout(this);

    // Quick connect bar
    auto *quickLayout = new QHBoxLayout();
    auto *quickLabel = new QLabel("Quick Connect:", this);
    m_quickConnect = new QLineEdit(this);
    m_quickConnect->setPlaceholderText("user@host:port (e.g., root@192.168.1.1:2222)");
    connect(m_quickConnect, &QLineEdit::returnPressed, this, &SshDialog::onQuickConnect);
    auto *quickBtn = new QPushButton("Connect", this);
    connect(quickBtn, &QPushButton::clicked, this, &SshDialog::onQuickConnect);
    quickLayout->addWidget(quickLabel);
    quickLayout->addWidget(m_quickConnect, 1);
    quickLayout->addWidget(quickBtn);
    mainLayout->addLayout(quickLayout);

    // Splitter: bookmarks list | edit form
    auto *splitter = new QSplitter(Qt::Horizontal, this);

    // Left: bookmark list
    auto *listGroup = new QGroupBox("Bookmarks", this);
    auto *listLayout = new QVBoxLayout(listGroup);
    m_bookmarkList = new QListWidget(listGroup);
    connect(m_bookmarkList, &QListWidget::currentRowChanged, this, &SshDialog::onBookmarkSelected);
    connect(m_bookmarkList, &QListWidget::doubleClicked, this, &SshDialog::onConnectNewTab);
    listLayout->addWidget(m_bookmarkList);
    splitter->addWidget(listGroup);

    // Right: edit form
    auto *editGroup = new QGroupBox("Connection Details", this);
    auto *formLayout = new QFormLayout(editGroup);

    m_nameEdit = new QLineEdit(editGroup);
    m_nameEdit->setPlaceholderText("My Server");
    formLayout->addRow("Name:", m_nameEdit);

    m_hostEdit = new QLineEdit(editGroup);
    m_hostEdit->setPlaceholderText("hostname or IP");
    formLayout->addRow("Host:", m_hostEdit);

    m_userEdit = new QLineEdit(editGroup);
    m_userEdit->setPlaceholderText("username");
    formLayout->addRow("User:", m_userEdit);

    m_portSpin = new QSpinBox(editGroup);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(22);
    formLayout->addRow("Port:", m_portSpin);

    m_identityEdit = new QLineEdit(editGroup);
    m_identityEdit->setPlaceholderText("~/.ssh/id_rsa (optional)");
    formLayout->addRow("Identity File:", m_identityEdit);

    m_extraArgsEdit = new QLineEdit(editGroup);
    m_extraArgsEdit->setPlaceholderText("-o StrictHostKeyChecking=no (optional)");
    formLayout->addRow("Extra Args:", m_extraArgsEdit);

    // Buttons
    auto *btnLayout = new QHBoxLayout();
    auto *saveBtn = new QPushButton("Save", editGroup);
    connect(saveBtn, &QPushButton::clicked, this, &SshDialog::onSave);
    btnLayout->addWidget(saveBtn);

    auto *deleteBtn = new QPushButton("Delete", editGroup);
    connect(deleteBtn, &QPushButton::clicked, this, &SshDialog::onDelete);
    btnLayout->addWidget(deleteBtn);
    formLayout->addRow(btnLayout);

    auto *connectLayout = new QHBoxLayout();
    auto *connectNewBtn = new QPushButton("Connect (New Tab)", editGroup);
    connect(connectNewBtn, &QPushButton::clicked, this, &SshDialog::onConnectNewTab);
    connectLayout->addWidget(connectNewBtn);

    auto *connectCurBtn = new QPushButton("Connect (Current)", editGroup);
    connect(connectCurBtn, &QPushButton::clicked, this, &SshDialog::onConnectCurrent);
    connectLayout->addWidget(connectCurBtn);
    formLayout->addRow(connectLayout);

    splitter->addWidget(editGroup);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);

    mainLayout->addWidget(splitter, 1);
}

void SshDialog::setBookmarks(const QList<SshBookmark> &bookmarks) {
    m_bookmarks = bookmarks;
    refreshList();
}

void SshDialog::refreshList() {
    m_bookmarkList->clear();
    for (const auto &bm : m_bookmarks) {
        QString label = bm.name.isEmpty() ? bm.host : bm.name;
        if (!bm.user.isEmpty()) label += " (" + bm.user + "@" + bm.host + ")";
        m_bookmarkList->addItem(label);
    }
}

void SshDialog::onBookmarkSelected(int row) {
    m_currentIndex = row;
    if (row < 0 || row >= m_bookmarks.size()) return;

    const SshBookmark &bm = m_bookmarks[row];
    m_nameEdit->setText(bm.name);
    m_hostEdit->setText(bm.host);
    m_userEdit->setText(bm.user);
    m_portSpin->setValue(bm.port);
    m_identityEdit->setText(bm.identityFile);
    m_extraArgsEdit->setText(bm.extraArgs);
}

SshBookmark SshDialog::currentFormData() const {
    SshBookmark bm;
    bm.name = m_nameEdit->text().trimmed();
    bm.host = m_hostEdit->text().trimmed();
    bm.user = m_userEdit->text().trimmed();
    bm.port = m_portSpin->value();
    bm.identityFile = m_identityEdit->text().trimmed();
    bm.extraArgs = m_extraArgsEdit->text().trimmed();
    return bm;
}

void SshDialog::onSave() {
    SshBookmark bm = currentFormData();
    if (bm.host.isEmpty()) {
        QMessageBox::warning(this, "SSH Manager", "Host is required.");
        return;
    }

    if (m_currentIndex >= 0 && m_currentIndex < m_bookmarks.size()) {
        m_bookmarks[m_currentIndex] = bm;
    } else {
        m_bookmarks.append(bm);
    }
    refreshList();
    emit bookmarksChanged(m_bookmarks);
}

void SshDialog::onDelete() {
    if (m_currentIndex >= 0 && m_currentIndex < m_bookmarks.size()) {
        m_bookmarks.removeAt(m_currentIndex);
        m_currentIndex = -1;
        refreshList();
        emit bookmarksChanged(m_bookmarks);
    }
}

void SshDialog::onConnectNewTab() {
    SshBookmark bm = currentFormData();
    if (bm.host.isEmpty()) return;
    emit connectRequested(bm.toSshCommand(m_controlMaster), true);
    accept();
}

void SshDialog::onConnectCurrent() {
    SshBookmark bm = currentFormData();
    if (bm.host.isEmpty()) return;
    emit connectRequested(bm.toSshCommand(m_controlMaster), false);
    accept();
}

void SshDialog::onQuickConnect() {
    QString input = m_quickConnect->text().trimmed();
    if (input.isEmpty()) return;

    SshBookmark bm;
    // Parse user@host:port format
    int atIdx = input.indexOf('@');
    if (atIdx >= 0) {
        bm.user = input.left(atIdx);
        input = input.mid(atIdx + 1);
    }

    // 0.7.54 (2026-04-27 indie-review) — IPv6 host parsing. The naive
    // `indexOf(':')` split mangled `[2001:db8::1]:2222` because the
    // first colon is INSIDE the bracketed address, not the
    // host/port separator. Per RFC 3986 §3.2.2, IPv6 literals MUST
    // be bracketed in URI authority components; the bracket form is
    // also the convention OpenSSH expects on the command line
    // (`ssh -p 2222 [2001:db8::1]` works).
    //
    // Detect a leading `[` and locate the matching `]`. If found, the
    // host is the bracketed body and the port (if any) follows after
    // the closing `]:`. Otherwise fall back to the legacy single-
    // colon split (covers IPv4 hosts and DNS names).
    if (input.startsWith('[')) {
        int rb = input.indexOf(']');
        if (rb > 0) {
            bm.host = input.mid(1, rb - 1);
            // After `]` we expect `:port` or end-of-string.
            if (rb + 1 < input.size() && input[rb + 1] == ':') {
                bm.port = input.mid(rb + 2).toInt();
                if (bm.port <= 0 || bm.port > 65535) bm.port = 22;
            }
        } else {
            // Unmatched `[` — treat the whole thing as a host. Lets
            // a typo like `[2001:db8::1` still fall through to ssh,
            // which will fail with a sensible error rather than
            // silently parsing as host=`[2001` port=0.
            bm.host = input;
        }
    } else {
        int colonIdx = input.indexOf(':');
        if (colonIdx >= 0) {
            bm.host = input.left(colonIdx);
            bm.port = input.mid(colonIdx + 1).toInt();
            if (bm.port <= 0 || bm.port > 65535) bm.port = 22;
        } else {
            bm.host = input;
        }
    }

    if (!bm.host.isEmpty()) {
        emit connectRequested(bm.toSshCommand(m_controlMaster), true);
        accept();
    }
}
