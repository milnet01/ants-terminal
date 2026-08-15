// ANTS-3833 TU 1/12 — Dispatcher and shared helpers.
//
// ANTS-4125 — include list swept 2026-08-12. The ANTS-3833 decomposition moved
// most verb bodies out to the sibling remotecontrol_*.cpp TUs but left their
// engine headers behind here: 48 project headers (plus a stray <cmath>) named
// nothing this TU uses, leaving the 13 that do. Add an
// include only when THIS TU names the symbol; a verb body that lives in a
// sibling takes its header with it.
#include "remotecontrol.h"
#include "remotecontrol_internal.h"  // ANTS-3833 — shared rcdetail helpers
#include "mcpspill.h"        // ANTS-2094 — read_spill
#include "mainwindow.h"
#include "mcpprojection.h"
#include "projectsettings.h"    // ANTS-2160 — .ants/project.json overrides
#include "resolvedroot.h"
#include "roadmapdialog.h"
#include "roadmapindex.h"
#include "markdownscan.h"    // ANTS-4404 — fence extents, shared with the walk
#include "verifyengine.h"
#include "verifytrust.h"
#include "debuglog.h"
#include "secureio.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDate>
#include <QSaveFile>
// ANTS-2049 — e2e inject verbs: synthetic Qt event posting + widget grab.
#include <QApplication>
#include <QWidget>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <algorithm>
#include <QHash>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QCollator>
#include <QLocale>
#include <QSet>
#include <QStandardPaths>
#include <QTabWidget>
#include <QThread>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QCryptographicHash>
#include <QScopeGuard>
#include <QPointer>
#include <QTimer>

// safeToUnlinkLocalSocket lives in secureio.h as of ANTS-1132 (0.7.66)
// so the Claude hook + MCP server start paths can share the same
// helper. The file-scope static here was unified with that lift.

using namespace rcdetail;  // ANTS-3833

