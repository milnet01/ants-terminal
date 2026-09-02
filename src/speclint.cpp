// ANTS-3662 — see speclint.h.

#include "speclint.h"

#include <algorithm>

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringView>
#include <QVector>

#include "markdownscan.h"
#include "specparse.h"

namespace SpecLint {
namespace {

// The two tombstone forms, given as literal anchored regexes because "begins
// with an italic marker" is not parser-decidable — it leaves open whether any
// leading italic run counts, whether the id is constrained, and whether the
// separator is an em dash or a hyphen (spec INV-3). `—` is U+2014.
//
// `*moved to <ID>*` is observed a dozen times in ANTS-3636. `*withdrawn — …*`
// had ZERO corpus occurrences when this shipped: it is introduced by the spec,
// because /write-spec sanctions withdrawal ("Withdraw or annotate; never
// renumber") while defining no syntax for it. Its fixture row exists so the
// invented form cannot ship unexercised.
bool isTombstone(const QString &body) {
    static const QRegularExpression movedRe(
        QStringLiteral(R"(^\*moved to ([A-Z]+-\d+)\*)"));
    static const QRegularExpression withdrawnRe(
        QStringLiteral(R"(^\*withdrawn — (.+?)\*)"));
    // ANTS-4351 — join the body onto one logical line before matching, which
    // is what a reader does. `.` does not cross a newline, so a hard-wrapped
    // tombstone was not exempt and came back as `invariant_no_test` — byte
    // for byte the finding a genuinely untested invariant gets. The natural
    // reading of that is "my vocabulary is wrong" rather than "my line
    // wrapping is wrong", and one reporting project spent three attempts on
    // it before this project reproduced it independently the same day.
    //
    // The anchors stay anchored at the START of the joined body, so this
    // widens the LAYOUT the forms accept and not the forms themselves: prose
    // that merely discusses a withdrawal is still a live invariant that owes
    // a test.
    static const QRegularExpression wsRun(QStringLiteral(R"(\s+)"));
    const QString b = body.trimmed().replace(wsRun, QStringLiteral(" "));
    return movedRe.match(b).hasMatch() || withdrawnRe.match(b).hasMatch();
}

// A fixed vocabulary, not a shape test: `QProcess` and `docs/standards/specs.md`
// are code spans too, and a rule like "contains a slash or a dash" fires on
// both (spec § 2.1).
const QSet<QString> &commandWords() {
    static const QSet<QString> v = {
        QStringLiteral("grep"),   QStringLiteral("rg"),      QStringLiteral("git"),
        QStringLiteral("ls"),     QStringLiteral("find"),    QStringLiteral("ctest"),
        QStringLiteral("cmake"),  QStringLiteral("ninja"),   QStringLiteral("make"),
        QStringLiteral("pytest"), QStringLiteral("python"),  QStringLiteral("python3"),
        QStringLiteral("node"),   QStringLiteral("npm"),     QStringLiteral("bash"),
        QStringLiteral("sh"),     QStringLiteral("sed"),     QStringLiteral("awk"),
        QStringLiteral("wc"),     QStringLiteral("cat"),     QStringLiteral("head"),
        QStringLiteral("tail"),   QStringLiteral("diff"),    QStringLiteral("jq"),
        QStringLiteral("curl"),   QStringLiteral("test"),
    };
    return v;
}

bool isCommandSpan(const QString &content) {
    QString s = content.trimmed();
    if (s.startsWith(QLatin1String("$ "))) s = s.mid(2).trimmed();
    const int sp = s.indexOf(QRegularExpression(QStringLiteral(R"(\s)")));
    return commandWords().contains(sp < 0 ? s : s.left(sp));
}

// True when the clause contains a command-shaped code span and no alphanumeric
// character follows the LAST such span — § 1e's "states nothing it should
// return", read literally. Trailing punctuation is not an expectation; a digit
// or letter is.
//
// Deliberately NOT keyed on the `→` arrow: that is the corpus's habit, not a
// documented convention (docs/standards/specs.md does not mention it), so a
// check keyed on it would report every clause written before the habit formed.
bool isCommandWithoutExpectation(const QString &clause) {
    if (clause.isEmpty()) return false;
    const QStringList lines = clause.split(QLatin1Char('\n'));
    const QVector<bool> noFence(lines.size(), false);

    // Flat offset of each line's first character, so a span's (line, col) can be
    // compared against the rest of the clause as one string.
    QVector<int> lineStart(lines.size(), 0);
    for (int i = 1; i < lines.size(); ++i)
        lineStart[i] = lineStart[i - 1] + lines[i - 1].size() + 1;

    int lastEnd = -1;
    const auto spans = MarkdownScan::codeSpans(lines, noFence);
    for (const auto &s : spans) {
        if (s.startLine < 0 || s.startLine >= lines.size()) continue;
        if (s.endLine < 0 || s.endLine >= lines.size()) continue;
        QString content;
        if (s.startLine == s.endLine) {
            content = lines[s.startLine].mid(s.startCol, s.endCol - s.startCol);
        } else {
            content = lines[s.startLine].mid(s.startCol);
            for (int l = s.startLine + 1; l < s.endLine; ++l)
                content += QLatin1Char(' ') + lines[l];
            content += QLatin1Char(' ') + lines[s.endLine].left(s.endCol);
        }
        if (isCommandSpan(content))
            lastEnd = lineStart[s.endLine] + s.endCol + s.delimLen;
    }
    if (lastEnd < 0) return false;

    const QString tail = clause.mid(lastEnd);
    for (const QChar &c : tail)
        if (c.isLetterOrNumber()) return false;
    return true;
}

// GFM row cells, honouring the `\|` escape — a spec that TABULATES table shapes
// writes one, and splitting naively turns its single cell into nine.
QStringList tableCells(const QString &line) {
    QString s = line.trimmed();
    if (s.startsWith(QLatin1Char('|'))) s = s.mid(1);
    if (s.endsWith(QLatin1Char('|')) &&
        !s.endsWith(QLatin1String("\\|")))
        s.chop(1);

    QStringList cells;
    QString cur;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\') && i + 1 < s.size() &&
            s.at(i + 1) == QLatin1Char('|')) {
            cur += QLatin1String("\\|");
            ++i;
        } else if (c == QLatin1Char('|')) {
            cells.append(cur.trimmed());
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells.append(cur.trimmed());
    return cells;
}

bool isTableRow(const QString &line) {
    const QString s = line.trimmed();
    return s.startsWith(QLatin1Char('|')) && s.size() > 1;
}

bool isDelimiterRow(const QString &line) {
    static const QRegularExpression re(
        QStringLiteral(R"(^\|?[\s:|-]*-[\s:|-]*\|?$)"));
    const QString s = line.trimmed();
    return isTableRow(line) && re.match(s).hasMatch();
}

// ANTS-4127 — the spec's own `**Status:**` reduced to the single word § 2.5's
// table keys on. THREE steps, not one, because the corpus needs all three:
// a value may open bold (`**considered / shelved (2026-07-19, user decision)`)
// and routinely carries trailing punctuation (`accepted (2026-05-27),`). The
// literal first word is `**considered` and `accepted,`, which match no row and
// fall to the catch-all — filing a CANDIDATE where a FINDING was required,
// silently, because the catch-all absorbs them.
//
// Truncating at the first character outside `[a-z0-9]` is also what makes "first
// word" implicit: no split is needed, and `v1 shipped 2026-05-13` yields `v1`.
QString statusWord(const QString &status) {
    QString s = status.trimmed();
    int lead = 0;
    while (lead < s.size() && s.at(lead) == QLatin1Char('*')) ++lead;
    s = s.mid(lead).toLower();
    int n = 0;
    while (n < s.size()) {
        const QChar c = s.at(n);
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
            (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            ++n;
        else
            break;
    }
    return s.left(n);
}

// The only two vocabularies § 2.5's table names. Everything else — including an
// absent Status, which is 53 specs of this corpus — falls to the catch-all and
// is a CANDIDATE. That row is the one doing the real work: an unrecognised word
// is the common case, not the exotic one, and it must never reach FINDING.
const QSet<QString> &shippedStatusWords() {
    static const QSet<QString> v = {
        QStringLiteral("shipped"), QStringLiteral("implemented"),
        QStringLiteral("v1"),
    };
    return v;
}

// Abandoned on purpose: the spec is skipped BEFORE either check, so it yields
// nothing at all and contributes nothing to `surfacesResolved`. Per-spec rather
// than per-check, or the Status-proof wiring check fires on work nobody intends
// to finish and reports it as drift forever.
const QSet<QString> &abandonedStatusWords() {
    static const QSet<QString> v = {
        QStringLiteral("superseded"), QStringLiteral("considered"),
    };
    return v;
}

QString collapseWs(const QString &s) {
    static const QRegularExpression ws(QStringLiteral(R"(\s+)"));
    return s.trimmed().replace(ws, QStringLiteral(" "));
}

// ANTS-4345 — the number prefix of a normalised heading, if it has one.
// `## 6. Tests` yes, `## Cold-eyes loop log` no. A required-section entry is
// matched EXACTLY when it carries a number and by NAME when it does not, which
// lets one matcher serve two standards: the global config repo fixes its
// numbering 1..12 and treats the number as part of a section's identity, while
// this project's standard never fixes numbers at all — its optional sections
// are inserted wherever they read best, shifting every heading after them.
bool headingIsNumbered(const QString &normalisedHeading) {
    static const QRegularExpression re(
        QStringLiteral(R"(^#{2,6}\s+\d+(?:\.\d+)*\.?\s+\S)"));
    return re.match(normalisedHeading).hasMatch();
}

// The name half of a normalised heading: `## 6. Tests` -> `Tests`.
// Returns the trimmed input unchanged when it does not look like a heading, so
// a malformed entry degrades to an exact comparison rather than to a wildcard.
QString sectionNameOf(const QString &normalisedHeading) {
    static const QRegularExpression re(
        QStringLiteral(R"(^#{2,6}\s+(?:\d+(?:\.\d+)*\.?\s+)?(\S.*)$)"));
    const auto m = re.match(normalisedHeading);
    return m.hasMatch() ? m.captured(1).trimmed() : normalisedHeading.trimmed();
}

}  // namespace

QSet<int> invariantNumbers(const QString &text) {
    static const QRegularExpression anchorRe(
        QStringLiteral(R"(^ {0,3}(?:-\s+\*\*|\|\s*)INV-([0-9]+)[a-z]?)"),
        QRegularExpression::MultilineOption);
    QSet<int> out;
    auto it = anchorRe.globalMatch(text);
    while (it.hasNext()) out.insert(it.next().captured(1).toInt());
    return out;
}

QStringList parseRequiredSections(const QString &standardText,
                                  bool *prefixMatch) {
    if (prefixMatch) *prefixMatch = false;
    const QStringList lines = standardText.split(QLatin1Char('\n'));
    // ANTS-4738 — the marker may carry `: prefix`, which asks for the numbered
    // entries to match on their `## N. Name` prefix so a trailing qualifier
    // passes. Verbatim stays the default: loosening it silently would weaken
    // every corpus already relying on the exact match.
    static const QRegularExpression markerRe(
        QStringLiteral(R"(<!--\s*required-sections(\s*:\s*prefix)?\s*-->)"));
    int marker = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const auto m = markerRe.match(lines[i]);
        if (m.hasMatch()) {
            marker = i;
            if (prefixMatch) *prefixMatch = m.capturedLength(1) > 0;
            break;
        }
    }
    if (marker < 0) return {};

