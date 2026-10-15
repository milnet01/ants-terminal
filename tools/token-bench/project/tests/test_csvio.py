import os
import tempfile
import unittest

from tally.csvio import parse_participants, read_ledger


def write_csv(text):
    fd, path = tempfile.mkstemp(suffix=".csv")
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        fh.write(text)
    return path


class ParticipantsTest(unittest.TestCase):
    def test_plain(self):
        self.assertEqual(parse_participants("Ana; Ben ;Cy"), (("Ana", "Ben", "Cy"), None))

    def test_weighted(self):
        self.assertEqual(parse_participants("Ana*2;Ben"), (("Ana", "Ben"), {"Ana": 2, "Ben": 1}))


class ReadLedgerTest(unittest.TestCase):
    def test_reads_rows(self):
        path = write_csv(
            "date,payer,amount,participants,note\n"
            "2026-01-01,Ana,30.00,Ana;Ben,lunch\n"
            "2026-01-02,Ben,10.50,Ana*2;Ben,taxi\n"
        )
        self.addCleanup(os.remove, path)
        ledger = read_ledger(path)
        self.assertEqual(ledger.total(), 4050)
        self.assertEqual(ledger.balances(), {"Ana": 800, "Ben": -800})

    def test_wrong_columns(self):
        path = write_csv("when,who,how_much\n")
        self.addCleanup(os.remove, path)
        with self.assertRaises(ValueError):
            read_ledger(path)

    def test_bad_amount_names_the_line(self):
        path = write_csv(
            "date,payer,amount,participants,note\n"
            "2026-01-01,Ana,lots,Ana;Ben,lunch\n"
        )
        self.addCleanup(os.remove, path)
        with self.assertRaisesRegex(ValueError, r":2: "):
            read_ledger(path)


if __name__ == "__main__":
    unittest.main()
