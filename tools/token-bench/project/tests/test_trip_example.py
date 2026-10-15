"""The worked example in examples/trip.csv, checked against its receipts."""
import io
import unittest
from contextlib import redirect_stdout
from pathlib import Path

from tally.__main__ import main
from tally.csvio import read_ledger
from tally.settle import Transfer, settle

TRIP = Path(__file__).resolve().parent.parent / "examples" / "trip.csv"


class TripExampleTest(unittest.TestCase):
    def setUp(self):
        self.ledger = read_ledger(TRIP)

    def test_total_matches_the_receipts(self):
        # 120.00 + 40.50 + 12.80 + 9.99 + 60.00 + 33.33
        self.assertEqual(self.ledger.total(), 27662)

    def test_balances(self):
        self.assertEqual(
            self.ledger.balances(),
            {"Ana": 4675, "Ben": 1368, "Cy": -7043, "Dee": 1000},
        )

    def test_settlement(self):
        self.assertEqual(
            settle(self.ledger.balances()),
            [Transfer("Cy", "Ana", 4675), Transfer("Cy", "Ben", 1368), Transfer("Cy", "Dee", 1000)],
        )

    def test_printed_report(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(main([str(TRIP)]), 0)
        self.assertIn("Total spent: 276.62\n", out.getvalue())
        self.assertIn("Cy pays Ana 46.75\n", out.getvalue())


if __name__ == "__main__":
    unittest.main()
