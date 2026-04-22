// Fixture: ssh_argv_dash_host — ssh argv construction missing the `--`
// terminator before a user-controlled host token. CVE-2017-1000117 class:
// a host that starts with `-` would be parsed by ssh(1) as an option.

#include <QStringList>
#include <QString>

static QString shellQuote(const QString &s);

static QStringList buildArgvPlain(const QString &host) {
    QStringList args;
    args << "-o" << "StrictHostKeyChecking=yes";
    args << shellQuote(host);                 // @expect ssh_argv_dash_host
    return args;
}

static QStringList buildArgvWithUser(const QString &user, const QString &host) {
    QStringList args;
    args << "-i" << "/home/me/.ssh/id_ed25519";
    args << shellQuote(user + "@" + host);    // @expect ssh_argv_dash_host
    return args;
}
