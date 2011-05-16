import unittest
import random
from sparsedict import SparseDict


class TestSparseDictStress(unittest.TestCase):

    def test_stress(self):
        items = range(1000)
        random.seed(0)
        random.shuffle(items)

        for i in xrange(len(items)):
            d = SparseDict()
            items_part = set(items[:i])
            for item in items_part:
                d[item] = item
            self.assertEqual(set(d), items_part)
            self.assertEqual(set(d.iterkeys()), items_part)
            self.assertEqual(set(d.itervalues()), items_part)
            self.assertEqual(set(d.keys()), items_part)
            self.assertEqual(set(d.values()), items_part)
            for item in items_part:
                del d[item]
            self.assertEqual(len(d), 0)
            self.assertEqual(d, {})
