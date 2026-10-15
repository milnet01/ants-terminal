import unittest

from tally import split


class SplitEvenlyTest(unittest.TestCase):
    def test_even(self):
        self.assertEqual(split.split_evenly(900, ["a", "b", "c"]), {"a": 300, "b": 300, "c": 300})

    def test_leftover_cents_go_to_the_first_people(self):
        self.assertEqual(split.split_evenly(1001, ["a", "b", "c"]), {"a": 334, "b": 334, "c": 333})

    def test_nobody(self):
        with self.assertRaises(ValueError):
            split.split_evenly(100, [])

    def test_duplicate(self):
        with self.assertRaises(ValueError):
            split.split_evenly(100, ["a", "a"])


class SplitByWeightTest(unittest.TestCase):
    def test_weights(self):
        self.assertEqual(split.split_by_weight(1000, {"a": 2, "b": 1, "c": 1}), {"a": 500, "b": 250, "c": 250})

    def test_shares_add_up(self):
        shares = split.split_by_weight(1000, {"a": 1, "b": 1, "c": 1})
        self.assertEqual(sum(shares.values()), 1000)
        self.assertEqual(shares, {"a": 334, "b": 333, "c": 333})

    def test_bad_weights(self):
        for weights in [{}, {"a": 0}, {"a": 2, "b": -1}]:
            with self.subTest(weights=weights), self.assertRaises(ValueError):
                split.split_by_weight(100, weights)


if __name__ == "__main__":
    unittest.main()
