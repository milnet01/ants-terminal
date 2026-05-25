// ANTS-1548: see changeloglog.h.

#include "changeloglog.h"

#include <QStringList>

namespace ChangelogLog {

namespace {
// Canonical Keep-a-Changelog category order (spec § sections).
const QStringList &canonicalCategories() {
    static const QStringList v = {
        QStringLiteral("Added"),   QStringLiteral("Changed"),
        QStringLiteral("Deprecated"), QStringLiteral("Removed"),
        QStringLiteral("Fixed"),   QStringLiteral("Security"),
    };
    return v;
}
}  // namespace

QString kindToCategory(const QString &kind) {
    // Added — net-new surface.
    if (kind == QLatin1String("feature") ||
        kind == QLatin1String("implement") ||
        kind == QLatin1String("enhancement")) {
        return QStringLiteral("Added");
    }
    // Fixed — bug / regression / audit / review fixes.
    if (kind == QLatin1String("fix") ||
        kind == QLatin1String("doc-fix") ||
        kind == QLatin1String("audit-fix") ||
        kind == QLatin1String("review-fix")) {
        return QStringLiteral("Fixed");
    }
    // Security — its own Keep-a-Changelog category.
    if (kind == QLatin1String("security")) {
        return QStringLiteral("Security");
    }
    // Everything else (refactor/perf/optimize/chore/test/doc/release/
    // package/marketing/ux/investigate/research/accessibility) is a
    // change to existing behaviour.
    return QStringLiteral("Changed");
}

bool isValidCategory(const QString &category) {
    return canonicalCategories().contains(category);
}

QString formatBullet(const QString &summary, const QString &body,
                     const QString &id) {
    QString head = summary.trimmed();
    QString out = QStringLiteral("- **") + head + QStringLiteral("**");
    if (!id.isEmpty()) {
        out += QStringLiteral(" (") + id.trimmed() + QStringLiteral(")");
    }
    if (!body.trimmed().isEmpty()) {
        const QStringList lines = body.split(QLatin1Char('\n'));
        for (const QString &ln : lines) {
            const QString t = ln.trimmed();
            out += QLatin1Char('\n');
            if (!t.isEmpty()) out += QStringLiteral("  ") + t;
        }
    }
    return out;
}

InsertResult insertUnreleasedEntry(const QString &markdown,
                                   const QString &category,
                                   const QString &bulletBlock) {
    InsertResult r;
    if (!isValidCategory(category)) {
        r.code = QStringLiteral("bad_category");
        r.error = QStringLiteral(
            "changelog_log: \"%1\" is not a Keep-a-Changelog category "
            "(Added/Changed/Deprecated/Removed/Fixed/Security)")
                .arg(category);
        return r;
    }

    QStringList lines = markdown.split(QLatin1Char('\n'));

    // 1. Locate `## [Unreleased]`.
    int unrel = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines.at(i).trimmed();
        if (t.compare(QStringLiteral("## [Unreleased]"),
                      Qt::CaseInsensitive) == 0) {
            unrel = i;
            break;
        }
    }
    if (unrel < 0) {
        r.code = QStringLiteral("not_unreleased");
        r.error = QStringLiteral(
            "changelog_log: no `## [Unreleased]` heading found — the "
            "CHANGELOG must follow Keep-a-Changelog with an Unreleased "
            "section at the top");
        return r;
    }

    // 2. Bound the Unreleased section [unrel+1, sectionEnd).
    int sectionEnd = lines.size();
    for (int i = unrel + 1; i < lines.size(); ++i) {
        if (lines.at(i).startsWith(QStringLiteral("## "))) {
            sectionEnd = i;
            break;
        }
    }

    // 3. Find the `### <category>` heading within the section, and
    //    record the first later-ordered category heading (for ordered
    //    creation when ours is absent).
    const int wantOrder = canonicalCategories().indexOf(category);
    int catHeading = -1;
    int laterHeading = -1;  // first ### whose order > wantOrder
    for (int i = unrel + 1; i < sectionEnd; ++i) {
        const QString t = lines.at(i).trimmed();
        if (!t.startsWith(QStringLiteral("### "))) continue;
        const QString name = t.mid(4).trimmed();
        if (name.compare(category, Qt::CaseInsensitive) == 0) {
            catHeading = i;
            break;
        }
        const int ord = canonicalCategories().indexOf(name);
        if (ord > wantOrder && laterHeading < 0) laterHeading = i;
    }

    const QStringList bulletLines = bulletBlock.split(QLatin1Char('\n'));

    if (catHeading >= 0) {
        // Insert at the top of the existing category: right after the
        // heading line and its single blank spacer (if present).
        int insertAt = catHeading + 1;
        if (insertAt < sectionEnd && lines.at(insertAt).trimmed().isEmpty())
            ++insertAt;
        QStringList block = bulletLines;
        block.append(QString());  // blank line after the new bullet
        for (int k = 0; k < block.size(); ++k)
            lines.insert(insertAt + k, block.at(k));
        r.ok = true;
        r.markdown = lines.join(QLatin1Char('\n'));
        r.line = insertAt + 1;
        return r;
    }

    // 4. Category heading absent — create it in canonical order.
    int headingAt = (laterHeading >= 0) ? laterHeading : sectionEnd;
    QStringList block;
    block.append(QStringLiteral("### ") + category);
    block.append(QString());
    block += bulletLines;
    block.append(QString());
    for (int k = 0; k < block.size(); ++k)
        lines.insert(headingAt + k, block.at(k));
    r.ok = true;
    r.created_category = true;
    r.markdown = lines.join(QLatin1Char('\n'));
    r.line = headingAt + 3;  // 1-based line of the inserted bullet
    return r;
}

}  // namespace ChangelogLog
