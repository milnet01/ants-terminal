// VerifyTrustClient — content-trust gate for `.ants/verify.json`
// (ANTS-1337). Qt::Core only, no widgets — the modal that grants
// trust lives in a separate chrome-layer client that overrides
// `prompt()`. Spec: docs/specs/ANTS-1337.md.
//
// The gate sits between `VerifyEngine::loadGateConfig` and its
// caller. When a `.ants/verify.json` is found in the project root,
// the engine asks the client whether the file's SHA-256 (or the
// canonical project path) is trusted. If yes, the bespoke gates
// load as today. If no, the engine falls through to auto-detect —
// the same path as a project with no `.ants/verify.json`.
//
// This defends against a hostile cloned repo silently running
// arbitrary shell via the MCP `verify_changes` tool: the user has
// to confirm trust once per (SHA, repo) pair before the bespoke
// `command` fields are honoured. Phase 1 (this header) ships the
// data layer + nullptr-passthrough back-compat; Phase 2 wires the
// modal + flips `cmdVerifyChanges` to use the real client.

#pragma once

#include <QHash>
#include <QString>

namespace VerifyTrust {

// Outcome of asking the client whether a particular `.ants/verify.json`
// is trusted. `Headless` covers both "no MainWindow to prompt from"
// and "embedded fake client returned Deny" — the engine treats both
// the same way (fall through to auto-detect).
enum class Outcome {
    Trusted,           // SHA in trusted_shas OR (repo, sha) match
    UntrustedFellBack, // prompt() returned Deny / Cancel
    Headless,          // no MainWindow / fake client / no prompt path
};

// Single trust decision result. `outcome` says what happened.
// `shaHex` is the SHA-256 hex of the `.ants/verify.json` bytes the
// client checked — caller can surface it in MCP responses /
// telemetry.
struct Decision {
    Outcome outcome = Outcome::Headless;
    QString shaHex;
};

// Abstract client interface. Tests inject fakes; production code
// uses `FilePersistedTrustClient` below.
class Client {
public:
    virtual ~Client() = default;

    // Look up a (projectPath, configBytes) pair against the trust
    // store. Computes SHA-256 over `configBytes` (raw, no
    // normalisation), then checks `trusted_shas` and
    // `trusted_repos`. If neither matches, calls `prompt()` (the
    // GUI-bearing subclass shows a modal; the base class returns
    // Headless). The return value's `outcome` field tells the
    // caller whether to honour the bespoke gates or fall back.
    //
    // `configBytes` is the exact bytes of `.ants/verify.json` —
    // the SHA is computed over them as-is (no whitespace or JSON
    // normalisation), so byte-identical configs across repos share
    // one trust entry.
    virtual Decision outcomeForConfig(const QString &projectPath,
                                      const QByteArray &configBytes) = 0;

    // Persist a new SHA-trust entry. Idempotent — adding a SHA
    // that's already trusted is a no-op. Returns true on success.
    virtual bool addTrustedSha(const QString &shaHex,
                               const QString &note = {}) = 0;

    // Persist a new repo-trust entry. `untilShaChanges=true`
    // (default) records the current SHA and re-prompts when the
    // file is edited; `untilShaChanges=false` is the power-user
    // "trust this repo forever" form.
    virtual bool addTrustedRepo(const QString &canonicalProjectPath,
                                const QString &currentShaHex,
                                bool untilShaChanges = true) = 0;

    // Clear the in-RAM session cache of "denied this session" SHAs.
    // The cache prevents prompt-fatigue (a misbehaving Claude that
    // retries verify_changes on every turn doesn't re-prompt for
    // the same SHA in the same Ants session). Tests use this to
    // exercise the prompt logic after an initial Deny.
    virtual void clearSessionCache() = 0;
};

// Production trust client — backed by `~/.config/ants-terminal/
// verify-trust.json` (mode 0600, atomic write via tmpfile + rename
// + fsyncParentDir). The path can be overridden at construction
// for tests. `prompt()` is left pure-virtual on subclasses; the
// chrome-layer modal client overrides it. The base class's
// `outcomeForConfig` returns Outcome::Headless when prompting is
// reached — i.e., a `FilePersistedTrustClient` constructed
// directly (no subclass) effectively means "no modal available;
// untrusted configs fall back to auto-detect."
class FilePersistedTrustClient : public Client {
public:
    // Default ctor uses ~/.config/ants-terminal/verify-trust.json.
    // Test ctor accepts an explicit path.
    FilePersistedTrustClient();
    explicit FilePersistedTrustClient(const QString &trustFilePath);

