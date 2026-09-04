# Feature: credentials do not leave the machine in audit artifacts

**Status:** shipped (ANTS-4448)

A git remote may legitimately carry credentials in its userinfo —
`https://user:token@host/owner/repo.git` is an ordinary form, and a bare
token is routinely placed in the username slot on its own. That value
reached two egress surfaces:

- **SARIF.** `buildVcsProvenanceBlock` wrote the remote verbatim into
  `versionControlProvenance[].repositoryUri`. SARIF is the artifact people
  attach to a ticket.
- **The wire.** `last_audit_summary` reads `repository_uri` back out of a
  captured SARIF, so every file already on disk keeps sending the
  credentialled remote until its audit is re-run.

Separately, AI triage hand-builds its request body and POSTs it through a
raw `QNetworkAccessManager`, bypassing `LlmClient::buildRequestBody`, which
scrubs its prompts. The prompt embeds the finding's source snippet — and for
a `secrets_scan` or `gitleaks` finding, that snippet **is** the credential.

`SecretRedact::scrub` cannot answer the URL case: it matches secret
*shapes*, and a plain password matches none of them.

## Invariants

**INV-1 — `stripUrlCredentials` removes userinfo from a scheme-bearing
URL.** Behavioural. `https://user:token@host/o/r.git` and
`https://token@host/o/r.git` both come back with no userinfo, and the host
and path are preserved.

**INV-2 — an scp-style remote is returned unchanged.** Behavioural.
`git@github.com:owner/repo.git` parses with no scheme, and its `git@` is a
username rather than a secret, so the input is returned as given. A URL with
no userinfo is likewise unchanged.

**INV-3 — the SARIF writer strips before writing.** Source-grep against
`src/auditengine.cpp`: `buildVcsProvenanceBlock`'s body must pass the remote
through `stripUrlCredentials` before it is assigned to `repositoryUri`.

**INV-4 — the read-back path strips too.** Source-grep against
`src/remotecontrol_state.cpp`: the `repository_uri` assignment must call
`stripUrlCredentials`. Writing alone is not enough, because SARIFs captured
before the fix are still on disk.

**INV-5 — AI triage scrubs its prompt.** Source-grep against
`src/auditdialog.cpp`: the file must reference `SecretRedact`, and the user
message placed into the request body must be the scrubbed text rather than
the raw one.
