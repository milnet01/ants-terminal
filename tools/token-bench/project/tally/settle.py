"""Turn balances into a short list of payments that squares everyone up."""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Transfer:
    sender: str
    receiver: str
    amount: int  # cents


def settle(balances: dict[str, int]) -> list[Transfer]:
    """Pay the biggest creditor from the biggest debtor until all are even.

    Ties are broken by name, so the same balances always give the same list.
    """
    if sum(balances.values()) != 0:
        raise ValueError(f"balances do not add up to zero: {balances}")
    creditors = {n: b for n, b in balances.items() if b > 0}
    debtors = {n: -b for n, b in balances.items() if b < 0}
    transfers = []
    while creditors:
        receiver = min(creditors, key=lambda n: (-creditors[n], n))
        sender = min(debtors, key=lambda n: (-debtors[n], n))
        amount = min(creditors[receiver], debtors[sender])
        transfers.append(Transfer(sender, receiver, amount))
        creditors[receiver] -= amount
        debtors[sender] -= amount
        if creditors[receiver] == 0:
            del creditors[receiver]
        if debtors[sender] == 0:
            del debtors[sender]
    return transfers
