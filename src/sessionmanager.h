#pragma once

#include <QString>
#include <QByteArray>
#include <QJsonArray>

class TerminalGrid;

// Serializes and deserializes terminal session state (scrollback + screen)
class SessionManager {
public:
    // Serialize grid state to compressed binary data
    static QByteArray serialize(const TerminalGrid *grid);

    // Restore grid state from serialized data
    static bool restore(TerminalGrid *grid, const QByteArray &data);

    // Session file management
    static QString sessionDir();
    static QString sessionPath(const QString &tabId);
    static void saveSession(const QString &tabId, const TerminalGrid *grid);
    static bool loadSession(const QString &tabId, TerminalGrid *grid);
    static void removeSession(const QString &tabId);
    static QStringList savedSessions();

    // Cleanup old sessions (older than maxAgeDays)
    static void cleanupOldSessions(int maxAgeDays = 30);

private:
    static constexpr uint32_t MAGIC = 0x414E5453; // "ANTS"
    static constexpr uint32_t VERSION = 1;
};
