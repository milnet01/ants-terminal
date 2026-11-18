"""The `Kind:` rules the tools/roadmap-*.py scripts share.

CANONICAL_KINDS mirrors canonicalKinds() in src/roadmapparse.cpp, the
roadmap-format.md § 3.5.3 taxonomy. When the C++ list changes, change it here.
"""
import re

CANONICAL_KINDS = frozenset({
    "implement", "fix", "audit-fix", "review-fix", "doc", "doc-fix", "refactor",
    "test", "chore", "release", "perf", "security", "feature", "enhancement",
    "investigate", "research", "accessibility", "optimize", "package",
    "marketing", "ux",
})

# A `Kind:` declaration, bare (`Kind: fix.`) or bold (`**Kind:** fix.`).
# Callers scan indented continuation lines only, so a headline ABOUT the field
# is not read as declaring it. Last match wins: a rendered bullet puts the body
# before the trailer.
#
# Like rxKind() in src/roadmapparse.cpp it is un-anchored, since a trailer is
# often written inline after a prose sentence, and case-sensitive, so prose
# ("...the kind: of work...") does not match. It differs from rxKind() in three
# ways:
#   - only a backtick directly before the label is guarded, not "`*" or "`**";
#   - it has no `(?!:)` lookahead;
#   - the value is letters, digits, space and `_/+-`. The `+` admits compound
#     corpus values such as `process + tooling`.
# Prose that quotes the label (`the "Kind: ..." line`) matches with an empty
# capture, because the value stops before the `.`; is_kind_shaped() rejects it.
KIND_VALUE = re.compile(r"(?<!`)(?:\*\*)?Kind:(?:\*\*)?\s*([A-Za-z0-9 _/+-]+?)\s*(?:\.|$)")

# Un-anchoring also matches prose that capitalises the label mid-sentence. A
# capture longer than this is prose, not a value; the limits leave headroom
# over every value in the corpus.
KIND_MAX_WORDS, KIND_MAX_CHARS = 4, 30


def is_kind_shaped(value):
    """True when a capture is non-empty and short enough to be a value."""
    return (bool(value) and len(value) <= KIND_MAX_CHARS
            and len(value.split()) <= KIND_MAX_WORDS)