namespace rcdetail {
// Forward decl for early callers (ANTS-1347 cmdLaunch / cmdNewTab,
// post-bundle-A). Definition lives in the rcdetail namespace below
// next to the rest of the git_state helpers. Every rcdetail block in
// this TU names the same namespace, so this forward decl resolves at
// the same `resolveRootCanonical` symbol (ANTS-3833).
QString resolveRootCanonical(MainWindow *main);
// ANTS-1391 — read-verb overload: prefer caller_cwd in the request
// body over the focused-tab default. Definition next to the legacy
// one below.
QString resolveRootCanonical(MainWindow *main, const QJsonObject &req);

// ANTS-1459 — shared path list for ROADMAP.md discovery under a
// project root. roadmap_query and roadmap_log both call this helper
// so we don't duplicate the path-widening list (and so neither
// function body trips the per-function-size regression guard in
// tests/features/mcp_roadmap_unrecognised_format/).
//
// Common subdirs probed:
//   ./, docs/, docs/private/, docs/internal/, .github/
// Surfaced by a RetroArch CC session 2026-05-17 where the project
// kept its roadmap at docs/private/ROADMAP.md and the prior
// "repo-root only" search returned no_roadmap_loaded.
QString findRoadmapUnder(const QString &canonicalRoot) {
    if (canonicalRoot.isEmpty()) return {};
    // Single-directory probe: .ants/project.json roadmap override (ANTS-2160)
    // then the candidate list under `dir` (ANTS-1459 — docs/, docs/private/,
    // docs/internal/, .github/).
    auto probeDir = [](const QString &dir) -> QString {
        if (const auto rm = ProjectSettings::load(dir).roadmap) {
            const QString c = dir + QLatin1Char('/') + *rm;
            if (QFileInfo(c).isFile()) return c;
        }
        static const QStringList kCandidates = {
            QStringLiteral("ROADMAP.md"),
            QStringLiteral("roadmap.md"),
            QStringLiteral("Roadmap.md"),
            QStringLiteral("docs/ROADMAP.md"),
            QStringLiteral("docs/roadmap.md"),
            QStringLiteral("docs/private/ROADMAP.md"),
            QStringLiteral("docs/private/roadmap.md"),
            QStringLiteral("docs/internal/ROADMAP.md"),
            QStringLiteral("docs/internal/roadmap.md"),
            QStringLiteral(".github/ROADMAP.md"),
            QStringLiteral(".github/roadmap.md"),
        };
        for (const QString &n : kCandidates) {
            const QString c = dir + QLatin1Char('/') + n;
            if (QFileInfo::exists(c)) return c;
        }
        return {};
    };
    // ANTS-3350 — resolve from a project SUBDIRECTORY too: walk up to the
    // nearest ancestor that holds a roadmap, bounded by the enclosing git
    // repo (.git) so the search never escapes the project. caller_cwd == root
    // returns on the first probe (byte-identical to the prior single-dir
    // behaviour); only a subdir caller walks up. Matches the code read verbs,
    // which already resolve the project from a subdir.
    QString dir = canonicalRoot;
    for (int depth = 0; depth < 64 && !dir.isEmpty(); ++depth) {
        const QString hit = probeDir(dir);
        if (!hit.isEmpty()) return hit;
        if (QFileInfo::exists(dir + QStringLiteral("/.git"))) break;
        const QString parent = QFileInfo(dir).path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

// ANTS-1548 — CHANGELOG.md resolver, same shape as findRoadmapUnder.
QString findChangelogUnder(const QString &canonicalRoot) {
    if (canonicalRoot.isEmpty()) return {};
    auto probeDir = [](const QString &dir) -> QString {
        // ANTS-2160 — .ants/project.json changelog override.
        if (const auto cl = ProjectSettings::load(dir).changelog) {
            const QString c = dir + QLatin1Char('/') + *cl;
            if (QFileInfo(c).isFile()) return c;
        }
        static const QStringList kCandidates = {
            QStringLiteral("CHANGELOG.md"),
            QStringLiteral("changelog.md"),
            QStringLiteral("Changelog.md"),
            QStringLiteral("docs/CHANGELOG.md"),
            QStringLiteral("docs/changelog.md"),
        };
        for (const QString &n : kCandidates) {
            const QString c = dir + QLatin1Char('/') + n;
            if (QFileInfo::exists(c)) return c;
        }
        return {};
    };
    // ANTS-3350 — walk up to the repo (.git) boundary, mirroring
    // findRoadmapUnder, so changelog_log resolves from a subdirectory.
    QString dir = canonicalRoot;
    for (int depth = 0; depth < 64 && !dir.isEmpty(); ++depth) {
        const QString hit = probeDir(dir);
        if (!hit.isEmpty()) return hit;
        if (QFileInfo::exists(dir + QStringLiteral("/.git"))) break;
        const QString parent = QFileInfo(dir).path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

// ANTS-2040 — YAML-changelog resolver. project_layout already
// discovers these (ANTS-1574: RetroDB-style structured changelogs at
// data/changelog.yaml, plus root-level CHANGELOG.{yaml,yml}), but the
// Keep-a-Changelog writer (findChangelogUnder above) only matches
// Markdown. When the Markdown probe misses, changelog_log uses this to
// distinguish "no changelog at all" (no_changelog) from "found a YAML
// changelog the writer can't append to yet" (format_mismatch).
QString findYamlChangelogUnder(const QString &canonicalRoot) {
    if (canonicalRoot.isEmpty()) return {};
    // Mirrors the YAML subset of projectlayoutengine.cpp's
    // kChangelogCandidates so reader/writer discovery stay in lockstep.
    static const QStringList kYamlCandidates = {
        QStringLiteral("CHANGELOG.yaml"),
        QStringLiteral("CHANGELOG.yml"),
        QStringLiteral("data/changelog.yaml"),
        QStringLiteral("data/changelog.yml"),
        QStringLiteral("data/CHANGELOG.yaml"),
        QStringLiteral("data/CHANGELOG.yml"),
    };
    for (const QString &n : kYamlCandidates) {
        const QString c = canonicalRoot + QLatin1Char('/') + n;
        if (QFileInfo::exists(c)) return c;
    }
    return {};
}

// ANTS-1463 — canonical hint emitted on every unrecognised_format
// refusal envelope across roadmap_query (bullets + section_index
// modes) and roadmap_log (append + flip terminal branches). One
// constant defeats per-site copy-drift; the test in
// tests/features/mcp_roadmap_unrecognised_format/ asserts the
// hint carries both bullet-format signatures (`- [ ]` for GFM
// and the 📋 emoji byte sequence for ants-v1) so a rewording
// that drops either trips the regression guard. Emoji codepoints
// are inline UTF-8 byte escapes per remotecontrol.cpp:1435-1437.
const QString &kUnrecognisedFormatHint() {
    static const QString v = QStringLiteral(
        "Roadmap content didn't match GFM-task-list "
        "(`- [ ]` / `- [x]`) or Ants-v1 emoji-status "
        "(`- \xF0\x9F\x93\x8B/\xF0\x9F\x9A\xA7/"
        "\xE2\x9C\x85/\xF0\x9F\x92\xAD [PROJ-NNNN]`) bullet "
        "formats. See docs/standards/roadmap-format.md for the "
        "Ants-v1 spec; reformat the roadmap, write a converter, "
        "or edit the markdown directly.");
    return v;
}

// ANTS-1463 — expected_format[] array on every unrecognised_format
// envelope so callers can branch on a single field regardless of
// which verb refused.
QJsonArray kUnrecognisedFormatExpected() {
    QJsonArray a;
    a.append(QStringLiteral("GFM-task-list"));
    a.append(QStringLiteral("Ants-v1 emoji"));
    return a;
}

// ANTS-2031 — roadmap_log's writers only emit GFM / ants-v1 bullets.
// A `#### Pass N.M` heading roadmap (ANTS-1530) parses fine on the
// READ side, so the unrecognised_format gate (which fires on zero
// parsed bullets) never trips — and an unguarded write would splice a
// GFM/ants-v1 bullet into a heading file, corrupting its format.
// parseBullets classifies the whole doc as one format, so a single
// pass-headings record means the file is pass-headings.
bool rcBulletsArePassHeadings(
        const QVector<RoadmapDialog::BulletRecord> &parsed) {
    for (const auto &rec : parsed) {
        if (rec.format == QStringLiteral("pass-headings")) return true;
    }
    return false;
}

// ANTS-2031 — refusal envelope steering the caller to Edit. The
// minimum-viable support the bullet asks for until a heading-format
// writer exists. `op` names the refused verb for a precise message.
QJsonDocument rcPassHeadingsWriteRefusal(const QString &path,
                                         const QString &op) {
    QJsonObject env;
    env["ok"]     = false;
    env["code"]   = QStringLiteral("format_mismatch");
    env["error"]  = QStringLiteral(
        "roadmap_log op:\"%1\": \"%2\" is a `#### Pass N.M` heading "
        "roadmap; the writer only emits GFM / ants-v1 bullets and "
        "can't splice these without corrupting the heading format")
            .arg(op, path);
    env["path"]   = path;
    env["format"] = QStringLiteral("pass-headings");
    env["hint"]   = QStringLiteral(
        "Edit the file directly: append a `#### Pass N.M …` heading "
        "with its `- **Status**:` bullets, or change the sub-bullet's "
        "Status line in place. Heading-format writes aren't supported "
        "by roadmap_log yet (ANTS-2031).");
    return QJsonDocument(env);
}

// ANTS-1517 — per-bullet body truncation cap. 2 KiB strikes a
// balance between "long enough to capture the rationale of a typical
// roadmap bullet" and "short enough that 500 bullets × 2 KiB stays
// under the response soft cap". Callers needing the verbatim full
// body should follow up with a targeted Read.
constexpr int kRoadmapQueryBodyCap = 2000;
// ANTS-3402 — the cache stores each body up to this larger ceiling so a
// TARGETED single-bullet / id-set fetch can opt into more than the 2000
// list default via `max_body_bytes` (a multi-phase epic narrator bullet
// holds its whole phase plan in one ~5 KiB body — the 2000 cap truncated
// it, forcing an awk fallback; Album Builder feedback). Aggregate memory
// stays bounded by the file size: only the rare oversized body stores
// more than 2000, and Σ bodies ≤ the roadmap file. List/section emission
// still re-truncates to kRoadmapQueryBodyCap (rcCapBodyFields), so only
// the opt-in id/ids path sees the larger body.
constexpr int kRoadmapQueryBodyStoreCap = 16384;

// ANTS-3736 — a truncated body keeps its HEAD *and* its TAIL. On a long-lived
// epic the body is an append-only progress log: the head says what the item
// IS, the tail says where it currently STANDS. Head-only truncation dropped
// exactly the part a caller asking "what is the state of this?" needs, and
// nothing in the envelope said the omitted part was the NEWER part — a DOOM
// fetch returned "NEXT: implement L1c then L1d" for work that had shipped
// three days earlier. Raising max_body_bytes could not rescue it: a body past
// the 16 KiB store cap lost its tail in the CACHE, so no emission-time cap
// could reach it. Fixed at both truncation sites for that reason.
//
// The marker is a fixed string carrying no counts, so it stays byte-stable
// across calls (prompt-cache / ETag friendly) and a second elision at
// emission cannot render a stale count.
//
// ANTS-4091 — it also names the remedy. Saying only that text was elided left
// the caller with no route to the omitted span, and the span that goes is the
// MIDDLE — where a bullet's resume plan sits. Still count-free, for the
// byte-stability reason above.
const QString &kBodyElisionMarker() {
    static const QString m = QStringLiteral(
        "\n\n… [body elided — tail follows; refetch by id with "
        "max_body_bytes for more] …\n\n");
    return m;
}

// Chars of a truncated body reserved for its tail. Capped at cap/3 as well,
// so a small cap still leads with the head rather than becoming tail-only.
constexpr int kRoadmapQueryBodyTailCap = 1024;

// head + marker + tail, sized to land exactly on `cap`. Falls back to a plain
// head clip when `cap` is too small to carry both ends plus the marker.
QString rcElideBody(const QString &body, int cap) {
    const QString &marker = kBodyElisionMarker();
    const int tailLen = qMin(kRoadmapQueryBodyTailCap, cap / 3);
    const int headLen = cap - tailLen - marker.size();
    if (headLen <= 0) return body.left(cap);
    return body.left(headLen) + marker + body.right(tailLen);
}

// Always populate body + body_truncated on every cached bullet
// (regardless of the caller's include_body preference), so a later
// call that DOES want include_body doesn't need to rebuild the cache.
// rcStripBodyFields below removes them just before emission when
// include_body is false. `cap` defaults to the 2000 list cap; the cache
// builder passes kRoadmapQueryBodyStoreCap (ANTS-3402).
void rcSetBodyFields(QJsonObject &o, const QString &body,
                     int cap) {
    if (body.size() > cap) {
        o["body"] = rcElideBody(body, cap);   // ANTS-3736 — head + tail
        o["body_truncated"] = true;
    } else {
        o["body"] = body;
    }
}

// ANTS-3402 — re-truncate already-cached body fields to `cap` at emission.
// The cache stores bodies up to kRoadmapQueryBodyStoreCap; every path that
// emits from the cache (full-file list at 2000, id/ids at max_body_bytes)
// runs this so the wire payload honours the effective cap. Preserves an
// existing body_truncated:true (a body truncated at the store cap stays
// flagged even if it now fits `cap` — it can't, since cap ≤ store cap,
// but the OR keeps the flag monotonic).
void rcCapBodyFields(QJsonArray &arr, int cap) {
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr.at(i).toObject();
        const auto it = o.constFind(QStringLiteral("body"));
        if (it == o.constEnd()) continue;
        const QString body = it->toString();
        if (body.size() > cap) {
            // ANTS-3736 — head + tail here too. Re-eliding an already-elided
            // cached body is safe: the emission cap is <= the store cap, so
            // the new head sits inside the old head and the new tail inside
            // the old tail — the stored marker falls in neither slice.
            o["body"] = rcElideBody(body, cap);
            o["body_truncated"] = true;
            arr.replace(i, o);
        }
    }
}

// Strip body fields from a paginated bullets slice before envelope
// assembly. No-op if the bullets predate ANTS-1517 (older cached
// entries simply have no `body` field to remove).
void rcStripBodyFields(QJsonArray &arr) {
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr.at(i).toObject();
        if (o.contains(QStringLiteral("body")) ||
            o.contains(QStringLiteral("body_truncated"))) {
            o.remove(QStringLiteral("body"));
            o.remove(QStringLiteral("body_truncated"));
            arr.replace(i, o);
        }
    }
}

// ANTS-1876 — clip a single text-bearing field to at most `cap` UTF-8
// bytes (the ellipsis is counted INSIDE the budget — see ANTS-1876
// INV-2). Returns the clipped form; fields whose unclipped UTF-8 form
// already fits are returned verbatim (no ellipsis added). The UTF-8
// continuation-byte check (0x80..0xBF) backs up to the prior code-
// point boundary, so we never emit a half-character.
QString rcClipMatchBytes(const QString &s, int cap) {
    if (cap <= 0) return s;
    const QByteArray utf8 = s.toUtf8();
    if (utf8.size() <= cap) return s;
    // Reserve 3 bytes for the UTF-8 ellipsis (U+2026 → E2 80 A6).
    constexpr int kEllipsisBytes = 3;
    int budget = cap - kEllipsisBytes;
    if (budget < 0) budget = 0;
    // Back up across any UTF-8 continuation bytes (0x80..0xBF) at
    // the budget boundary so we don't split a multi-byte sequence.
    int cut = std::min<int>(budget, utf8.size());
    while (cut > 0 &&
           (static_cast<unsigned char>(utf8.at(cut)) & 0xC0) == 0x80) {
        --cut;
    }
    // ANTS-4389 — the marker is a CHARACTER, not three bytes.
    // `QStringLiteral("\xE2\x80\xA6")` reads a narrow literal as Latin-1, so
    // U+2026's UTF-8 bytes each became their own QChar and re-encoded as the
    // mojibake `â¦`. That mattered beyond looks: the schema documents the clip
    // as "payload prefix + 3-byte ellipsis", so a caller stripping the
    // documented marker never matched it, and a caller grepping the returned
    // text verbatim — which is what a cross-document review does when
    // verifying a quotation — got three spurious characters at the boundary.
    // `max_match_bytes` defaults to 512 (ANTS-3548), so this was on by
    // default. The 3-byte reservation above is unchanged and still correct:
    // U+2026 is 3 bytes in UTF-8.
    return QString::fromUtf8(utf8.constData(), cut) + QChar(0x2026);
}

// ANTS-1876 — apply `max_match_bytes` clip to every text-bearing
// field in the matches array: each match's `text` (or `headline`,
// post-rename) and every `text` field inside its `context_before`
// / `context_after` arrays. `also_at` carries no text and is
// untouched. Pipeline step 3 (after dedup, before headline_only
// rename + context drop).
void rcClipMatchTextFields(QJsonArray &matches, int cap) {
    if (cap <= 0) return;
    for (int i = 0; i < matches.size(); ++i) {
        QJsonObject m = matches.at(i).toObject();
        if (m.contains(QStringLiteral("text"))) {
            m[QStringLiteral("text")] =
                rcClipMatchBytes(m.value(QStringLiteral("text"))
                                     .toString(), cap);
        }
        for (const auto &arrKey :
             {QStringLiteral("context_before"),
              QStringLiteral("context_after")}) {
            if (!m.contains(arrKey)) continue;
            QJsonArray ctx = m.value(arrKey).toArray();
            for (int j = 0; j < ctx.size(); ++j) {
                QJsonObject c = ctx.at(j).toObject();
                if (c.contains(QStringLiteral("text"))) {
                    c[QStringLiteral("text")] =
                        rcClipMatchBytes(c.value(QStringLiteral("text"))
                                             .toString(), cap);
                }
                ctx.replace(j, c);
            }
            m[arrKey] = ctx;
        }
        matches.replace(i, m);
    }
}

// ANTS-1876 — apply `headline_only:true` projection to every match.
// Pipeline step 4 (after clip): rename `text` → `headline`, drop
// `context_before` / `context_after`. `also_at` (which has no
// `text` to begin with) is untouched.
void rcApplyHeadlineOnly(QJsonArray &matches) {
    for (int i = 0; i < matches.size(); ++i) {
        QJsonObject m = matches.at(i).toObject();
        const QString text =
            m.value(QStringLiteral("text")).toString();
        m.remove(QStringLiteral("text"));
        m.remove(QStringLiteral("context_before"));
        m.remove(QStringLiteral("context_after"));
        m[QStringLiteral("headline")] = text;
        matches.replace(i, m);
    }
}

// ANTS-1877 — sniff the existing roadmap for stable-prefix bullet
// IDs (e.g. "Sh4", "MT8" — anything matching
// ^[A-Za-z][A-Za-z0-9_-]+$ that is NOT the canonical ^ANTS-[0-9]+$
// shape). Returns the first matching id within the first 50
// parsed bullets, or empty string when none. Used by
// cmdRoadmapLogAppend to surface a helpful hint when the
// .roadmap-counter is missing AND the project uses stable IDs the
// allocator doesn't currently support.
QString rlDetectStablePrefixId(const QString &markdown) {
    static const QRegularExpression antsRe(
        QStringLiteral("^ANTS-[0-9]+$"));
    static const QRegularExpression stableRe(
        QStringLiteral("^[A-Za-z][A-Za-z0-9_-]+$"));
    const auto bullets = RoadmapDialog::parseBullets(markdown);
    constexpr int kSniffCap = 50;
    const int upTo = std::min<int>(bullets.size(), kSniffCap);
    for (int i = 0; i < upTo; ++i) {
        const QString id = bullets.at(i).id;
        if (id.isEmpty()) continue;
        if (antsRe.match(id).hasMatch()) continue;
        if (stableRe.match(id).hasMatch()) return id;
    }
    return QString();
}

// ANTS-3397 — true when any of the first 50 parsed bullets carries a
// non-empty id (of any shape: counter-style ANTS-NNNN, a project
// prefix, or a stable string). Lets the counter allocator tell a
// greenfield roadmap (no ids of any kind → safe to auto-init
// .roadmap-counter at 0) apart from a roadmap that already allocated
// ids but lost its counter file (a real desync we keep refusing).
bool rlRoadmapHasAnyBulletId(const QString &markdown) {
    const auto bullets = RoadmapDialog::parseBullets(markdown);
    constexpr int kSniffCap = 50;
    const int upTo = std::min<int>(bullets.size(), kSniffCap);
    for (int i = 0; i < upTo; ++i)
        if (!bullets.at(i).id.isEmpty()) return true;
    return false;
}

// ANTS-2054 — infer the dominant counter-style ID prefix from the
// existing roadmap so op:append / op:append_batch render
// `[<prefix>-NNNN]` matching the project (e.g. "mame-curator-1073")
// instead of the hardcoded "ANTS-1073". Scans the first 50 parsed
// bullets for ids of shape ^(<prefix>)-(<digits>)$ and returns the
// most common prefix; empty when none found, in which case the caller
// falls back to "ANTS" (back-compat for a fresh/id-less roadmap). The
// prefix is the run before the FINAL `-digits`, so "mame-curator-1065"
// → "mame-curator" and "ANTS-2057" → "ANTS".
QString rlDetectCounterPrefix(const QString &markdown) {
    static const QRegularExpression counterRe(
        // ANTS-3492 — digit-led-but-letter-containing prefix (3D_E-0042).
        QStringLiteral(
            "^((?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*)"
            "-([0-9]{1,8})$"));
    const auto bullets = RoadmapDialog::parseBullets(markdown);
    constexpr int kSniffCap = 50;
    const int upTo = std::min<int>(bullets.size(), kSniffCap);
    QHash<QString, int> counts;
    QString best;
    int bestN = 0;
    for (int i = 0; i < upTo; ++i) {
        const QString id = bullets.at(i).id;
        if (id.isEmpty()) continue;
        const auto m = counterRe.match(id);
        if (!m.hasMatch()) continue;
        const int n = ++counts[m.captured(1)];
        if (n > bestN) { bestN = n; best = m.captured(1); }
    }
    return best;
}

// ANTS-2076 — id_prefix arg shape. Looser than op:flip's prefix_hint
// (which is uppercase-only) so a caller can pin a lowercase or
// mixed-case project prefix (e.g. "mame-curator", "DOOM").
// ANTS-3492 — prefix may be digit-led if it contains ≥1 letter (3D_E);
// a letter-free prefix (2026) is still rejected. {0,15} cap unchanged.
// ANTS-3498 — the same grammar is single-sourced as
// RoadmapFoldIn::isValidIdPrefix (used by the three fold-in verbs); keep the
// two literals in sync (both carry the ANTS-3492 letter-lookahead).
const QRegularExpression kIdPrefixShape(
    QStringLiteral("^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]{0,15}$"));

// ANTS-2076 — project-default counter-ID prefix derived from the
// caller's leaf directory (uppercase first 4 chars) — the same source
// op:flip already uses for caret-anchor prefixes. Used as the
// op:append / op:append_batch fallback when no explicit id_prefix is
// given AND the roadmap has no existing counter IDs to sniff, so a
// fresh project (DOOM_Ants → "DOOM") gets a project-shaped prefix
// instead of the hardcoded "ANTS".
QString rlLeafDirPrefix(const QString &callerCanonical) {
    const QString leaf = QFileInfo(callerCanonical).fileName();
    QString pfx = leaf.left(4).toUpper();
    if (pfx.isEmpty()) pfx = QStringLiteral("ROOT");
    return pfx;
}

// ANTS-2076 — resolve the counter-ID prefix for op:append /
// append_batch. Precedence: explicit id_prefix (caller override, empty
// when not given) > prefix sniffed from existing roadmap IDs
// (rlDetectCounterPrefix, ANTS-2054) > project-dir default
// (rlLeafDirPrefix). The id_prefix arg is expected pre-validated
// against kIdPrefixShape by the caller.
QString rlResolveCounterPrefix(const QString &idPrefixArg,
                               const QString &markdown,
                               const QString &callerCanonical) {
    if (!idPrefixArg.isEmpty()) return idPrefixArg;
    const QString sniffed = rlDetectCounterPrefix(markdown);
    if (!sniffed.isEmpty()) return sniffed;
    return rlLeafDirPrefix(callerCanonical);
}

// ANTS-2179 — highest numeric suffix among existing [pfx-NNNN] bullet ids
// in the parsed roadmap. The op:append / op:append_batch paths reconcile
// their .roadmap-counter against this: the counter is only a hint, and if
// it lags the file (a manual roadmap edit, a cross-tool append, a counter
// reset) then counter+1 would reissue a live id and silently violate
// roadmap-format.md's never-reuse-ids invariant. The bullets are already
// parsed at every call site (preflightBullets), so this is a free in-memory
// scan. Returns 0 when no id matches `pfx`.
qint64 rlMaxExistingIdForPrefix(
        const QVector<RoadmapDialog::BulletRecord> &bullets,
        const QString &pfx) {
    static const QRegularExpression idRe(
        // ANTS-3492 — digit-led-but-letter-containing prefix (3D_E-0042).
        QStringLiteral(
            "^((?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*)"
            "-([0-9]{1,8})$"));
    qint64 maxN = 0;
    for (const auto &b : bullets) {
        if (b.id.isEmpty()) continue;
        const auto m = idRe.match(b.id);
        if (!m.hasMatch() || m.captured(1) != pfx) continue;
        bool ok = false;
        const qint64 n = m.captured(2).toLongLong(&ok);
        if (ok && n > maxN) maxN = n;
    }
    return maxN;
}

// ANTS-2055 — collect the child-subsection slugs of `sec`: any indexed
// heading deeper than `sec` whose heading line falls inside sec's span.
// op:append / op:append_batch splice at sec.lineEnd; for a `##`
// milestone whose body is `###` subsections that lands the bullet past
// the last `###` child AND past the milestone's closing `---`, in the
// dead zone before the next `##` — where roadmap_query then mis-attributes
// it to the last child slug. A non-empty result means "ambiguous target":
// the caller must pick a leaf child slug (or create a new one). Empty →
// `sec` is a leaf and the append proceeds.
QStringList rcSectionChildSlugs(
        const QVector<RoadmapIndex::Section> &index,
        const RoadmapIndex::Section &sec) {
    QStringList children;
    for (const auto &c : index) {
        if (c.level > sec.level &&
            c.lineStart > sec.lineStart &&
            c.lineStart < sec.lineEnd) {
            children.append(c.slug);
        }
    }
    return children;
}

// ANTS-2055 — shared refusal envelope for an append into a parent
// section that has subsections. Lists the child slugs so the caller can
// re-target a leaf (or op:create_section a new one).
QJsonDocument rcSectionHasSubsectionsRefusal(const QString &slug,
                                             const QStringList &children) {
    QJsonObject env;
    env["ok"]    = false;
    env["code"]  = QStringLiteral("section_has_subsections");
    env["error"] = QStringLiteral(
        "roadmap_log: section \"%1\" has subsections — appending here "
        "would drop the bullet past the last child heading. Re-target "
        "one of its child slugs, or op:create_section a new one.")
            .arg(slug);
    env["section"]      = slug;
    env["child_slugs"]  = QJsonArray::fromStringList(children);
    return QJsonDocument(env);
}

// ANTS-1881 — project each bullet object to exactly the four-key set
// {id, status, headline_oneline, section_slug} for
// mode:"headline_only". Mutates in place. Rollup / narrator bullets
// keep their natural emptiness (id:"" for both; headline_oneline:""
// only for rollups whose source `headline` is empty) — INV-2.
void rcProjectHeadlineOnly(QJsonArray &arr) {
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject src = arr.at(i).toObject();
        QJsonObject p;
        p[QStringLiteral("id")] =
            src.value(QStringLiteral("id"));
        p[QStringLiteral("status")] =
            src.value(QStringLiteral("status"));
        p[QStringLiteral("headline_oneline")] =
            src.value(QStringLiteral("headline_oneline"));
        p[QStringLiteral("section_slug")] =
            src.value(QStringLiteral("section_slug"));
        arr.replace(i, p);
    }
}

// ANTS-3576 — project each changelog entry object to the lean
// mode:"headline_only" shape {version, category, ids, text_oneline}.
// Mutates in place. Mirrors cmdChangelogQuery's entryToJson headlineOnly
// branch so an auto-downshift (ANTS-3543) of the fat entries[] yields
// byte-identical rows to a native headline_only call. text_oneline =
// text.simplified() (the fat entry stores the full multi-line `text`).
void rcProjectChangelogHeadlineOnly(QJsonArray &arr) {
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject src = arr.at(i).toObject();
        QJsonObject p;
        p[QStringLiteral("version")]  = src.value(QStringLiteral("version"));
        p[QStringLiteral("category")] = src.value(QStringLiteral("category"));
        p[QStringLiteral("ids")]      = src.value(QStringLiteral("ids"));
        p[QStringLiteral("text_oneline")] =
            src.value(QStringLiteral("text")).toString().simplified();
        arr.replace(i, p);
    }
}

// ANTS-1521 — collapse a possibly multi-line headline to a single
// line: \r and \n become spaces, then runs of whitespace collapse to
// one space, then trim. Used to populate the `headline_oneline`
// companion field on every bullet emission site so an LLM caller
// concatenating headlines into prose gets a clean string without
// having to post-process every consumer. Keep `headline` intact for
// disk-parity.
QString rcHeadlineOneline(const QString &headline) {
    if (headline.isEmpty()) return QString();
    QString s;
    s.reserve(headline.size());
    bool prevSpace = false;
    for (QChar c : headline) {
        const bool isWs = c.isSpace() || c == QChar('\n') ||
                          c == QChar('\r') || c == QChar('\t');
        if (isWs) {
            if (!prevSpace) s.append(QChar(' '));
            prevSpace = true;
        } else {
            s.append(c);
            prevSpace = false;
        }
    }
    return s.trimmed();
}

// ANTS-2075 — when the parser capped the headline at 120 chars (long
// narrator bullets), also emit the untruncated `headline_full` so a
// roadmap_log headline locator — which hashes the FULL disk headline —
// is usable from the same roadmap_query result without a second read.
// Omitted when no truncation occurred, keeping the common-case payload
// lean.
void rcMaybeEmitHeadlineFull(QJsonObject &o,
                             const RoadmapDialog::BulletRecord &b) {
    if (!b.headlineFull.isEmpty() && b.headlineFull != b.headline) {
        o[QStringLiteral("headline_full")] = b.headlineFull;
    }
}

// ANTS-3382 — echo the bullet's `Evidence:` file paths when present.
// Additive + gated: omitted entirely when the bullet has no evidence, so
// the common-case payload is unchanged. Called from every roadmap_query
// bullet-fill site (the same posture as the inline `lanes` emit).
void rcMaybeEmitEvidence(QJsonObject &o,
                         const RoadmapDialog::BulletRecord &b) {
    if (b.evidence.isEmpty()) return;
    QJsonArray ev;
    for (const QString &p : b.evidence) ev.append(p);
    o[QStringLiteral("evidence")] = ev;
}

// ANTS-2080 — confirm-after compact echo for roadmap_log write verbs.
// When the caller passes return:"headline_only", the success envelope
// carries `post_bullets`: the just-touched bullet(s) in the same compact
// {id, status, headline_oneline} shape roadmap_query mode:headline_only
// emits — folding the verify read into the write. `status` is the word
// form (planned / in-progress / shipped / considered), not the emoji.
bool rcReturnHeadlineOnly(const QJsonObject &req) {
    return req.value(QStringLiteral("return")).toString() ==
           QStringLiteral("headline_only");
}
QJsonObject rcCompactBullet(const QString &id, const QString &statusWord,
                            const QString &headline) {
    QJsonObject o;
    o[QStringLiteral("id")]               = id;
    o[QStringLiteral("status")]           = statusWord;
    o[QStringLiteral("headline_oneline")] = rcHeadlineOneline(headline);
    return o;
}

// ANTS-2089 — reverse a status emoji to its canonical word form for the
// post_bullets compact echo. The flip path stores status as an emoji
// (✅ 🚧 💭 📋); rcCompactBullet wants the word (the inverse of the
// word→emoji map op:flip applies). Already-word / unknown input passes
// through unchanged.
QString rcStatusWord(const QString &emoji) {
    if (emoji == QString::fromUtf8("\xE2\x9C\x85"))     return QStringLiteral("shipped");     // ✅
    if (emoji == QString::fromUtf8("\xF0\x9F\x9A\xA7")) return QStringLiteral("in-progress"); // 🚧
    if (emoji == QString::fromUtf8("\xF0\x9F\x92\xAD")) return QStringLiteral("considered");  // 💭
    if (emoji == QString::fromUtf8("\xF0\x9F\x93\x8B")) return QStringLiteral("planned");      // 📋
    return emoji;
}

// ANTS-1743 — sanitise a single-line bullet field (headline / layman /
// source) before splicing it into ROADMAP.md. rcHeadlineOneline folds
// embedded \n/\r/\t + whitespace runs to single spaces so a stray
// newline can't split the bullet and break the markdown; this then
// drops any leftover C0/C1 control chars rcHeadlineOneline doesn't
// treat as whitespace, and caps length so a pathological field can't
// bloat the file. `body` is exempt — it is intentionally multi-line.
QString rcSanitizeBulletField(const QString &in, int maxLen) {
    const QString folded = rcHeadlineOneline(in);
    QString out;
    out.reserve(folded.size());
    for (QChar c : folded) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7f) continue;
        out.append(c);
    }
    if (maxLen > 0 && out.size() > maxLen) {
        out.truncate(maxLen);
        out = out.trimmed();
        out.append(QChar(0x2026));  // …
    }
    return out;
}

