import unittest2 as unittest
import random
from sparsedict import SparseDict


class TestSparseDictStress(unittest.TestCase):

    def _stress(self, items):
        # needs improvement
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
            d[0] = 0
            self.assertEqual(len(d), 1)
            self.assertEqual(d, {0: 0})

    def test_stress(self):
        random.seed(0)

        items = range(1000)
        random.shuffle(items)
        self._stress(items)

        items = map(str, xrange(1000))
        random.shuffle(items)
        self._stress(items)

        items = [random.getrandbits(31) for i in xrange(1000)]
        self._stress(items)

        items = range(100) + map(str, xrange(100))
        self._stress(items)

    def test_string_rehash(self):
        d = dict(('\x00' * i, i) for i in xrange(100))
        sd = SparseDict(d)
        sd.resize(200)
        self.assertEqual(d, sd)
