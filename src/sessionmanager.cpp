#include "sessionmanager.h"
#include "secureio.h"
#include "terminalgrid.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cstdio>      // std::rename — atomic POSIX-compliant overwrite
#include <cstring>     // std::strerror
#include <sys/stat.h>
#include <unistd.h>    // fsync — durability guarantee before atomic rename

QString SessionManager::sessionDir() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                  + "/ants-terminal/sessions";
    QDir().mkpath(dir);
    // ANTS-1141 — tighten the directory perms to 0700. Pre-fix
    // code inherited the process umask (typically 0022 → 0755),
    // which left filenames + mtimes + sizes enumerable to other
    // local users. Individual session_*.dat files are 0600 (per
    // the saveSession path), but the directory listing leaks
    // active-session timing and scrollback volume — symmetric
    // with the config dir which is already 0700.
    QFile::setPermissions(dir,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner |
        QFileDevice::ExeOwner);
    return dir;
}

QString SessionManager::sessionPath(const QString &tabId) {
    // Validate tabId to prevent path traversal (must be UUID-like: alnum + hyphens)
    static const QRegularExpression validId(QStringLiteral("^[a-zA-Z0-9\\-]+$"));
    if (!validId.match(tabId).hasMatch()) return {};
    return sessionDir() + "/session_" + tabId + ".dat";
}

QByteArray SessionManager::serialize(const TerminalGrid *grid,
                                     const QString &cwd,
                                     const QString &pinnedTitle) {
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

    // V3: Manual tab rename pin (empty string when user hasn't renamed
    // the tab). Trailing field — V2 readers simply stop here.
    out << pinnedTitle;

    // Compress
    QByteArray compressed = qCompress(raw, 6);

    // V4 envelope: SHEC magic + envelope version + SHA-256(compressed)
    // + payload length + compressed payload. Tampering with the payload
    // (or the envelope's length field) flips the hash. Legacy V1-V3
    // files lacked any payload integrity — anyone with write access to
    // the sessions dir could plant arbitrary codepoints/colors/flags
    // into the next restore.
    QByteArray sha = QCryptographicHash::hash(compressed,
                                              QCryptographicHash::Sha256);

    QByteArray envelope;
    envelope.reserve(ENVELOPE_HEADER_SIZE + compressed.size());
    QDataStream env(&envelope, QIODevice::WriteOnly);
    env.setVersion(QDataStream::Qt_6_0);
    env << ENVELOPE_MAGIC << ENVELOPE_VERSION;
    env.writeRawData(sha.constData(), sha.size());
    env << static_cast<uint32_t>(compressed.size());
    env.writeRawData(compressed.constData(), compressed.size());
    return envelope;
}

