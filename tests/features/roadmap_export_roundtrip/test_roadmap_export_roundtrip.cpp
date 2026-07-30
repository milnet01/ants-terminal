// Feature-conformance test for ANTS-3761. Contract:
// tests/features/roadmap_export_roundtrip/spec.md
//
// Currently covers INV-19 (RFC 8785 conformance). INV-1, 2, 5, 12, 13 and 18
// join this file as the export writer lands — § 6 of the spec assigns all
// seven to this one directory.
//
// The vectors are the RFC's own, committed under vectors/. Measuring the
// writer against material we did not author is the entire point: INV-1
// compares the writer against itself, so any deterministic writer passes it.

#include <gtest/gtest.h>

#include "jsoncanonical.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstring>

#ifndef ANTS_JCS_VECTOR_DIR
#  error "ANTS_JCS_VECTOR_DIR compile definition required"
#endif

namespace {

QByteArray slurp(const QString &name) {
    QFile f(QStringLiteral(ANTS_JCS_VECTOR_DIR "/") + name);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << "missing fixture " << name.toStdString();
    return f.readAll();
}

double fromBits(const char *hex) {
    const quint64 bits = QByteArray(hex).toULongLong(nullptr, 16);
    double d = 0;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

}  // namespace

// INV-19 leg 1 — the six published vector files, byte for byte.
TEST(RoadmapExportCanonical, Inv19PublishedVectorFiles) {
    for (const char *name : {"arrays", "french", "structures", "unicode", "values", "weird"}) {
        const QByteArray in = slurp(QStringLiteral("input-%1.json").arg(QLatin1String(name)));
        QByteArray want = slurp(QStringLiteral("output-%1.json").arg(QLatin1String(name)));
        while (want.endsWith('\n') || want.endsWith('\r'))
            want.chop(1);

        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(in, &perr);
        ASSERT_EQ(perr.error, QJsonParseError::NoError)
            << name << ": " << perr.errorString().toStdString();

        QByteArray got;
        QString err;
        ASSERT_TRUE(JsonCanonical::serialise(
            doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), &got, &err))
            << name << ": " << err.toStdString();
        EXPECT_EQ(got.toStdString(), want.toStdString()) << "vector: " << name;
    }
}

// INV-19 leg 2 — RFC 8785 Appendix B, Table 1. Kept separate from leg 1
// because this is the table Qt fails: 21 of 24 through QJsonDocument, and all
// three misses are ECMAScript's fixed-versus-exponential boundary. A leg-1-only
// test would go green against the very writer § 2.2 rules out.
TEST(RoadmapExportCanonical, Inv19AppendixBNumberSamples) {
    struct Sample { const char *bits; const char *want; const char *comment; };
    static const Sample samples[] = {
        {"0000000000000000", "0", "zero"},
        {"8000000000000000", "0", "minus zero"},
        {"0000000000000001", "5e-324", "min positive"},
        {"8000000000000001", "-5e-324", "min negative"},
        {"7fefffffffffffff", "1.7976931348623157e+308", "max positive"},
        {"ffefffffffffffff", "-1.7976931348623157e+308", "max negative"},
        {"4340000000000000", "9007199254740992", "max positive int"},
        {"c340000000000000", "-9007199254740992", "max negative int"},
        {"4430000000000000", "295147905179352830000", "~2**68"},
        {"44b52d02c7e14af5", "9.999999999999997e+22", ""},
        {"44b52d02c7e14af6", "1e+23", ""},
        {"44b52d02c7e14af7", "1.0000000000000001e+23", ""},
        {"444b1ae4d6e2ef4e", "999999999999999700000", ""},
        {"444b1ae4d6e2ef4f", "999999999999999900000", ""},
        {"444b1ae4d6e2ef50", "1e+21", ""},
        {"3eb0c6f7a0b5ed8c", "9.999999999999997e-7", "Qt pads the exponent"},
        {"3eb0c6f7a0b5ed8d", "0.000001", "Qt emits 1e-06"},
        {"41b3de4355555553", "333333333.3333332", ""},
        {"41b3de4355555554", "333333333.33333325", ""},
        {"41b3de4355555555", "333333333.3333333", ""},
        {"41b3de4355555556", "333333333.3333334", ""},
        {"41b3de4355555557", "333333333.33333343", ""},
        {"becbf647612f3696", "-0.0000033333333333333333", "Qt emits -3.33...e-06"},
        {"43143ff3c1cb0959", "1424953923781206.2", "round to even"},
    };

    for (const Sample &s : samples) {
        EXPECT_EQ(JsonCanonical::numberToString(fromBits(s.bits)).toStdString(),
                  std::string(s.want))
            << "IEEE 754 " << s.bits << (s.comment[0] ? "  — " : "") << s.comment;
    }
}

// INV-19 leg 3 — the RFC's two mandatory error cases. § 3.2.2.2 requires a
// conforming implementation to TERMINATE on a lone surrogate, and ANTS-3761
// § 2.4 turns that into "abort the export and report the row" — never a
// replacement character, never a partial file. Reachable in practice: SQLite
// TEXT does not validate encoding.
TEST(RoadmapExportCanonical, Inv19LoneSurrogateAborts) {
    QJsonObject o;
    o.insert(QStringLiteral("body"), QString(QChar(0xD800)) + QStringLiteral("x"));

    QByteArray out;
    QString err;
    EXPECT_FALSE(JsonCanonical::serialise(o, &out, &err))
        << "a lone surrogate must terminate serialisation, not be substituted";
    EXPECT_TRUE(err.contains(QStringLiteral("surrogate"))) << err.toStdString();

    // And a well-formed surrogate PAIR must still serialise — the check must
    // reject invalid UTF-16, not all non-BMP text.
    QJsonObject ok;
    // U+1F41C ANT, spelled as a code point: a narrow "\xF0\x9F…" literal would
    // be widened as Latin-1 and never reach the surrogate-pair path at all.
    ok.insert(QStringLiteral("body"), QStringLiteral("\U0001F41C"));
    err.clear();
    EXPECT_TRUE(JsonCanonical::serialise(ok, &out, &err)) << err.toStdString();
    EXPECT_EQ(out.toStdString(), std::string("{\"body\":\"\xF0\x9F\x90\x9C\"}"));
}

// INV-19 leg 4 — key order is JCS's, not insertion order or a reading order.
// § 2.2 warns that § 2.3's record shapes are written readably for humans and
// are NOT byte-exact; this is what makes that warning testable.
TEST(RoadmapExportCanonical, Inv19KeysSortByUtf16CodeUnit) {
    QJsonObject o;
    o.insert(QStringLiteral("t"), QStringLiteral("item"));
    o.insert(QStringLiteral("id"), QStringLiteral("ANTS-1"));
    o.insert(QStringLiteral("é"), 1);   // é, U+00E9 — sorts after ASCII
    o.insert(QStringLiteral("Z"), 2);        // uppercase sorts before lowercase

    QByteArray out;
    QString err;
    ASSERT_TRUE(JsonCanonical::serialise(o, &out, &err)) << err.toStdString();
    EXPECT_EQ(out.toStdString(),
              std::string("{\"Z\":2,\"id\":\"ANTS-1\",\"t\":\"item\",\"\xC3\xA9\":1}"));
}
