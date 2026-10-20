#pragma once

// function_sizes (ANTS-5787) — the longest functions in the given paths, as a
// ranked shortlist for a readability review. A long function is not a
// finding by itself; the reviewer decides. Contract:
// tests/features/mcp_function_sizes/spec.md. Its own translation unit so a
// test links it without RemoteControl (docs/standards/mcp-tools.md § Tests).

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace FunctionSizes {

inline constexpr int kDefaultTop = 20;
inline constexpr int kMaxTop = 500;

// One measured function. Lines are 1-based and inclusive: `bodyStart` is the
// line FileOutline reports for the symbol, `bodyEnd` the closing brace, or
// for Python the body's last non-blank line.
struct Function {
    QString symbol;
    int bodyStart = 0;
    int bodyEnd = 0;
    int nestingDepth = 0;
};

// Measures every function FileOutline finds in `lines` of `absPath`. A
// declaration with no body is skipped; one whose end cannot be found is
// counted in `*unmeasured`.
QList<Function> measure(const QString &absPath, const QStringList &lines,
                        int *unmeasured);

// The whole call below the resolved project root: the envelope, or a
// refusal {ok:false, code, error}.
QJsonObject run(const QString &rootCanonical, const QJsonObject &args);

} // namespace FunctionSizes
