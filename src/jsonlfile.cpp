// ANTS-2119 — see jsonlfile.h.
#include "jsonlfile.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "secureio.h"   // setOwnerOnlyPerms, ensurePrivateDir

namespace JsonlFile {

QList<QByteArray> readLines(const QString &path) {
    QList<QByteArray> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray all = f.readAll();
    for (const QByteArray &ln : all.split('\n'))
        if (!ln.trimmed().isEmpty()) out.append(ln);
    return out;
}

bool writeLinesAtomic(const QString &path, const QList<QByteArray> &lines) {
    const QFileInfo fi(path);
    // ANTS-1988 — private (0700) cache dir, no world-readable mkpath window.
    if (!ensurePrivateDir(fi.absolutePath())) return false;
    QSaveFile sf(path);
    if (!sf.open(QIODevice::WriteOnly)) return false;
    for (const QByteArray &ln : lines) {
        if (sf.write(ln) != ln.size() || sf.write("\n", 1) != 1) {
            sf.cancelWriting();
            return false;
        }
    }
    // Tighten perms on the temp file before the atomic rename so the final
    // file is never briefly group/world-readable (no create-then-chmod window).
    setOwnerOnlyPerms(sf);
    return sf.commit();
}

}  // namespace JsonlFile
