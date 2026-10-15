"""Read expenses from a CSV file.

Columns: date, payer, amount, participants, note. Participants are separated
by ";". A participant may carry a weight as "Name*2"; an expense where any
participant has a weight is split by weight, the others counting as 1.
"""
from __future__ import annotations

import csv
from pathlib import Path

from . import money
from .ledger import Expense, Ledger

COLUMNS = ["date", "payer", "amount", "participants", "note"]


def parse_participants(field: str) -> tuple[tuple[str, ...], dict[str, int] | None]:
    names = []
    weights = {}
    weighted = False
    for item in field.split(";"):
        item = item.strip()
        if not item:
            continue
        name, star, weight = item.partition("*")
        name = name.strip()
        if star:
            weighted = True
            weights[name] = int(weight)
        else:
            weights[name] = 1
        names.append(name)
    return tuple(names), (weights if weighted else None)


def read_ledger(path: str | Path) -> Ledger:
    ledger = Ledger()
    with open(path, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if reader.fieldnames != COLUMNS:
            raise ValueError(f"{path}: expected columns {COLUMNS}, got {reader.fieldnames}")
        for line, row in enumerate(reader, start=2):
            try:
                participants, weights = parse_participants(row["participants"])
                ledger.add(Expense(
                    date=row["date"].strip(),
                    payer=row["payer"].strip(),
                    amount=money.parse(row["amount"]),
                    participants=participants,
                    note=row["note"].strip(),
                    weights=weights,
                ))
            except ValueError as err:
                raise ValueError(f"{path}:{line}: {err}") from err
    return ledger
