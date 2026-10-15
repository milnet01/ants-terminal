"""Plain-text report of a ledger and how to settle it."""
from __future__ import annotations

from . import money
from .ledger import Ledger
from .settle import settle


def render(ledger: Ledger) -> str:
    lines = [f"Total spent: {money.format(ledger.total())}", ""]
    paid, consumed, balances = ledger.paid(), ledger.consumed(), ledger.balances()
    width = max((len(n) for n in ledger.people()), default=4)
    lines.append(f"{'Name':<{width}}  {'Paid':>9}  {'Share':>9}  {'Balance':>9}")
    for name in ledger.people():
        lines.append(
            f"{name:<{width}}  {money.format(paid[name]):>9}"
            f"  {money.format(consumed[name]):>9}  {money.format(balances[name]):>9}"
        )
    lines.append("")
    transfers = settle(balances)
    if not transfers:
        lines.append("Everyone is square.")
    for t in transfers:
        lines.append(f"{t.sender} pays {t.receiver} {money.format(t.amount)}")
    return "\n".join(lines) + "\n"
