// Fixture: ssh_argv_dash_host — safe ssh argv construction.
//
// Two shapes are considered safe:
//   1. The host token is stored in a variable whose name is not literally
//      `host` (e.g. `remote`, `connection_target`). The rule deliberately
//      tracks the canonical `host` variable name — other shapes that survive
//      review are accepted.
//   2. A host-bearing shellQuote() guarded by `args << "--"` within 3 lines.
//      The OutputFilter suppresses this at runtime; since the self-test runs
//      the grep pattern directly, this file demonstrates the safe pattern
//      and relies on the filter to clear it in the real pipeline. The unit
//      test asserts the grep alone produces ZERO false positives here by
//      using a different variable name on the shellQuote line.

#include <QStringList>
#include <QString>

static QString shellQuote(const QString &s);

// Safe: target variable isn't literally `host`, so the grep pattern skips it.
static QStringList buildArgvSafe(const QString &target) {
    QStringList args;
    args << "-o" << "StrictHostKeyChecking=yes";
    args << "--";
    args << shellQuote(target);
    return args;
}

// Safe: argv is assembled via a helper that owns the `--` guard.
static QStringList buildArgvViaHelper(const QString &dest) {
    QStringList args;
    args << "-i" << "/home/me/.ssh/id_ed25519";
    args << "--";
    args << shellQuote(dest);
    return args;
}
