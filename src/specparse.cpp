// ANTS-3665 — see specparse.h. Moved verbatim from remotecontrol.cpp's
// anonymous namespace.

#include "specparse.h"

#include <QJsonArray>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QSet>

namespace SpecParse {

QJsonObject parseSpecBody(const QString &body) {
    QJsonObject out;
    QString title, status, kind;

    // Title: first line starting with `# <PREFIX>-NNNN — title` or
    // `# <PREFIX>-NNNN - title`. Tolerate either em-dash or hyphen.
    // ANTS-3356 — generalised the `ANTS-` prefix to any `<PREFIX>-NNNN`
    // (e.g. `# DOOM-0009 — Path tracer`) so non-Ants spec titles parse;
    // the id arm is non-greedy-anchored by the `-[0-9]+` + separator.
    static const QRegularExpression titleRe(
        QStringLiteral(R"(^#\s+[A-Za-z][A-Za-z0-9_-]*-[0-9]+\s*[—\-]\s*(.+?)\s*$)"),
        QRegularExpression::MultilineOption);
    const auto titleM = titleRe.match(body);
    if (titleM.hasMatch()) title = titleM.captured(1);

    // Metadata: `**Status:** ...` and `**Kind:** ...` lines.
    static const QRegularExpression statusRe(
        QStringLiteral(R"(^\*\*Status:\*\*\s*(.+?)\s*$)"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression kindRe(
        QStringLiteral(R"(^\*\*Kind:\*\*\s*(.+?)\s*$)"),
        QRegularExpression::MultilineOption);
    const auto statusM = statusRe.match(body);
    if (statusM.hasMatch()) status = statusM.captured(1);
    const auto kindM = kindRe.match(body);
    if (kindM.hasMatch()) kind = kindM.captured(1);

    out["title"]  = title;
    out["status"] = status;
    out["kind"]   = kind;

    // Locate the Invariants section. Accept `## N. Invariants`,
    // `## Invariants`, `### Invariants`, case-insensitive.
    QJsonArray invariants;
    static const QRegularExpression hdrRe(
        QStringLiteral(R"(^(#{2,3})\s+(?:\d+\.\s+)?[Ii]nvariants\b.*$)"),
        QRegularExpression::MultilineOption);
    const auto hdrM = hdrRe.match(body);
    if (hdrM.hasMatch()) {
        const int sectionStart = hdrM.capturedEnd();
        // Section ends at the next `## ` heading of equal-or-lower
        // depth (treat any subsequent `## ` as the boundary).
        static const QRegularExpression nextHdrRe(
            QStringLiteral(R"(^##\s+\S)"),
            QRegularExpression::MultilineOption);
        const auto nextM = nextHdrRe.match(body, sectionStart);
        const int sectionEnd =
            nextM.hasMatch() ? nextM.capturedStart() : body.size();
        const QString section = body.mid(sectionStart,
                                         sectionEnd - sectionStart);

        // (a) Table-form rows: `| INV-N | body | test_surface |`.
        static const QRegularExpression tableRe(
            QStringLiteral(R"(^\|\s*(INV-[0-9]+)\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|\s*$)"),
            QRegularExpression::MultilineOption);
        auto it = tableRe.globalMatch(section);
        while (it.hasNext()) {
            const auto m = it.next();
            QJsonObject inv;
            inv["id"]           = m.captured(1);
            inv["body"]         = m.captured(2);
            inv["test_surface"] = m.captured(3);
            invariants.append(inv);
        }

        // (b) Bullet-form: `- **INV-N** — body...` (multi-line until
        // next `- **INV-` or blank-line-plus-non-indent). Skip if
        // table form already matched (avoids dup).
        if (invariants.isEmpty()) {
            // Split on the bullet anchor — capture group keeps the
            // INV-N marker, then accumulate body lines until the
            // next anchor.
            static const QRegularExpression bulletStartRe(
                QStringLiteral(R"(^-\s+\*\*(INV-[0-9]+)[\.]?\*\*\s*[—\-:]?\s*)"),
                QRegularExpression::MultilineOption);
            auto bit = bulletStartRe.globalMatch(section);
            QList<QPair<QString, int>> starts;  // id, position-after-marker
            while (bit.hasNext()) {
                const auto m = bit.next();
                starts.append({m.captured(1), m.capturedEnd()});
            }
            for (int i = 0; i < starts.size(); ++i) {
                const int from = starts[i].second;
                const int to = (i + 1 < starts.size())
                                   ? starts[i + 1].first.startsWith(
                                         QStringLiteral("INV-"))
                                         ? section.lastIndexOf(
                                               QStringLiteral("\n- **INV-"),
                                               starts[i + 1].second - 1)
                                         : section.size()
                                   : section.size();
                const int end = to > from ? to : section.size();
                QString invBody = section.mid(from, end - from).trimmed();

                // ANTS-3665 — lift the `*Test:*` clause into its own field.
                // specs.md § 6 promises `test_surface` from BOTH forms, but
                // only the table branch above delivered it, so every
                // bullet-form spec — which is nearly all of them — returned
                // the clause buried in `body` and the field absent. ANTS-3662's
                // `invariant_no_test` check cannot be asked of a parser that
                // never extracts one.
                //
                // The clause ends at its PARAGRAPH, not at the end of the
                // invariant. Bullets here routinely carry further paragraphs
                // arguing why the invariant is shaped as it is; that prose is
                // about the invariant, not about how to test it. It stays in
                // `body`, which is why this splices the clause out rather than
                // truncating at it — nothing is lost either way.
                static const QRegularExpression testRe(
                    QStringLiteral(R"(\*Test:\*\s*)"));
                const auto testM = testRe.match(invBody);
                QString testSurface;
                if (testM.hasMatch()) {
                    const int clauseFrom = testM.capturedEnd();
                    const int para =
                        invBody.indexOf(QStringLiteral("\n\n"), clauseFrom);
                    const int clauseTo = para >= 0 ? para : invBody.size();
                    testSurface =
                        invBody.mid(clauseFrom, clauseTo - clauseFrom).trimmed();

                    QString head = invBody.left(testM.capturedStart()).trimmed();
                    const QString tail =
                        para >= 0 ? invBody.mid(clauseTo).trimmed() : QString();
                    if (!tail.isEmpty()) {
                        if (!head.isEmpty()) head += QStringLiteral("\n\n");
                        head += tail;
                    }
                    invBody = head;
                }

                QJsonObject inv;
                inv["id"]   = starts[i].first;
                inv["body"] = invBody;
                // Omitted, never empty-string: absence is what says "this
                // invariant has no test surface" (see specparse.h).
                if (!testSurface.isEmpty()) inv["test_surface"] = testSurface;
                invariants.append(inv);
            }
        }
    }

    // ANTS-3569 — surface invariants declared inline in prose (outside the
    // recognized table/bullet forms, e.g. `**Invariant (INV-N): ...**`) so a
    // caller trusting invariants_count knows the structured list may be
    // incomplete. Count distinct INV-N tokens present in the spec body but
    // absent from the structured list. Inline prose is not the sanctioned
    // form (specs.md § bullet form), so we hint rather than parse every shape.
    QSet<QString> structuredInvIds;
    for (const auto &v : invariants)
        structuredInvIds.insert(v.toObject().value(QStringLiteral("id")).toString());
    static const QRegularExpression invTokenRe(QStringLiteral("INV-[0-9]+"));
    QSet<QString> untabledInvIds;
    auto invTokIt = invTokenRe.globalMatch(body);
    while (invTokIt.hasNext()) {
        const QString id = invTokIt.next().captured(0);
        if (!structuredInvIds.contains(id)) untabledInvIds.insert(id);
    }

    out["invariants"]                   = invariants;
    out["invariants_count"]             = invariants.size();
    out["possible_untabled_invariants"] = untabledInvIds.size();
    return out;
}

}  // namespace SpecParse
