# Feature: the Projects dialog reads a session's summary once

## Problem

`ClaudeIntegration::discoverProjects` loads the first user message of the
first five sessions per project. `ClaudeProjectsDialog::populateSessions`
loads the rest when a project is selected, but never stores the result, so
every click on a project re-reads those transcripts' headers (ANTS-5092).

## Contract

When `populateSessions` loads a summary, it stores it on the dialog's copy
of the session, so selecting the same project again reads no transcript.
`refresh()` rediscovers the projects and starts over.

## Invariants

**INV-1 — a lazily loaded summary is kept.** A project with six sessions
is selected, and the sixth session shows its first user message. Its
transcript is deleted and the project is selected again. The sixth session
still shows that message, not "(empty session)".