bool SessionManager::restore(TerminalGrid *grid, const QByteArray &input,
                             QString *cwd, QString *pinnedTitle) {
    // Clear optional out-params up front so callers reading them after
    // a V1/V2 file don't see stale bytes. The version-gated read blocks
    // below populate them only when the on-disk format has the field.
    if (cwd) *cwd = QString();
    if (pinnedTitle) *pinnedTitle = QString();

    // Reject excessively large input (100MB limit)
    if (input.size() > 100 * 1024 * 1024) return false;

    // Detect V4 envelope: peek the first uint32. qCompress's first 4
    // bytes are the big-endian uncompressed length, which for any real
    // session is well below ENVELOPE_MAGIC (0x53484543 ≈ 1.4 GB), so
    // any byte sequence that starts with our magic is unambiguously a
    // V4 envelope. Legacy V1-V3 files fall through with `compressed`
    // pointing at the raw qCompress output.
    QByteArray compressed;
    if (input.size() >= ENVELOPE_HEADER_SIZE) {
        QDataStream env(input);
        env.setVersion(QDataStream::Qt_6_0);
        uint32_t envMagic = 0, envVersion = 0;
        env >> envMagic >> envVersion;
        if (envMagic == ENVELOPE_MAGIC) {
            if (envVersion != ENVELOPE_VERSION) return false;
            QByteArray sha(32, Qt::Uninitialized);
            if (env.readRawData(sha.data(), 32) != 32) return false;
            uint32_t payloadLen = 0;
            env >> payloadLen;
            if (env.status() != QDataStream::Ok) return false;
            // Bound payload length to the same 100MB ceiling we apply
            // to legacy files, and require the envelope to declare a
            // length that exactly matches the trailing bytes — refuses
            // truncated or padded files before we hash anything.
            if (payloadLen > 100u * 1024u * 1024u) return false;
            const qsizetype expectedSize = qsizetype(ENVELOPE_HEADER_SIZE)
                                         + qsizetype(payloadLen);
            if (input.size() != expectedSize) return false;
            compressed = input.mid(ENVELOPE_HEADER_SIZE,
                                   qsizetype(payloadLen));
            // Verify SHA-256. Mismatch ⇒ payload was tampered with or
            // truncated mid-write; refuse to restore rather than feed
            // attacker-controlled bytes into the grid.
            const QByteArray actual = QCryptographicHash::hash(
                compressed, QCryptographicHash::Sha256);
            if (actual != sha) return false;
        }
    }
    if (compressed.isEmpty()) {
        // Legacy V1-V3 file (raw qCompress output, no envelope, no checksum).
        compressed = input;
    }

    // Pre-validate qCompress's 4-byte big-endian uncompressed-length
    // prefix BEFORE qUncompress allocates. A crafted file claiming
    // 500 MB used to trigger a 500 MB allocation that the post-hoc
    // 500 MB cap could only catch after the damage was done; by
    // rejecting the claim up front we never touch the allocator.
    if (compressed.size() < 4) return false;
    const auto *lenBytes = reinterpret_cast<const uint8_t *>(
        compressed.constData());
    const uint32_t claimedUncompressed =
        (uint32_t(lenBytes[0]) << 24) |
        (uint32_t(lenBytes[1]) << 16) |
        (uint32_t(lenBytes[2]) <<  8) |
         uint32_t(lenBytes[3]);
    if (claimedUncompressed > MAX_UNCOMPRESSED) return false;

    QByteArray raw = qUncompress(compressed);
    if (raw.isEmpty()) return false;
    // Guard against decompression bombs (zlib can expand ~1000:1).
    // The pre-flight above bounds `claimedUncompressed`, but a payload
    // can still under-claim and over-deliver; keep this defensive cap.
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

    // Read a cell from the stream. Returns false if the stream went bad
    // mid-cell — refusing to commit half-decoded codepoints/colors/flags
    // to the grid. Pre-fix, a truncated stream silently wrote
    // uninitialized fg/bg/flags into Cell.
    auto readCell = [&in](Cell &c) -> bool {
        QRgb fg, bg;
        uint8_t flags;
        in >> c.codepoint >> fg >> bg >> flags;
        if (in.status() != QDataStream::Ok) return false;
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
        return true;
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
                if (in.status() != QDataStream::Ok) return false;
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
        for (int j = 0; j < cellCount; ++j) {
            if (!readCell(line.cells[j])) return false;
        }
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
        for (int col = 0; col < colCount && col < cols; ++col) {
            if (!readCell(screenLine.cells[col])) return false;
        }
        // Skip extra columns if saved grid was wider
        for (int col = cols; col < colCount; ++col) {
            Cell skip;
            if (!readCell(skip)) return false;
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
            if (!readCell(skip)) return false;
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

    // V3: Read pinned tab title if present. Older V2 files end after
    // cwd; atEnd() gates so the stream-status check below doesn't flip
    // to ReadPastEnd and fail the restore for pre-V3 files.
    if (version >= 3 && !in.atEnd()) {
        QString savedPinned;
        in >> savedPinned;
        if (pinnedTitle)
            *pinnedTitle = savedPinned;
    }

    return in.status() == QDataStream::Ok;
}

void SessionManager::saveSession(const QString &tabId, const TerminalGrid *grid,
                                 const QString &cwd,
                                 const QString &pinnedTitle) {
    QByteArray data = serialize(grid, cwd, pinnedTitle);
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
            // 0.7.52 (2026-04-27 indie-review CRITICAL — silent data
            // loss). Was QFile::rename, which on every POSIX target
            // refuses to overwrite an existing destination — every
            // session save AFTER the first silently failed (the .dat
            // file held the original snapshot, .dat.tmp accumulated
            // each new write). User scrollback never updated past the
            // first save. std::rename mirrors POSIX rename(2) which
            // atomically replaces the destination, matching Config's
            // 0.7.12 fix. Log the errno on failure (ENOSPC, EACCES,
            // EXDEV) and remove the orphaned .tmp so the disk doesn't
            // accumulate corpses across session lifetimes.
            const int rc = std::rename(tmpPath.toLocal8Bit().constData(),
                                       path.toLocal8Bit().constData());
            if (rc == 0) {
                // Post-rename chmod: rename(2) preserves perms on
                // most local FS, but FAT/exFAT/SMB/NFS edge cases or
                // Qt's copy+unlink fallback can drop the 0600 set on
                // the temp fd. Session blobs may hold scrollback
                // content (passwords mistyped at the prompt, ssh
                // command history, paste buffers); re-chmod the
                // final inode.
                setOwnerOnlyPerms(path);
                // ANTS-1141 — fsync parent dir for crash-safe
                // rename durability (Postgres pattern). See
                // Config::save for the full rationale.
                fsyncParentDir(path);
            } else {
                qWarning("SessionManager::saveSession rename(%s -> %s) "
                         "failed: errno=%d (%s) — prior session blob "
                         "unchanged, tmp removed",
                         qUtf8Printable(tmpPath), qUtf8Printable(path),
                         errno, std::strerror(errno));
                QFile::remove(tmpPath);
            }
        } else {
            file.close();
            QFile::remove(tmpPath);
        }
    }
    ::umask(oldMask);
}

