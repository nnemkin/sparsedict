# tests for key, value and item views
# borrowed from Python 2.7 test suite, changes:
#   * test SparseDict() instead of dict()
#   * do not use set literal syntax

import unittest2 as unittest
from sparsedict import SparseDict


class SparseDictSetTest(unittest.TestCase):

    def test_constructors_not_callable(self):
        kt = type(SparseDict().viewkeys())
        self.assertRaises(TypeError, kt, SparseDict())
        self.assertRaises(TypeError, kt)
        it = type(SparseDict().viewitems())
        self.assertRaises(TypeError, it, SparseDict())
        self.assertRaises(TypeError, it)
        vt = type(SparseDict().viewvalues())
        self.assertRaises(TypeError, vt, SparseDict())
        self.assertRaises(TypeError, vt)

    def test_dict_keys(self):
        d = SparseDict({1: 10, "a": "ABC"})
        keys = d.viewkeys()
        self.assertEqual(len(keys), 2)
        self.assertEqual(set(keys), set([1, "a"]))
        self.assertEqual(keys, set([1, "a"]))
        self.assertNotEqual(keys, set([1, "a", "b"]))
        self.assertNotEqual(keys, set([1, "b"]))
        self.assertNotEqual(keys, set([1]))
        self.assertNotEqual(keys, 42)
        self.assertIn(1, keys)
        self.assertIn("a", keys)
        self.assertNotIn(10, keys)
        self.assertNotIn("Z", keys)
        self.assertEqual(d.viewkeys(), d.viewkeys())
        e = SparseDict({1: 11, "a": "def"})
        self.assertEqual(d.viewkeys(), e.viewkeys())
        del e["a"]
        self.assertNotEqual(d.viewkeys(), e.viewkeys())

    def test_dict_items(self):
        d = SparseDict({1: 10, "a": "ABC"})
        items = d.viewitems()
        self.assertEqual(len(items), 2)
        self.assertEqual(set(items), set([(1, 10), ("a", "ABC")]))
        self.assertEqual(items, set([(1, 10), ("a", "ABC")]))
        self.assertNotEqual(items, set([(1, 10), ("a", "ABC"), "junk"]))
        self.assertNotEqual(items, set([(1, 10), ("a", "def")]))
        self.assertNotEqual(items, set([(1, 10)]))
        self.assertNotEqual(items, 42)
        self.assertIn((1, 10), items)
        self.assertIn(("a", "ABC"), items)
        self.assertNotIn((1, 11), items)
        self.assertNotIn(1, items)
        self.assertNotIn((), items)
        self.assertNotIn((1,), items)
        self.assertNotIn((1, 2, 3), items)
        self.assertEqual(d.viewitems(), d.viewitems())
        e = d.copy()
        self.assertEqual(d.viewitems(), e.viewitems())
        e["a"] = "def"
        self.assertNotEqual(d.viewitems(), e.viewitems())

    def test_dict_mixed_keys_items(self):
        d = SparseDict({(1, 1): 11, (2, 2): 22})
        e = SparseDict({1: 1, 2: 2})
        self.assertEqual(d.viewkeys(), e.viewitems())
        self.assertNotEqual(d.viewitems(), e.viewkeys())

    def test_dict_values(self):
        d = SparseDict({1: 10, "a": "ABC"})
        values = d.viewvalues()
        self.assertEqual(set(values), set([10, "ABC"]))
        self.assertEqual(len(values), 2)

    def test_dict_repr(self):
        d = SparseDict({1: 10, "a": "ABC"})
        self.assertIsInstance(repr(d), str)
        r = repr(d.viewitems())
        self.assertIsInstance(r, str)
        self.assertTrue(r == "SparseDict_Items([('a', 'ABC'), (1, 10)])" or
                        r == "SparseDict_Items([(1, 10), ('a', 'ABC')])")
        r = repr(d.viewkeys())
        self.assertIsInstance(r, str)
        self.assertTrue(r == "SparseDict_Keys(['a', 1])" or
                        r == "SparseDict_Keys([1, 'a'])")
        r = repr(d.viewvalues())
        self.assertIsInstance(r, str)
        self.assertTrue(r == "SparseDict_Values(['ABC', 10])" or
                        r == "SparseDict_Values([10, 'ABC'])")

    def test_keys_set_operations(self):
        d1 = SparseDict({'a': 1, 'b': 2})
        d2 = SparseDict({'b': 3, 'c': 2})
        d3 = SparseDict({'d': 4, 'e': 5})
        self.assertEqual(d1.viewkeys() & d1.viewkeys(), set(['a', 'b']))
        self.assertEqual(d1.viewkeys() & d2.viewkeys(), set(['b']))
        self.assertEqual(d1.viewkeys() & d3.viewkeys(), set())
        self.assertEqual(d1.viewkeys() & set(d1.viewkeys()), set(['a', 'b']))
        self.assertEqual(d1.viewkeys() & set(d2.viewkeys()), set(['b']))
        self.assertEqual(d1.viewkeys() & set(d3.viewkeys()), set())

        self.assertEqual(d1.viewkeys() | d1.viewkeys(), set(['a', 'b']))
        self.assertEqual(d1.viewkeys() | d2.viewkeys(), set(['a', 'b', 'c']))
        self.assertEqual(d1.viewkeys() | d3.viewkeys(), set(['a', 'b', 'd', 'e']))
        self.assertEqual(d1.viewkeys() | set(d1.viewkeys()), set(['a', 'b']))
        self.assertEqual(d1.viewkeys() | set(d2.viewkeys()), set(['a', 'b', 'c']))
        self.assertEqual(d1.viewkeys() | set(d3.viewkeys()),
                         set(['a', 'b', 'd', 'e']))

        self.assertEqual(d1.viewkeys() ^ d1.viewkeys(), set())
        self.assertEqual(d1.viewkeys() ^ d2.viewkeys(), set(['a', 'c']))
        self.assertEqual(d1.viewkeys() ^ d3.viewkeys(), set(['a', 'b', 'd', 'e']))
        self.assertEqual(d1.viewkeys() ^ set(d1.viewkeys()), set())
        self.assertEqual(d1.viewkeys() ^ set(d2.viewkeys()), set(['a', 'c']))
        self.assertEqual(d1.viewkeys() ^ set(d3.viewkeys()),
                         set(['a', 'b', 'd', 'e']))

        self.assertEqual(d1.viewkeys() - d1.viewkeys(), set())
        self.assertEqual(d1.viewkeys() - d2.viewkeys(), set(['a']))
        self.assertEqual(d1.viewkeys() - d3.viewkeys(), set(['a', 'b']))
        self.assertEqual(d1.viewkeys() - set(d1.viewkeys()), set())
        self.assertEqual(d1.viewkeys() - set(d2.viewkeys()), set(['a']))
        self.assertEqual(d1.viewkeys() - set(d3.viewkeys()), set(['a', 'b']))

        self.assertFalse(d1.viewkeys().isdisjoint(d2.viewkeys()))
        self.assertFalse(d1.viewkeys().isdisjoint(set(d2.viewkeys())))
        self.assertTrue(d1.viewkeys().isdisjoint(d3.viewkeys()))
        self.assertTrue(d1.viewkeys().isdisjoint(set(d3.viewkeys())))


    def test_items_set_operations(self):
        d1 = SparseDict({'a': 1, 'b': 2})
        d2 = SparseDict({'a': 2, 'b': 2})
        d3 = SparseDict({'d': 4, 'e': 5})
        self.assertEqual(
            d1.viewitems() & d1.viewitems(), set([('a', 1), ('b', 2)]))
        self.assertEqual(d1.viewitems() & d2.viewitems(), set([('b', 2)]))
        self.assertEqual(d1.viewitems() & d3.viewitems(), set())
        self.assertEqual(d1.viewitems() & set(d1.viewitems()),
                         set([('a', 1), ('b', 2)]))
        self.assertEqual(d1.viewitems() & set(d2.viewitems()), set([('b', 2)]))
        self.assertEqual(d1.viewitems() & set(d3.viewitems()), set())

        self.assertEqual(d1.viewitems() | d1.viewitems(),
                         set([('a', 1), ('b', 2)]))
        self.assertEqual(d1.viewitems() | d2.viewitems(),
                         set([('a', 1), ('a', 2), ('b', 2)]))
        self.assertEqual(d1.viewitems() | d3.viewitems(),
                         set([('a', 1), ('b', 2), ('d', 4), ('e', 5)]))
        self.assertEqual(d1.viewitems() | set(d1.viewitems()),
                         set([('a', 1), ('b', 2)]))
        self.assertEqual(d1.viewitems() | set(d2.viewitems()),
                         set([('a', 1), ('a', 2), ('b', 2)]))
        self.assertEqual(d1.viewitems() | set(d3.viewitems()),
                         set([('a', 1), ('b', 2), ('d', 4), ('e', 5)]))

        self.assertEqual(d1.viewitems() ^ d1.viewitems(), set())
        self.assertEqual(d1.viewitems() ^ d2.viewitems(),
                         set([('a', 1), ('a', 2)]))
        self.assertEqual(d1.viewitems() ^ d3.viewitems(),
                         set([('a', 1), ('b', 2), ('d', 4), ('e', 5)]))

        self.assertEqual(d1.viewitems() - d1.viewitems(), set())
        self.assertEqual(d1.viewitems() - d2.viewitems(), set([('a', 1)]))
        self.assertEqual(d1.viewitems() - d3.viewitems(), set([('a', 1), ('b', 2)]))

        self.assertFalse(d1.viewitems().isdisjoint(d2.viewitems()))
        self.assertFalse(d1.viewitems().isdisjoint(set(d2.viewitems())))
        self.assertTrue(d1.viewitems().isdisjoint(d3.viewitems()))
        self.assertTrue(d1.viewitems().isdisjoint(set(d3.viewitems())))
