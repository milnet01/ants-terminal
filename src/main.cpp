#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFont>
#include <QSurfaceFormat>
#include <QStringList>
#include <QStandardPaths>
#include <QIcon>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QTextStream>

#include <csignal>
#include <cstdio>

namespace {

// `ants-terminal --new-plugin <name>` — scaffold a plugin skeleton.
// Returns the process exit code (0 on success). Prints status to stdout/stderr.
int scaffoldPlugin(const QString &name) {
    static const QRegularExpression validName(R"(^[a-zA-Z][\w-]{0,63}$)");
    if (!validName.match(name).hasMatch()) {
        std::fprintf(stderr, "Invalid plugin name: %s\n"
                     "Use [A-Za-z][A-Za-z0-9_-]{0,63} (letters/digits/underscore/hyphen).\n",
                     qUtf8Printable(name));
        return 2;
    }

    QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
                   + "/ants-terminal/plugins";
    QString dir = base + "/" + name;
    if (QFile::exists(dir)) {
        std::fprintf(stderr, "Plugin already exists: %s\n", qUtf8Printable(dir));
        return 3;
    }
    if (!QDir().mkpath(dir)) {
        std::fprintf(stderr, "Failed to create plugin dir: %s\n", qUtf8Printable(dir));
        return 4;
    }

    auto writeFile = [&](const QString &relPath, const QString &content) -> bool {
        QFile f(dir + "/" + relPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        QTextStream out(&f);
        out << content;
        return true;
    };

    QString initLua = QString(
        "-- %1/init.lua\n"
        "-- Plugin entry point. Runs once per terminal launch in a sandboxed Lua VM.\n"
        "-- See https://github.com/milnet01/ants-terminal/blob/main/PLUGINS.md for the full API.\n"
        "\n"
        "ants.log('loaded %1 (ants ' .. (ants._version or '?') .. ')')\n"
        "\n"
        "-- Example: greet on each new prompt\n"
        "ants.on('prompt', function(_)\n"
        "    -- ants.set_status('hello from %1')\n"
        "end)\n"
    ).arg(name);

    QString manifest = QString(
        "{\n"
        "  \"name\": \"%1\",\n"
        "  \"version\": \"0.1.0\",\n"
        "  \"description\": \"Describe your plugin in one line.\",\n"
        "  \"author\": \"Your Name <you@example.com>\",\n"
        "  \"permissions\": []\n"
        "}\n"
    ).arg(name);

    QString readme = QString(
        "# %1\n"
        "\n"
        "Ants Terminal plugin. Edit `init.lua` and reload the terminal\n"
        "(or set `ANTS_PLUGIN_DEV=1` for hot-reload).\n"
        "\n"
        "## API\n"
        "\n"
        "See [PLUGINS.md](https://github.com/milnet01/ants-terminal/blob/main/PLUGINS.md)\n"
        "for the full `ants.*` surface.\n"
    ).arg(name);

    if (!writeFile("init.lua", initLua) ||
        !writeFile("manifest.json", manifest) ||
        !writeFile("README.md", readme)) {
        std::fprintf(stderr, "Failed to write plugin files in %s\n", qUtf8Printable(dir));
        return 5;
    }

    std::printf("Created plugin scaffold:\n  %s\n\n"
                "Next steps:\n"
                "  1. Edit %s/init.lua\n"
                "  2. Enable the plugin in Settings → Plugins (or set `enabled_plugins`)\n"
                "  3. Set ANTS_PLUGIN_DEV=1 during development for verbose logging\n",
                qUtf8Printable(dir), qUtf8Printable(dir));
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    // Ignore SIGPIPE — writing to a closed PTY delivers SIGPIPE which would
    // terminate the process.  Qt handles write errors via return codes.
    std::signal(SIGPIPE, SIG_IGN);
    // Set default surface format with alpha for per-pixel transparency
    // Do NOT set Core Profile here — it breaks QPainter's GL paint engine
    // font scaling on displays where physical DPI differs from logical DPI.
    // The GlRenderer requests Core Profile on its own context when needed.
    QSurfaceFormat fmt;
    fmt.setAlphaBufferSize(8);
    fmt.setDepthBufferSize(24);
    // Opt into GL_ARB_robustness: driver delivers GL_*_RESET_* errors instead
    // of silently killing the process when the GPU context is lost (VT switch,
    // driver update, GPU hang, suspend/resume). Without this, a context-loss
    // event terminates Ants with no recovery path.
    fmt.setOption(QSurfaceFormat::ResetNotification);
    QSurfaceFormat::setDefaultFormat(fmt);

    // Escape hatch for broken GPU drivers: `ANTS_SOFTWARE_GL=1` forces Mesa's
    // llvmpipe software rasterizer. Must be set before QApplication is
    // constructed, otherwise the attribute is ignored.
    if (qEnvironmentVariableIntValue("ANTS_SOFTWARE_GL") > 0) {
        QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }

    QApplication app(argc, argv);
    app.setApplicationName("Ants Terminal");
    app.setApplicationVersion(ANTS_VERSION);
    app.setStyle("Fusion");

    // CLI parsing — handles --help / --version / --new-plugin before GUI init.
    QCommandLineParser parser;
    parser.setApplicationDescription("Ants Terminal — a modern, themeable terminal emulator.");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption quakeOpt({"quake", "dropdown"}, "Run in Quake / drop-down mode.");
    parser.addOption(quakeOpt);
    QCommandLineOption newPluginOpt("new-plugin",
        "Create a new plugin skeleton in the user's plugin dir and exit.",
        "name");
    parser.addOption(newPluginOpt);
    parser.process(app);

    if (parser.isSet(newPluginOpt)) {
        return scaffoldPlugin(parser.value(newPluginOpt));
    }
    bool quakeMode = parser.isSet(quakeOpt);

    // Application icon (taskbar, window manager, dialogs)
    QIcon appIcon = QIcon::fromTheme("ants-terminal");
    if (appIcon.isNull()) {
        // Fallback: load from assets/ next to the executable
        QString base = QApplication::applicationDirPath() + "/../assets/ants-terminal";
        for (int sz : {16, 32, 48, 64, 128, 256})
            appIcon.addFile(QString("%1-%2.png").arg(base).arg(sz), QSize(sz, sz));
    }
    app.setWindowIcon(appIcon);

    QFont font("Monospace", 11);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    app.setFont(font);

    MainWindow window(quakeMode);
    window.show();

    return app.exec();
}
