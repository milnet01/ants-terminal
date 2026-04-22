#include "sessionmanager.h"
#include "secureio.h"
#include "terminalgrid.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>    // fsync — durability guarantee before atomic rename

QString SessionManager::sessionDir() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                  + "/ants-terminal/sessions";
    QDir().mkpath(dir);
    return dir;
}

QString SessionManager::sessionPath(const QString &tabId) {
    // Validate tabId to prevent path traversal (must be UUID-like: alnum + hyphens)
    static const QRegularExpression validId(QStringLiteral("^[a-zA-Z0-9\\-]+$"));
    if (!validId.match(tabId).hasMatch()) return {};
    return sessionDir() + "/session_" + tabId + ".dat";
}

QByteArray SessionManager::serialize(const TerminalGrid *grid, const QString &cwd) {
    QByteArray raw;
    QDataStream out(&raw, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);

    // Header
    out << MAGIC << VERSION;
    out << static_cast<int32_t>(grid->rows());
    out << static_cast<int32_t>(grid->cols());
    out << static_cast<int32_t>(grid->cursorRow());
    out << static_cast<int32_t>(grid->cursorCol());

    // Scrollback lines
    int sbSize = grid->scrollbackSize();
    out << static_cast<int32_t>(sbSize);

    for (int i = 0; i < sbSize; ++i) {
        const auto &cells = grid->scrollbackLine(i);
        bool wrapped = grid->scrollbackLineWrapped(i);
        out << static_cast<int32_t>(cells.size());
        out << wrapped;

        for (const auto &c : cells) {
            out << c.codepoint;
            out << c.attrs.fg.rgba();
            out << c.attrs.bg.rgba();
            uint8_t flags = 0;
            if (c.attrs.bold) flags |= 0x01;
            if (c.attrs.italic) flags |= 0x02;
            if (c.attrs.underline) flags |= 0x04;
            if (c.attrs.inverse) flags |= 0x08;
            if (c.attrs.dim) flags |= 0x10;
            if (c.attrs.strikethrough) flags |= 0x20;
            if (c.isWideChar) flags |= 0x40;
            if (c.isWideCont) flags |= 0x80;
            out << flags;
        }

        // Combining characters
        const auto &combining = grid->scrollbackCombining(i);
        out << static_cast<int32_t>(combining.size());
        for (const auto &[col, cps] : combining) {
            out << static_cast<int32_t>(col);
            out << static_cast<int32_t>(cps.size());
            for (uint32_t cp : cps) out << cp;
        }
    }

    // Screen lines
    int screenRows = grid->rows();
    out << static_cast<int32_t>(screenRows);
    for (int row = 0; row < screenRows; ++row) {
        int colCount = grid->cols();
        out << static_cast<int32_t>(colCount);
        for (int col = 0; col < colCount; ++col) {
            const Cell &c = grid->cellAt(row, col);
            out << c.codepoint;
            out << c.attrs.fg.rgba();
            out << c.attrs.bg.rgba();
            uint8_t flags = 0;
            if (c.attrs.bold) flags |= 0x01;
            if (c.attrs.italic) flags |= 0x02;
            if (c.attrs.underline) flags |= 0x04;
            if (c.attrs.inverse) flags |= 0x08;
            if (c.attrs.dim) flags |= 0x10;
            if (c.attrs.strikethrough) flags |= 0x20;
            if (c.isWideChar) flags |= 0x40;
            if (c.isWideCont) flags |= 0x80;
            out << flags;
        }

        // Screen combining characters
        const auto &combining = grid->screenCombining(row);
        out << static_cast<int32_t>(combining.size());
        for (const auto &[col, cps] : combining) {
            out << static_cast<int32_t>(col);
            out << static_cast<int32_t>(cps.size());
            for (uint32_t cp : cps) out << cp;
        }
    }

    // Window title
    out << grid->windowTitle();

    // V2: Working directory
    out << cwd;

    // Compress
    return qCompress(raw, 6);
}