// ANTS-1646 — walk a bullet-cache array and return an array of
// duplicate-ID descriptors. Each descriptor names an ID that
// appeared on more than one bullet (rollups + narrators with empty
// IDs are excluded) and lists the occurrences (section_slug +
// status) so a caller can decide whether the collision is a real
// drift bug or an intentional cross-section tracking cite. Output
// shape per duplicate:
//   { "id": "ANTS-NNNN",
//     "occurrences": [{ "section_slug": "...", "status": "..." }, ...] }
// IDs in the result preserve first-seen order so the array is
// stable across calls on the same cache. The whole array stays
// empty on a clean roadmap; cmdRoadmapQuery's emission gate suppresses
// the field entirely in that case so the envelope shape is unchanged
// for the common path.
//
// ANTS-1688 — two refinements for large legacy-format roadmaps:
//  (1) Correctness: key only on canonical allocated IDs
//      (RoadmapIndex::isCanonicalId). The GFM adapter (ANTS-1428)
//      synthesises 10-char content-hash nonces for bullets without a
//      [PROJ-NNNN] token, and surfaces Obsidian `^anchor` tokens; both
//      previously collided wholesale and surfaced as bogus duplicate
//      IDs (a `35ra39wbn1` reported 7×).
//  (2) Payload shrink: cap occurrences[] at kDuplicateOccurrencesCap
//      and record the dropped tail in a per-ID `truncated_count` so a
//      genuine large collision set can't blow the response-size budget.
constexpr int kDuplicateOccurrencesCap = 3;

