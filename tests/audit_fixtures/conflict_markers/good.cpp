// No conflict markers — plain source plus ASCII section-heading underlines
// and decorative banners that historically false-positived against the old
// `^={7}` pattern. None of these should match the tightened rule.

// ===========================================================================
// Section heading
// ===========================================================================
//
// -------- sub-heading --------
//
// Vendored single-header libs often use these banners:
// =====================================================================
//
// And inline:
//   "========" as a string literal delimiter

int answer() {
    // A single `=` in code, not a conflict marker.
    int x = 42;
    // Comparison operators aren't markers either.
    return (x == 42) ? x : 0;
}

// Eight `=` in a row — one more than a real marker, must not match.
// ========
