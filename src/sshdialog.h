#pragma once

#include <QDialog>
#include <QListWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonObject>

struct SshBookmark {
    QString name;
    QString host;
    QString user;
    int port = 22;
    QString identityFile;
    QString extraArgs;

    QJsonObject toJson() const;
    static SshBookmark fromJson(const QJsonObject &obj);
    QString toSshCommand() const;
};

// SSH Manager dialog — manage bookmarks and connect
class SshDialog : public QDialog {
    Q_OBJECT

public:
    explicit SshDialog(QWidget *parent = nullptr);

    void setBookmarks(const QList<SshBookmark> &bookmarks);
    const QList<SshBookmark> &bookmarks() const { return m_bookmarks; }

signals:
    void connectRequested(const QString &sshCommand, bool newTab);
    void bookmarksChanged(const QList<SshBookmark> &bookmarks);

private slots:
    void onBookmarkSelected(int row);
    void onSave();
    void onDelete();
    void onConnectNewTab();
    void onConnectCurrent();
    void onQuickConnect();

private:
    void refreshList();
    SshBookmark currentFormData() const;

    QListWidget *m_bookmarkList = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_identityEdit = nullptr;
    QLineEdit *m_extraArgsEdit = nullptr;
    QLineEdit *m_quickConnect = nullptr;

    QList<SshBookmark> m_bookmarks;
    int m_currentIndex = -1;
};
