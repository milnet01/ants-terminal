// dependency_inventory (ANTS-5515). See dependencyinventory.h.

#include "dependencyinventory.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace DependencyInventory {
namespace {

constexpr int kMaxFileBytes = 1024 * 1024;

struct Inventory {
    QJsonArray rows;
    QSet<QString> runtimesSeen;
    int filesRead = 0;
};

QJsonObject baseRow(const char *kind, const QString &file, int line) {
    return {{QStringLiteral("kind"), QLatin1String(kind)},
            {QStringLiteral("file"), file},
            {QStringLiteral("line"), line}};
}

QStringList readLines(const QString &root, const QString &rel, Inventory *inv) {
    QFile f(root + QLatin1Char('/') + rel);
    if (f.size() > kMaxFileBytes || !f.open(QIODevice::ReadOnly)) return {};
    ++inv->filesRead;
    return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
}

bool isExpression(const QString &value) {
    return value.contains(QLatin1Char('$'));
}

QString unquoted(QString s) {
    s = s.trimmed();
    if (s.size() >= 2 && (s.front() == QLatin1Char('"') || s.front() == QLatin1Char('\'')) &&
        s.back() == s.front())
        s = s.mid(1, s.size() - 2);
    return s;
}

// A trailing ` # comment`, which a value's own `#` (a URL fragment) does not start.
QString withoutComment(const QString &s, QString *comment = nullptr) {
    static const QRegularExpression re(QStringLiteral(R"((^|\s)#\s*(.*)$)"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return s;
    if (comment) *comment = m.captured(2).trimmed();
    return s.left(m.capturedStart());
}

void addRuntime(Inventory *inv, const QString &name, const QString &version,
                const QString &file, int line) {
    QJsonObject row = baseRow("runtime", file, line);
    row[QStringLiteral("name")] = name;
    row[QStringLiteral("version")] = version;
    if (isExpression(version)) row[QStringLiteral("unresolved")] = true;
    inv->rows.append(row);
    inv->runtimesSeen.insert(name);
}

void addImage(Inventory *inv, const QString &ref, const QString &file, int line) {
    QJsonObject row = baseRow("container", file, line);
    row[QStringLiteral("image")] = ref;
    if (isExpression(ref)) {
        row[QStringLiteral("unresolved")] = true;
    } else {
        QString name = ref;
        const int at = name.indexOf(QLatin1Char('@'));
        if (at >= 0) {
            row[QStringLiteral("digest")] = name.mid(at + 1);
            name = name.left(at);
        }
        // A colon before the last slash is a registry's port, not a tag.
        const int colon = name.lastIndexOf(QLatin1Char(':'));
        if (colon > name.lastIndexOf(QLatin1Char('/')))
            row[QStringLiteral("tag")] = name.mid(colon + 1);
    }
    inv->rows.append(row);
}

void addUses(Inventory *inv, const QString &ref, const QString &comment,
             const QString &file, int line) {
    if (ref.startsWith(QLatin1String("./")) || ref.startsWith(QLatin1String("../"))) {
        QJsonObject row = baseRow("local_action", file, line);
        row[QStringLiteral("path")] = ref;
        inv->rows.append(row);
        return;
    }
    if (ref.startsWith(QLatin1String("docker://"))) {
        QJsonObject row = baseRow("docker_action", file, line);
        row[QStringLiteral("image")] = ref.mid(9);
        inv->rows.append(row);
        return;
    }
    QJsonObject row = baseRow("action", file, line);
    const int at = ref.indexOf(QLatin1Char('@'));
    const QString target = at >= 0 ? ref.left(at) : ref;
    const QStringList parts = target.split(QLatin1Char('/'));
    row[QStringLiteral("repo")] = parts.mid(0, 2).join(QLatin1Char('/'));
    if (parts.size() > 2)
        row[QStringLiteral("subpath")] = parts.mid(2).join(QLatin1Char('/'));
    row[QStringLiteral("ref")] = at >= 0 ? ref.mid(at + 1) : QString();
    if (!comment.isEmpty()) row[QStringLiteral("version_comment")] = comment;
    inv->rows.append(row);
}

void addRunner(Inventory *inv, const QString &label, const QString &file, int line) {
    QJsonObject row = baseRow("runner", file, line);
    row[QStringLiteral("label")] = label;
    if (isExpression(label)) row[QStringLiteral("unresolved")] = true;
    inv->rows.append(row);
}

void scanWorkflow(const QString &root, const QString &rel, Inventory *inv) {
    static const QRegularExpression usesRe(QStringLiteral(R"(^\s*(?:-\s*)?uses:\s*(.+)$)"));
    static const QRegularExpression runsOnRe(QStringLiteral(R"(^\s*runs-on:\s*(.*)$)"));
    static const QRegularExpression imageRe(QStringLiteral(R"(^\s*(?:-\s*)?(?:image|container):\s*(.+)$)"));
    static const QRegularExpression setupRe(
        QStringLiteral(R"(^\s*(node|python|go|java|ruby|dotnet|rust)-version:\s*(.+)$)"));
    static const QRegularExpression listItemRe(QStringLiteral(R"(^\s*-\s*(.+)$)"));

    const QStringList lines = readLines(root, rel, inv);
    bool inRunnerList = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        if (line.trimmed().startsWith(QLatin1Char('#'))) continue;
        if (inRunnerList) {
            const auto item = listItemRe.match(line);
            if (item.hasMatch()) {
                addRunner(inv, unquoted(withoutComment(item.captured(1))), rel, i + 1);
                continue;
            }
            inRunnerList = false;
        }
        QRegularExpressionMatch m;
        if ((m = usesRe.match(line)).hasMatch()) {
            QString comment;
            const QString ref = unquoted(withoutComment(m.captured(1), &comment));
            addUses(inv, ref, comment, rel, i + 1);
        } else if ((m = runsOnRe.match(line)).hasMatch()) {
            const QString value = withoutComment(m.captured(1)).trimmed();
            if (value.isEmpty()) {
                inRunnerList = true;
            } else if (value.startsWith(QLatin1Char('['))) {
                for (const QString &label : value.mid(1, value.size() - 2).split(QLatin1Char(',')))
                    addRunner(inv, unquoted(label), rel, i + 1);
            } else {
                addRunner(inv, unquoted(value), rel, i + 1);
            }
        } else if ((m = imageRe.match(line)).hasMatch()) {
            addImage(inv, unquoted(withoutComment(m.captured(1))), rel, i + 1);
        } else if ((m = setupRe.match(line)).hasMatch()) {
            addRuntime(inv, m.captured(1), unquoted(withoutComment(m.captured(2))), rel, i + 1);
        }
    }
}

void scanDockerfile(const QString &root, const QString &rel, Inventory *inv) {
    static const QRegularExpression fromRe(
        QStringLiteral(R"(^\s*FROM\s+(?:--platform=\S+\s+)?(\S+)(?:\s+AS\s+(\S+))?)"),
        QRegularExpression::CaseInsensitiveOption);
    const QStringList lines = readLines(root, rel, inv);
    QSet<QString> stages;
    for (int i = 0; i < lines.size(); ++i) {
        const auto m = fromRe.match(lines[i]);
        if (!m.hasMatch()) continue;
        const QString image = m.captured(1);
        if (image.compare(QLatin1String("scratch"), Qt::CaseInsensitive) != 0 &&
            !stages.contains(image.toLower()))
            addImage(inv, image, rel, i + 1);
        if (!m.captured(2).isEmpty()) stages.insert(m.captured(2).toLower());
    }
}

// One regex per file form, read from the first line matching it.
void scanByRegex(const QString &root, const QString &rel, const char *pattern,
                 const QString &name, Inventory *inv) {
    const QRegularExpression re(QString::fromLatin1(pattern));
    const QStringList lines = readLines(root, rel, inv);
    for (int i = 0; i < lines.size(); ++i) {
        const auto m = re.match(lines[i]);
        if (!m.hasMatch()) continue;
        addRuntime(inv, name, m.captured(1), rel, i + 1);
        return;
    }
}

void scanFirstLine(const QString &root, const QString &rel, const QString &name, Inventory *inv) {
    const QStringList lines = readLines(root, rel, inv);
    for (int i = 0; i < lines.size(); ++i) {
        const QString v = lines[i].trimmed();
        if (v.isEmpty() || v.startsWith(QLatin1Char('#'))) continue;
        addRuntime(inv, name, v, rel, i + 1);
        return;
    }
}

void scanToolVersions(const QString &root, const QString &rel, Inventory *inv) {
    const QStringList lines = readLines(root, rel, inv);
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = withoutComment(lines[i]).trimmed();
        const QStringList parts = t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2) addRuntime(inv, parts[0], parts[1], rel, i + 1);
    }
}

