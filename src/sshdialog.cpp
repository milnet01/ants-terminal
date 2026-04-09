#include "sshdialog.h"

#include <QFormLayout>
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

// Shell-quote a string for safe inclusion in a shell command
static QString shellQuote(const QString &s) {
    if (s.isEmpty()) return QStringLiteral("''");
    // If it contains no special characters, return as-is
    if (!s.contains(QRegularExpression("[\\s'\"\\\\$`!#&|;(){}]")))
        return s;
    // Single-quote with escaping of embedded single quotes
    QString quoted = s;
    quoted.replace("'", "'\\''");
    return "'" + quoted + "'";
}

QString SshBookmark::toSshCommand() const {
    QStringList args;
    args << "ssh";
    if (port != 22)
        args << "-p" << QString::number(port);
    if (!identityFile.isEmpty())
        args << "-i" << shellQuote(identityFile);
    if (!extraArgs.isEmpty())
        args << extraArgs; // User-provided verbatim; they control their own args
    if (!user.isEmpty())
        args << shellQuote(user + "@" + host);
    else
        args << shellQuote(host);
    return args.join(' ');
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
    emit connectRequested(bm.toSshCommand(), true);
    accept();
}

void SshDialog::onConnectCurrent() {
    SshBookmark bm = currentFormData();
    if (bm.host.isEmpty()) return;
    emit connectRequested(bm.toSshCommand(), false);
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
    int colonIdx = input.indexOf(':');
    if (colonIdx >= 0) {
        bm.host = input.left(colonIdx);
        bm.port = input.mid(colonIdx + 1).toInt();
        if (bm.port <= 0 || bm.port > 65535) bm.port = 22;
    } else {
        bm.host = input;
    }

    if (!bm.host.isEmpty()) {
        emit connectRequested(bm.toSshCommand(), true);
        accept();
    }
}
