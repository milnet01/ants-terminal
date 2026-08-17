// ANTS-1295 — central cwd-anchor validator. See header for contract.
// The body is the production-tested logic previously sitting at file
// scope in remotecontrol.cpp (ANTS-1250); promoted here so every
// path-accepting MCP/IPC handler calls a single chokepoint.

#include "pathvalidation.h"

#include <QChar>
#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QStringLiteral>

namespace PathValidation {

namespace {

bool hasControlOrBackslash(const QString &s) {
    for (QChar c : s) {
        if (c.unicode() < 0x20 || c == QLatin1Char('\\')) return true;
    }
    return false;
}

QJsonObject makeErr(const QString &toolName, const QString &paramName,
                    const QString &reason,
                    const QString &absPath = QString()) {
    QJsonObject o;
    o[QStringLiteral("ok")]    = false;
    o[QStringLiteral("error")] =
        QStringLiteral("%1: \"%2\" %3").arg(toolName, paramName, reason);
    o[QStringLiteral("code")]  = QStringLiteral("bad_path");
    // ANTS-4421 — name the call that WOULD work when the rejected path is a
    // cross-session feedback file. The refusal itself is correct and stays:
    // ANTS-3430's exception admits such a file only from an ANCESTOR of the
    // project root, and a session whose project lives elsewhere entirely
    // (~/.claude, whose parent is /home/ants, reading a report under
    // /mnt/Games/Scripts/Linux) is legitimately outside it. No widening can
    // serve that session, so what was missing was the route out.
    //
    // Every session on this machine carries a standing instruction to read
    // these files via Ants MCP, and the workflow is cross-project BY
    // CONSTRUCTION — the file never lives under the project it is about. The
    // reporting session found the working form (caller_cwd at the file's own
    // directory, plus a bare relative filename) by guessing; the envelope
    // already knows the path it rejected, so it can just say so. Same shape
    // as ANTS-4373's skipped_hint.
    //
    // The asymmetry that made this surprising is real and worth the words:
    // feedback_log takes an absolute path happily, so the WRITE verb works
    // where the READ verbs refuse.
    if (!absPath.isEmpty()
        && QFileInfo(absPath).fileName().endsWith(
               QStringLiteral("_Ants_MCP_Feedback.md"))) {
        const QString dir = QFileInfo(absPath).absolutePath();
        if (!dir.isEmpty())
            o[QStringLiteral("hint")] = QStringLiteral(
                "this is a cross-session feedback file outside your project "
                "root. Re-issue with caller_cwd:\"%1\" and path:\"%2\" (a "
                "bare relative filename). feedback_query / feedback_log "
                "accept the absolute path directly.")
                    .arg(dir, QFileInfo(absPath).fileName());
    }
    return o;
}

// ANTS-1837 — NFC-insensitive anchor test; defined below, declared here so
// isFeedbackFile (ANTS-3616) can reuse it rather than hand-roll the same
// prefix comparison.
bool anchoredUnder(const QString &candidate, const QString &root);

// ANTS-3430 — the conventional cross-session feedback file lives one level
// ABOVE the project root (<shared>/<proj>_Ants_MCP_Feedback.md). It is a
// world-readable shared coordination file that legitimately lives outside
// every project dir, so the general project-scoped verbs (read_region,
// file_outline, read_regions, workspace_search, apply_edits) are permitted
// to reach it. Supersedes ANTS-3419, which kept the bad_path refusal and
// only added a redirecting hint to feedback_query / feedback_log.
//
// ANTS-3616 — the exception is now anchored to a DIRECTORY as well as a
// basename suffix. It used to key on the suffix alone, with no anchoring
// of any kind, so it admitted the suffix at any depth from any directory:
// `../../../anywhere/X_Ants_MCP_Feedback.md` passed. Since apply_edits
// runs through this same chokepoint, that made "ends with
// _Ants_MCP_Feedback.md" a filesystem-wide WRITE primitive.
//
// The bound is "the file sits in an ANCESTOR directory of the project
// root" — not, as first written, "the root's immediate parent". The
// convention names the parent, but `rootCanonical` here is the caller's
// cwd (resolveRootCanonical returns rr.cwd; it does NOT walk up to a git
// root), so a session invoked from a SUBDIRECTORY of its project has a
// root one or more levels below the shared dir. A parent-only rule would
// lock exactly those sessions out of their own feedback file. The
// ancestor chain keeps them working while still excluding every sibling
// and unrelated tree — `<shared>/elsewhere/deep/` is not an ancestor of
// `<shared>/project`, so it stays refused.
//
// `absPath` must be the resolved/cleaned absolute path, NOT the caller's
// raw string: the directory test is meaningless on `../x.md`.
bool isFeedbackFile(const QString &absPath, const QString &rootCanonical) {
    if (!QFileInfo(absPath).fileName().endsWith(
            QStringLiteral("_Ants_MCP_Feedback.md")))
        return false;
    if (rootCanonical.isEmpty()) return false;   // fail closed, as elsewhere
    const QString dir = QFileInfo(absPath).absolutePath()
                            .normalized(QString::NormalizationForm_C);
    const QString root = rootCanonical.normalized(QString::NormalizationForm_C);
    if (dir.isEmpty()) return false;
    // `dir` must be root itself or one of its ancestors — i.e. root lies
    // at or beneath dir. anchoredUnder() is exactly that test, inverted.
    return anchoredUnder(root, dir);
}

// ANTS-1837 — NFC-insensitive anchor test. Both arguments are expected to
// be canonical or cleaned absolute paths. Normalise BOTH sides to NFC
// before comparing: `validatePath` already NFC-normalises its input, so
// without normalising the root too a decomposed (NFD) accented home dir
// false-rejects a composed (NFC) candidate that genuinely lives inside
// it. ASCII paths are unaffected (NFC == NFD == identity).
bool anchoredUnder(const QString &candidate, const QString &root) {
    const QString c = candidate.normalized(QString::NormalizationForm_C);
    const QString r = root.normalized(QString::NormalizationForm_C);
    return c == r || c.startsWith(r + QLatin1Char('/'));
}

}  // namespace

Check validatePath(const QString &rawPath,
                   const QString &rootCanonical,
                   const QString &toolName,
                   const QString &paramName,
                   bool allowOutsideRoot) {
    Check pc;
    if (rawPath.isEmpty()) return pc;

    const QString nfc = rawPath.normalized(QString::NormalizationForm_C);
    if (hasControlOrBackslash(nfc)) {
        pc.bad = true;
        pc.err = makeErr(toolName, paramName,
            QStringLiteral("contains control or backslash characters"));
        return pc;
    }

    QString joined;
    if (QFileInfo(nfc).isAbsolute()) {
        joined = nfc;
    } else {
        joined = QDir(rootCanonical).filePath(nfc);
    }
    const QString resolved = QFileInfo(joined).canonicalFilePath();

    // ANTS-1455 — allowOutsideRoot=true skips the project-root anchor
    // check but still applies the NFC + control-char + backslash +
    // canonicalisation pipeline above. `resolved` is still set when
    // the path canonicalises so callers can sanity-check existence.
    if (!allowOutsideRoot) {
        // ANTS-1805 — fail CLOSED on an empty root. Otherwise the anchor
        // check below collapses to `resolved.startsWith("/")`, which is true
        // for every absolute path — the single project-wide path chokepoint
        // would silently accept /etc/passwd if any one of its ~14 call-sites
        // ever forgot its own empty-root guard. Cheap defense-in-depth that
        // changes no current behaviour (all call-sites pass non-empty roots).
        if (rootCanonical.isEmpty()) {
            pc.bad = true;
            pc.err = makeErr(toolName, paramName,
                QStringLiteral("no project root to anchor against"));
            return pc;
        }
        // If the path canonicalises, the canonical form is the source of
        // truth — symlink escapes from inside the tree to outside get
        // caught. If it doesn't canonicalise (e.g. a git pathspec for a
        // deleted file), apply the lexical fallback so non-existent
        // escapes still reject.
        // ANTS-3430 — a cross-session feedback file may escape the root
        // (it lives one level above it by convention); permit exactly that
        // suffix and nothing else.
        if (!resolved.isEmpty()) {
            if (!anchoredUnder(resolved, rootCanonical)
                && !isFeedbackFile(resolved, rootCanonical)) {
                pc.bad = true;
                pc.err = makeErr(toolName, paramName,
                    QStringLiteral("escapes project root"), resolved);
                return pc;
            }
            pc.resolved = resolved;
        } else {
            const QString cleaned = QDir::cleanPath(joined);
            if (!anchoredUnder(cleaned, rootCanonical)
                && !isFeedbackFile(cleaned, rootCanonical)) {
                pc.bad = true;
                pc.err = makeErr(toolName, paramName,
                    QStringLiteral("escapes project root"), cleaned);
                return pc;
            }
        }
    } else {
        // allowOutsideRoot=true: caller accepts paths anywhere
        // readable. Still surface the canonical form when available.
        if (!resolved.isEmpty()) pc.resolved = resolved;
    }

    // ANTS-1250-INV-5: prefix `./` when leading char is `-` so the
    // value cannot be misread as a CLI flag when passed positionally.
    QString form = nfc;
    if (form.startsWith(QLatin1Char('-'))) {
        form = QStringLiteral("./") + form;
    }
    pc.argvForm = form;
    return pc;
}

bool isInsideProject(const QString &projectPath, const QString &candidate) {
    const QString rootCanon = QFileInfo(projectPath).canonicalFilePath();
    if (rootCanon.isEmpty()) return false;
    const QString candCanon = QFileInfo(candidate).canonicalFilePath();
    if (candCanon.isEmpty()) return false;
    return anchoredUnder(candCanon, rootCanon);
}

}  // namespace PathValidation
