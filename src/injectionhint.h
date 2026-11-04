// Copyright (c) 2026 Anthony Schemel
// SPDX-License-Identifier: GPL-3.0-or-later

// ANTS-5184 — name the instruction-like phrases in terminal text bound for
// Claude, so a reply can warn that the text may be a prompt injection.
// A signal, not a block: the text itself is never changed.
//
// Contract: tests/features/mcp_prompt_injection_hint/spec.md.
//
// Header-only, like secretredact.h beside it.

#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace InjectionHint {

namespace detail {

inline const std::vector<std::pair<QString, QRegularExpression>> &rules()
{
    constexpr auto ci = QRegularExpression::CaseInsensitiveOption;
    static const std::vector<std::pair<QString, QRegularExpression>> kRules = {
        { QStringLiteral("ignore_instructions"),
          QRegularExpression(QStringLiteral(
              "\\b(?:ignore|disregard|forget|override)\\s+"
              "(?:(?:all|any|the|your)\\s+)?"
              "(?:previous|prior|above|earlier|preceding|system)\\s+"
              "(?:instructions?|prompts?|rules|directions|messages|context)\\b"), ci) },
        { QStringLiteral("new_instructions"),
          QRegularExpression(QStringLiteral(
              "\\b(?:new|updated|real)\\s+instructions\\s*:"
              "|\\byou\\s+are\\s+now\\s+an?\\b"), ci) },
        { QStringLiteral("chat_template"),
          QRegularExpression(QStringLiteral(
              "<\\|im_start\\|>|<\\|im_end\\|>|\\[/?INST\\]|<<SYS>>|</?system>"), ci) },
        { QStringLiteral("conceal_from_user"),
          QRegularExpression(QStringLiteral(
              "\\b(?:do\\s+not|don't|never)\\s+(?:tell|inform|show)\\s+the\\s+user\\b"), ci) },
    };
    return kRules;
}

} // namespace detail

// The kinds of instruction-like phrase found in `text`, sorted, each once.
inline QStringList scan(const QString &text)
{
    QStringList found;
    if (text.isEmpty()) return found;
    for (const auto &[kind, re] : detail::rules())
        if (re.match(text).hasMatch()) found << kind;
    found.sort();
    return found;
}

} // namespace InjectionHint
