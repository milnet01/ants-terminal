// ANTS-6362 / ANTS-6366 — regex scrapes over whole source files.
//
// std::regex's matcher recurses and allocates per character: over a
// multi-megabyte source it took tens of seconds under ASan, against a 60 s
// test limit. PCRE2, through QRegularExpression, does the same search far
// faster.
//
// The haystack is read as Latin-1, one char per byte, so every position and
// length below is a byte offset into the std::string the caller holds. The
// patterns are ASCII, and QRegularExpression's \w and \b are ASCII-only by
// default, so a UTF-8 sequence in the source matches as it did under
// std::regex.
//
// An invalid pattern fails the test rather than reading as "no match", which
// would pass a negative check.

#pragma once

#include <QRegularExpression>
#include <QString>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ants_test {

inline QRegularExpression srcRe(const char *pattern) {
    QRegularExpression re(QString::fromLatin1(pattern));
    if (!re.isValid())
        ADD_FAILURE() << "bad pattern " << pattern << ": "
                      << re.errorString().toStdString();
    return re;
}

inline QString srcText(const std::string &hay) {
    return QString::fromLatin1(hay.data(), static_cast<qsizetype>(hay.size()));
}

inline bool srcSearch(const std::string &hay, const char *pattern) {
    return srcRe(pattern).match(srcText(hay)).hasMatch();
}

inline long srcCount(const std::string &hay, const char *pattern) {
    long n = 0;
    auto it = srcRe(pattern).globalMatch(srcText(hay));
    while (it.hasNext()) { it.next(); ++n; }
    return n;
}

// One match: byte offset, byte length, and capture groups (group 0 is the
// whole match; an unmatched group is "").
struct SrcMatch {
    std::size_t pos = 0;
    std::size_t len = 0;
    std::vector<std::string> groups;
};

inline std::vector<SrcMatch> srcMatches(const std::string &hay,
                                        const char *pattern) {
    const QRegularExpression re = srcRe(pattern);
    std::vector<SrcMatch> out;
    auto it = re.globalMatch(srcText(hay));
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        SrcMatch s;
        s.pos = static_cast<std::size_t>(m.capturedStart());
        s.len = static_cast<std::size_t>(m.capturedLength());
        for (int g = 0; g <= re.captureCount(); ++g)
            s.groups.push_back(m.captured(g).toLatin1().toStdString());
        out.push_back(std::move(s));
    }
    return out;
}

// The first match, written to *out when given; false when there is none.
inline bool srcFirst(const std::string &hay, const char *pattern,
                     SrcMatch *out = nullptr) {
    const QRegularExpression re = srcRe(pattern);
    const QRegularExpressionMatch m = re.match(srcText(hay));
    if (!m.hasMatch()) return false;
    if (out) {
        out->pos = static_cast<std::size_t>(m.capturedStart());
        out->len = static_cast<std::size_t>(m.capturedLength());
        out->groups.clear();
        for (int g = 0; g <= re.captureCount(); ++g)
            out->groups.push_back(m.captured(g).toLatin1().toStdString());
    }
    return true;
}

}  // namespace ants_test
