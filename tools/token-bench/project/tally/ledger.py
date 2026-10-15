"""A list of shared expenses, and what each person is owed or owes."""
from __future__ import annotations

from dataclasses import dataclass, field

from . import split


@dataclass(frozen=True)
class Expense:
    date: str
    payer: str
    amount: int  # cents
    participants: tuple[str, ...]
    note: str = ""
    weights: dict[str, int] | None = field(default=None, hash=False)

    def shares(self) -> dict[str, int]:
        """What each participant's part of this expense comes to, in cents."""
        if self.weights is not None:
            return split.split_by_weight(self.amount, self.weights)
        return split.split_evenly(self.amount, list(self.participants))


class Ledger:
    def __init__(self) -> None:
        self.expenses: list[Expense] = []

    def add(self, expense: Expense) -> None:
        if expense.amount <= 0:
            raise ValueError(f"an expense must be more than zero: {expense}")
        self.expenses.append(expense)

    def people(self) -> list[str]:
        names = set()
        for e in self.expenses:
            names.add(e.payer)
            names.update(e.participants)
        return sorted(names)

    def total(self) -> int:
        return sum(e.amount for e in self.expenses)

    def paid(self) -> dict[str, int]:
        out = {name: 0 for name in self.people()}
        for e in self.expenses:
            out[e.payer] += e.amount
        return out

    def consumed(self) -> dict[str, int]:
        out = {name: 0 for name in self.people()}
        for e in self.expenses:
            for name, share in e.shares().items():
                out[name] += share
        return out

    def balances(self) -> dict[str, int]:
        """Paid minus consumed, per person: positive means they are owed."""
        paid, consumed = self.paid(), self.consumed()
        return {name: paid[name] - consumed[name] for name in self.people()}
