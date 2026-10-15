"""Amounts of money, held as whole cents so that sums are exact."""
from __future__ import annotations


def parse(text: str) -> int:
    """Parse an amount such as "12", "12.3" or "-12.34" into cents."""
    raw = text.strip()
    sign = 1
    if raw.startswith("-"):
        sign = -1
        raw = raw[1:]
    whole, _, frac = raw.partition(".")
    if not whole and not frac:
        raise ValueError(f"not an amount: {text!r}")
    if (whole and not whole.isdigit()) or (frac and not frac.isdigit()):
        raise ValueError(f"not an amount: {text!r}")
    if len(frac) > 2:
        raise ValueError(f"more than two decimal places: {text!r}")
    cents = int(whole or "0") * 100 + int(frac or "0")
    return sign * cents


def format(cents: int) -> str:
    """Format cents as "12.34", with a leading "-" when negative."""
    sign = "-" if cents < 0 else ""
    whole, rest = divmod(abs(cents), 100)
    return f"{sign}{whole}.{rest:02d}"