bool SessionManager::restore(TerminalGrid *grid, const QByteArray &compressed, QString *cwd) {
    // Reject excessively large compressed data (100MB limit)
    if (compressed.size() > 100 * 1024 * 1024) return false;

    QByteArray raw = qUncompress(compressed);
    if (raw.isEmpty()) return false;
    // Guard against decompression bombs (zlib can expand ~1000:1)
    if (raw.size() > 500 * 1024 * 1024) return false;

    QDataStream in(&raw, QIODevice::ReadOnly);
    in.setVersion(QDataStream::Qt_6_0);

    uint32_t magic, version;
    in >> magic >> version;
    if (magic != MAGIC || version > VERSION) return false;

    int32_t rows, cols, curRow, curCol;
    in >> rows >> cols >> curRow >> curCol;
    if (in.status() != QDataStream::Ok) return false;

    // Bounds-check deserialized dimensions to prevent memory exhaustion
    if (rows <= 0 || rows > 500 || cols <= 0 || cols > 1000) return false;

    // Resize grid to match saved dimensions
    grid->resize(rows, cols);

    // Helper to read a cell from stream
    auto readCell = [&in](Cell &c) {
        QRgb fg, bg;
        uint8_t flags;
        in >> c.codepoint >> fg >> bg >> flags;
        c.attrs.fg = QColor::fromRgba(fg);
        c.attrs.bg = QColor::fromRgba(bg);
        c.attrs.bold = flags & 0x01;
        c.attrs.italic = flags & 0x02;
        c.attrs.underline = flags & 0x04;
        c.attrs.inverse = flags & 0x08;
        c.attrs.dim = flags & 0x10;
        c.attrs.strikethrough = flags & 0x20;
        c.isWideChar = flags & 0x40;
        c.isWideCont = flags & 0x80;
    };

    // Helper to read combining characters
    auto readCombining = [&in](std::unordered_map<int, std::vector<uint32_t>> &combining) -> bool {
        int32_t combCount;
        in >> combCount;
        if (in.status() != QDataStream::Ok || combCount < 0 || combCount > 10000) return false;
        for (int j = 0; j < combCount; ++j) {
            int32_t col, cpCount;
            in >> col >> cpCount;
            if (in.status() != QDataStream::Ok || cpCount < 0 || cpCount > 8) return false;
            std::vector<uint32_t> cps;
            cps.reserve(cpCount);
            for (int k = 0; k < cpCount; ++k) {
                uint32_t cp;
                in >> cp;
                cps.push_back(cp);
            }
            if (!cps.empty())
                combining[col] = std::move(cps);
        }
        return true;
    };

    // Read and restore scrollback lines
    int32_t sbSize;
    in >> sbSize;
    if (in.status() != QDataStream::Ok || sbSize < 0 || sbSize > 1000000) return false;

    for (int i = 0; i < sbSize; ++i) {
        int32_t cellCount;
        bool wrapped;
        in >> cellCount >> wrapped;
        if (in.status() != QDataStream::Ok || cellCount < 0 || cellCount > 10000) return false;

        TermLine line;
        line.softWrapped = wrapped;
        line.cells.resize(cellCount);
        for (int j = 0; j < cellCount; ++j)
            readCell(line.cells[j]);
        if (!readCombining(line.combining)) return false;

        grid->pushScrollbackLine(std::move(line));
    }

    // Read and restore screen lines
    int32_t screenRows;
    in >> screenRows;
    if (in.status() != QDataStream::Ok || screenRows < 0 || screenRows > 500) return false;

    for (int row = 0; row < screenRows && row < rows; ++row) {
        int32_t colCount;
        in >> colCount;
        if (in.status() != QDataStream::Ok || colCount < 0 || colCount > 10000) return false;

        TermLine &screenLine = grid->screenLine(row);
        for (int col = 0; col < colCount && col < cols; ++col)
            readCell(screenLine.cells[col]);
        // Skip extra columns if saved grid was wider
        for (int col = cols; col < colCount; ++col) {
            Cell skip;
            readCell(skip);
        }
        if (!readCombining(screenLine.combining)) return false;
    }
    // Skip extra rows if saved grid was taller
    for (int row = rows; row < screenRows; ++row) {
        int32_t colCount;
        in >> colCount;
        if (in.status() != QDataStream::Ok || colCount < 0 || colCount > 10000) return false;
        for (int col = 0; col < colCount; ++col) {
            Cell skip;
            readCell(skip);
        }
        std::unordered_map<int, std::vector<uint32_t>> skipComb;
        if (!readCombining(skipComb)) return false;
    }

    // Restore cursor position and title
    grid->setCursorPosition(std::clamp(curRow, 0, rows - 1),
                            std::clamp(curCol, 0, cols - 1));

    QString title;
    in >> title;
    if (!title.isEmpty())
        grid->setTitle(title);

    // V2: Read working directory if present
    if (version >= 2 && !in.atEnd()) {
        QString savedCwd;
        in >> savedCwd;
        if (cwd && !savedCwd.isEmpty())
            *cwd = savedCwd;
    }

    return in.status() == QDataStream::Ok;
}

