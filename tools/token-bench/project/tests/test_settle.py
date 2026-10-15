import unittest

from tally.settle import Transfer, settle


class SettleTest(unittest.TestCase):
    def test_one_debtor(self):
        self.assertEqual(
            settle({"a": 500, "b": -500}),
            [Transfer("b", "a", 500)],
        )

    def test_biggest_first(self):
        self.assertEqual(
            settle({"a": 700, "b": 300, "c": -600, "d": -400}),
            [Transfer("c", "a", 600), Transfer("d", "b", 300), Transfer("d", "a", 100)],
        )

    def test_square(self):
        self.assertEqual(settle({"a": 0, "b": 0}), [])

    def test_unbalanced(self):
        with self.assertRaises(ValueError):
            settle({"a": 1})


if __name__ == "__main__":
    unittest.main()
