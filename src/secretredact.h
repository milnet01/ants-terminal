// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// OWASP LLM06 defense: regex-scrub well-known secret shapes from any
// string before it leaves the process via an AI request.
//
// Contract: tests/features/ai_context_redaction/spec.md.
//
// Header-only because there's exactly one consumer today (AiDialog) and
// a .cpp would buy us nothing the compiler's COMDAT dedup doesn't
// already give us. The regex objects are static-initialised once per
// process (C++11 magic-statics).

#pragma once

#include <QRegularExpression>
#include <QString>

#include <algorithm>
#include <vector>

namespace SecretRedact {

struct Result {
    QString text;
    int redactedCount = 0;
};

namespace detail {

struct Rule {
    QRegularExpression re;
    QString kind;
    // Which capture group's range is replaced with `[REDACTED:<kind>]`.
    // 0 = whole match (the default — used when the whole shape is secret,
    // e.g. a GitHub PAT). For patterns whose literal prefix should be
    // kept (e.g. "Bearer ...", "api_key=..."), the relevant capture
    // group narrows the redaction to just the sensitive substring.
    int captureGroup = 0;
};

inline const std::vector<Rule> &rules()
{
    // Priority order (earlier beats later on overlap at the same start):
    //   1. Multi-line blocks (PEM) first so their matches swallow any
    //      sub-matches inside the block body.
    //   2. High-specificity prefixed tokens (GitHub fine-grained PAT,
    //      github_pat_, sk-ant-, sk-proj-) before their less-specific
    //      cousins (ghp_, sk-).
    //   3. Concrete provider shapes (AWS, Slack, Stripe, JWT) next.
    //   4. Context-wrapping patterns (Bearer header, generic secret
    //      assignment) last — the specific shapes they often wrap
    //      should win on the narrow value range.
    static const std::vector<Rule> kRules = {
        // PEM private-key block — multi-line. DotMatchesEverythingOption
        // lets `.` cross newlines; the non-greedy `.*?` is anchored by
        // the END marker so we don't swallow neighbouring blocks.
        { QRegularExpression(
              QStringLiteral("-----BEGIN [^\\-\\n]*PRIVATE KEY-----.*?-----END [^\\-\\n]*PRIVATE KEY-----"),
              QRegularExpression::DotMatchesEverythingOption),
          QStringLiteral("private_key"), 0 },

        // GitHub fine-grained PAT: `github_pat_` prefix + 82 chars.
        // Must precede the classic `ghp_` pattern so the longer label
        // is applied when both could match (they can't, but it keeps
        // priority ordering coherent across adjacent shapes).
        { QRegularExpression(QStringLiteral("\\bgithub_pat_[A-Za-z0-9_]{82}\\b")),
          QStringLiteral("github_fine_grained_pat"), 0 },
        // GitHub classic PAT.
        { QRegularExpression(QStringLiteral("\\bghp_[A-Za-z0-9]{36}\\b")),
          QStringLiteral("github_pat"), 0 },
        // GitHub OAuth access token.
        { QRegularExpression(QStringLiteral("\\bgho_[A-Za-z0-9]{36}\\b")),
          QStringLiteral("github_oauth"), 0 },
        // GitHub app installation / user-to-server / refresh.
        { QRegularExpression(QStringLiteral("\\bgh[sur]_[A-Za-z0-9]{36}\\b")),
          QStringLiteral("github_app"), 0 },
        // GitLab personal/project/group access token: `glpat-` + 20+ chars
        // (length has grown across GitLab versions; lower-bound on the opaque
        // payload rather than an exact count). indie-review #8 (2026-06-26).
        { QRegularExpression(QStringLiteral("\\bglpat-[A-Za-z0-9_\\-]{20,}")),
          QStringLiteral("gitlab_pat"), 0 },

        // AWS access key ID. AKIA = long-lived, ASIA = STS temp
        // credential. 16 uppercase alphanum suffix is the documented
        // AWS format.
        { QRegularExpression(QStringLiteral("\\b(?:AKIA|ASIA)[0-9A-Z]{16}\\b")),
          QStringLiteral("aws_access_key"), 0 },

        // Anthropic API key. 90+ chars of opaque payload after the
        // `sk-ant-` prefix. Must precede the OpenAI-legacy `sk-` shape
        // so the Anthropic label wins.
        { QRegularExpression(QStringLiteral("\\bsk-ant-[A-Za-z0-9_\\-]{90,}")),
          QStringLiteral("anthropic"), 0 },
        // OpenAI project-scoped key (current format). 80+ chars after
        // `sk-proj-`. Must precede the legacy `sk-` shape.
        { QRegularExpression(QStringLiteral("\\bsk-proj-[A-Za-z0-9_\\-]{80,}")),
          QStringLiteral("openai_project"), 0 },
        // OpenAI legacy key: `sk-` + 48 alphanum. The priority-resolve
        // step drops this when `sk-ant-` or `sk-proj-` already covered
        // the range.
        { QRegularExpression(QStringLiteral("\\bsk-[A-Za-z0-9]{48}\\b")),
          QStringLiteral("openai"), 0 },

        // Slack tokens: xoxb/xoxa/xoxp/xoxr/xoxo/xoxs.
        { QRegularExpression(QStringLiteral("\\bxox[abpros]-[A-Za-z0-9-]{10,}")),
          QStringLiteral("slack"), 0 },
        // Slack app-level token (`xapp-1-...`) — distinct prefix from the
        // `xox*-` workspace tokens above, so it needs its own rule.
        // indie-review #8 (2026-06-26).
        { QRegularExpression(QStringLiteral("\\bxapp-[A-Za-z0-9-]{10,}")),
          QStringLiteral("slack_app"), 0 },

        // Google API keys — `AIza` prefix + 35 chars (39 total).
        // Documented shape per Google Cloud's API-key format.
        // ANTS-1184.
        { QRegularExpression(QStringLiteral("\\bAIza[A-Za-z0-9_\\-]{35}\\b")),
          QStringLiteral("google_api_key"), 0 },
        // Google OAuth 2.0 access token — `ya29.` prefix + opaque
        // base64url-ish payload. Length varies; 20+ is a safe lower
        // bound that doesn't false-positive on `ya29.` words.
        // ANTS-1184.
        { QRegularExpression(QStringLiteral("\\bya29\\.[A-Za-z0-9_\\-]{20,}")),
          QStringLiteral("google_oauth"), 0 },

        // Stripe live secret/restricted keys. 24+ alphanum payload.
        { QRegularExpression(QStringLiteral("\\b(?:sk|rk)_live_[A-Za-z0-9]{24,}\\b")),
          QStringLiteral("stripe"), 0 },

        // JWT: three base64url segments separated by dots. First two
        // segments start with `eyJ` (base64 of `{"`). Third segment is
        // the signature.
        { QRegularExpression(QStringLiteral(
              "\\beyJ[A-Za-z0-9_\\-]{10,}\\.eyJ[A-Za-z0-9_\\-]{10,}\\.[A-Za-z0-9_\\-]{10,}\\b")),
          QStringLiteral("jwt"), 0 },

        // Bearer header — redact the whole `Bearer <token>` so the
        // output preserves the context word ("Authorization:
        // [REDACTED:bearer]") rather than a half-scrubbed
        // "Authorization: Bearer [REDACTED:bearer]".
        { QRegularExpression(QStringLiteral("\\b[Bb]earer\\s+\\S{20,}")),
          QStringLiteral("bearer"), 0 },

        // Generic secret assignment. Redacts only the value (capture
        // group 1) so `API_KEY=xxx` becomes `API_KEY=[REDACTED:...]`,
        // preserving the variable name. The value character class
        // (`A-Za-z0-9+/=_\-.`) deliberately excludes whitespace,
        // quotes, and shell metacharacters so we don't slurp the rest
        // of a command line.
        { QRegularExpression(QStringLiteral(
              "(?i)(?:api[_-]?key|token|password|passwd|secret)\\s*[:=]\\s*['\"]?([A-Za-z0-9+/=_\\-\\.]{8,})['\"]?")),
          QStringLiteral("generic_secret"), 1 },
    };
    return kRules;
}

} // namespace detail

inline Result scrub(const QString &input)
{
    Result out{input, 0};
    if (input.isEmpty()) return out;

    struct Match {
        int start;
        int length;
        int priority; // lower = higher priority; ties lose to later
        QString kind;
    };

    std::vector<Match> matches;
    const auto &rr = detail::rules();
    for (std::size_t i = 0; i < rr.size(); ++i) {
        const auto &rule = rr[i];
        auto it = rule.re.globalMatch(input);
        while (it.hasNext()) {
            const auto m = it.next();
            const int s = m.capturedStart(rule.captureGroup);
            const int l = m.capturedLength(rule.captureGroup);
            if (s < 0 || l <= 0) continue;
            matches.push_back({s, l, static_cast<int>(i), rule.kind});
        }
    }

    if (matches.empty()) return out;

    // Sort by start ascending; at the same start, higher priority wins.
    std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        if (a.start != b.start) return a.start < b.start;
        return a.priority < b.priority;
    });

    // Drop overlapping matches. A later match whose start falls inside
    // an already-kept range is discarded. Priority is encoded in the
    // sort order above, so at equal starts the best-label wins.
    std::vector<Match> kept;
    kept.reserve(matches.size());
    int coveredEnd = 0;
    for (const auto &m : matches) {
        if (m.start < coveredEnd) continue;
        kept.push_back(m);
        coveredEnd = m.start + m.length;
    }

    QString result;
    result.reserve(input.size());
    int cursor = 0;
    for (const auto &m : kept) {
        if (m.start > cursor) result.append(input.mid(cursor, m.start - cursor));
        result.append(QStringLiteral("[REDACTED:"));
        result.append(m.kind);
        result.append(QLatin1Char(']'));
        cursor = m.start + m.length;
    }
    if (cursor < input.size()) result.append(input.mid(cursor));

    out.text = std::move(result);
    out.redactedCount = static_cast<int>(kept.size());
    return out;
}

} // namespace SecretRedact
