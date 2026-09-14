# Feature: verify log splitting

## Invariants

**INV-1 — complete lines are split before the single-line cap.**
`runOneGate` in `src/verifyengine.cpp` extracts every newline-terminated line
from a read before force-splitting the unterminated remainder, so one large
read cannot turn many lines into one.

## Rationale

The ANTS-5102 performance pass found that a read carrying ctest's FAILED
list could be kept as one capped line.

## Test surface

`test_verify_and_audit_guards.cpp` reads `src/verifyengine.cpp` (located from
the test's own path).

## Regression history

- **ANTS-5102:** the defect above. Locked by this spec.
