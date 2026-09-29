#include "appimageenv.h"

#include <QList>

namespace AppImageEnv {

QByteArray withoutEntry(const QByteArray &pathList, const QByteArray &entry) {
    QList<QByteArray> kept;
    for (const QByteArray &e : pathList.split(':'))
        if (e != entry) kept.append(e);
    return kept.join(':');
}

void dropOpenSsl3Shim() {
    const QByteArray shim = qgetenv("ANTS_OPENSSL3_SHIM");
    if (shim.isEmpty()) return;
    const QByteArray rest = withoutEntry(qgetenv("LD_LIBRARY_PATH"), shim);
    if (rest.isEmpty())
        qunsetenv("LD_LIBRARY_PATH");
    else
        qputenv("LD_LIBRARY_PATH", rest);
    qunsetenv("ANTS_OPENSSL3_SHIM");
}

}  // namespace AppImageEnv
