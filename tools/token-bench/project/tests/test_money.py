import unittest

from tally import money


class ParseTest(unittest.TestCase):
    def test_whole_and_cents(self):
        self.assertEqual(money.parse("12.34"), 1234)
        self.assertEqual(money.parse("0.05"), 5)
        self.assertEqual(money.parse("7"), 700)

    def test_negative(self):
        self.assertEqual(money.parse("-3.10"), -310)

    def test_surrounding_space(self):
        self.assertEqual(money.parse("  8.00 "), 800)

    def test_rejects_garbage(self):
        for bad in ["", ".", "abc", "1.2.3", "1.234", "1,50", "--2"]:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                money.parse(bad)


class FormatTest(unittest.TestCase):
    def test_format(self):
        self.assertEqual(money.format(1234), "12.34")
        self.assertEqual(money.format(5), "0.05")
        self.assertEqual(money.format(-310), "-3.10")
        self.assertEqual(money.format(0), "0.00")


if __name__ == "__main__":
    unittest.main()