bool SessionManager::loadSession(const QString &tabId, TerminalGrid *grid,
                                 QString *cwd, QString *pinnedTitle) {
    QString path = sessionPath(tabId);
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    if (file.size() > 100 * 1024 * 1024) return false; // 100MB limit before reading
    QByteArray data = file.readAll();
    return restore(grid, data, cwd, pinnedTitle);
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
        // 0.7.52 — same QFile::rename → std::rename fix as saveSession.
        // tab_order.txt was even worse: QFile::rename failing meant the
        // FIRST run's tab order was the only one ever persisted. Every
        // tab open/close/reorder thereafter looked like it saved but
        // restoring a future session resurrected the original layout.
        const int rc = std::rename(tmpPath.toLocal8Bit().constData(),
                                   path.toLocal8Bit().constData());
        if (rc == 0) {
            setOwnerOnlyPerms(path);
            // ANTS-1141 — fsync parent dir; see Config::save.
            fsyncParentDir(path);
        } else {
            qWarning("SessionManager::saveTabOrder rename(%s -> %s) "
                     "failed: errno=%d (%s) — prior tab_order.txt "
                     "unchanged, tmp removed",
                     qUtf8Printable(tmpPath), qUtf8Printable(path),
                     errno, std::strerror(errno));
            QFile::remove(tmpPath);
        }
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

    // ANTS-1141 — do NOT remove the manifest on read. Pre-fix
    // code unconditionally deleted tab_order.txt before
    // saveAllSessions had a chance to overwrite it, so a crash
    // within the 5-second uptime floor on
    // MainWindow::saveAllSessions would lose the tab order
    // permanently (the per-session .dat blobs survive but
    // ordering reverts to mtime-based fallback). Atomic-write
    // on save overwrites this file each time, so leaving it in
    // place across reads is safe.
    file.close();

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
    // ANTS-1141 — sweep orphan .tmp files older than 1 day.
    // saveSession + saveTabOrder both write to <name>.tmp and
    // rename(2) onto the canonical path; on rename failure
    // (ENOSPC, EROFS, EBUSY etc.) the tmp is QFile::removed in
    // the failure branch. But a hard kill mid-rename can leave
    // the tmp behind. Without this sweep they'd accumulate.
    QFileInfoList tmps = dir.entryInfoList({"*.tmp"}, QDir::Files);
    QDateTime tmpCutoff = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo &fi : tmps) {
        if (fi.lastModified() < tmpCutoff)
            QFile::remove(fi.absoluteFilePath());
    }
}
