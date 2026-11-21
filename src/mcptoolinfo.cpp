#include "mcptoolinfo.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace mcp {

namespace {

// Every description string under one argument's schema, nested ones included.
void collectDescriptions(const QJsonValue &v, QString *out) {  // NOLINT(misc-no-recursion): walks this server's own tool schemas
    if (v.isArray()) {
        for (const QJsonValue &e : v.toArray())
            collectDescriptions(e, out);
        return;
    }
    const QJsonObject o = v.toObject();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        if (it.key() == QLatin1String("description") && it.value().isString())
            *out += it.value().toString() + QLatin1Char('\n');
        else
            collectDescriptions(it.value(), out);
    }
}

bool namesOp(const QString &text, const QString &op) {
    return text.contains(QLatin1Char('"') + op + QLatin1Char('"'));
}

bool namesAnyOp(const QString &text, const QStringList &ops) {
    return std::any_of(ops.begin(), ops.end(),
                       [&](const QString &o) { return namesOp(text, o); });
}

// The sentences of `text` in `op`'s runs, as the header defines a run.
QStringList opSentences(const QString &text, const QString &op,
                        const QStringList &ops) {
    static const QRegularExpression kSentenceEnd(QStringLiteral("(?<=\\.)\\s+"));
    QStringList out;
    bool inRun = false;
    for (const QString &s : text.split(kSentenceEnd, Qt::SkipEmptyParts)) {
        if (namesAnyOp(s, ops))
            inRun = namesOp(s, op);
        if (inRun)
            out << s;
    }
    return out;
}

QJsonObject refusal(const QString &code, const QString &error) {
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("code"), code},
                       {QStringLiteral("error"), error}};
}

}  // namespace

QJsonObject toolInfoForOp(const QJsonObject &descriptor, const QString &op) {
    const QString name = descriptor.value(QStringLiteral("name")).toString();
    const QJsonObject schema = descriptor.value(QStringLiteral("inputSchema")).toObject();
    const QJsonObject props = schema.value(QStringLiteral("properties")).toObject();
    const QJsonObject opProp = props.value(QStringLiteral("op")).toObject();
    QStringList ops;
    for (const QJsonValue &v : opProp.value(QStringLiteral("enum")).toArray())
        ops << v.toString();
    if (ops.isEmpty())
        return refusal(QStringLiteral("bad_args"),
            QStringLiteral("tool_info: %1 takes no `op`; omit it").arg(name));
    if (!ops.contains(op)) {
        QJsonObject env = refusal(QStringLiteral("unknown_op"),
            QStringLiteral("tool_info: %1 has no op \"%2\"").arg(name, op.left(64)));
        env[QStringLiteral("ops")] = QJsonArray::fromStringList(ops);
        return env;
    }

    QJsonObject kept;
    QJsonArray other;
    for (auto it = props.constBegin(); it != props.constEnd(); ++it) {
        if (it.key() == QLatin1String("op"))
            continue;
        QString text;
        collectDescriptions(it.value(), &text);
        if (namesOp(text, op) || !namesAnyOp(text, ops))
            kept.insert(it.key(), it.value());
        else
            other.append(it.key());
    }
    QJsonObject sliced = schema;
    sliced[QStringLiteral("properties")] = kept;
    if (schema.contains(QStringLiteral("required"))) {
        QJsonArray required;
        for (const QJsonValue &r : schema.value(QStringLiteral("required")).toArray())
            if (kept.contains(r.toString()))
                required.append(r);
        sliced[QStringLiteral("required")] = required;
    }

    const QStringList detail =
        opSentences(descriptor.value(QStringLiteral("detail")).toString(), op, ops)
        + opSentences(opProp.value(QStringLiteral("description")).toString(), op, ops);
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("name"), name},
        {QStringLiteral("op"), op},
        {QStringLiteral("description"), descriptor.value(QStringLiteral("description"))},
        {QStringLiteral("selection_hint"), descriptor.value(QStringLiteral("selection_hint"))},
        {QStringLiteral("op_detail"), detail.join(QLatin1Char(' '))},
        {QStringLiteral("inputSchema"), sliced},
        {QStringLiteral("other_arguments"), other},
    };
}

}  // namespace mcp
