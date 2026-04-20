#include "mainwindow.h"
#include "remotecontrol.h"

#include <QApplication>
#include <QJsonObject>
#include "debuglog.h"
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
#include <QProxyStyle>
#include <QStyle>
#include <QStyleFactory>

#include <csignal>
#include <cstdio>

namespace {
// QProxyStyle that reports every style-hint animation duration as 0
// and every `SH_Widget_Animate` as false. Installed globally on the
// QApplication to neutralise Fusion's 60 Hz style-driven
// QPropertyAnimation(target=QWidget, prop=geometry) cycle, which
// cascades a full LayoutRequest → UpdateRequest → paint pass every
// frame and surfaces as the dropdown-menu flicker the user reported
// 2026-04-20.  `setEffectEnabled(UI_AnimateMenu, false)` only covers
// menu show/hide; the per-style `SH_Widget_Animation_Duration` hint
// is what drives the persistent QWidgetAnimator loop.
class NoAnimStyle : public QProxyStyle {
public:
    explicit NoAnimStyle(QStyle *base) : QProxyStyle(base) {}
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override {
        switch (hint) {
        case SH_Widget_Animation_Duration:
        case SH_Widget_Animate:
            return 0;
        default:
            return QProxyStyle::styleHint(hint, option, widget, returnData);
        }
    }
};
}  // namespace

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
    // Wrap Fusion in NoAnimStyle so SH_Widget_Animation_Duration reports 0.
    // QProxyStyle takes ownership of the inner QStyle. See note on the
    // NoAnimStyle class for rationale.
    app.setStyle(new NoAnimStyle(QStyleFactory::create("Fusion")));

    // Disable Qt's built-in UI animations. These animate menu show/
    // hide, combobox dropdown, tooltip fade, and toolbox expand as
    // QPropertyAnimation(target=QWidget, prop=geometry) at ~60 Hz,
    // and each animation frame drives a LayoutRequest cascade that
    // repaints every widget in the main window. On Ants that
    // surfaces as visible dropdown flicker any time a menu is open
    // (root cause of the 2026-04-20 user report). We have zero use
    // cases for animated menus — the terminal is a precision tool,
    // not a presentation app.
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateToolBox, false);

    // Debug-mode bootstrap — see src/debuglog.h. When ANTS_DEBUG is
    // set, we start logging immediately to ~/.local/share/ants-terminal/
    // debug.log and also mirror to stderr so `2>` redirects still
    // capture. Runtime toggles via Tools → Debug Mode only write to
    // the file.
    //   ANTS_DEBUG=all
    //   ANTS_DEBUG=paint,events,vt
    //   ANTS_DEBUG=1          # legacy: == paint
    const QByteArray debugSpec = qgetenv("ANTS_DEBUG");
    if (!debugSpec.isEmpty()) {
        DebugLog::setActive(DebugLog::parseCategories(
            QString::fromLocal8Bit(debugSpec)));
    }

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
    // Remote-control client mode (0.8.0 Kitty-style rc_protocol). When
    // set, the binary connects to the running instance's Unix socket,
    // dispatches a single command, prints the JSON response to stdout,
    // and exits — no GUI init, no MainWindow construction. Single
    // command per invocation; scriptable via shell.
    QCommandLineOption remoteOpt("remote",
        "Send a remote-control command to the running Ants Terminal and "
        "exit. Currently supported: `ls`. Socket path: $ANTS_REMOTE_SOCKET "
        "or $XDG_RUNTIME_DIR/ants-terminal.sock.",
        "cmd");
    parser.addOption(remoteOpt);
    QCommandLineOption remoteSocketOpt("remote-socket",
        "Override the remote-control socket path "
        "(else $ANTS_REMOTE_SOCKET or the XDG default).",
        "path");
    parser.addOption(remoteSocketOpt);
    // Generic per-command options. Each rc_protocol command reads
    // whichever of these apply; commands ignore options they don't
    // care about. Kept to a handful rather than scaling one option
    // per command to keep `--help` output readable.
    QCommandLineOption remoteTabOpt("remote-tab",
        "Target tab index (0-based). Used by tab-scoped commands "
        "(`send-text`, `select-window`, `get-text`, ...); omit to target "
        "the active tab.",
        "index");
    parser.addOption(remoteTabOpt);
    QCommandLineOption remoteTextOpt("remote-text",
        "Text payload for commands like `send-text`. When omitted, "
        "`send-text` reads stdin until EOF instead — lets shell pipes "
        "feed commands into the terminal.",
        "string");
    parser.addOption(remoteTextOpt);
    parser.process(app);

    if (parser.isSet(newPluginOpt)) {
        return scaffoldPlugin(parser.value(newPluginOpt));
    }
    if (parser.isSet(remoteOpt)) {
        const QString socketPath = parser.isSet(remoteSocketOpt)
            ? parser.value(remoteSocketOpt)
            : RemoteControl::defaultSocketPath();
        const QString cmd = parser.value(remoteOpt);
        QJsonObject args;
        if (parser.isSet(remoteTabOpt)) {
            bool ok = false;
            int idx = parser.value(remoteTabOpt).toInt(&ok);
            if (!ok || idx < 0) {
                std::fprintf(stderr,
                    "ants-terminal --remote: invalid --remote-tab value: %s\n",
                    qUtf8Printable(parser.value(remoteTabOpt)));
                return 1;
            }
            args["tab"] = idx;
        }
        if (cmd == QLatin1String("send-text")) {
            QString text;
            if (parser.isSet(remoteTextOpt)) {
                text = parser.value(remoteTextOpt);
            } else {
                // Read stdin until EOF. Lets callers do
                //   echo -ne 'ls\n' | ants-terminal --remote send-text
                // without worrying about shell-quoting escape
                // sequences or newlines on the command line.
                QFile in;
                if (!in.open(stdin, QIODevice::ReadOnly)) {
                    std::fprintf(stderr,
                        "ants-terminal --remote send-text: "
                        "cannot read stdin\n");
                    return 1;
                }
                text = QString::fromUtf8(in.readAll());
            }
            args["text"] = text;
        }
        return RemoteControl::runClient(cmd, args, socketPath);
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
