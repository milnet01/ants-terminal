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
    // controlMaster=true appends -o ControlMaster=auto / ControlPath / ControlPersist,
    // so repeat connections to the same host multiplex over the first session.
    QString toSshCommand(bool controlMaster = false) const;

    // Take a whitespace-separated extraArgs string and return the
    // tokens safe to forward to ssh. Rejects tokens that enable arbitrary
    // command execution via ssh's option system:
    //   -oProxyCommand=…, -o ProxyCommand=…
    //   -oLocalCommand=…, -o LocalCommand=…
    //   -oPermitLocalCommand=yes (plus the surrounding -o form)
    // These options let the bookmark owner (potentially: a contributed
    // plugin, a config-syncing tool, a synced dotfile repo) run
    // arbitrary shell commands when the user connects — the classic
    // CVE-2017-1000117-adjacent attack via bookmark data rather than
    // hostnames. The `--` separator already guards against dash-host
    // injection; this list guards against the `-o` surface.
    //
    // Returns the filtered tokens; `out_rejected`, if non-null, gets a
    // list of the rejected raw tokens so callers can surface them in a
    // debug log or UI warning.
    static QStringList sanitizeExtraArgs(const QString &extraArgs,
                                         QStringList *out_rejected = nullptr);
};

// SSH Manager dialog — manage bookmarks and connect
class SshDialog : public QDialog {
    Q_OBJECT

public:
    explicit SshDialog(QWidget *parent = nullptr);

    void setBookmarks(const QList<SshBookmark> &bookmarks);
    const QList<SshBookmark> &bookmarks() const { return m_bookmarks; }

    // Tells connect-builders whether to splice -o ControlMaster=auto etc.
    // onto each invocation. MainWindow mirrors Config::sshControlMaster()
    // here before show()ing the dialog.
    void setControlMaster(bool enabled) { m_controlMaster = enabled; }

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
    bool m_controlMaster = false;
};