    Decision outcomeForConfig(const QString &projectPath,
                              const QByteArray &configBytes) override;
    bool addTrustedSha(const QString &shaHex,
                       const QString &note = {}) override;
    bool addTrustedRepo(const QString &canonicalProjectPath,
                        const QString &currentShaHex,
                        bool untilShaChanges = true) override;
    void clearSessionCache() override;

    // Where the file lives. Useful for tests + log messages.
    QString trustFilePath() const { return m_path; }

protected:
    // Subclass hook: invoked when neither SHA nor repo is trusted.
    // The base class returns Decision{Headless, shaHex} so calls
    // through a default-constructed client never run untrusted
    // configs (defense-in-depth: if Phase 2 wiring forgets to
    // construct the chrome-layer subclass, the worst case is
    // "auto-detect fallback", not "arbitrary shell").
    //
    // Subclasses show a modal and return Trusted (with side-effect
    // of persisting via addTrustedSha / addTrustedRepo), or
    // UntrustedFellBack on Deny/Cancel.
    virtual Decision prompt(const QString &projectPath,
                            const QString &shaHex,
                            const QByteArray &configBytes);

private:
    bool loadFromDisk();
    bool saveToDisk() const;

    // Compute SHA-256 over raw bytes; return hex digest.
    static QString sha256Hex(const QByteArray &bytes);

    QString m_path;

    // ANTS-1825 — set when loadFromDisk saw a `version` greater than
    // kSchemaVersion (a file written by a newer Ants whose schema this
    // build doesn't understand). When true, trust falls closed (no
    // entries loaded) and saveToDisk no-ops so the older client can't
    // downgrade-clobber the newer file.
    bool m_futureSchema = false;

    // Trusted SHA → metadata (first-trusted ISO8601 + optional note).
    QHash<QString, QString> m_trustedShas;
    // Trusted canonical repo path → (SHA pin, untilShaChanges flag).
    struct RepoEntry {
        QString shaHex;
        bool    untilShaChanges = true;
    };
    QHash<QString, RepoEntry> m_trustedRepos;

    // In-RAM denied-this-session SHA cache. Not persisted — cleared
    // on Ants restart.
    QHash<QString, bool> m_sessionDenied;
};

// Test fake: trusts everything. Used by Phase-1 engine tests + by
// any caller that wants pre-ANTS-1337 behavior temporarily during
// the rollout.
class AlwaysTrustClient : public Client {
public:
    Decision outcomeForConfig(const QString &projectPath,
                              const QByteArray &configBytes) override {
        Q_UNUSED(projectPath);
        Q_UNUSED(configBytes);
        return {Outcome::Trusted, {}};
    }
    bool addTrustedSha(const QString &, const QString &) override { return true; }
    bool addTrustedRepo(const QString &, const QString &, bool) override { return true; }
    void clearSessionCache() override {}
};

// Test fake: denies everything (forces auto-detect fallback).
class AlwaysDenyClient : public Client {
public:
    Decision outcomeForConfig(const QString &projectPath,
                              const QByteArray &configBytes) override {
        Q_UNUSED(projectPath);
        Q_UNUSED(configBytes);
        return {Outcome::UntrustedFellBack, {}};
    }
    bool addTrustedSha(const QString &, const QString &) override { return true; }
    bool addTrustedRepo(const QString &, const QString &, bool) override { return true; }
    void clearSessionCache() override {}
};

}  // namespace VerifyTrust