void scanPackageJson(const QString &root, const QString &rel, Inventory *inv) {
    const QStringList lines = readLines(root, rel, inv);
    const QString node = QJsonDocument::fromJson(lines.join(QLatin1Char('\n')).toUtf8())
                             .object().value(QStringLiteral("engines")).toObject()
                             .value(QStringLiteral("node")).toString();
    if (node.isEmpty()) return;
    int at = 0;
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].contains(QLatin1String("\"node\"")) && lines[i].contains(node)) at = i + 1;
    addRuntime(inv, QStringLiteral("node"), node, rel, at);
}

void scanRuntimeFile(const QString &root, const QString &rel, const QString &base, Inventory *inv) {
    if (base == QLatin1String(".nvmrc") || base == QLatin1String(".node-version"))
        scanFirstLine(root, rel, QStringLiteral("node"), inv);
    else if (base == QLatin1String(".python-version"))
        scanFirstLine(root, rel, QStringLiteral("python"), inv);
    else if (base == QLatin1String(".ruby-version"))
        scanFirstLine(root, rel, QStringLiteral("ruby"), inv);
    else if (base == QLatin1String(".tool-versions"))
        scanToolVersions(root, rel, inv);
    else if (base == QLatin1String("rust-toolchain"))
        scanFirstLine(root, rel, QStringLiteral("rust"), inv);
    else if (base == QLatin1String("rust-toolchain.toml"))
        scanByRegex(root, rel, R"re(^\s*channel\s*=\s*"([^"]+)")re", QStringLiteral("rust"), inv);
    else if (base == QLatin1String("go.mod"))
        scanByRegex(root, rel, R"(^go\s+(\S+))", QStringLiteral("go"), inv);
    else if (base == QLatin1String("pyproject.toml"))
        scanByRegex(root, rel, R"re(^\s*requires-python\s*=\s*"([^"]+)")re", QStringLiteral("python"), inv);
    else if (base == QLatin1String("Cargo.toml"))
        scanByRegex(root, rel, R"re(^\s*rust-version\s*=\s*"([^"]+)")re", QStringLiteral("rust"), inv);
    else if (base == QLatin1String("package.json"))
        scanPackageJson(root, rel, inv);
    else if (base == QLatin1String("CMakeLists.txt")) {
        scanByRegex(root, rel, R"(^\s*cmake_minimum_required\s*\(\s*VERSION\s+([0-9.]+))",
                    QStringLiteral("cmake"), inv);
        scanByRegex(root, rel, R"(^\s*find_package\s*\(\s*Qt[56]\s+([0-9.]+))",
                    QStringLiteral("qt"), inv);
    }
}

// A marker file says the runtime is used. With no version found anywhere, the row
// says so: the skill rejects a version guessed from a header, README or filename.
void addUnsourcedRuntimes(const QStringList &tracked, Inventory *inv) {
    static const QList<QPair<QString, QString>> markers = {
        {QStringLiteral("package.json"), QStringLiteral("node")},
        {QStringLiteral("pyproject.toml"), QStringLiteral("python")},
        {QStringLiteral("setup.py"), QStringLiteral("python")},
        {QStringLiteral("requirements.txt"), QStringLiteral("python")},
        {QStringLiteral("go.mod"), QStringLiteral("go")},
        {QStringLiteral("Cargo.toml"), QStringLiteral("rust")},
        {QStringLiteral("CMakeLists.txt"), QStringLiteral("cmake")},
    };
    for (const auto &[file, name] : markers) {
        if (inv->runtimesSeen.contains(name) || !tracked.contains(file)) continue;
        QJsonObject row = baseRow("runtime", file, 0);
        row[QStringLiteral("name")] = name;
        row[QStringLiteral("version")] = QJsonValue();
        row[QStringLiteral("no_authoritative_source")] = true;
        inv->rows.append(row);
        inv->runtimesSeen.insert(name);
    }
}

bool isVendored(const QString &path) {
    return path.startsWith(QLatin1String("third_party/")) ||
           path.startsWith(QLatin1String("vendor/")) ||
           path.contains(QLatin1String("node_modules/"));
}

}  // namespace

QJsonObject run(const QString &root, const QStringList &trackedFiles) {
    QJsonObject out;
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("not_covered")] = QJsonArray{
        QStringLiteral("library manifests and lock files: audit_run's scanners read those"),
        QStringLiteral("an image or version named in a script or Makefile"),
        QStringLiteral("third_party/, vendor/ and node_modules/"),
    };
    if (trackedFiles.isEmpty()) {
        out[QStringLiteral("rows")] = QJsonArray();
        out[QStringLiteral("skipped")] = QJsonArray{QStringLiteral("tracked_files")};
        out[QStringLiteral("hint")] = QStringLiteral(
            "git listed no tracked files, so nothing was read. This does not say the "
            "project pins nothing.");
        return out;
    }

    Inventory inv;
    static const QRegularExpression dockerRe(
        QStringLiteral(R"(^(?:Dockerfile|Containerfile)(?:\..+)?$|\.(?:Dockerfile|Containerfile)$)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString &path : trackedFiles) {
        if (isVendored(path)) continue;
        const QString base = path.section(QLatin1Char('/'), -1);
        if (path.startsWith(QLatin1String(".github/workflows/")) &&
            (base.endsWith(QLatin1String(".yml")) || base.endsWith(QLatin1String(".yaml"))))
            scanWorkflow(root, path, &inv);
        else if (dockerRe.match(base).hasMatch())
            scanDockerfile(root, path, &inv);
        else if (!path.contains(QLatin1Char('/')) || base == QLatin1String("CMakeLists.txt"))
            scanRuntimeFile(root, path, base, &inv);
    }
    addUnsourcedRuntimes(trackedFiles, &inv);

    QJsonObject counts;
    for (const QJsonValue &v : std::as_const(inv.rows)) {
        const QString kind = v.toObject().value(QStringLiteral("kind")).toString();
        counts[kind] = counts.value(kind).toInt() + 1;
    }
    out[QStringLiteral("rows")] = inv.rows;
    out[QStringLiteral("counts")] = counts;
    out[QStringLiteral("files_read")] = inv.filesRead;
    out[QStringLiteral("skipped")] = QJsonArray();
    return out;
}

}  // namespace DependencyInventory
