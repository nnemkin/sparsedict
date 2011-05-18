# borrowed from Python 2.7 test suite, changes:
#   * test SparseDict() instead of dict()
#   * some tests for dict()-specific bugs are dropped

from __future__ import with_statement
import unittest2 as unittest
import weakref
import gc
import random
import pickle
from . import mapping_tests
from sparsedict import SparseDict


class SparseDictSubclass(SparseDict):

    def __init__(self, *args, **kwargs):
        SparseDict.__init__(self, *args, **kwargs)
        self.a = 'aval' # to test unpickling
        self.b = 'bval'


class TestGeneralMapping(mapping_tests.TestHashMappingProtocol):

    type2test = SparseDict

    def _full_mapping(self, data):
        return self.type2test(data)


class TestSubclassMapping(mapping_tests.TestHashMappingProtocol):

    type2test = SparseDictSubclass

    def _full_mapping(self, data):
        return self.type2test(data)


class TestSparseDictAsDict(unittest.TestCase):

    def test_tuple_keyerror(self):
        # SF #1576657
        d = SparseDict()
        with self.assertRaises(KeyError) as c:
            d[(1,)]
        if isinstance(c.exception, Exception):
            self.assertEqual(c.exception.args, ((1,),))
        else:
            # work around 2.6 bug (issue 7853)
            self.assertEqual(c.exception, ((1,),))

    def test_bad_key(self):
        # Dictionary lookups should fail if __cmp__() raises an exception.
        class CustomException(Exception):
            pass

        class BadDictKey:
            def __hash__(self):
                return hash(self.__class__)

            def __cmp__(self, other):
                if isinstance(other, self.__class__):
                    raise CustomException
                return other

        d = SparseDict()
        x1 = BadDictKey()
        x2 = BadDictKey()
        d[x1] = 1
        for stmt in ['d[x2] = 2',
                     'z = d[x2]',
                     'x2 in d',
                     'd.has_key(x2)',
                     'd.get(x2)',
                     'd.setdefault(x2, 42)',
                     'd.pop(x2)',
                     'd.update({x2: 2})']:
            with self.assertRaises(CustomException):
                exec stmt in locals()

    def test_resize2(self):
        # Another dict resizing bug (SF bug #1456209).
        # This caused Segmentation faults or Illegal instructions.

        class X(object):
            def __hash__(self):
                return 5
            def __eq__(self, other):
                if resizing:
                    d.clear()
                return False
        d = SparseDict()
        resizing = False
        d[X()] = 1
        d[X()] = 2
        d[X()] = 3
        d[X()] = 4
        d[X()] = 5
        # now trigger a resize
        resizing = True
        d[9] = 6

    def test_container_iterator(self):
        # Bug #3680: tp_traverse was not implemented for dictiter objects
        class C(object):
            pass
        iterators = (SparseDict.iteritems, SparseDict.itervalues, SparseDict.iterkeys)
        for i in iterators:
            obj = C()
            ref = weakref.ref(obj)
            container = SparseDict([(obj, 1)])
            obj.x = i(container)
            del obj, container
            gc.collect()
            self.assertIs(ref(), None, "Cycle was not collected")

    def _not_tracked(self, t):
        # Nested containers can take several collections to untrack
        gc.collect()
        gc.collect()
        self.assertFalse(gc.is_tracked(t), t)

    def _tracked(self, t):
        self.assertTrue(gc.is_tracked(t), t)
        gc.collect()
        gc.collect()
        self.assertTrue(gc.is_tracked(t), t)

    @unittest.skipIf(not hasattr(gc, 'is_tracked'), 'missing gc.is_tracked')
    def test_track_literals(self):
        # Test GC-optimization of dict literals
        x, y, z, w = 1.5, "a", (1, None), []

        self._not_tracked(SparseDict())
        self._not_tracked(SparseDict({x:(), y:x, z:1}))
        self._not_tracked(SparseDict({1: "a", "b": 2}))
        # (None, True, False, ()) tuple is untracked on GC but SparseDict itself is not
        # self._not_tracked(SparseDict({1: 2, (None, True, False, ()): int}))
        self._not_tracked(SparseDict({1: object()}))

        # Dicts with mutable elements are always tracked, even if those
        # elements are not tracked right now.
        self._tracked(SparseDict({1: []}))
        self._tracked(SparseDict({1: ([],)}))
        self._tracked(SparseDict({1: {}}))
        self._tracked(SparseDict({1: set()}))

    @unittest.skipIf(not hasattr(gc, 'is_tracked'), 'missing gc.is_tracked')
    def test_track_dynamic(self):
        # Test GC-optimization of dynamically-created dicts
        class MyObject(object):
            pass
        x, y, z, w, o = 1.5, "a", (1, object()), [], MyObject()

        d = SparseDict()
        self._not_tracked(d)
        d[1] = "a"
        self._not_tracked(d)
        d[y] = 2
        self._not_tracked(d)
        d[z] = 3
        self._not_tracked(d)
        self._not_tracked(d.copy())
        d[4] = w
        self._tracked(d)
        self._tracked(d.copy())
        # Unlike dict, we don't have a _PyDict_MaybeUntrack hook
        # d[4] = None
        # self._not_tracked(d)
        # self._not_tracked(d.copy())

        # dd isn't tracked right now, but it may mutate and therefore d
        # which contains it must be tracked.
        d = SparseDict()
        dd = SparseDict()
        d[1] = dd
        self._not_tracked(dd)
        self._tracked(d)
        dd[1] = d
        self._tracked(dd)

        d = SparseDict.fromkeys([x, y, z])
        self._not_tracked(d)
        dd = SparseDict()
        dd.update(d)
        self._not_tracked(dd)
        d = SparseDict.fromkeys([x, y, z, o])
        self._tracked(d)
        dd = SparseDict()
        dd.update(d)
        self._tracked(dd)

        d = SparseDict(x=x, y=y, z=z)
        self._not_tracked(d)
        d = SparseDict(x=x, y=y, z=z, w=w)
        self._tracked(d)
        d = SparseDict()
        d.update(x=x, y=y, z=z)
        self._not_tracked(d)
        d.update(w=w)
        self._tracked(d)

        d = SparseDict([(x, y), (z, 1)])
        self._not_tracked(d)
        d = SparseDict([(x, y), (z, w)])
        self._tracked(d)
        d = SparseDict()
        d.update([(x, y), (z, 1)])
        self._not_tracked(d)
        d.update([(x, y), (z, w)])
        self._tracked(d)

    @unittest.skipIf(not hasattr(gc, 'is_tracked'), 'missing gc.is_tracked')
    def test_track_subtypes(self):
        # SparseDict subtypes are always tracked
        class MyDict(SparseDict):
            pass
        self._tracked(MyDict())

    def test_pickle(self):
        d = SparseDict({1: 'a', 'b': 2, ('c',): [3, 4]})
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pd = pickle.loads(pickle.dumps(d, proto))
            self.assertEquals(d, pd)

        d = SparseDictSubclass({1: 'a', 'b': 2, ('c',): [3, 4]})
        for proto in range(pickle.HIGHEST_PROTOCOL + 1):
            pd = pickle.loads(pickle.dumps(d, proto))
            self.assertEquals(d, pd)
            self.assertEquals(pd.a, 'aval')
            self.assertEquals(pd.b, 'bval')

    def test_repr_roundtrip(self):
        d = SparseDict()
        pd = eval(repr(d))
        self.assertEqual(d, pd)
        self.assertIs(type(d), type(pd))

        d = SparseDict({1: 'a', 'b': 2, ('c',): [3, 4]})
        pd = eval(repr(d))
        self.assertEqual(d, pd)
        self.assertIs(type(d), type(pd))

    def test_resize_reentrancy(self):
        d = SparseDict()
        class Key(int):
            resize = False
            def __hash__(self):
                if self.resize:
                    d.resize(128)
                return int.__hash__(self)

        d[Key(0)] = 0
        Key.resize = True
        self.assertRaises(RuntimeError, lambda: d.resize(64))

    def test_shrink_to_static(self):
        d = SparseDict()
        d[0] = 0
        static_size = d.__sizeof__()
        for i in xrange(100):
            d[i] = i
        for i in xrange(100):
            del d[i]
        d[0] = 0
        self.assertEqual(d.__sizeof__(), static_size)

    def test_stats(self):
        d = SparseDict({1: 'a', 'b': 2, ('c',): [3, 4]})
        stats = d._stats()
        self.assertIsInstance(stats, dict, stats)
        for key in ["block_size", "num_blocks", "max_items", "num_items", "num_deleted",
                    "consider_shrink", "disable_resize", "string_lookup"]:
            self.assertIn(key, stats, key)