    QStringList out;
    QChar closer;
    int   closerRun = 0;   // ANTS-4820 — the opener's run length
    for (int i = marker + 1; i < lines.size(); ++i) {
        if (closer.isNull()) {
            closer = MarkdownScan::fenceOpenerChar(lines[i], 3, &closerRun);
            // A non-blank line before the fence means the marker was not
            // introducing one: no block, so no list (the skip signal).
            if (closer.isNull() && !lines[i].trimmed().isEmpty()) return {};
            continue;
        }
        // ANTS-4820 — CommonMark § 4.5: a shorter run does not close.
        if (MarkdownScan::fenceCloses(lines[i], closer, closerRun)) break;
        const QString h = lines[i].trimmed();
        if (!h.isEmpty()) out.append(h);
    }
    return out;
}

Result check(const QString &text, const QString &relPath,
             const Options &opts) {
    Result r;
    const QStringList lines = text.split(QLatin1Char('\n'));
    r.lineCount = lines.size();
    if (r.lineCount > 0 && lines.last().isEmpty()) --r.lineCount;

    const QVector<bool> fence = MarkdownScan::fenceMask(lines);

    int emitted = 0;
    // ANTS-4127 — `extra` carries the per-kind detail (`invariant`, `surface`,
    // `spec_status`); it is empty for every kind that predates it, and
    // DocFinding::toJson emits nothing for an empty one.
    const auto add = [&](const QString &kind, int line, const QString &msg,
                         bool autoFixable = false,
                         const QJsonObject &extra = {}) {
        if (r.findings.size() >= opts.maxFindings) {
            r.truncated = true;
            return;
        }
        DocFinding::Finding f;
        f.verb          = QStringLiteral("spec_lint");
        f.kind          = kind;
        f.file          = relPath;
        f.line          = line;
        f.message       = msg;
        f.autoFixable   = autoFixable;
        f.extra         = extra;
        f.emissionIndex = emitted++;
        r.findings.append(f);
    };

    // --- headings (fence-aware) ------------------------------------------
    static const QRegularExpression headingRe(
        QStringLiteral(R"(^ {0,3}(#{1,6})\s+(\S.*?)\s*$)"));
    // The Invariants section's line range, mirroring parseSpecBody's own
    // boundaries (its heading regex, then the next `## `). The anchor scan below
    // is confined to it: an `- **INV-4**` written in § 7 as an EXAMPLE is prose,
    // and counting it would invent both an invariant and an id gap.
    //
    // ANTS-4115 — strict-first, then a heading whose text merely CONTAINS the
    // word (`## 5. Correctness invariants`). Mirroring parseSpecBody is the whole
    // point of this block: a lint whose section boundaries disagree with the
    // parser's reads one document and reports on another.
    static const QRegularExpression invHdrRe(
        QStringLiteral(R"(^ {0,3}#{2,3}\s+(?:\d+\.\s+)?[Ii]nvariants\b)"));
    static const QRegularExpression invHdrLooseRe(
        QStringLiteral(R"(^ {0,3}#{2,3}\s+(?:\d+\.\s+)?.*\b[Ii]nvariants\b)"));
    static const QRegularExpression nextH2Re(QStringLiteral(R"(^ {0,3}##\s+\S)"));
    int invHdrStrict = -1, invHdrLoose = -1;
    QStringList headingLines;   // normalised `## N. Name`
    for (int i = 0; i < lines.size(); ++i) {
        if (i < fence.size() && fence[i]) continue;
        const auto m = headingRe.match(lines[i]);
        if (m.hasMatch())
            headingLines.append(collapseWs(m.captured(1) + QLatin1Char(' ') +
                                           m.captured(2)));
        if (invHdrStrict < 0 && invHdrRe.match(lines[i]).hasMatch())
            invHdrStrict = i;
        if (invHdrLoose < 0 && invHdrLooseRe.match(lines[i]).hasMatch())
            invHdrLoose = i;
    }
    const int invHdrLine = invHdrStrict >= 0 ? invHdrStrict : invHdrLoose;
    int invStart = invHdrLine >= 0 ? invHdrLine + 1 : -1;
    int invEnd   = lines.size();
    for (int i = invStart; invStart >= 0 && i < lines.size(); ++i) {
        if (i < fence.size() && fence[i]) continue;
        if (nextH2Re.match(lines[i]).hasMatch()) { invEnd = i; break; }
    }

    // --- missing_section (gated on an injected list) ----------------------
    // `line:0` is document scope per ANTS-3664 § 2.1, which is exactly what
    // "this document lacks a section" means — the heading scan has no entry for
    // a heading that is absent.
    if (!opts.requiredSections.isEmpty()) {
        r.sectionsChecked = true;
        // ANTS-4739 — a document may exempt itself, mirroring the
        // `invariant-id-base` opt-out above and doc_integrity's
        // suppressed-heading case. A mixed corpus otherwise reports the same
        // permanent rows forever: specs written before the project adopted its
        // section run, plus build plans and ledgers that are not specs at all
        // but live in the specs dir. A reader re-triages that residue on every
        // run to find the few rows that are new, which is the failure the check
        // exists to prevent — noise is where a real finding hides.
        //
        // Outside fenced code, so an example quoted in prose cannot exempt the
        // document quoting it.
        static const QRegularExpression exemptRe(
            QStringLiteral(R"(^ {0,3}<!--\s*spec-lint:\s*no-required-sections\s*-->\s*$)"));
        for (int i = 0; i < lines.size(); ++i) {
            if (i < fence.size() && fence[i]) continue;
            if (exemptRe.match(lines[i]).hasMatch()) { r.sectionsExempt = true; break; }
        }
        const QSet<QString> present(headingLines.begin(), headingLines.end());
        // ANTS-4345 — a name-keyed index beside the exact one, so an entry
        // written without a number matches whatever number the document
        // carries. Built once, not per required entry.
        QSet<QString> presentNames;
        presentNames.reserve(headingLines.size());
        for (const QString &h : headingLines) presentNames.insert(sectionNameOf(h));

        // ANTS-4738 — under prefix matching a numbered entry is satisfied by a
        // heading that starts with it and continues with a qualifier. The
        // boundary check is what keeps `## 6. Test` from matching `## 6. Tests`.
        const auto numberedFound = [&](const QString &want) {
            if (present.contains(want)) return true;
            if (!opts.sectionsPrefixMatch) return false;
            for (const QString &h : headingLines) {
                if (h.size() <= want.size() || !h.startsWith(want)) continue;
                if (!h.at(want.size()).isLetterOrNumber()) return true;
            }
            return false;
        };
        for (const QString &req : opts.requiredSections) {
            const QString want = collapseWs(req);
            const bool found = headingIsNumbered(want)
                                   ? numberedFound(want)
                                   : presentNames.contains(sectionNameOf(want));
            if (!found && !r.sectionsExempt)
                add(QStringLiteral("missing_section"), 0,
                    QStringLiteral("required section is absent: %1").arg(want));
        }
    }

    // --- INV-N anchor lines ----------------------------------------------
    // parseSpecBody supplies neither line numbers nor sections nor loop-log
    // rows, so every `line` on this verb's findings comes from this scan.
    // ANTS-4107 — `[a-z]?` so a sub-lettered id anchors like any other.
    static const QRegularExpression bulletAnchorRe(
        QStringLiteral(R"(^ {0,3}-\s+\*\*(INV-\d+[a-z]?)\.?\*\*)"));
    static const QRegularExpression rowAnchorRe(
        QStringLiteral(R"(^ {0,3}\|\s*(INV-\d+[a-z]?)\s*\|)"));
    QHash<QString, int> anchorLine;   // id -> 1-based line, first occurrence
    for (int i = qMax(invStart, 0); invStart >= 0 && i < invEnd; ++i) {
        if (i < fence.size() && fence[i]) continue;
        auto m = bulletAnchorRe.match(lines[i]);
        if (!m.hasMatch()) m = rowAnchorRe.match(lines[i]);
        if (m.hasMatch() && !anchorLine.contains(m.captured(1)))
            anchorLine.insert(m.captured(1), i + 1);
    }

    // --- invariant_no_test / command_test_no_expectation -------------------
    // The ANCHOR SCAN is the authority on which invariants EXIST; the parser is
    // the authority on what each one says. They are separate because the
    // parser's table branch requires three non-empty cells, so a row whose
    // test-surface cell is empty — the exact defect `invariant_no_test` names —
    // does not match and is returned by nothing. Trusting the parser's list
    // alone would therefore make the check blind in the table form AND invent an
    // id gap where the dropped row was (INV-2, INV-3).
    const QJsonObject parsed = SpecParse::parseSpecBody(text);
    QHash<QString, QJsonObject> parsedById;
    for (const auto &v : parsed.value(QStringLiteral("invariants")).toArray()) {
        const QJsonObject o = v.toObject();
        parsedById.insert(o.value(QStringLiteral("id")).toString(), o);
    }

    QSet<QString> allIds;
    for (auto it = anchorLine.constBegin(); it != anchorLine.constEnd(); ++it)
        allIds.insert(it.key());
    for (auto it = parsedById.constBegin(); it != parsedById.constEnd(); ++it)
        allIds.insert(it.key());

    // ANTS-4107 — the checks below walk the ID STRINGS; only the gap scan
    // reduces them to numbers. Rebuilding `INV-%1` from an int silently dropped
    // every sub-lettered invariant from every check, so one could ship with no
    // test surface at all and pass the lint — and a /cold-eyes split into 3 and
    // 3b is exactly how a sub-lettered id comes to exist, which put the blind
    // spot precisely where a contract had just been divided in two.
    struct InvId {
        int     n;
        QString letter;
        QString id;
    };
    static const QRegularExpression invIdRe(
        QStringLiteral(R"(^INV-([0-9]+)([a-z]?)$)"));
    QVector<InvId> ids;
    for (const QString &id : allIds) {
        const auto m = invIdRe.match(id);
        if (!m.hasMatch()) continue;
        ids.append({m.captured(1).toInt(), m.captured(2), id});
    }
    std::sort(ids.begin(), ids.end(), [](const InvId &a, const InvId &b) {
        return a.n != b.n ? a.n < b.n : a.letter < b.letter;
    });

    // --- ANTS-4127: test-surface resolution --------------------------------
    // The same absent directory is a defect or a forward reference depending on
    // whether the spec claims to have shipped, so the bucket is chosen by the
    // document's OWN Status — a claim it makes about itself, which is why only a
    // confidently-shipped word yields a FINDING (spec § 2.5).
    const QString docStatus =
        statusWord(parsed.value(QStringLiteral("status")).toString());
    const QJsonValue statusJson =
        docStatus.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(docStatus);

    // Reported as the injected state, not as a per-document outcome: false
    // EXACTLY when the verb layer supplied nothing, which is the one state where
    // neither check ran. A `superseded` spec still sets it true — nothing was
    // checked in that document, but resolution was on (INV-10 keys on the set,
    // INV-5 on the counter and the findings).
    r.surfacesChecked = !opts.existingTestDirs.isEmpty();
    const bool resolveSurfaces =
        r.surfacesChecked && !abandonedStatusWords().contains(docStatus);

    // Harvest only the one form the corpus uses at scale. The trailing
    // `(?!\w|\*)` is the whole of the wildcard rule: without it
    // `tests/features/audit_*` reads as a directory named `audit_` and is
    // reported absent, manufacturing a finding against a spec that correctly
    // named a FAMILY of tests. The measurement in this spec's own § 1 made that
    // mistake, which is why the extractor must not.
    static const QRegularExpression surfaceRe(
        QStringLiteral(R"(tests/features/([a-z0-9]+(?:_[a-z0-9]+)*)(?!\w|\*))"));

    // Per DOCUMENT, not per clause (§ 2.3): two invariants naming one directory
    // are one surface, so they count once and report once.
    QSet<QString> seenSurfaces;

    for (const InvId &e : ids) {
        const QString     id = e.id;
        const QJsonObject o  = parsedById.value(id);
        const int         line = anchorLine.value(id, 0);

        // A withdrawn or moved invariant has nothing left to test. Without the
        // exemption this check fires against every tombstone in the corpus —
        // docs/specs/ANTS-3663.md carries two today.
        if (isTombstone(o.value(QStringLiteral("body")).toString())) continue;

        // Blank counts as absent, and that is not defensive: the bullet branch
        // OMITS the key when there is no clause, while the table branch's
        // three-cell regex captures a whitespace-only cell as a present-but-
        // blank surface. Keying on the key's presence alone would therefore
        // report the bullet form and silently pass the table form.
        const QString clause =
            o.value(QStringLiteral("test_surface")).toString().trimmed();
        if (clause.isEmpty()) {
            add(QStringLiteral("invariant_no_test"), line,
                QStringLiteral("%1 carries no test-surface clause").arg(id));
            continue;
        }

        // A CANDIDATE, never a verdict: "is this clause a command?" is a
        // heuristic, and a manual recipe or a bare test-file path is a
        // legitimate surface with nothing to state. Never autoFixable, and this
        // engine runs no subprocess — /write-spec Step 3 owns executing a
        // clause, at write time, where a failure is free (INV-4).
        //
        // Execution splits by artefact, and only one half moved (ANTS-4108):
        // a PATTERN stated as a ```regex pcre2 fence with an | input |
        // expected | table beside it is run by the spec_conformance verb,
        // which needs no subprocess because applying a pattern to a string
        // cannot reach the filesystem. A COMMAND clause — this branch — still
        // has no runtime owner but /write-spec Step 3, and fenced FIXTURES have
        // none at all — permanently. ANTS-4127 was where that was to be settled
        // and settled it the other way (user decision, 2026-08-12): `docs/`
        // carries one `python` fence against 442 illustrative `cpp` ones, so an
        // interpreter and a sandbox would be built for a defect class this
        // corpus cannot exhibit. There is no follow-up id.
        if (isCommandWithoutExpectation(clause))
            add(QStringLiteral("command_test_no_expectation"), line,
                QStringLiteral("%1's test clause is a command but states "
                               "nothing it should return (candidate)").arg(id));

        // ANTS-4127 — resolve what the clause NAMES. The clause is the parser's
        // joined `test_surface`, so a bullet that hard-wraps its path onto a
        // continuation line is harvested whole; scanning line-wise here would
        // miss most of what the corpus actually writes.
        if (!resolveSurfaces) continue;
        auto sit = surfaceRe.globalMatch(clause);
        while (sit.hasNext()) {
            const QString name = sit.next().captured(1);
            if (seenSurfaces.contains(name)) continue;
            seenSurfaces.insert(name);

            QJsonObject extra;
            extra[QStringLiteral("invariant")]   = id;
            extra[QStringLiteral("surface")]     =
                QStringLiteral("tests/features/") + name;
            extra[QStringLiteral("spec_status")] = statusJson;

            if (!opts.existingTestDirs.contains(name)) {
                const bool shipped = shippedStatusWords().contains(docStatus);
                add(shipped ? QStringLiteral("test_surface_absent")
                            : QStringLiteral("test_surface_unresolved"),
                    line,
                    shipped
                        ? QStringLiteral("%1 names tests/features/%2, which does "
                                         "not exist").arg(id, name)
                        : QStringLiteral("%1 names tests/features/%2, which does "
                                         "not exist (candidate)").arg(id, name),
                    false, extra);
                continue;
            }
            ++r.surfacesResolved;

            // A directory that exists can still hold a test that never runs —
            // the trap CLAUDE.md documents as recurring. Its own kind, not
            // `test_surface_absent`: the directory is right there, and a
            // consumer told "absent" goes hunting for a missing one.
            //
            // Skipped wholesale on an empty `wiredTestDirs`, which is what an
            // unreadable or moved CMakeLists.txt produces. Without that, one
            // failed file read files a FINDING against every resolved surface
            // in the corpus.
            if (!opts.wiredTestDirs.isEmpty() &&
                !opts.wiredTestDirs.contains(name))
                add(QStringLiteral("test_surface_unwired"), line,
                    QStringLiteral("%1 names tests/features/%2, which exists but "
                                   "has no test source in any bundle").arg(id, name),
                    false, extra);
        }
    }

    // --- invariant_id_gap --------------------------------------------------
    // The id set includes tombstones. Excluding them first is the mistake this
    // check guards against: it would report every moved or withdrawn invariant
    // as a gap (INV-3).
    if (!ids.isEmpty()) {
        QSet<int> have;
        for (const InvId &e : ids) have.insert(e.n);
        // From the document's OWN MINIMUM, not from 1. A spec whose sequence
        // legitimately starts above 1 — docs/specs/ANTS-1358.md opens at INV-14
        // because it continues an earlier spec's numbering — has an offset
        // origin, not thirteen gaps. Anchoring at 1 reported exactly that
        // against the corpus (measured: it was most of the 109 id-gap findings
        // in the first calibration run), and § 1e asks for gaps in "a doc's own
        // numbered ids".
        int lo = ids.first().n, hi = ids.first().n;
        for (const InvId &e : ids) { lo = qMin(lo, e.n); hi = qMax(hi, e.n); }
        // ANTS-3784 — a floor the DOCUMENT declares, for a gap it skipped on
        // purpose. ANTS-4110's sibling set answers the same question but is
        // corpus-wide and all-or-nothing: one number shared by two specs
        // anywhere turns it off for every document. A corpus that numbers
        // per-document EXCEPT for one family therefore has no route to it, and
        // that is this project — measured 2026-08-15, ANTS-3782 still reported
        // eleven gaps (INV-15..25, owned by ANTS-3756) out of the corpus's 39,
        // in the bucket review-contract feeds straight into its verified list.
        //
        // Per-document because the fact is: only this file knows its numbering
        // continues someone else's. A number BELOW the floor is not a gap; a
        // number the document does own below its own floor is still a real
        // anchor and is untouched. Read outside fenced code, so a document
        // DOCUMENTING the syntax does not accidentally declare one.
        int idBase = 0;
        {
            static const QRegularExpression baseRe(QStringLiteral(
                R"(^ {0,3}<!--\s*invariant-id-base:\s*([0-9]+)\s*-->\s*$)"));
            for (int i = 0; i < lines.size(); ++i) {
                if (i < fence.size() && fence.at(i)) continue;
                const auto m = baseRe.match(lines.at(i));
                if (m.hasMatch()) { idBase = m.captured(1).toInt(); break; }
            }
        }
        for (int n = lo; n <= hi; ++n) {
            if (have.contains(n)) continue;
            if (n < idBase) {
                ++r.idGapsSuppressed;
                continue;
            }
            // ANTS-4110 — a number a SIBLING spec owns is not a gap. On a
            // project that numbers invariants once across the corpus, every id
            // living in a neighbouring document reads as one here, and the two
            // repairs the finding invites are both forbidden by the standard it
            // is meant to enforce: renumber (ids are permanent) or tombstone ids
            // that were never in this file. Suppressed rather than filtered
            // afterwards, so the count below is the whole story.
            if (opts.siblingInvNumbers.contains(n)) {
                ++r.idGapsSuppressed;
                continue;
            }
            // A missing invariant has no line of its own, so the finding
            // anchors on the bullet FOLLOWING the gap.
            int line = 0;
            for (const InvId &e : ids) {
                if (e.n <= n) continue;
                const int cand = anchorLine.value(e.id, 0);
                if (cand > 0) { line = cand; break; }
            }
            // ANTS-3684 — a CANDIDATE, not a verdict. A gap is evidence, and
            // the residual population after the origin fix (ANTS-3662's
            // document-minimum anchor) and the sibling-corpus set (ANTS-4110)
            // is one legitimate class: a spec carrying a SUBSET of a parent
            // spec's invariants, keeping the parent's ids. Renumbering those
            // would break the citation, which the spec-format standard
            // forbids — so the document is correct and the gap is real.
            //
            // Neither existing opt-out reaches it. `invariant-id-base` sets a
            // FLOOR, and this class has holes in the interior. The message
            // names the cause so a reader triages in one read rather than
            // opening the parent spec to work out why.
            add(QStringLiteral("invariant_id_gap"), line,
                QStringLiteral("INV-%1 is missing from the id sequence with no "
                               "tombstone (candidate — a spec carrying a subset "
                               "of a parent's invariants keeps the parent's ids, "
                               "and renumbering would break the citation)")
                    .arg(n));
        }
    }

    // --- loop_row_no_outcome ----------------------------------------------
    // A loop log is a GFM table whose FIRST HEADER CELL is exactly `Loop`; the
    // outcome is the row's LAST cell. Four incompatible column layouts are in
    // use across this corpus (spec § 2.2) and the prose column is last in all
    // four, so position matches every shape where a column NAME matches one.
    for (int i = 0; i + 1 < lines.size(); ++i) {
        if (i < fence.size() && fence[i]) continue;
        if (!isTableRow(lines[i]) || isDelimiterRow(lines[i])) continue;
        if (!isDelimiterRow(lines[i + 1])) continue;
        const QStringList hdr = tableCells(lines[i]);
        if (hdr.isEmpty() || hdr.first() != QLatin1String("Loop")) continue;

        for (int j = i + 2; j < lines.size(); ++j) {
            if (j < fence.size() && fence[j]) break;
            if (!isTableRow(lines[j])) break;
            const QStringList cells = tableCells(lines[j]);
            if (cells.size() < 2 || !cells.last().isEmpty()) continue;
            add(QStringLiteral("loop_row_no_outcome"), j + 1,
                QStringLiteral("cold-eyes loop row %1 records no outcome")
                    .arg(cells.first()));
        }
    }

    // --- ANTS-4623: § Invariants <-> § Tests parity -----------------------
    // The two invariant checks above ask whether an invariant declares a test.
    // Neither asks the other direction: whether the ids the Tests section
    // claims to cover are the ids § Invariants declares. The two sections
    // restate one set in different words and share no token to grep for, so
    // they can disagree indefinitely — which is why a citation search cannot
    // find this class and a check has to.
    {
        static const QRegularExpression testsHdrRe(
            QStringLiteral(R"(^ {0,3}##\s+(?:\d+\.\s+)?[Tt]ests\b)"));
        static const QRegularExpression testsHdrLooseRe(
            QStringLiteral(R"(^ {0,3}##\s+(?:\d+\.\s+)?[^\n]*\b[Tt]ests\b)"));
        // Strict before loose, the same order the Invariants header uses and
        // for the same reason: a document carrying both a real Tests section
        // and a heading that merely mentions tests must take the real one.
        int hdr = -1;
        for (int i = 0; i < lines.size() && hdr < 0; ++i) {
            if (i < fence.size() && fence[i]) continue;
            if (testsHdrRe.match(lines[i]).hasMatch()) hdr = i;
        }
        for (int i = 0; i < lines.size() && hdr < 0; ++i) {
            if (i < fence.size() && fence[i]) continue;
            if (testsHdrLooseRe.match(lines[i]).hasMatch()) hdr = i;
        }

        // No Tests section: the comparison has one half. SKIP rather than
        // report every invariant as uncovered — the contract `sectionsChecked`
        // already sets, and the difference between the two states is what
        // `testCoverageChecked` exists to carry.
        if (hdr >= 0 && !ids.isEmpty()) {
            r.testCoverageChecked = true;
            int end = lines.size();
            for (int i = hdr + 1; i < lines.size(); ++i) {
                if (i < fence.size() && fence[i]) continue;
                if (nextH2Re.match(lines[i]).hasMatch()) { end = i; break; }
            }
            QString section;
            for (int i = hdr + 1; i < end; ++i)
                section += lines[i] + QLatin1Char('\n');

            // The second operand's `INV-` prefix is OPTIONAL, and that is not
            // tidiness: `INV-1..7` is how this corpus most often writes a
            // range, and requiring the prefix on both ends made every id
            // inside such a range read as uncovered. It was the single
            // largest false-positive class in the pre-ship measurement.
            //
            // A RANGE defeats the comparison rather than answering it. The
            // cheaper rule of the two the item offered: say so, and report no
            // gaps at all for this document. Expanding the range would mean
            // guessing which ids the author meant; reporting gaps ALONGSIDE it
            // would be worse than silence, since every id inside the range
            // would read as uncovered.
            // Two shapes, one meaning: notation this comparison cannot expand.
            //
            //   INV-1..INV-9   a fully-qualified range
            //   INV-1..7       a range whose second operand is BARE
            //   INV-1/2        a slash-joined pair
            //   INV-3, 4 and 5 a comma/and list continuing with bare numbers
            //
            // The trigger for the second shape is a BARE continuation, which
            // is what makes it undecidable: `INV-1, INV-2` names two ids and
            // is perfectly comparable, so it is not caught here and must not
            // be. Every corpus form found in the pre-ship measurement is one
            // of these two.
            static const QRegularExpression rangeRe(QStringLiteral(
                R"(INV-[0-9]+[a-z]?\s*(?:\.\.\.?|\x{2013}|\x{2014}|-{1,2}|to)\s*INV-[0-9]+[a-z]?)"
                R"(|INV-[0-9]+[a-z]?\s*(?:\.\.\.?|\x{2013}|\x{2014}|-{1,2}|/|,|\band\b|\bto\b)\s*[0-9]+[a-z]?\b)"));
            const auto rangeM = rangeRe.match(section);
            if (rangeM.hasMatch()) {
                add(QStringLiteral("test_coverage_unverifiable"), hdr + 1,
                    QStringLiteral("the Tests section states coverage as a "
                                   "range (%1), which cannot be compared "
                                   "against the declared ids (candidate)")
                        .arg(rangeM.captured(0)));
            } else {
                static const QRegularExpression namedRe(
                    QStringLiteral(R"(INV-[0-9]+[a-z]?(?![A-Za-z0-9]))"));
                QSet<QString> named;
                auto nit = namedRe.globalMatch(section);
                while (nit.hasNext()) named.insert(nit.next().captured(0));

                // A Tests section naming NO id is not partial coverage — it is
                // a document that does not use per-id notation at all, and
                // comparing against it condemns every invariant it declares.
                // That is the rule `Options` above already states for its own
                // injected sets: empty means SKIP, because a check against an
                // empty set condemns everything it reads.
                //
                // Measured before the gate: 382 findings over this corpus,
                // 16.9% of all invariant anchors, and the sampled ones were
                // TRUE — ANTS-1120's Tests section says the bullet ships no
                // code and names nothing. True and useless is still noise, and
                // noise is where a real finding hides.
                //
                // What survives is the defect the item described: a spec that
                // names SOME ids and misses others.
                if (named.isEmpty()) {
                    r.testCoverageChecked = false;
                } else
                for (const InvId &e : ids) {
                    if (named.contains(e.id)) continue;
                    // A withdrawn or moved invariant has nothing left to
                    // test, so the Tests section is right not to name it —
                    // the exemption both invariant checks above already make.
                    const QJsonObject o = parsedById.value(e.id);
                    if (isTombstone(o.value(QStringLiteral("body")).toString()))
                        continue;
                    // A CANDIDATE, never a verdict: a spec may legitimately
                    // leave an invariant to a surface the Tests section does
                    // not enumerate. Never autoFixable — nothing here knows
                    // what the missing test would assert.
                    add(QStringLiteral("test_coverage_gap"),
                        anchorLine.value(e.id, hdr + 1),
                        QStringLiteral("%1 is declared but the Tests section "
                                       "names no coverage for it (candidate)")
                            .arg(e.id));
                }
            }
        }
    }

    return r;
}

}  // namespace SpecLint