void SessionManager::saveSession(const QString &tabId, const TerminalGrid *grid, const QString &cwd) {
    QByteArray data = serialize(grid, cwd);
    QString path = sessionPath(tabId);
    if (path.isEmpty()) return;
    QString tmpPath = path + QStringLiteral(".tmp");
    mode_t oldMask = ::umask(0077);
    QFile file(tmpPath);
    if (file.open(QIODevice::WriteOnly)) {
        setOwnerOnlyPerms(file);
        if (file.write(data) == data.size()) {
            // fsync before rename — see Config::save for rationale.
            // Scrollback blobs are worth durability; losing a session
            // from the last 200 ms of the previous run to a kernel
            // crash is a worse outcome than the one-syscall cost.
            ::fsync(file.handle());
            file.close();
            QFile::rename(tmpPath, path);
        } else {
            file.close();
            QFile::remove(tmpPath);
        }
    }
    ::umask(oldMask);
}

bool SessionManager::loadSession(const QString &tabId, TerminalGrid *grid, QString *cwd) {
    QString path = sessionPath(tabId);
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    if (file.size() > 100 * 1024 * 1024) return false; // 100MB limit before reading
    QByteArray data = file.readAll();
    return restore(grid, data, cwd);
}

void SessionManager::removeSession(const QString &tabId) {
    QFile::remove(sessionPath(tabId));
}

QStringList SessionManager::savedSessions() {
    QDir dir(sessionDir());
    QStringList files = dir.entryList({"session_*.dat"}, QDir::Files, QDir::Time | QDir::Reversed);
    QStringList ids;
    for (const QString &f : files) {
        QString id = f.mid(8); // Remove "session_"
        id.chop(4);            // Remove ".dat"
        ids.append(id);
    }
    return ids;
}

void SessionManager::saveTabOrder(const QStringList &tabIds, int activeIndex) {
    QString path = sessionDir() + "/tab_order.txt";
    QString tmpPath = path + QStringLiteral(".tmp");
    mode_t oldMask = ::umask(0077);
    QFile file(tmpPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setOwnerOnlyPerms(file);
        // First line: active tab index
        file.write(QStringLiteral("active:%1\n").arg(activeIndex).toUtf8());
        for (const QString &id : tabIds) {
            file.write(id.toUtf8());
            file.write("\n");
        }
        file.flush();
        // fsync after flush — flush is Qt-layer (kernel-layer not guaranteed).
        // tab_order.txt is what anchors session restore order: if this file
        // is missing or empty after a crash, saved session blobs orphan.
        ::fsync(file.handle());
        file.close();
        QFile::rename(tmpPath, path);
    }
    ::umask(oldMask);
}

QStringList SessionManager::loadTabOrder(int *activeIndex) {
    QString path = sessionDir() + "/tab_order.txt";
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};

    QStringList ids;
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty()) continue;
        // Parse active tab index line
        if (line.startsWith(QLatin1String("active:"))) {
            if (activeIndex)
                *activeIndex = line.mid(7).toInt();
            continue;
        }
        ids.append(line);
    }

    // Remove the manifest after reading
    file.close();
    QFile::remove(path);

    // Only return IDs whose session files actually exist,
    // adjusting active index to account for filtered-out entries
    QStringList valid;
    int adjustedActive = 0;
    int savedActive = activeIndex ? *activeIndex : 0;

    for (int i = 0; i < ids.size(); ++i) {
        if (QFile::exists(sessionPath(ids[i]))) {
            if (i == savedActive)
                adjustedActive = valid.size();
            valid.append(ids[i]);
        }
    }

    if (activeIndex) {
        if (valid.isEmpty())
            *activeIndex = 0;
        else
            *activeIndex = std::clamp(adjustedActive, 0, static_cast<int>(valid.size()) - 1);
    }

    return valid;
}

void SessionManager::cleanupOldSessions(int maxAgeDays) {
    QDir dir(sessionDir());
    QFileInfoList files = dir.entryInfoList({"session_*.dat"}, QDir::Files);
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-maxAgeDays);
    for (const QFileInfo &fi : files) {
        if (fi.lastModified() < cutoff)
            QFile::remove(fi.absoluteFilePath());
    }
}
