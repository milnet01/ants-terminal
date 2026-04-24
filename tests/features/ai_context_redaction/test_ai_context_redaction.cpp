// AI context redaction — OWASP LLM06. See spec.md.
//
// Behavioral corpus drives SecretRedact::scrub through the 14 secret
// shapes listed in the spec plus 6 negative controls. Source-grep
// invariants (INV-7) assert AiDialog::sendRequest wires scrub() to
// both m_terminalContext and the userMessage.

#include "secretredact.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SRC_AIDIALOG_CPP
#error "SRC_AIDIALOG_CPP compile definition required"
#endif

namespace {

int g_failures = 0;
const char *g_currentCase = "<unset>";

void fail(const char *msg)
{
    std::fprintf(stderr, "FAIL [%s]: %s\n", g_currentCase, msg);
    ++g_failures;
}

// Assert that `out` does NOT contain the raw secret substring and DOES
// contain a [REDACTED:<expectedKind>] label. Used for positive cases.
void expectRedacted(const QString &out, const QString &rawSecret, const QString &expectedKind)
{
    if (rawSecret.isEmpty()) {
        fail("test bug: rawSecret is empty — specify the actual secret substring");
        return;
    }
    if (out.contains(rawSecret)) {
        std::fprintf(stderr, "  output still contains raw secret: %s\n",
                     rawSecret.toLocal8Bit().constData());
        std::fprintf(stderr, "  full output: %s\n", out.toLocal8Bit().constData());
        fail("raw secret survived scrub() — leak");
    }
    const QString expectedLabel = QStringLiteral("[REDACTED:") + expectedKind + QLatin1Char(']');
    if (!out.contains(expectedLabel)) {
        std::fprintf(stderr, "  expected label: %s\n",
                     expectedLabel.toLocal8Bit().constData());
        std::fprintf(stderr, "  got: %s\n", out.toLocal8Bit().constData());
        fail("missing or mislabeled [REDACTED:*] token");
    }
}

// Positive case: surrounding text must survive. Caller provides the
// portions that should appear verbatim.
void expectSurvives(const QString &out, const QString &expectedFragment)
{
    if (expectedFragment.isEmpty()) return;
    if (!out.contains(expectedFragment)) {
        std::fprintf(stderr, "  missing surrounding fragment: %s\n",
                     expectedFragment.toLocal8Bit().constData());
        std::fprintf(stderr, "  full output: %s\n", out.toLocal8Bit().constData());
        fail("surrounding text was eaten by scrub()");
    }
}

std::string slurp(const char *path)
{
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // ─────────────────────────────────────────────────────────────
    // INV-1: SecretRedact::scrub exists with the documented signature.
    // (Compile-time check — if this file builds, the signature matches.)
    // ─────────────────────────────────────────────────────────────

    // ─────────────────────────────────────────────────────────────
    // INV-2, INV-3, INV-6: the 14 secret shapes redact with the correct
    // <kind> label. Each line is a plausible terminal-output context.
    // ─────────────────────────────────────────────────────────────

    struct PositiveCase {
        const char *label;
        QString input;
        QString rawSecret;
        QString expectedKind;
        QString expectedFragment; // text that must survive
    };

    // Anthropic: sk-ant- + 90+ chars from [A-Za-z0-9_-]. Payload built
    // explicitly so a later reader can count on sight.
    const QString antPayload90 = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyz") +   // 26
        QStringLiteral("0123456789") +    // 10 → 36
        QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + // 26 → 62
        QStringLiteral("-_") +            //  2 → 64
        QStringLiteral("abcdefghijklmnopqrstuvwxyz"); // 26 → 90
    const QString antKey = QStringLiteral("sk-ant-") + antPayload90;
    // OpenAI project: sk-proj- + 80+ chars.
    const QString openaiProjPayload80 = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyz") +   // 26
        QStringLiteral("0123456789") +    // 10 → 36
        QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + // 26 → 62
        QStringLiteral("-_") +            //  2 → 64
        QStringLiteral("abcdefghijklmnop"); // 16 → 80
    const QString openaiProjKey = QStringLiteral("sk-proj-") + openaiProjPayload80;
    // 48-char payload for legacy OpenAI.
    const QString openaiLegacyKey = QStringLiteral("sk-") +
        QStringLiteral("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV"); // 48 chars
    // Classic GitHub PAT: ghp_ + exactly 36 alphanum. Same length for
    // gho_ and ghs_ variants — the token format is documented by GitHub.
    const QString ghPayload36 = QStringLiteral(
        "abcdefghijklmnopqrstuvwxyz0123456789"); // 26 + 10 = 36
    const QString ghPat   = QStringLiteral("ghp_") + ghPayload36;
    const QString ghOAuth = QStringLiteral("gho_") + ghPayload36;
    const QString ghApp   = QStringLiteral("ghs_") + ghPayload36;
    // Fine-grained: github_pat_ + 82 chars from [A-Za-z0-9_].
    const QString ghFine = QStringLiteral("github_pat_") +
        QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789") + // 36
        QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") + // 36 → 72
        QStringLiteral("_abcdefghi"); // 10 → total 82
    // Slack token (xoxb-…). Prefix split across QStringLiteral chunks
    // so static secret scanners (GitHub push protection, trufflehog
    // pre-commit) don't flag the source file — the runtime-concatenated
    // string still exercises the regex identically.
    const QString slackTok = QStringLiteral("xox") + QStringLiteral("b-") +
                             QStringLiteral("1234567890-abcdefghij");
    // Stripe live secret key. Same split rationale as above.
    const QString stripeKey = QStringLiteral("sk_") + QStringLiteral("live_") +
                              QStringLiteral("abcdefghij0123456789ABCD");
    // JWT: header.payload.signature.
    const QString jwt = QStringLiteral(
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiIxMjM0NTY3ODkwIiwibmFtZSI6IkpvaG4ifQ."
        "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c");

    std::vector<PositiveCase> positives = {
        { "AWS_AKIA",
          QStringLiteral("export AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE"),
          QStringLiteral("AKIAIOSFODNN7EXAMPLE"),
          QStringLiteral("aws_access_key"),
          QStringLiteral("export AWS_ACCESS_KEY_ID=") },
        { "AWS_ASIA_STS",
          QStringLiteral("ASIAQGB5QRX42EXAMPLE"),
          QStringLiteral("ASIAQGB5QRX42EXAMPLE"),
          QStringLiteral("aws_access_key"),
          QStringLiteral("") },
        { "GitHub_PAT_classic",
          QStringLiteral("git clone https://") + ghPat + QStringLiteral("@github.com/me/repo.git"),
          ghPat,
          QStringLiteral("github_pat"),
          QStringLiteral("git clone https://") },
        { "GitHub_OAuth",
          QStringLiteral("Authorization: token ") + ghOAuth,
          ghOAuth,
          QStringLiteral("github_oauth"),
          QStringLiteral("Authorization: ") },
        { "GitHub_App",
          QStringLiteral("env GHS=") + ghApp + QStringLiteral(" run"),
          ghApp,
          QStringLiteral("github_app"),
          QStringLiteral("env GHS=") },
        { "GitHub_fine_grained_PAT",
          QStringLiteral("export GH_TOKEN=") + ghFine,
          ghFine,
          QStringLiteral("github_fine_grained_pat"),
          QStringLiteral("export GH_TOKEN=") },
        { "Anthropic",
          QStringLiteral("ANTHROPIC_API_KEY=") + antKey,
          antKey,
          QStringLiteral("anthropic"),
          QStringLiteral("ANTHROPIC_API_KEY=") },
        { "OpenAI_project",
          QStringLiteral("OPENAI_API_KEY=") + openaiProjKey,
          openaiProjKey,
          QStringLiteral("openai_project"),
          QStringLiteral("OPENAI_API_KEY=") },
        { "OpenAI_legacy",
          openaiLegacyKey,
          openaiLegacyKey,
          QStringLiteral("openai"),
          QStringLiteral("") },
        { "Slack_xoxb",
          QStringLiteral("curl -X POST ... ") + slackTok,
          slackTok,
          QStringLiteral("slack"),
          QStringLiteral("curl -X POST ... ") },
        { "Stripe_live",
          QStringLiteral("STRIPE=") + stripeKey,
          stripeKey,
          QStringLiteral("stripe"),
          QStringLiteral("STRIPE=") },
        { "JWT",
          QStringLiteral("cookie: session=") + jwt + QStringLiteral("; Path=/"),
          jwt,
          QStringLiteral("jwt"),
          QStringLiteral("cookie: session=") },
        { "Bearer_header",
          QStringLiteral("curl -H \"Authorization: Bearer abc123def456ghi789jklmnop\""),
          QStringLiteral("abc123def456ghi789jklmnop"),
          QStringLiteral("bearer"),
          QStringLiteral("Authorization: ") },
        { "Generic_secret_assignment",
          QStringLiteral("password = \"hunter2longenough\""),
          QStringLiteral("hunter2longenough"),
          QStringLiteral("generic_secret"),
          QStringLiteral("password = ") },
        { "Generic_api_key_colon",
          QStringLiteral("api_key: sneakyvalue123"),
          QStringLiteral("sneakyvalue123"),
          QStringLiteral("generic_secret"),
          QStringLiteral("api_key:") },
        { "PEM_private_key_multiline",
          QStringLiteral("$ cat ~/.ssh/id_rsa\n"
                         "-----BEGIN RSA PRIVATE KEY-----\n"
                         "MIIEpAIBAAKCAQEAxyzABCdef123/4567890\n"
                         "KEYMATERIALFOLLOWS==\n"
                         "-----END RSA PRIVATE KEY-----\n"
                         "$ "),
          QStringLiteral("KEYMATERIALFOLLOWS=="),
          QStringLiteral("private_key"),
          QStringLiteral("$ cat ~/.ssh/id_rsa") },
    };

    for (const auto &pc : positives) {
        g_currentCase = pc.label;
        const auto r = SecretRedact::scrub(pc.input);
        if (r.redactedCount < 1) {
            fail("redactedCount must be >= 1 for a positive case (INV-4)");
        }
        expectRedacted(r.text, pc.rawSecret, pc.expectedKind);
        expectSurvives(r.text, pc.expectedFragment);
    }

    // ─────────────────────────────────────────────────────────────
    // INV-5: negative controls pass through unchanged and report
    // redactedCount == 0.
    // ─────────────────────────────────────────────────────────────

    struct NegativeCase {
        const char *label;
        QString input;
    };

    std::vector<NegativeCase> negatives = {
        { "empty",                   QStringLiteral("") },
        { "plain_ls",                QStringLiteral("ls -la /etc") },
        { "url_no_creds",            QStringLiteral("https://github.com/user/repo.git") },
        { "uuid",                    QStringLiteral("550e8400-e29b-41d4-a716-446655440000") },
        { "commit_sha_40hex",        QStringLiteral("a1b2c3d4e5f6789012345678901234567890abcd") },
        { "prose_about_tokens",      QStringLiteral("please set your api token in the config") },
    };

    for (const auto &nc : negatives) {
        g_currentCase = nc.label;
        const auto r = SecretRedact::scrub(nc.input);
        if (r.redactedCount != 0) {
            std::fprintf(stderr, "  redactedCount = %d (expected 0)\n", r.redactedCount);
            std::fprintf(stderr, "  output: %s\n", r.text.toLocal8Bit().constData());
            fail("INV-5: negative control should not redact anything");
        }
        if (r.text != nc.input) {
            std::fprintf(stderr, "  output differs from input\n");
            std::fprintf(stderr, "  input : %s\n", nc.input.toLocal8Bit().constData());
            std::fprintf(stderr, "  output: %s\n", r.text.toLocal8Bit().constData());
            fail("INV-5: negative control output MUST equal input byte-for-byte");
        }
    }

    // ─────────────────────────────────────────────────────────────
    // INV-4: cumulative count across multiple secrets.
    // ─────────────────────────────────────────────────────────────
    {
        g_currentCase = "cumulative_count";
        const QString combined =
            QStringLiteral("line1 ") + ghPat + QLatin1Char('\n') +
            QStringLiteral("line2 ") + jwt + QLatin1Char('\n') +
            QStringLiteral("line3 AKIAIOSFODNN7EXAMPLE\n");
        const auto r = SecretRedact::scrub(combined);
        if (r.redactedCount != 3) {
            std::fprintf(stderr, "  redactedCount = %d (expected 3)\n", r.redactedCount);
            std::fprintf(stderr, "  output: %s\n", r.text.toLocal8Bit().constData());
            fail("INV-4: three distinct secrets must count as three redactions");
        }
    }

    // ─────────────────────────────────────────────────────────────
    // INV-3 precedence: sk-ant-… must be labeled "anthropic", not
    // mislabeled as legacy "openai".
    // ─────────────────────────────────────────────────────────────
    {
        g_currentCase = "precedence_sk_ant_vs_sk";
        const auto r = SecretRedact::scrub(antKey);
        if (!r.text.contains(QStringLiteral("[REDACTED:anthropic]"))) {
            std::fprintf(stderr, "  got: %s\n", r.text.toLocal8Bit().constData());
            fail("INV-3: sk-ant-… must be labeled anthropic, not openai — "
                 "priority ordering in rules() is wrong");
        }
        if (r.text.contains(QStringLiteral("[REDACTED:openai]"))) {
            fail("INV-3: sk-ant-… incorrectly produced an openai label — "
                 "legacy sk- pattern stole the match");
        }
    }

    // ─────────────────────────────────────────────────────────────
    // INV-7: AiDialog::sendRequest wires SecretRedact::scrub to both
    // inbound strings before either reaches the JSON body.
    // ─────────────────────────────────────────────────────────────

    {
        g_currentCase = "sendRequest_wires_scrub";
        const std::string src = slurp(SRC_AIDIALOG_CPP);
        const std::string marker = "AiDialog::sendRequest";
        const auto pos = src.find(marker);
        if (pos == std::string::npos) {
            fail("INV-7a: AiDialog::sendRequest definition not found in source");
        } else {
            // Look at the next ~3500 chars — covers the current function
            // body comfortably; if a future refactor grows it past this,
            // the grep bail-out message tells the reader what to fix.
            const std::string body = src.substr(pos, 3500);
            const std::string needleCtx  = "SecretRedact::scrub(m_terminalContext)";
            const std::string needleUser = "SecretRedact::scrub(userMessage)";
            if (body.find(needleCtx) == std::string::npos) {
                fail("INV-7b: sendRequest must call SecretRedact::scrub(m_terminalContext) — "
                     "without it the scrollback is shipped verbatim (OWASP LLM06)");
            }
            if (body.find(needleUser) == std::string::npos) {
                fail("INV-7c: sendRequest must call SecretRedact::scrub(userMessage) — "
                     "pasted secrets in the question itself are equally exposed");
            }
            // The scrubbed values must actually feed the JSON body (as
            // opposed to being computed and discarded).
            if (body.find(".arg(scrubbedContext.text)") == std::string::npos) {
                fail("INV-7d: scrubbedContext.text must feed the systemMsg content (.arg(...))"
                     " — the scrubbed value must actually reach the wire");
            }
            if (body.find("scrubbedUser.text") == std::string::npos) {
                fail("INV-7e: scrubbedUser.text must populate userMsg[\"content\"] — "
                     "a dangling scrub call with no consumer is a silent regression");
            }
        }
    }

    // ─────────────────────────────────────────────────────────────
    // INV-8: on positive redactedCount the user is informed.
    // Heuristic: sendRequest body contains an appendMessage("System", …)
    // gated on a redactedCount-sourced condition.
    // ─────────────────────────────────────────────────────────────

    {
        g_currentCase = "user_informed_on_redaction";
        const std::string src = slurp(SRC_AIDIALOG_CPP);
        const auto pos = src.find("AiDialog::sendRequest");
        if (pos != std::string::npos) {
            const std::string body = src.substr(pos, 3500);
            const bool hasAppendSystem = body.find("appendMessage(QStringLiteral(\"System\")") != std::string::npos
                                      || body.find("appendMessage(\"System\"") != std::string::npos;
            const bool gatedOnCount   = body.find("totalRedacted > 0") != std::string::npos
                                     || body.find("redactedCount > 0") != std::string::npos;
            if (!hasAppendSystem) {
                fail("INV-8a: sendRequest should call appendMessage(\"System\", …) to surface "
                     "redactions — silent redaction is worse than visible redaction");
            }
            if (!gatedOnCount) {
                fail("INV-8b: the user-notice appendMessage must be gated on a redactedCount "
                     "(>0) check — unconditional notices spam every request");
            }
        }
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d invariant check(s) failed — see spec.md for context\n",
                     g_failures);
        return 1;
    }
    std::printf("OK: ai_context_redaction — %zu positive + %zu negative + wiring grep\n",
                positives.size(), negatives.size());
    return 0;
}
