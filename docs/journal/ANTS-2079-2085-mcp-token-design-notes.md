# Design notes — deferred MCP token-saving items (ANTS-2079/2082/2083/2084/2085)

Written 2026-06-11 alongside the implemented half of the ANTS-2078..2088
bundle (2078, 2080, 2081, 2086, 2087, 2088 shipped). These five needed a
design decision before code; this captures the direction so a future
session implements without re-deriving. Not a spec — the new-verb item
(2084) still needs `docs/specs/`.

## ANTS-2082 — leaner default for the bare roadmap_query — RESOLVED by ANTS-2086

The item offered two options: (a) flip the default away from
`mode:bullets`+`status:all`, or (b) echo a "use mode:headline_only" hint
on large responses. Option (a) is back-compat-breaking (every existing
caller's payload shape changes) and was explicitly flagged "decide
deliberately". **Decision: do NOT flip the default.** Option (b) is now
delivered generically by ANTS-2086's `leaner_call_hint` — a large
`roadmap_query` response in `bullets` mode already carries
`leaner_call_hint: "pass mode=\"headline_only\" … or status=\"active\" /
mode=\"section_index\" …"`. The back-compat-safe half of 2082 is done;
the default-flip half is intentionally declined. → flip 2082 to shipped,
crediting 2086.

## ANTS-2079 — trim load-bearing tool-description blobs

Largest inline schema descriptions: `roadmap_log` (~2.1 KB) and
`roadmap_query` (~1.5 KB), paid whenever `tools/list` loads. Direction:
keep a one-paragraph **essentials** summary inline (name, the op set, the
load-bearing refusal codes other sessions branch on), and move the
per-op encyclopedic prose behind `tool_info {name:"…"}` on demand.

Blocker / why deferred: `tool_info` today serves the *same* descriptor
that ships in `tools/list` (`m_lastToolsList` snapshot) — there is no
"short inline / long on-demand" split. Implementing 2079 means giving
each tool TWO description strings (a short `description` for `tools/list`
and a long-form served only by `tool_info`), which is a schema-builder
change touching every verb, plus a careful audit that no session branches
on a code that moved out of the short form. The feature tests
source-string-match these blobs (we just bumped two windows for 2078/2080
growth) — those anchors must be re-pointed at the new short forms. High
value, high blast radius; warrants its own focused pass.

## ANTS-2083 — auto-304 repeat-call short-circuit

The trap: `mcp-caches.md` forbids *shadowing* (serving stale data). A
naive "remember last response and replay it" would shadow. **Key
insight: the handler must still run** (or the etag be recomputed over
fresh state) before short-circuiting — otherwise a file changed between
calls is served stale. So the saving is on **body re-emission, not
compute**: remember the last etag issued per `(tool, normalised-args)`
within a session; on a verbatim repeat, auto-inject `etag_match=<that
etag>` so the existing `applyEtagPattern` path recomputes the live etag
and, if unchanged, returns `{ok, unchanged, etag, repeat_of_call}`
instead of the full body. This is exactly "auto-thread the etag the
agent forgot" — same correctness as a manual `etag_match`, no shadow.
Bounded RAM: a small per-session LRU (e.g. ≤64 entries) keyed by tool +
canonicalised args JSON. Only applies to `isEtagSupportedTool` verbs.
Deferred: needs the LRU + arg-canonicalisation + a test that a
between-call file edit still re-emits (anti-shadow regression).

## ANTS-2084 — mcp_bundle cross-tool read multiplex (needs a spec)

A read-only verb taking `calls:[{tool, args}, …]` and returning
`results:[…]` with per-sub-call error isolation (mirrors
`append_batch`'s `skipped[]`). Amortises the `<ants_mcp_data>` wrapper +
framing across N reads and lets the server dedupe overlapping file reads
(e.g. `file_outline` + `read_region` on the same path). Constraints to
spec: a **read-only allowlist** (never route a write verb through it);
a sub-call cap (RAM budget — e.g. ≤8 calls/bundle); each sub-call still
honours its own `caller_cwd` contract + path validation; the bundle
result is NOT itself etag-able (its members may be). This is a new verb
→ write `docs/specs/ANTS-2084.md` first, cold-eyes loop, then implement.

## ANTS-2085 — session verbosity preference

A `session_memory`-backed knob (`verbosity:"terse"|"normal"`, default
normal) read once by read verbs and applied as the default for
`headline_only` / `fields` / `include_body`. **Precedence (must be
explicit in the spec): per-call arg > session preference > built-in
default.** Honoured by the field-projection set
(`mcp::isFieldProjectionTool`) plus the mode-bearing verbs
(`roadmap_query`). Storage: `session_memory` (no TTL), read anchored to
`caller_cwd` (ANTS-1435 read routing). Generalises 2082 across the whole
read surface. Deferred: touches many verbs and needs the precedence rule
nailed down so a terse session can still force a full read per-call.
