# borrowed from Python 2.7 test suite, changes:
#   * test SparseDict() instead of dict()
#   * some tests for dict()-specific bugs are dropped

import unittest
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
        d = {}
        with self.assertRaises(KeyError) as c:
            d[(1,)]
        self.assertEqual(c.exception.args, ((1,),))

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

        d = {}
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
        d = {}
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
        iterators = (dict.iteritems, dict.itervalues, dict.iterkeys)
        for i in iterators:
            obj = C()
            ref = weakref.ref(obj)
            container = {obj: 1}
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

    def test_track_literals(self):
        # Test GC-optimization of dict literals
        x, y, z, w = 1.5, "a", (1, None), []

        self._not_tracked({})
        self._not_tracked({x:(), y:x, z:1})
        self._not_tracked({1: "a", "b": 2})
        self._not_tracked({1: 2, (None, True, False, ()): int})
        self._not_tracked({1: object()})

        # Dicts with mutable elements are always tracked, even if those
        # elements are not tracked right now.
        self._tracked({1: []})
        self._tracked({1: ([],)})
        self._tracked({1: {}})
        self._tracked({1: set()})

    def test_track_dynamic(self):
        # Test GC-optimization of dynamically-created dicts
        class MyObject(object):
            pass
        x, y, z, w, o = 1.5, "a", (1, object()), [], MyObject()

        d = dict()
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
        d[4] = None
        self._not_tracked(d)
        self._not_tracked(d.copy())

        # dd isn't tracked right now, but it may mutate and therefore d
        # which contains it must be tracked.
        d = dict()
        dd = dict()
        d[1] = dd
        self._not_tracked(dd)
        self._tracked(d)
        dd[1] = d
        self._tracked(dd)

        d = dict.fromkeys([x, y, z])
        self._not_tracked(d)
        dd = dict()
        dd.update(d)
        self._not_tracked(dd)
        d = dict.fromkeys([x, y, z, o])
        self._tracked(d)
        dd = dict()
        dd.update(d)
        self._tracked(dd)

        d = dict(x=x, y=y, z=z)
        self._not_tracked(d)
        d = dict(x=x, y=y, z=z, w=w)
        self._tracked(d)
        d = dict()
        d.update(x=x, y=y, z=z)
        self._not_tracked(d)
        d.update(w=w)
        self._tracked(d)

        d = dict([(x, y), (z, 1)])
        self._not_tracked(d)
        d = dict([(x, y), (z, w)])
        self._tracked(d)
        d = dict()
        d.update([(x, y), (z, 1)])
        self._not_tracked(d)
        d.update([(x, y), (z, w)])
        self._tracked(d)

    def test_track_subtypes(self):
        # Dict subtypes are always tracked
        class MyDict(dict):
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
