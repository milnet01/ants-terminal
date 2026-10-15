"""Divide an amount between people without losing or inventing a cent."""
from __future__ import annotations


def split_evenly(total: int, people: list[str]) -> dict[str, int]:
    """Give each person an equal share of `total` cents.

    Cents that do not divide evenly go one each to the first people in the
    list, so the shares always add up to `total`.
    """
    if not people:
        raise ValueError("nobody to split between")
    if len(set(people)) != len(people):
        raise ValueError(f"a person is listed twice: {people}")
    base, extra = divmod(total, len(people))
    return {name: base + (1 if i < extra else 0) for i, name in enumerate(people)}


def split_by_weight(total: int, weights: dict[str, int]) -> dict[str, int]:
    """Give each person a share of `total` cents in proportion to a weight.

    Leftover cents go to the largest remainders first, ties broken by name.
    """
    if not weights or sum(weights.values()) <= 0:
        raise ValueError("weights must add up to more than zero")
    if any(w < 0 for w in weights.values()):
        raise ValueError("a weight is negative")
    weight_sum = sum(weights.values())
    shares = {}
    remainders = []
    for name, weight in weights.items():
        share, remainder = divmod(total * weight, weight_sum)
        shares[name] = share
        remainders.append((-remainder, name))
    leftover = total - sum(shares.values())
    for _, name in sorted(remainders)[:leftover]:
        shares[name] += 1
    return shares