// ANTS-1882 — filter a duplicate-ids descriptor array (the shape
// produced by `rcComputeDuplicateIds` above) to only entries whose
// `occurrences[]` array contains at least one bullet inside the
// named section. Used by the `roadmap_query` section path so the
// duplicate_ids field is section-scoped (preserves per-section ETag
// invariance — when section B's duplicates change but foo doesn't
// have any duplicates, foo's response doesn't carry them). Returns
// an empty array when no duplicates involve the section.
// ANTS-1696 — classify a section slice (raw markdown) when
// parseBullets returned zero entries. Counts non-blank,
// non-heading, non-bullet content lines; declares the shape
// "table" iff any such line begins with `|` (after a leading
// whitespace strip), else "prose". Empty slice → "empty" with
// non_bullet_lines:0; emit-time skips when shape == "empty"
// (the existing back-compat envelope shape is preserved). The
// list comprehension stays in one pass so the helper costs
// O(slice size) on the rare emit-empty branch.
// ANTS-1907 — per-section ETag. Hash of the section's sliced UTF-8
// bytes (SHA-256, first 16 hex chars) — matches the "byte range + file
// content" key proposed in the roadmap bullet without dragging in the
// file-level etag (the file etag flips on ANY edit; the per-section
// etag is invariant under edits to OTHER sections, which is the
// whole point). 16 hex chars = 64 bits of collision space — same
// budget the dispatch-layer file ETag uses (helpers in remotecontrol.cpp
// at L10086+). Stable across runs (no salt).
QString rcComputeSectionEtag(const QString &slice) {
    return QString::fromUtf8(
        QCryptographicHash::hash(slice.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex().left(16));
}

QJsonObject rcSectionShape(const QString &slice) {
    int nonBullet = 0;
    bool sawPipe = false;
    const QStringList lines = slice.split(QChar('\n'));
    for (const QString &raw : lines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty()) continue;
        // Skip headings (#, ##, ###, …) — section-marker rows.
        if (trimmed.startsWith(QChar('#'))) continue;
        // Skip bullet rows (any of -, *, +). parseBullets accepts
        // exactly these for ants-v1; GFM uses the same three. A
        // single-char line "-" is unlikely but still a non-bullet
        // content line, so require the dash + space.
        if (trimmed.startsWith(QStringLiteral("- ")) ||
            trimmed.startsWith(QStringLiteral("* ")) ||
            trimmed.startsWith(QStringLiteral("+ "))) {
            continue;
        }
        // Skip numbered-list rows (1. 2. 10. etc.). Same rationale.
        bool isNumberedList = false;
        if (trimmed.size() >= 3 && trimmed.at(0).isDigit()) {
            int i = 1;
            while (i < trimmed.size() && trimmed.at(i).isDigit()) ++i;
            if (i < trimmed.size() && trimmed.at(i) == QChar('.') &&
                i + 1 < trimmed.size() &&
                trimmed.at(i + 1) == QChar(' ')) {
                isNumberedList = true;
            }
        }
        if (isNumberedList) continue;
        ++nonBullet;
        if (trimmed.startsWith(QChar('|'))) sawPipe = true;
    }
    QJsonObject out;
    if (nonBullet == 0) {
        out[QStringLiteral("shape")] = QLatin1String("empty");
        out[QStringLiteral("non_bullet_lines")] = 0;
        return out;
    }
    out[QStringLiteral("shape")] =
        sawPipe ? QLatin1String("table") : QLatin1String("prose");
    out[QStringLiteral("non_bullet_lines")] = nonBullet;
    return out;
}

QJsonArray rcFilterDuplicateIdsForSection(const QJsonArray &dupes,
                                          const QString &sectionSlug) {
    if (dupes.isEmpty() || sectionSlug.isEmpty()) return QJsonArray();
    QJsonArray out;
    for (const auto &v : dupes) {
        const QJsonObject entry = v.toObject();
        const QJsonArray occs =
            entry.value(QStringLiteral("occurrences")).toArray();
        bool relevant = false;
        for (const auto &o : occs) {
            if (o.toObject().value(QStringLiteral("section_slug"))
                    .toString() == sectionSlug) {
                relevant = true;
                break;
            }
        }
        if (relevant) out.append(entry);
    }
    return out;
}

QJsonArray rcComputeDuplicateIds(const QJsonArray &bullets) {
    QJsonArray out;
    if (bullets.isEmpty()) return out;
    QHash<QString, QJsonArray> occurrencesById;
    QStringList firstSeenOrder;
    for (const auto &v : bullets) {
        const QJsonObject o = v.toObject();
        const QString id = o.value(QStringLiteral("id")).toString();
        // ANTS-1688 — anchors / hash nonces / hyphen-less legacy bold
        // IDs are not allocated IDs and can't be drift collisions.
        if (!RoadmapIndex::isCanonicalId(id)) continue;
        QJsonObject occ;
        occ[QStringLiteral("section_slug")] =
            o.value(QStringLiteral("section_slug")).toString();
        occ[QStringLiteral("status")] =
            o.value(QStringLiteral("status")).toString();
        if (!occurrencesById.contains(id)) {
            firstSeenOrder.append(id);
        }
        occurrencesById[id].append(occ);
    }
    for (const QString &id : firstSeenOrder) {
        const QJsonArray &occs = occurrencesById.value(id);
        if (occs.size() < 2) continue;
        QJsonObject entry;
        entry[QStringLiteral("id")] = id;
        // ANTS-1688 — emit at most the cap; record any dropped tail.
        if (occs.size() > kDuplicateOccurrencesCap) {
            QJsonArray capped;
            for (int i = 0; i < kDuplicateOccurrencesCap; ++i) {
                capped.append(occs.at(i));
            }
            entry[QStringLiteral("occurrences")] = capped;
            entry[QStringLiteral("truncated_count")] =
                occs.size() - kDuplicateOccurrencesCap;
        } else {
            entry[QStringLiteral("occurrences")] = occs;
        }
        out.append(entry);
    }
    return out;
}

// ANTS-1424 / ANTS-1717 — strip leaked tool-call XML wrappers from a
// body/note string before it lands in ROADMAP.md. Some harnesses
// serialise sibling array/object params as literal
// `<parameter name="X">…</parameter>` blocks inside the prose; this
// removes those (and stray `</body>` / `</invoke>` closers), records
// recognised sibling names in `scrubbedNames` so the caller can warn
// that a typed argument was lost, collapses blank-line runs, and trims
// trailing whitespace. Hoisted out of cmdRoadmapLogAppend's local
// lambda (ANTS-1717) so the flip/annotate note path scrubs identically.
// ANTS-1995 — cap on caller-supplied `note` length before it reaches
// rcScrubLeakedToolXml's lazy [\s\S]*? backtracking regex. A note packed
// with many unclosed <parameter …> openers makes the globalMatch O(n²),
// a same-UID slow-regex DoS via roadmap_log op:flip_batch / annotate.
// 4 KiB is far above any real annotation; bodies (op:append) keep their
// own size handling and are not routed through this cap.
constexpr int kRcMaxNoteChars = 4096;

// ANTS-3640 — neutralise a code fence the prose opens and never closes.
//
// Bodies and notes are written as continuation lines indented two spaces
// under their bullet, and that indent saves nobody: CommonMark opens a
// fence at `^ {0,3}(```|~~~)`, and this file's own walkGfmBullets /
// walkAntsV1Bullets toggle on a `trimmed()` line starting with ``` at ANY
// indent. So one quoted-but-unclosed fence opener in a body swallows every
// bullet below it until the next stray fence. That is not hypothetical:
// ANTS-3635's body quoted a fence opener, ANTS-3638's quoted another, and
// the pair fenced off ANTS-3637 between them — surfacing two ops later as
// an anchor_unsafe_context refusal naming the innocent bullet.
//
// Only an UNCLOSED opener is escaped. A body quoting a whole balanced code
// block leaves the file well-formed, and quoting one is a legitimate thing
// to do, so it passes through untouched. The escape is CommonMark's
// backslash form — `\``` renders as a literal ``` and opens nothing — and
// it also falls outside the walkers' `startsWith("```")` toggle, so both
// readers agree on the repaired line.
void rcEscapeUnclosedFence(QString &text) {
    if (!text.contains(QLatin1Char('`')) && !text.contains(QLatin1Char('~')))
        return;
    QStringList ls = text.split(QChar('\n'));
    // Toggle exactly as the walkers do, so "balanced" means the same thing
    // to this guard as it does to the code that would later refuse.
    int openAt = -1;
    for (int i = 0; i < ls.size(); ++i) {
        const QString t = ls.at(i).trimmed();
        if (!t.startsWith(QStringLiteral("```")) &&
            !t.startsWith(QStringLiteral("~~~"))) {
            continue;
        }
        openAt = (openAt < 0) ? i : -1;
    }
    if (openAt < 0) return;
    QString &l = ls[openAt];
    int at = 0;
    while (at < l.size() && l.at(at).isSpace()) ++at;
    l.insert(at, QLatin1Char('\\'));
    text = ls.join(QChar('\n'));
}

// ANTS-3640 — the fenced-bullet refusals name the bullet being edited,
// which is never where the fix goes: that bullet is innocent, some earlier
// line opened a fence and swallowed it. Appended to those messages so the
// reader is pointed at the line they actually have to repair. Empty when
// the opener is unknown (older walk results), so the message degrades to
// its pre-ANTS-3640 wording rather than claiming a bogus line.
QString rcFenceOpenerHint(int fenceOpenLine) {
    if (fenceOpenLine < 0) return {};
    return QStringLiteral(" — the fence opens at line %1")
        .arg(fenceOpenLine + 1);
}

