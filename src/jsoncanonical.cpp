// ANTS-3761 — RFC 8785 serialisation. Spec § 2.2.
#include "jsoncanonical.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <charconv>
#include <cmath>

namespace {

// RFC 8785 § 3.2.2.2, quoted rather than paraphrased: C0 controls are escaped
// with LOWERCASE \uhhhh except the five with short forms; everything outside
// that range is emitted as is, save for \ and ".
bool escapeString(const QString &s, QByteArray *out, QString *error) {
    // The lone-surrogate check the RFC requires (§ 3.2.2.2) — "may lead to
    // interoperability issues including broken signatures ... MUST cause a
    // compliant JCS implementation to terminate with an appropriate error".
    // QString is UTF-16 and holds unpaired surrogates happily; toUtf8() would
    // quietly substitute U+FFFD, which is the substitution § 2.4 forbids.
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c.isHighSurrogate()) {
            if (i + 1 >= s.size() || !s.at(i + 1).isLowSurrogate()) {
                if (error)
                    *error = QStringLiteral("lone high surrogate U+%1 at index %2")
                                 .arg(uint(c.unicode()), 4, 16, QLatin1Char('0'))
                                 .arg(i);
                return false;
            }
            ++i;
        } else if (c.isLowSurrogate()) {
            if (error)
                *error = QStringLiteral("lone low surrogate U+%1 at index %2")
                             .arg(uint(c.unicode()), 4, 16, QLatin1Char('0'))
                             .arg(i);
            return false;
        }
    }

    out->append('"');
    QString plain;  // run of characters needing no escape, flushed as UTF-8
    const auto flush = [&] {
        if (!plain.isEmpty()) {
            out->append(plain.toUtf8());
            plain.clear();
        }
    };
    for (const QChar c : s) {
        const char16_t u = c.unicode();
        switch (u) {
        case u'"':    flush(); out->append("\\\""); continue;
        case u'\\':   flush(); out->append("\\\\"); continue;
        case u'\b':   flush(); out->append("\\b"); continue;
        case u'\t':   flush(); out->append("\\t"); continue;
        case u'\n':   flush(); out->append("\\n"); continue;
        case u'\f':   flush(); out->append("\\f"); continue;
        case u'\r':   flush(); out->append("\\r"); continue;
        default: break;
        }
        if (u < 0x20) {
            flush();
            out->append(QByteArray("\\u00") +
                        QByteArray::number(u, 16).rightJustified(2, '0'));
        } else {
            plain.append(c);
        }
    }
    flush();
    out->append('"');
    return true;
}

bool write(const QJsonValue &v, QByteArray *out, QString *error);

bool writeObject(const QJsonObject &o, QByteArray *out, QString *error) {
    // RFC 8785 § 3.2.3 — sorted on the RAW (unescaped) property names, by
    // UTF-16 code unit. QString::compare() is exactly that comparison, which
    // is why it is used rather than localeAwareCompare: locale collation would
    // vary with LC_COLLATE and make the export machine-dependent.
    QStringList keys = o.keys();
    std::sort(keys.begin(), keys.end(),
              [](const QString &a, const QString &b) { return a.compare(b) < 0; });

    out->append('{');
    bool first = true;
    for (const QString &k : keys) {
        if (!first)
            out->append(',');
        first = false;
        if (!escapeString(k, out, error))
            return false;
        out->append(':');
        if (!write(o.value(k), out, error))
            return false;
    }
    out->append('}');
    return true;
}

bool write(const QJsonValue &v, QByteArray *out, QString *error) {
    switch (v.type()) {
    case QJsonValue::Null:
        out->append("null");
        return true;
    case QJsonValue::Bool:
        out->append(v.toBool() ? "true" : "false");
        return true;
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (!std::isfinite(d)) {
            if (error)
                *error = QStringLiteral("number is not finite (%1)").arg(d);
            return false;
        }
        out->append(JsonCanonical::numberToString(d));
        return true;
    }
    case QJsonValue::String:
        return escapeString(v.toString(), out, error);
    case QJsonValue::Array: {
        // Array element order is never changed (§ 3.2.3), only scanned.
        out->append('[');
        const QJsonArray a = v.toArray();
        for (qsizetype i = 0; i < a.size(); ++i) {
            if (i)
                out->append(',');
            if (!write(a.at(i), out, error))
                return false;
        }
        out->append(']');
        return true;
    }
    case QJsonValue::Object:
        return writeObject(v.toObject(), out, error);
    case QJsonValue::Undefined:
        break;
    }
    if (error)
        *error = QStringLiteral("undefined JSON value");
    return false;
}

}  // namespace

QByteArray JsonCanonical::numberToString(double d) {
    // ECMAScript Number::toString (ECMA-262 § 6.1.6.1.20), adopted whole by
    // RFC 8785 § 3.2.2.3. std::to_chars supplies the shortest digit string
    // that round-trips; ES6's own contribution is where to put the decimal
    // point, and that is precisely the part Qt gets wrong.
    if (d == 0)
        return QByteArrayLiteral("0");  // covers -0: Appendix B renders it "0"

    const bool negative = d < 0;
    const double magnitude = negative ? -d : d;

    char buf[64];
    const auto res =
        std::to_chars(buf, buf + sizeof buf, magnitude, std::chars_format::scientific);
    const QByteArray sci(buf, res.ptr - buf);  // "d[.ddd]e±XX"

    const int epos = sci.indexOf('e');
    QByteArray digits = sci.left(epos);
    const int dot = digits.indexOf('.');
    if (dot >= 0)
        digits.remove(dot, 1);
    const int k = int(digits.size());       // number of significant digits
    const int n = sci.mid(epos + 1).toInt() + 1;  // value == 0.digits × 10^n

    QByteArray outv;
    if (k <= n && n <= 21) {
        outv = digits + QByteArray(n - k, '0');
    } else if (0 < n && n <= 21) {
        outv = digits.left(n) + '.' + digits.mid(n);
    } else if (-6 < n && n <= 0) {
        // The branch QJsonDocument does not have: ES6 stays in fixed notation
        // down to 1e-6, so 0.000001 is NOT 1e-06.
        outv = QByteArrayLiteral("0.") + QByteArray(-n, '0') + digits;
    } else {
        const int e = n - 1;
        outv = k == 1 ? digits : digits.left(1) + '.' + digits.mid(1);
        // Exponent sign always written, magnitude never zero-padded.
        outv += 'e';
        outv += (e < 0 ? '-' : '+');
        outv += QByteArray::number(e < 0 ? -e : e);
    }
    return negative ? '-' + outv : outv;
}

bool JsonCanonical::serialise(const QJsonValue &value, QByteArray *out, QString *error) {
    Q_ASSERT(out);
    out->clear();
    return write(value, out, error);
}
