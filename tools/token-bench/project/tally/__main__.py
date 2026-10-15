"""python3 -m tally EXPENSES.csv — print who owes whom."""
from __future__ import annotations

import sys

from .csvio import read_ledger
from .report import render


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: python3 -m tally EXPENSES.csv", file=sys.stderr)
        return 2
    try:
        ledger = read_ledger(argv[0])
    except (OSError, ValueError) as err:
        print(f"tally: {err}", file=sys.stderr)
        return 1
    sys.stdout.write(render(ledger))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
