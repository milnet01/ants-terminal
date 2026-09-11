# OSC 8 hyperlink phishing guard (ANTS-2119 terminalwidget M2)

## Problem

`TerminalWidget::openHyperlink` warned before following an OSC 8 link only when
the *visible label* itself parsed as a hostname that differed from the URL host
(the classic homograph: label `github.com` → URL `evil.com`). The far more
common phishing shape — a benign label (`Download`, `click here`) over a hostile
URL — yielded an empty `visibleHost`, so the guard was skipped and the link
opened with no prompt.

Blanket-prompting on every non-host label is not the fix: OSC 8 links in a
terminal overwhelmingly carry benign descriptive labels, so it would be
confirmation fatigue that trains users to click through.

## Fix

Split the policy out of the modal dialog into a pure, unit-testable
`TerminalWidget::classifyHyperlink(visibleLabel, url)` returning:

- `None` — open silently.
- `LabelHostMismatch` — the label looks like host A but the URL is host B
  (the existing homograph case, unchanged).
- `SuspiciousDestination` — the label carries **no** host, **and** the URL host
  is objectively suspicious: a punycode/IDN homograph (`xn--`) or a bare
  non-loopback, non-link-local IP literal. Benign descriptive labels over
  benign hosts stay `None`.

`openHyperlink` renders a warning dialog for either non-`None` verdict.

## Invariants

- **INV-1** — label host matches URL host → `None`
  (`github.com` label → `https://github.com/x`; `www.` stripped for comparison).
- **INV-2** — label host ≠ URL host → `LabelHostMismatch`
  (`github.com` label → `https://evil.example/x`).
- **INV-3** — benign non-host label + benign host → `None`
  (`Download` → `https://github.com/x`): no confirmation fatigue.
- **INV-4** — non-host label + punycode/IDN destination → `SuspiciousDestination`
  (`Download` → `https://xn--pypal-4ve.com`).
- **INV-5** — non-host label + bare public IP literal → `SuspiciousDestination`
  (`click here` → `http://203.0.113.7/x`); loopback (`127.0.0.1`) and
  link-local stay `None` (dev-server links must not nag).
- **INV-6** — no host at all (e.g. `mailto:`) → `None`.
- **INV-7 (ANTS-5028)** — `openHyperlink` takes its `UrlSpan` by value. The
  warning dialog runs a nested event loop, during which output can clear the
  span cache. A span held by reference into that cache would then be freed
  before the URL is opened.

## Test

INV-1 to INV-6 are pure calls to the static `classifyHyperlink` — no widget
instance, no modal dialog. Label + URL in, verdict out. INV-7 is a source
scrape of `terminalwidget.h`, because the dialog path needs a modal.