void rcScrubLeakedToolXml(QString &text, QStringList &scrubbedNames) {
    if (text.isEmpty()) return;
    // Matched <parameter name="X">…</parameter> pairs. [\s\S] spans
    // newlines (QRegularExpression '.' doesn't by default).
    static const QRegularExpression pairRx(
        QStringLiteral("<parameter\\s+name=(?:\"([^\"]*)\"|"
                       "'([^']*)'|([^\\s>]+))[^>]*>"
                       "[\\s\\S]*?</parameter>"),
        QRegularExpression::CaseInsensitiveOption);
    auto it = pairRx.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        QString name = m.captured(1);
        if (name.isEmpty()) name = m.captured(2);
        if (name.isEmpty()) name = m.captured(3);
        if (!name.isEmpty() && !scrubbedNames.contains(name)) {
            scrubbedNames.append(name);
        }
    }
    text.remove(pairRx);
    static const QRegularExpression reOrphanOpen(
        QStringLiteral("<parameter\\s+name=[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(reOrphanOpen);
    static const QRegularExpression reOrphanClose(
        QStringLiteral("</parameter>"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(reOrphanClose);
    static const QRegularExpression reStrayBody(
        QStringLiteral("</body>"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(reStrayBody);
    static const QRegularExpression reCloseTag(
        QStringLiteral("</invoke>"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(reCloseTag);
    static const QRegularExpression reBlankRun(QStringLiteral("\\n{3,}"));
    text.replace(reBlankRun, QStringLiteral("\n\n"));
    QStringList ls = text.split(QChar('\n'));
    for (QString &l : ls) {
        while (!l.isEmpty() && (l.endsWith(QChar(' ')) ||
                                l.endsWith(QChar('\t')))) {
            l.chop(1);
        }
    }
    text = ls.join(QChar('\n'));
    // ANTS-3703 — a bare XML-ish tag sitting at the very START or END of the
    // text is leakage, not prose: a truncated tool-call wrapper is the
    // commonest shape a leak takes, and a stray `</note>` reached ROADMAP.md
    // verbatim before this. Only the edges are stripped; markup mid-sentence
    // is legitimate prose and stays untouched. Looped so a doubled leak
    // (`</note></invoke>`) clears in one call.
    static const QRegularExpression reEdgeTag(
        QStringLiteral("\\A\\s*</?[a-z][a-z0-9_-]*\\s*>|"
                       "</?[a-z][a-z0-9_-]*\\s*>\\s*\\z"),
        QRegularExpression::CaseInsensitiveOption);
    for (;;) {
        const QString before = text;
        text.remove(reEdgeTag);
        if (text == before) break;
    }
    rcEscapeUnclosedFence(text);   // ANTS-3640
    while (text.endsWith(QChar('\n'))) text.chop(1);
}

// ANTS-3417 — strip trailing spaces/tabs from an emitted roadmap line so
// the writer's output is pre-commit-clean by construction. Without it, an
// empty body/note continuation line renders as the bare 2-space hang indent
// ("  ") — trailing whitespace that the ubiquitous trim-trailing-whitespace
// pre-commit hook rejects, forcing a re-stage + re-commit after every write.
QString rcRightStrip(QString s) {
    while (s.endsWith(QLatin1Char(' ')) || s.endsWith(QLatin1Char('\t')))
        s.chop(1);
    return s;
}

// ANTS-3702 — `bytes_written` must mean the same thing on every roadmap_log
// op: the bytes this operation ADDED to ROADMAP.md. op:append reported the
// appended bullet (a delta), but every whole-file-rewrite op — flip,
// flip_batch, amend_body and the pass-heading variants — reported the size of
// the rewritten file, so six short annotate-only notes came back as a 459 KB
// write and the field could not serve the cheap "did this write roughly what
// I asked?" check it invites. The whole-file figure is still reported, as
// `file_bytes`. A pure status flip legitimately writes 0 added bytes (the
// emoji swap is byte-for-byte the same width) — that is the honest number.
//
// ANTS-3723 — the same helper now serves changelog_log, which had the other
// convention on every op: a ~700-byte entry came back as 1150003, CHANGELOG.md's
// exact size, with no `file_bytes` to disambiguate. Identical field names
// carrying opposite meanings across two sibling verbs is worse than either
// convention alone, because the reason a session trusts one number is that the
// pair agree. ANTS-3702 fixed this inside roadmap_log and stopped at that verb.
//
// ANTS-3724 — spec_log was the last write verb on the old convention; it now
// routes through here too, so every ROADMAP/CHANGELOG/spec write reports the
// same two fields with the same meanings.
void rcSetWriteBytes(QJsonObject &out, qint64 before, qint64 after) {
    out[QStringLiteral("bytes_written")] = after - before;
    out[QStringLiteral("file_bytes")]    = after;
}

// ANTS-1717/1793 — append `note` as indented continuation line(s) at
// the end of the body of the bullet whose headline sits at
// `headlineLine`. Format-agnostic: the body of both ants-v1 and
// GFM-task-list bullets is the run of indented continuation lines that
// follows the headline, terminated by a blank line, a column-0 line
// (next bullet / heading), or EOF. The note inherits the bullet's
// existing continuation indent when it has one, else a 2-space hang
// (matching op:"append"'s body treatment). Blank lines inside a
// multi-line note stay blank (no trailing-space lint). Returns the
// 0-based index of the first inserted line.
//
// ANTS-3440 — retry idempotency: when the exact rendered note already
// occupies the trailing body lines immediately before the insertion
// point, the append is skipped and `*alreadyPresent` (when supplied) is
// set true; the return value then points at the first line of the
// existing copy. See the dedup rationale block below.
int appendBodyNote(QStringList &lines, int headlineLine,
                   const QString &note, bool *alreadyPresent) {
    if (alreadyPresent) *alreadyPresent = false;
    int insertAt = headlineLine + 1;
    QString indent = QStringLiteral("  ");
    bool sawBody = false;
    // ANTS-3696 — the body is the bullet's WHOLE indented continuation run,
    // blank lines included; it ends at the next column-0 line (next bullet /
    // heading / `---`) or EOF. Breaking on the first blank line instead put
    // the note after the body's FIRST paragraph, so a multi-paragraph bullet
    // read as resolved and then went on arguing for itself, with the
    // `Kind:`/`Source:` trailer stranded below the resolution. This is the
    // span amendBodyExact() has walked since ANTS-3467 — the two now agree
    // on where a bullet's body ends.
    while (insertAt < lines.size()) {
        const QString &ln = lines.at(insertAt);
        if (!ln.isEmpty() && !ln.at(0).isSpace()) break;
        if (!sawBody && !ln.trimmed().isEmpty()) {
            int w = 0;
            while (w < ln.size() && ln.at(w) == QLatin1Char(' ')) ++w;
            if (w > 0) indent = ln.left(w);
            sawBody = true;
        }
        ++insertAt;
    }
    // Back up over the blank line(s) separating this bullet from the next, so
    // the note lands against the last real body line rather than adrift in
    // the gap (where the bullet walkers would read it as a sibling's).
    while (insertAt > headlineLine + 1 &&
           lines.at(insertAt - 1).trimmed().isEmpty()) {
        --insertAt;
    }
    const QStringList noteLines = note.split(QChar('\n'));
    // Render each note line exactly as it will be written, so the dedup
    // compare below is byte-exact against a previously-appended copy.
    // ANTS-3417 — a whitespace-only note line collapses to "" (no dangling
    // indent); a real line is right-stripped so no trailing whitespace
    // reaches ROADMAP.md.
    QStringList rendered;
    rendered.reserve(noteLines.size());
    for (const QString &nl : noteLines) {
        rendered.append(nl.trimmed().isEmpty()
                            ? QString()
                            : rcRightStrip(indent + nl));
    }
    // ANTS-3440 — retry idempotency. A flip/annotate that committed to
    // disk but surfaced to the caller as a (client-side) transport
    // timeout gets retried; the status flip is naturally idempotent
    // (emoji→same emoji) but a second note append is not. If the exact
    // rendered note already occupies the trailing body lines immediately
    // before the insertion point, this IS that retry — skip the insert
    // and report it, so the resolution note is never duplicated. The
    // compare is byte-exact and bounded to this bullet's body (blockStart
    // must stay at/after the first body line), so a legitimately different
    // note (new date, reworded) never false-dedups.
    const int blockStart = insertAt - static_cast<int>(rendered.size());
    if (blockStart >= headlineLine + 1) {
        bool identical = true;
        for (int k = 0; k < rendered.size(); ++k) {
            if (lines.at(blockStart + k) != rendered.at(k)) {
                identical = false;
                break;
            }
        }
        if (identical) {
            if (alreadyPresent) *alreadyPresent = true;
            return blockStart;
        }
    }
    for (int k = 0; k < rendered.size(); ++k) {
        lines.insert(insertAt + k, rendered.at(k));
    }
    return insertAt;
}

// ANTS-3406 — replace an exact single-line substring `oldText` with
// `newText` inside the continuation body of the bullet whose headline
// sits at `headlineLine`. The body span is the same contiguous indented
// run appendBodyNote() targets (blank line / column-0 line / EOF
// terminated), so a match can never leak into a sibling bullet or touch
// the headline. Requires exactly one occurrence across the span's lines:
// returns the total count found; only on a unique match (count == 1) is
// the replacement applied in place and *matchedLine set to the 0-based
// edited line index. On a 0- or multi-match `lines` is left untouched so
// the caller can refuse without a rollback. `oldText` must be non-empty
// (the caller enforces this) and is matched verbatim (case-sensitive);
// a phrase spanning a line boundary won't match any single line and is
// reported as not-found by design (the exact-match patch is single-line).
int amendBodyExact(QStringList &lines, int headlineLine,
                   const QString &oldText, const QString &newText,
                   int *matchedLine) {
    // ANTS-3467 — the body block is the headline line's full indented
    // continuation: every following line up to (not including) the next
    // non-indented, non-empty line (the next top-level bullet / heading /
    // `---`, all of which start at column 0). Blank lines INSIDE the block
    // are spanned, not treated as terminators. Previously the walk also
    // broke on the first blank line, which truncated the block before a
    // blank-line-separated nested sub-list (a common ROADMAP "Scope:"
    // shape) — so old_text on a nested sub-bullet refused
    // body_match_not_found though it was present. Blank lines hold no
    // oldText, so spanning them is safe; the non-indented break still
    // protects sibling bullets.
    int spanEnd = headlineLine + 1;
    while (spanEnd < lines.size()) {
        const QString &ln = lines.at(spanEnd);
        if (!ln.isEmpty() && !ln.at(0).isSpace()) break;
        ++spanEnd;
    }
    int total   = 0;
    int hitLine  = -1;
    for (int i = headlineLine + 1; i < spanEnd; ++i) {
        const int c = lines.at(i).count(oldText);
        if (c > 0 && hitLine < 0) hitLine = i;
        total += c;
    }
    if (total != 1) return total;   // 0 → not found, >1 → ambiguous

    // ANTS-3752 — a multi-line `newText` must not land flush-left. This is a
    // plain `QString::replace` into ONE list element, so an embedded newline
    // becomes a real line at column 0 when the list is joined. Every such
    // line stops being body (roadmap-format.md § 3.5 requires the
    // continuation indent), which cuts the bullet in two — silently, because
    // the envelope still returns {ok:true, amended:true} and only the NEXT
    // reader discovers it. Measured at filing: 16 lines detached from one
    // bullet, after which amend_body could no longer locate text it had
    // itself just written, and roadmap_query include_body stopped at the
    // break.
    //
    // Give each continuation line the matched line's own indent, which is the
    // same normalisation op:append already applies to `body` — that is why
    // appending a long body works and amending one did not. Relative
    // indentation the caller supplied is PRESERVED (prefix, not replace), so
    // a deeper nested sub-bullet still nests. Genuinely empty lines are left
    // empty rather than being filled with trailing whitespace.
    QString patch = newText;
    if (patch.contains(QLatin1Char('\n'))) {
        const QString &target = lines.at(hitLine);
        int w = 0;
        while (w < target.size() && target.at(w).isSpace()) ++w;
        const QString indent = target.left(w);
        if (!indent.isEmpty()) {
            QStringList parts = patch.split(QLatin1Char('\n'));
            for (int i = 1; i < parts.size(); ++i)
                if (!parts.at(i).isEmpty()) parts[i].prepend(indent);
            patch = parts.join(QLatin1Char('\n'));
        }
    }
    lines[hitLine].replace(oldText, patch);
    if (matchedLine) *matchedLine = hitLine;
    return 1;
}

// ANTS-1462 — render a header-inventory envelope from a built
// RoadmapIndex. Used by cmdRoadmapQuery as a fall-through when the
// bullet parser yields zero entries but the file still has ##/###
// headings (RetroArch-style table-plus-sections roadmaps). The
// envelope mirrors the section_index-mode response shape so
// callers can branch on `mode` alone. Inventory capped at
// kHeaderInventoryMax to defend against a malformed file with
// thousands of headings; overflow drops the tail and the envelope
// carries truncated:true.
constexpr int kHeaderInventoryMax = 200;

QJsonObject buildHeaderInventoryEnvelope(
    const QVector<RoadmapIndex::Section> &index,
    const QString &path,
    qint64 bytes) {
    QJsonObject env;
    env["ok"]    = true;
    env["mode"]  = QStringLiteral("header_inventory_fallback");
    env["path"]  = path;
    env["bytes"] = bytes;
    QJsonArray sections;
    const int cap = qMin(index.size(), kHeaderInventoryMax);
    for (int i = 0; i < cap; ++i) {
        const RoadmapIndex::Section &s = index.at(i);
        QJsonObject o;
        o["slug"]     = s.slug;
        o["headline"] = s.headingText;
        o["level"]    = s.level;
        sections.append(o);
    }
    env["sections"]        = sections;
    env["count"]           = cap;
    env["truncated"]       = (index.size() > kHeaderInventoryMax);
    env["hint"]            = QStringLiteral(
        "no GFM/Ants-v1 bullets parsed; section inventory "
        "emitted instead — use Read for the full markdown.");
    env["expected_format"] = kUnrecognisedFormatExpected();
    return env;
}

// ANTS-1428 Tier 2 — pure helpers for the op:"flip" locator. These
// are intentional duplicates of roadmapdialog.cpp's anon-namespace
// helpers (fnv1a64 / normaliseHeadline / extractBoldId /
// extractCaretAnchor / base36Lower). The parser side has feature
// tests pinning the byte-equal behaviour; the small duplication
// avoids exporting them through the QWidget-shaped roadmapdialog
// header just to feed this one verb. Spec § Status-flip locator.
quint64 rcFnv1a64(const QString &normalised) {
    constexpr quint64 kFnvOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr quint64 kFnvPrime       = 0x100000001b3ULL;
    quint64 h = kFnvOffsetBasis;
    const QByteArray bytes = normalised.toUtf8();
    for (char c : bytes) {
        h ^= static_cast<quint64>(static_cast<unsigned char>(c));
        h *= kFnvPrime;
    }
    return h;
}

QString rcNormaliseHeadline(const QString &raw) {
    QString s = raw.toLower();
    QString out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (QChar c : s) {
        if (c.isSpace()) {
            if (!out.isEmpty() && !prevSpace) out.append(QLatin1Char(' '));
            prevSpace = true;
        } else {
            out.append(c);
            prevSpace = false;
        }
    }
    while (!out.isEmpty()) {
        const QChar c = out.back();
        if (c == QLatin1Char('.') || c == QLatin1Char(',') ||
            c == QLatin1Char(';') || c == QLatin1Char(':') ||
            c == QLatin1Char('!') || c == QLatin1Char('?') ||
            c == QLatin1Char(' ')) {
            out.chop(1);
        } else {
            break;
        }
    }
    return out;
}

// ANTS-3388 — structural signature for `<verb> <path>`-template bullets
// (e.g. "Author src/mame_curator/<mod>/spec.md"), whose discriminating
// tokens are all paths/filenames the clustering denoiser strips — leaving
// them un-clusterable by shared-token overlap. The signature keeps the
// leading verb + the first and last path segments + segment count, so
// per-module template bullets that vary only in a middle path segment
// share it, while a different template (different verb / root / leaf /
// depth) does not. Empty when the headline is not template-shaped (no
// alphabetic leading verb, or no multi-segment path token). Cheap: one
// normalise + split, computed once per bullet.
QString rcStructuralStem(const QString &headline) {
    const QStringList toks = rcNormaliseHeadline(headline)
                                 .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (toks.size() < 2) return {};
    const QString verb = toks.first();
    for (const QChar c : verb)
        if (!c.isLetter()) return {};   // a real action word, not a path/number
    for (int i = 1; i < toks.size(); ++i) {
        if (!toks[i].contains(QLatin1Char('/'))) continue;
        const QStringList segs =
            toks[i].split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (segs.size() < 2) continue;
        return verb + QLatin1Char('|') + segs.first() + QLatin1Char('|') +
               segs.last() + QLatin1Char('|') + QString::number(segs.size());
    }
    return {};   // no path token ⟹ not a <verb> <path> template
}

// ANTS-3387 — classify a roadmap id/locator token that is bracket-token
// SHAPED (a `<prefix>-<digits>` id, as authored in `[PREFIX-NNNN]`) but
// fails the canonical PROJ-NNNN gate — the letter-led
// `[A-Za-z][A-Za-z0-9_-]*-\d+` of roadmap-format.md § 3.5.1 (ANTS-1405
// INV-4). A token like `3D_E-0022` (digit-leading prefix) never parses as
// a project id, so the parser assigns that bullet a synthetic content-hash
// id and the authored token is unaddressable on BOTH the read (id/ids) and
// write (flip/annotate) locator paths. Returning a bare found:false /
// bullet_not_found reads as "the item vanished"; callers use this to emit
// a targeted bad_id_format that names the real cause. Returns true iff the
// token is id-ISH but non-canonical (the only divergence from the gate is a
// non-letter lead char), so a genuinely-absent conforming id (INV-4) and a
// non-id-shaped string both correctly return false.
bool rcIsNonconformingIdToken(const QString &tok) {
    static const QRegularExpression kIdIsh(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_-]*-\\d+$"));
    // ANTS-3492 — kCanonical widens to "contains a letter" (3D_E-0042 is
    // canonical); kIdIsh above stays digit-permissive — do NOT widen it, or
    // this guard collapses to X && !X ≡ always-false. A letter-free id-shaped
    // token (2026-07) stays non-canonical → bad_id_format, as before.
    static const QRegularExpression kCanonical(
        QStringLiteral(
            "^(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\\d+$"));
    return kIdIsh.match(tok).hasMatch() &&
           !kCanonical.match(tok).hasMatch();
}

// ANTS-1922 — token-Jaccard ratio of two already-tokenised headlines,
// or -1.0 when fewer than `minShared` tokens overlap (the stop-word
// floor: a sub-floor pair returns -1.0, which fails every caller's
// `>=` threshold, so callers apply ONLY their ratio threshold and must
// not re-implement a separate count check). 2nd call-site of the
// rcComputePossibleDuplicates scoring (Rule-of-Three extract); the
// dup-detector keeps its 0.60 gate, bundles clustering uses 0.50, the
// ✅-sibling check uses 0.60.
double rcHeadlineJaccard(const QSet<QString> &tokA,
                         const QSet<QString> &tokB,
                         int minShared) {
    if (tokA.isEmpty() || tokB.isEmpty()) return -1.0;
    const QSet<QString> &small = (tokA.size() <= tokB.size()) ? tokA : tokB;
    const QSet<QString> &large = (tokA.size() <= tokB.size()) ? tokB : tokA;
    int inter = 0;
    for (const QString &t : small) {
        if (large.contains(t)) ++inter;
    }
    if (inter < minShared) return -1.0;
    const int uni = tokA.size() + tokB.size() - inter;
    if (uni <= 0) return -1.0;
    return static_cast<double>(inter) / uni;
}

// ANTS-2043 — soft near-duplicate CONTENT detector for op:append /
// append_batch. Ants already flags exact duplicate IDs
// (rcComputeDuplicateIds, canonical-ID collisions); this catches two
// bullets that *say* nearly the same thing so a caller doesn't re-file
// an existing item. Reuses the existing normalise + hash machinery: an
// exact normalised-headline match scores 100; otherwise a token Jaccard
// overlap (shared tokens / union of tokens) gates at 60 % with a
// ≥2-shared-token floor so two short headlines sharing a stop-word pair
// don't false-fire. Non-blocking — the verb still appends; the result
// is an advisory list returned in the success envelope. Top 5 by score.
QJsonArray rcComputePossibleDuplicates(
        const QVector<RoadmapDialog::BulletRecord> &existing,
        const QString &newHeadline) {
    const QString normNew = rcNormaliseHeadline(newHeadline);
    if (normNew.isEmpty()) return {};
    const quint64 hashNew = rcFnv1a64(normNew);
    const QStringList tokNewList =
        normNew.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const QSet<QString> tokNew(tokNewList.begin(), tokNewList.end());
    if (tokNew.isEmpty()) return {};

    struct Cand { QString id; QString headline; int score; };
    QVector<Cand> cands;
    for (const auto &rec : existing) {
        if (rec.headline.isEmpty()) continue;
        const QString normEx = rcNormaliseHeadline(rec.headline);
        if (normEx.isEmpty()) continue;
        int score = 0;
        if (rcFnv1a64(normEx) == hashNew) {
            score = 100;  // exact normalised match
        } else {
            const QStringList exList =
                normEx.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const QSet<QString> tokEx(exList.begin(), exList.end());
            // ANTS-1922 — extracted scoring (was an inline inter/uni loop
            // + ≥2-token floor + 0.60 gate here). rcHeadlineJaccard returns
            // -1.0 below the floor, so the >= 0.60 test subsumes the old
            // `inter >= 2` guard; behaviour is unchanged.
            const double jac = rcHeadlineJaccard(tokNew, tokEx);
            if (jac >= 0.60) {
                score = static_cast<int>(jac * 100.0 + 0.5);
            }
        }
        if (score > 0) cands.append({rec.id, rec.headline, score});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b) { return a.score > b.score; });
    QJsonArray out;
    const int cap = std::min<int>(cands.size(), 5);
    for (int i = 0; i < cap; ++i) {
        // Collapse multi-line headlines + bound the echo (parity with
        // headline_oneline; keeps the advisory payload small).
        QString hl = rcHeadlineOneline(cands[i].headline);
        if (hl.size() > 120) { hl.truncate(119); hl.append(QChar(0x2026)); }
        QJsonObject o;
        o["id"]       = cands[i].id;
        o["headline"] = hl;
        o["score"]    = cands[i].score;
        out.append(o);
    }
    return out;
}

bool rcExtractBoldId(const QString &lineHead, QString *id) {
    // ANTS-1937 — match RoadmapParse::extractBoldId's looser pattern
    // to support composite IDs like "Ts20-DE1, Ts20-DE2". Captures any text
    // up to 80 chars, then trims trailing period + whitespace (matching the
    // read-path post-processing in roadmapparse.cpp).
    static const QRegularExpression rx(QStringLiteral(
        "^\\*\\*(.{1,80}?)\\*\\*"));
    const auto m = rx.match(lineHead);
    if (!m.hasMatch()) return false;
    QString captured = m.captured(1);
    if (captured.endsWith(QLatin1Char('.'))) captured.chop(1);
    captured = captured.trimmed();
    if (captured.isEmpty()) return false;
    if (id) *id = captured;
    return true;
}

QString rcExtractCaretAnchor(const QString &line) {
    static const QRegularExpression rx(QStringLiteral(
        "\\^([a-z0-9-]+)\\s*$"));
    const auto m = rx.match(line);
    if (!m.hasMatch()) return QString();
    return m.captured(1);
}

// ANTS-3378 — the single canonical headline a GFM bullet is reported by.
// The write-path walker (walkGfmBullets) stores the RAW post-checkbox
// head: `**AX11. Audio device hot-swap** — automatically re-route audio`,
// markdown emphasis + bold-ID label + em-dash tail intact. roadmap_query
// (roadmapdialog.cpp parseBullets) reports a *de-marked-up* headline: the
// post-em-dash prose when a `**ID** — text` split exists, else the bold
// span / plain head, in both cases with the `**` emphasis and any trailing
// caret anchor removed. A caller copies THAT token, so the flip headline
// locator must derive the same form. Mirrors splitOnEmDash + the
// rxTrailAnchor strip in roadmapdialog.cpp's GFM branch.
QString rcGfmCanonicalHeadline(const QString &rawHead) {
    QString s = rawHead;
    static const QString kEmDash = QString::fromUtf8(" \xE2\x80\x94 ");  // " — "
    int idx = s.indexOf(kEmDash);
    int sepLen = kEmDash.size();
    if (idx < 0) {
        idx = s.indexOf(QStringLiteral(" -- "));
        sepLen = 4;
    }
    if (idx >= 0) s = s.mid(idx + sepLen);  // post-em-dash prose
    s.remove(QStringLiteral("**"));          // de-markup
    static const QRegularExpression rxTrailAnchor(
        QStringLiteral("\\s*\\^[a-z0-9-]+\\s*$"));
    s.replace(rxTrailAnchor, QString());
    return s.trimmed();
}

// ANTS-3378 — every distinct headline form the flip locator will accept
// for one GFM bullet. A caller may pass the canonical headline
// roadmap_query reports (the common case the old matcher missed), but the
// legacy raw head, the de-marked-up whole head, and the bold-ID label all
// stay valid. Returns normalised match hashes; the matcher tests the
// locator's hash for membership.
QSet<quint64> rcGfmHeadlineMatchHashes(const QString &rawHead,
                                       const QString &boldId) {
    QSet<quint64> hashes;
    auto add = [&](QString s) {
        static const QRegularExpression rxTrailAnchor(
            QStringLiteral("\\s*\\^[a-z0-9-]+\\s*$"));
        s.replace(rxTrailAnchor, QString());
        const QString n = rcNormaliseHeadline(s);
        if (!n.isEmpty()) hashes.insert(rcFnv1a64(n));
    };
    add(rawHead);                                  // legacy raw head
    QString deMark = rawHead;
    deMark.remove(QStringLiteral("**"));
    add(deMark);                                   // de-marked whole head
    add(rcGfmCanonicalHeadline(rawHead));          // post-em-dash prose
    if (!boldId.isEmpty()) add(boldId);            // bold-ID label
    return hashes;
}


// kAdapterEmoji* — same byte sequences as roadmapdialog.cpp's
// kEmojiDone/etc. Duplicated locally for the walker's inline-emoji
// prefix strip.
constexpr const char *kAdapterEmojiDone       = "\xE2\x9C\x85";        // ✅
constexpr const char *kAdapterEmojiPlanned    = "\xF0\x9F\x93\x8B";    // 📋
constexpr const char *kAdapterEmojiInProgress = "\xF0\x9F\x9A\xA7";    // 🚧
constexpr const char *kAdapterEmojiConsidered = "\xF0\x9F\x92\xAD";    // 💭

// ANTS-4404 — fence extents are MarkdownScan's (ANTS-3603), never a local
// `trimmed().startsWith("```")` toggle. The hand-rolled test both walkers
// carried was wrong the same two ways ANTS-4403 removed from the migration
// walk: it ignored CommonMark § 4.5, which forbids a backtick in a BACKTICK
// fence's info string precisely so that ```` ```python ```` — how a document
// quotes fence syntax — stays a paragraph (ANTS-3655); and it bounded the
// indent nowhere.
//
// Unlike the migration's, these two feed `insideFenced`, which every
// roadmap_log write op consults. So the cost here was not a short read but a
// WRITE OUTAGE: one qualifying line at this project's own ROADMAP.md:31099
// opened a fence nothing closed, and flip / flip_batch / annotate /
// amend_body / amend_headline then refused anchor_unsafe_context on every
// bullet below it — naming that innocent line as the cause.
QVector<bool> rcFenceExtents(const QStringList &lines) {
    return MarkdownScan::fenceMask(lines);
}

// Opening line of the masked run each line belongs to, -1 outside a fence.
// A maximal masked run is one fenced block, opener .. closer; an unterminated
// opener runs to end-of-input, the same leniency the local scanners had.
// Derived from the mask rather than re-scanned, so the opener rule is stated
// once — the shape ANTS-4403 settled on in roadmapmigrate.cpp's walkSource().
QVector<int> rcFenceOpenerOf(const QVector<bool> &fenced) {
    QVector<int> openerOf(fenced.size(), -1);
    for (int i = 0; i < fenced.size(); ++i) {
        if (!fenced.at(i)) continue;
        int j = i;
        while (j + 1 < fenced.size() && fenced.at(j + 1)) ++j;
        for (int k = i; k <= j; ++k) openerOf[k] = i;
        i = j;
    }
    return openerOf;
}

QVector<GfmBullet> walkGfmBullets(const QStringList &lines) {
    QVector<GfmBullet> out;
    const QVector<bool> fenced = rcFenceExtents(lines);
    const QVector<int>  openerOf = rcFenceOpenerOf(fenced);
    for (int i = 0; i < lines.size(); ++i) {
        const QString &ln = lines.at(i);
        if (!ln.startsWith(QStringLiteral("- [ ]")) &&
            !ln.startsWith(QStringLiteral("- [x]")) &&
            !ln.startsWith(QStringLiteral("- [X]"))) {
            continue;
        }
        GfmBullet b;
        b.firstLine     = i;
        b.headlineLine  = i;
        b.insideFenced  = fenced.value(i);
        b.fenceOpenLine = fenced.value(i) ? openerOf.value(i, -1) : -1;

        // Parse status from checkbox char.
        const QChar cb = ln.size() > 3 ? ln.at(3) : QChar(' ');
        if (cb == QLatin1Char('x') || cb == QLatin1Char('X')) {
            b.status = QStringLiteral("✅");
        } else {
            b.status = QStringLiteral("📋");
        }
        // Head = post-`- [ ] ` strip (6 chars; tolerate missing space).
        QString head = ln.mid(5).trimmed();
        // Inline emoji prefix overrides checkbox state.
        auto tryStrip = [&](const char *emoji, const QString &st) {
            const QString e = QString::fromUtf8(emoji);
            if (head.startsWith(e)) {
                b.status = st;
                head.remove(0, e.size());
                while (!head.isEmpty() && head.front().isSpace())
                    head.remove(0, 1);
                return true;
            }
            return false;
        };
        // Try each emoji in turn — short-circuit on first match.
        // Plain if-else chain (not nested-if) so clang-tidy doesn't
        // mis-read the indentation as a misleading block boundary.
        if      (tryStrip(kAdapterEmojiDone,       QStringLiteral("✅"))) {}
        else if (tryStrip(kAdapterEmojiPlanned,    QStringLiteral("📋"))) {}
        else if (tryStrip(kAdapterEmojiInProgress, QStringLiteral("🚧"))) {}
        else    {  tryStrip(kAdapterEmojiConsidered, QStringLiteral("💭")); }

        QString boldId;
        if (rcExtractBoldId(head, &boldId)) b.boldId = boldId;
        b.headline = head;
        b.anchor   = rcExtractCaretAnchor(ln);

        // Walk continuation lines (2-space indent, non-metadata) to
        // find the last headline-content line. Stops at blank, next
        // bullet, or a metadata key. The headline content line is the
        // anchor injection target.
        static const QRegularExpression rxMeta(QStringLiteral(
            "^\\s+\\*\\*?(Lanes|Kind|Source|Layman|Evidence)"
            ":\\*?\\*?"));
        for (int j = i + 1; j < lines.size(); ++j) {
            const QString &cont = lines.at(j);
            if (cont.trimmed().isEmpty()) break;
            if (cont.startsWith(QStringLiteral("- ")) ||
                cont.startsWith(QStringLiteral("* "))) break;
            if (!cont.startsWith(QLatin1Char(' '))) break;
            if (rxMeta.match(cont).hasMatch()) break;
            b.headlineLine = j;
            // anchor on a continuation line wins over the checkbox
            // line (per spec § Anchor placement, anchor goes on the
            // *last* line of the headline content).
            const QString contAnchor = rcExtractCaretAnchor(cont);
            if (!contAnchor.isEmpty()) b.anchor = contAnchor;
        }
        out.append(b);
    }
    return out;
}

// Apply a status flip + (optional) anchor injection to `lines` in
// place. Returns the byte count written so callers can surface
// bytes_written. The bullet must be located in `lines` already.
//
// statusEmoji is the target Ants emoji ("✅"/"📋"/"🚧"/"💭"); the
// adapter encodes ✅ as `[x]`, the other three as `[ ]` + (for
// 🚧/💭) an inline emoji prefix. Existing inline emoji prefixes are
// stripped before re-emission so the line carries exactly the
// requested status state.
//
// anchorToInject is non-empty iff the locator decided anchor
// injection is required (neither bold-ID nor existing anchor).
void applyGfmFlip(QStringList &lines,
                  const GfmBullet &b,
                  const QString &statusEmoji,
                  const QString &anchorToInject) {
    // 1) Rewrite the checkbox line. The line shape is
    //    `- [ ]<space><possible-inline-emoji><space><headline>`
    // or `- [x]<space><headline>`. Strip the checkbox and any
    // leading status emoji from `head`, then re-emit per
    // `statusEmoji`.
    QString line = lines.at(b.firstLine);
    QString head;
    if (line.size() >= 5) head = line.mid(5);
    // Trim a leading space to normalise — re-added on emit.
    while (!head.isEmpty() && head.front().isSpace()) head.remove(0, 1);
    // Strip any inline status emoji prefix.
    const QStringList kEmojiPrefixes = {
        QString::fromUtf8(kAdapterEmojiDone),
        QString::fromUtf8(kAdapterEmojiPlanned),
        QString::fromUtf8(kAdapterEmojiInProgress),
        QString::fromUtf8(kAdapterEmojiConsidered),
    };
    for (const QString &e : kEmojiPrefixes) {
        if (head.startsWith(e)) {
            head.remove(0, e.size());
            while (!head.isEmpty() && head.front().isSpace())
                head.remove(0, 1);
            break;
        }
    }
    // Build the new line.
    QString rewritten;
    if (statusEmoji == QStringLiteral("✅")) {
        rewritten = QStringLiteral("- [x] ") + head;
    } else if (statusEmoji == QStringLiteral("📋")) {
        rewritten = QStringLiteral("- [ ] ") + head;
    } else {
        // 🚧 / 💭 ride as inline-emoji prefix on an unchecked box.
        rewritten = QStringLiteral("- [ ] ") + statusEmoji +
                    QLatin1Char(' ') + head;
    }
    lines[b.firstLine] = rewritten;

    // 2) Inject the anchor on b.headlineLine. If headlineLine ==
    //    firstLine the rewritten line is the target; otherwise the
    //    continuation line is. Anchor is " ^prefix-NNNN" appended at
    //    the end. Whitespace before the caret is the single space
    //    spec § Anchor placement requires.
    if (anchorToInject.isEmpty()) return;
    const int targetIdx = b.headlineLine;
    QString target = lines.at(targetIdx);
    // Trim trailing whitespace before appending the anchor.
    while (!target.isEmpty() && target.back().isSpace()) target.chop(1);
    target += QLatin1Char(' ') + QStringLiteral("^") + anchorToInject;
    lines[targetIdx] = target;
}


// Match the bracket-ID token that immediately follows the status
// emoji + space. Anchored loosely; the walker checks the prefix
// explicitly before invoking this.
// ANTS-2051 — accept a lowercase / mixed-case leading letter so the
// write parser recognises the same bracket ids the READ path does.
// The read path's shared idTokenPattern() (roadmapdialog.cpp) is
// `[A-Za-z][A-Za-z0-9_-]*-\d+`; the old `[A-Z]…` here rejected
// lowercase project prefixes like `[mame-curator-1065]`, so flip /
// flip_batch / append refused a markerless ants-v1 roadmap that
// roadmap_query reads fine (MAME Curator HIGH, cross-session 2026-06-10).
// Keep the {1,8} digit bound as a sanity ceiling.
static const QRegularExpression rxAntsV1IdBracket(
    // ANTS-3492 — digit-led-but-letter-containing prefix (3D_E-0042).
    QStringLiteral("\\[((?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\\d{1,8})\\]"));

QVector<AntsV1Bullet> walkAntsV1Bullets(const QStringList &lines) {
    QVector<AntsV1Bullet> out;
    const QVector<bool> fenced = rcFenceExtents(lines);
    const QVector<int>  openerOf = rcFenceOpenerOf(fenced);
    auto matchEmojiAt = [](const QString &line, int pos) -> QString {
        const QString done = QString::fromUtf8(kAdapterEmojiDone);
        const QString plan = QString::fromUtf8(kAdapterEmojiPlanned);
        const QString prog = QString::fromUtf8(kAdapterEmojiInProgress);
        const QString cons = QString::fromUtf8(kAdapterEmojiConsidered);
        if (line.mid(pos, done.size()) == done) return done;
        if (line.mid(pos, plan.size()) == plan) return plan;
        if (line.mid(pos, prog.size()) == prog) return prog;
        if (line.mid(pos, cons.size()) == cons) return cons;
        return QString();
    };
    for (int i = 0; i < lines.size(); ++i) {
        const QString &ln = lines.at(i);
        if (!ln.startsWith(QStringLiteral("- "))) continue;
        // Status emoji sits at position 2 (just past "- ").
        const QString emoji = matchEmojiAt(ln, 2);
        if (emoji.isEmpty()) continue;
        const int afterEmoji = 2 + emoji.size();
        // Expect a space, then optionally a [PROJ-NNNN] id bracket.
        if (ln.size() <= afterEmoji + 1 ||
            ln.at(afterEmoji) != QLatin1Char(' ')) {
            continue;
        }
        AntsV1Bullet b;
        b.firstLine     = i;
        b.insideFenced  = fenced.value(i);
        b.fenceOpenLine = fenced.value(i) ? openerOf.value(i, -1) : -1;
        // ANTS-2059 — the id bracket is OPTIONAL. A fully id-less bullet
        // (`- 📋 **Headline.**`, no bracket at all) is still a real
        // ants-v1 bullet: the READ path (parseBullets) synthesises an id
        // for it and roadmap_query reads it, so the write path must parse
        // it too — flip/flip_batch/annotate resolve ants-v1 bullets by
        // headline + line_range, neither of which needs an id. When no
        // valid bracket follows the emoji, leave b.id empty and take the
        // whole post-emoji remainder as the headline. (ANTS-2051 relaxed
        // only the bracket's leading-letter case but kept the bracket
        // itself mandatory, leaving id-less roadmaps read-only to flip.)
        int headStart = afterEmoji + 1;
        if (ln.at(afterEmoji + 1) == QLatin1Char('[')) {
            const QRegularExpressionMatch m =
                rxAntsV1IdBracket.match(ln, afterEmoji);
            if (m.hasMatch() && m.capturedStart(0) == afterEmoji + 1) {
                b.id      = m.captured(1);
                headStart = m.capturedEnd(0);
            }
        }
        if      (emoji == QString::fromUtf8(kAdapterEmojiDone))
            b.status = QStringLiteral("✅");
        else if (emoji == QString::fromUtf8(kAdapterEmojiPlanned))
            b.status = QStringLiteral("📋");
        else if (emoji == QString::fromUtf8(kAdapterEmojiInProgress))
            b.status = QStringLiteral("🚧");
        else
            b.status = QStringLiteral("💭");
        // Headline: post-id text (or post-emoji when id-less), strip
        // leading space + bold wrapper.
        QString head = ln.mid(headStart).trimmed();
        // ANTS-4109 — the bold-ID form (`- 📋 **LOTTO-0019** …`). The
        // bracket above was the only id shape this walker knew, so on a
        // roadmap whose ids are bold every bullet came back id-less and
        // roadmap_log's `id` locator matched zero of them — while
        // roadmap_query resolved the same id fine, because the READ path
        // extracts it (roadmapparse.cpp fillBulletRecord's native branch).
        // Mirror that branch's rule exactly: adopt the leading bold token
        // only when it is ID-SHAPED (a single whitespace-free token), so a
        // bold-prose narrator (`- 🚧 **In-progress thing.**`) stays id-less.
        // Additive — the headline is left as it was, so a caller already
        // locating these bullets by `headline` keeps working.
        if (b.id.isEmpty()) {
            QString boldCand;
            if (rcExtractBoldId(head, &boldCand)) {
                static const QRegularExpression rxIdShaped(
                    QStringLiteral("^[A-Za-z][A-Za-z0-9_.-]*$"));
                if (rxIdShaped.match(boldCand).hasMatch()) b.id = boldCand;
            }
        }
        if (head.startsWith(QStringLiteral("**"))) {
            head.remove(0, 2);
            const int closeIdx = head.indexOf(QStringLiteral("**"));
            if (closeIdx >= 0) head = head.left(closeIdx);
        }
        b.headline = head.trimmed();
        out.append(b);
    }
    return out;
}

// Apply a status-emoji swap in place. No anchor injection (ants-v1
// bullets already have the canonical [PREFIX-NNNN] id), no counter
// consumption. Just replace the emoji byte sequence.
void applyAntsV1Flip(QStringList &lines, const AntsV1Bullet &b,
                     const QString &targetEmoji) {
    QString line = lines.at(b.firstLine);
    const QString done = QString::fromUtf8(kAdapterEmojiDone);
    const QString plan = QString::fromUtf8(kAdapterEmojiPlanned);
    const QString prog = QString::fromUtf8(kAdapterEmojiInProgress);
    const QString cons = QString::fromUtf8(kAdapterEmojiConsidered);
    QString oldEmoji;
    if      (line.mid(2, done.size()) == done) oldEmoji = done;
    else if (line.mid(2, plan.size()) == plan) oldEmoji = plan;
    else if (line.mid(2, prog.size()) == prog) oldEmoji = prog;
    else if (line.mid(2, cons.size()) == cons) oldEmoji = cons;
    if (oldEmoji.isEmpty()) return;  // walker guarantees one of the four
    line.remove(2, oldEmoji.size());
    line.insert(2, targetEmoji);
    lines[b.firstLine] = line;
}

}  // namespace rcdetail

RemoteControl::RemoteControl(MainWindow *main, QObject *parent)
    : QObject(parent), m_main(main) {}

void RemoteControl::setVerifyTrustClient(
        std::unique_ptr<VerifyTrust::Client> c) {
    m_verifyTrustClient = std::move(c);
}

RemoteControl::~RemoteControl() {
    if (m_server) {
        m_server->close();
    }
}

QString RemoteControl::defaultSocketPath() {
    // Override wins unconditionally — lets the user script
    // multi-instance setups without touching the source.
    const QByteArray override = qgetenv("ANTS_REMOTE_SOCKET");
    if (!override.isEmpty()) return QString::fromLocal8Bit(override);

    const QString xdg = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (!xdg.isEmpty()) {
        return xdg + "/ants-terminal.sock";
    }
    // ANTS-1365 — /tmp fallback wraps the socket in a per-user 0700
    // subdir (`/tmp/ants-<uid>/`) so a same-UID rogue can't pre-create
    // the socket path as a regular file or symlink. The subdir is
    // brought up by `ensureSocketDir` in `start()` before listen().
    return QStringLiteral("/tmp/ants-%1/ants-terminal.sock")
        .arg(::getuid());
}

bool RemoteControl::start() {
    if (m_server) return true;

    const QString path = defaultSocketPath();
    // ANTS-1365 — bring up the socket-containing directory at 0700,
    // verified to be owned by us, before listen(). Replaces the
    // previous `QDir::mkpath` (which always creates with 0755 on
    // POSIX and offers no ownership/mode verification). On any
    // failure — wrong owner, wrong mode, inherited symlink, mkdir
    // failure — return false and disable rc/MCP for this process.
    // The XDG primary path is already a systemd-managed 0700 dir,
    // so this is a no-op there; the /tmp fallback is the real
    // beneficiary.
    const QString socketDir = QFileInfo(path).absolutePath();
    if (!ensureSocketDir(socketDir)) {
        ANTS_LOG(DebugLog::Network,
            "remote-control: socket dir %s unavailable; "
            "remote-control disabled for this process",
            qUtf8Printable(socketDir));
        return false;
    }

    m_server = new QLocalServer(this);
    // Restrict access to the owning user — matches the hook/MCP
    // sockets' posture. Must be set before listen() on Unix; Qt
    // enforces this on the socket itself.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

    // If a stale socket file exists (previous crash didn't clean up),
    // remove it. `removeServer` is a no-op if no socket exists and
    // succeeds when the path exists but is not actively bound.
    // If another live instance holds the lock, listen() fails and
    // we skip the takeover (see outer `if` below).
    if (!m_server->listen(path)) {
        if (safeToUnlinkLocalSocket(path)) {
            QLocalServer::removeServer(path);
        } else {
            ANTS_LOG(DebugLog::Network,
                "remote-control: refusing to unlink %s — not a socket "
                "owned by this user (possible symlink or foreign file); "
                "remote-control disabled for this process",
                qUtf8Printable(path));
            delete m_server;
            m_server = nullptr;
            return false;
        }
        if (!m_server->listen(path)) {
            ANTS_LOG(DebugLog::Network,
                "remote-control: listen(%s) failed — another instance "
                "may own the socket; remote-control disabled for this "
                "process", qUtf8Printable(path));
            delete m_server;
            m_server = nullptr;
            return false;
        }
    }
    setOwnerOnlyPerms(path);

    connect(m_server, &QLocalServer::newConnection,
            this, &RemoteControl::onNewConnection);
    ANTS_LOG(DebugLog::Network,
        "remote-control: listening on %s", qUtf8Printable(path));
    return true;
}

void RemoteControl::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QLocalSocket *socket = m_server->nextPendingConnection();
        // ANTS-1132 — SO_PEERCRED UID match. The trust-model comment
        // at the top of this file claims "UID-scoped + 0700 perms +
        // lstat-checked S_ISSOCK"; UserAccessOption + safeToUnlink
        // already cover the file-side guarantees, but the peer side
        // needs explicit getsockopt(SO_PEERCRED) to enforce that the
        // connecting process is the same UID. Defense in depth — on
        // Linux with 0700 socket perms, the kernel already gates
        // connect(2) on the file ACL, but if the socket path is
        // ever moved (ANTS_REMOTE_SOCKET env override, abstract
        // socket migration), the file ACL stops applying and only
        // the peer-cred check holds the line.
        // ANTS-1797 — fail CLOSED: if the socket fd is unavailable we cannot
        // verify the peer UID, so the connection must be refused rather than
        // served unauthenticated. (A bare `if (fd >= 0)` guard would skip the
        // whole check on fd<0 — exactly the moved-socket scenario the comment
        // above names as the case where only peer-cred holds the line.)
        const qintptr fd = socket->socketDescriptor();
        bool peerVerified = false;
        if (fd >= 0) {
            struct ucred cred{};
            socklen_t len = sizeof(cred);
            const int gscRet = ::getsockopt(static_cast<int>(fd), SOL_SOCKET,
                                            SO_PEERCRED, &cred, &len);
            if (gscRet == 0 && len == sizeof(cred) && cred.uid == ::getuid()) {
                peerVerified = true;
            } else {
                // getsockopt failed OR truncated struct OR UID mismatch.
                // Log strerror on the syscall-failure path so a zero-init
                // cred.uid isn't reported as a fake "root tried to connect".
                if (gscRet != 0 || len != sizeof(cred))
                    ANTS_LOG(DebugLog::Network,
                        "remote-control: SO_PEERCRED failed (%s) — disconnecting",
                        std::strerror(errno));
                else
                    ANTS_LOG(DebugLog::Network,
                        "remote-control: peer UID mismatch "
                        "(peer=%d self=%d) — disconnecting",
                        static_cast<int>(cred.uid),
                        static_cast<int>(::getuid()));
            }
        } else {
            ANTS_LOG(DebugLog::Network,
                "remote-control: no socket fd for peer-cred check — "
                "disconnecting (fail-closed)");
        }
        if (!peerVerified) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        // ANTS-1132 — slow-loris defence. Cap idle time per
        // connection at 5 seconds. Each message is one-shot; if
        // a peer hasn't sent a complete request within the
        // window, abort.
        QTimer *idleTimer = new QTimer(socket);
        idleTimer->setSingleShot(true);
        idleTimer->setInterval(5000);
        connect(idleTimer, &QTimer::timeout, socket,
                [socket]() { socket->abort(); });
        idleTimer->start();
        // Line-buffer incoming data. Each connection handles exactly
        // one request/response round-trip today — simpler than a
        // persistent-session protocol and good enough for the full
        // Kitty command set (which is also one-shot).
        socket->setProperty("_buf", QByteArray());
        // ANTS-2202 — re-entrancy latch, mirroring the MCP twin
        // (claudeintegration.cpp). Once a complete line is dispatched, _handled
        // blocks a second readyRead (e.g. fired from inside a nested event loop)
        // from re-dispatching buffered bytes.
        socket->setProperty("_handled", false);
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket, idleTimer]() {
            if (socket->property("_handled").toBool()) return;
            QByteArray buf = socket->property("_buf").toByteArray();
            buf += socket->readAll();
            // Bound the in-memory buffer for defence-in-depth against
            // a malicious client on the same machine. 1 MB is far
            // more than any realistic Kitty rc_protocol envelope.
            if (buf.size() > 1 * 1024 * 1024) {
                socket->disconnectFromServer();
                return;
            }
            socket->setProperty("_buf", buf);

            int nlIdx = buf.indexOf('\n');
            if (nlIdx < 0) return;  // partial line, wait for more

            // ANTS-2202 — a complete line is in hand. Latch _handled and drop the
            // consumed line from _buf so a re-entrant readyRead (should a future
            // RC verb pump a nested event loop) can't re-dispatch it.
            socket->setProperty("_handled", true);
            socket->setProperty("_buf", buf.mid(nlIdx + 1));

            // ANTS-2026 — stop the slow-loris idle timer BEFORE dispatching. No
            // current RC verb runs a nested event loop, but if one is added a
            // still-armed timer could fire timeout -> socket->abort() ->
            // disconnected -> deleteLater(), and that deleteLater would be
            // processed by the nested loop, freeing this socket before the write
            // below. Defensive, and parity with the MCP path (ANTS-2101).
            idleTimer->stop();

            const QByteArray line = buf.left(nlIdx);
            QJsonParseError err;
            QJsonDocument req = QJsonDocument::fromJson(line, &err);
            QJsonDocument resp;
            // ANTS-2026 — defence in depth: the peer can still disconnect during
            // a nested-loop dispatch, freeing the socket via the disconnected ->
            // deleteLater chain. A QPointer lets the post-dispatch write bail
            // instead of touching a dangling pointer.
            QPointer<QLocalSocket> guard(socket);
            if (err.error != QJsonParseError::NoError || !req.isObject()) {
                QJsonObject e;
                e["ok"] = false;
                e["error"] = QStringLiteral("invalid JSON: %1")
                    .arg(err.errorString());
                resp = QJsonDocument(e);
            } else {
                resp = dispatch(req.object());
            }
            if (!guard || socket->state() != QLocalSocket::ConnectedState)
                return;
            socket->write(resp.toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
            socket->disconnectFromServer();
        });
        connect(socket, &QLocalSocket::disconnected,
                socket, &QLocalSocket::deleteLater);
    }
}

QJsonDocument RemoteControl::dispatch(const QJsonObject &req) {
    const QString cmd = req.value("cmd").toString();
    // ANTS-1176: per-verb structured log so a same-UID-attack
    // post-mortem has a record. Deliberately does NOT include the
    // payload itself (text/cwd/command bodies can carry secrets);
    // size + tab + stripped-bytes count are the diagnostic axes.
    const int tabId = req.value("tab").toInt(-1);
    const int textBytes = req.value("text").toString().size();
    // ANTS-2119 M2 — record whether the control-char filter bypass was
    // requested (send-text / launch / new-tab honour raw:true). Without it a
    // post-mortem of a same-UID attack can't distinguish a benign filtered send
    // from a raw control-byte injection — the exact threat this log exists for.
    const int rawBypass = req.value("raw").toBool(false) ? 1 : 0;
    ANTS_LOG(DebugLog::Network,
             "rc dispatch cmd=%s tab=%d text_bytes=%d raw=%d",
             qUtf8Printable(cmd), tabId, textBytes, rawBypass);
    if (cmd == QLatin1String("ls")) {
        return cmdLs();
    }
    if (cmd == QLatin1String("send-text")) {
        return cmdSendText(req);
    }
    if (cmd == QLatin1String("new-tab")) {
        return cmdNewTab(req);
    }
    if (cmd == QLatin1String("select-window")) {
        return cmdSelectWindow(req);
    }
    if (cmd == QLatin1String("set-title")) {
        return cmdSetTitle(req);
    }
    if (cmd == QLatin1String("get-text")) {
        return cmdGetText(req);
    }
    if (cmd == QLatin1String("launch")) {
        return cmdLaunch(req);
    }
    if (cmd == QLatin1String("tab-list")) {
        return cmdTabList();
    }
    // ANTS-2049 — e2e inject verbs (socket-only). Gated behind m_e2eMode: on a
    // normal binary (no --e2e) the gate is false and every inject verb refuses
    // with code:"e2e_disabled" and posts no event / does no resize/grab
    // (INV-1). The verbs carry new argument shapes reached via --remote-json.
    if (cmd == QLatin1String("inject-key")
            || cmd == QLatin1String("inject-click")
            || cmd == QLatin1String("resize-window")
            || cmd == QLatin1String("grab-image")) {
        if (!m_e2eMode) {
            QJsonObject o;
            o["ok"]    = false;
            o["code"]  = QStringLiteral("e2e_disabled");
            o["error"] = cmd + QStringLiteral(
                ": refused — instance not launched with --e2e");
            return QJsonDocument(o);
        }
        if (cmd == QLatin1String("inject-key"))     return cmdInjectKey(req);
        if (cmd == QLatin1String("inject-click"))   return cmdInjectClick(req);
        if (cmd == QLatin1String("resize-window"))  return cmdResizeWindow(req);
        return cmdGrabImage(req);
    }
    if (cmd == QLatin1String("roadmap-query")) {
        // ANTS-1247: thread `req` through so `--remote roadmap-query
        // status=active` (if a future --remote-status flag lands)
        // reaches the filter.
        return cmdRoadmapQuery(req);
    }
    if (cmd == QLatin1String("workspace-search")) {
        // ANTS-1248-INV-4: IPC dispatch entry for the ripgrep wrapper.
        return cmdWorkspaceSearch(req);
    }
    if (cmd == QLatin1String("file-outline")) {
        // ANTS-1249: IPC dispatch entry for the file outline scanner.
        return cmdFileOutline(req);
    }
    if (cmd == QLatin1String("find-definition")) {
        // ANTS-1303: IPC dispatch entry for the symbol-definition scanner.
        return cmdFindDefinition(req);
    }
    if (cmd == QLatin1String("find-caller")) {
        // ANTS-1303: IPC dispatch entry for the symbol-caller scanner.
        return cmdFindCaller(req);
    }
    if (cmd == QLatin1String("similar-code")) {
        // ANTS-1305: IPC dispatch entry for the shape matcher.
        return cmdSimilarCode(req);
    }
    if (cmd == QLatin1String("git-state")) {
        // ANTS-1250: IPC dispatch entry for the consolidated git tool.
        // Inner op-switch lives in cmdGitState.
        return cmdGitState(req);
    }
    if (cmd == QLatin1String("subsystem")) {
        // ANTS-1251: IPC dispatch entry for the consolidated subsystem
        // tool. Inner op-switch lives in cmdSubsystem.
        return cmdSubsystem(req);
    }
    QJsonObject e;
    e["ok"] = false;
    e["error"] = QStringLiteral("unknown command: %1").arg(cmd);
    return QJsonDocument(e);
}

