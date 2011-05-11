#ifndef SPARSEDICT_H
#define SPARSEDICT_H

#include "sparsecoll.h"
#include <google/sparse_hash_map>

struct pyobject_hash
{
	size_t operator()(PyObject *o1) const
	{
		if (o1 == NULL)
			return 0;

		Py_hash_t hash = -1;
		if (PyString_CheckExact(o1)) {
			// strings cache their hash
			hash = ((PyStringObject *) o1)->ob_shash;
		}
		if (hash == -1)
			hash = PyObject_Hash(o1);
		return (size_t) hash;
	}
};

struct pyobject_equal
{
	bool operator()(PyObject *o1, PyObject *o2) const
	{
		if (o1 == o2)
			return true;
		if (o1 == NULL || o2 == NULL)
			return false;
		// errors will result in inequality
		return PyObject_RichCompareBool(o1, o2, Py_EQ) > 0; 
	}
};

typedef google::sparse_hash_map<PyObject *, PyObject *, pyobject_hash, pyobject_equal> pysparse_hash_map;

enum MetaElement { ME_KEYS, ME_VALUES, ME_ITEMS };
enum LoadFactor { LF_CURRENT, LF_MIN, LF_MAX };

struct SparseDict : public PyObject {
    pysparse_hash_map map;
	// incremented every time iterators are invalidated
	Py_ssize_t change_id;

	static PyTypeObject Type;

private:
	static inline int _resize(SparseDict *self, size_t size);

	/* Insert specified key/value (INCREF if necessary). Invalidates iterators.
	   Return 0 on success, -1 on failure. */
	static inline int _insert(SparseDict *self, PyObject *key, PyObject *value);

	/* Remove dictonary entry with the specified key.
	   Return 0 on success, -1 on failure. */
	static inline int _erase(SparseDict *self, PyObject *key);

	/* Insert a key/value pair (sequence of 2). Returns 0 on success, -1 on failure. */
	static inline int _insert_pair(SparseDict *self, PyObject *item);

	/* Merge contents of another SparseDict. Returns 0, always succeeds. */
	static int _update_from_sparsedict(SparseDict *self, SparseDict *other);
	/* Merge contents of a Python dict. Returns 0 on success, -1 on failure. */
	static int _update_from_dict(SparseDict *self, PyObject *dict);
	/* Merge contents of a mapping object (anything with keys() and __getitem__() qualifies).
	   Return 0 on success, -1 on failure. */
	static int _update_from_mapping(SparseDict *self, PyObject *mapping);
	/* Merge contents of a sequence of key-value pairs (sequences of 2).
	   Return 0 on success, -1 on failure. */
	static int _update_from_sequence(SparseDict *self, PyObject *seq);
	/* Return 0 on success, -1 on failure. */
	static int _update(SparseDict *self, PyObject *args, PyObject *kwds);

	/* Drop contained references, resize to zero. */
	static void _clear(SparseDict *self);

	/* Compares SparseDict and arbitrary Python object for equality.
	   Return values: 1 if equal, 0 if not equal, -1 if operation is not supported. */
	static int _equal(SparseDict *self, PyObject *other);

public:
	// dict methods
	static PyObject *update(SparseDict *self, PyObject *args, PyObject *kwds);
	static PyObject *clear(SparseDict *self);
	static PyObject *copy(SparseDict *self);
	static PyObject *sizeof_(SparseDict *self);
	static PyObject *fromkeys(PyObject *cls, PyObject *args);
	static PyObject *pop(SparseDict *self, PyObject *args);
	static PyObject *get(SparseDict *self, PyObject *args);
	static PyObject *setdefault(SparseDict *self, PyObject *args);
	static PyObject *popitem(SparseDict *self, PyObject *);
	static PyObject *contains(SparseDict *self, PyObject *arg);
	static PyObject *getitem(SparseDict *self, PyObject *arg);
	template<MetaElement ME>
	static PyObject *list_any(SparseDict *self, PyObject *);
	static PyObject *items(SparseDict *self, PyObject *);
	template <PyTypeObject *ITER_TYPE>
	static PyObject *iter_any(SparseDict *self, PyObject *);

	// SparseDict methods
	static PyObject *resize(SparseDict *self, PyObject *arg);
	static PyObject *get_load_factor(SparseDict *self, size_t closure);
	static int set_load_factor(SparseDict *self, PyObject *arg, size_t closure);

	// type methods
	static PyObject *tp_new(PyTypeObject *type, PyObject *, PyObject *);
	static void tp_dealloc(SparseDict *self);
	static int tp_init(SparseDict *self, PyObject *args, PyObject *kwds);
	static PyObject *tp_repr(SparseDict *self);
	static PyObject *tp_iter(SparseDict *self);
	static int tp_compare(SparseDict *o1, SparseDict *o2);
	static PyObject *tp_richcompare(PyObject *o1, PyObject *o2, int op);
	static int tp_traverse(SparseDict *self, visitproc visit, void *arg);
	static int tp_clear(SparseDict *self);

	// mapping methods
	static PyObject *mp_subscript(SparseDict *self, PyObject *key);
	static int mp_ass_subscript(SparseDict *self, PyObject *key, PyObject *value);
	static Py_ssize_t mp_length(SparseDict *self);

	// sequence methods
	static int sq_contains(SparseDict *self, PyObject *key);
};

struct SparseDictIter : public PyObject {
	SparseDict *sdict;
	pysparse_hash_map::const_iterator iter;
	Py_ssize_t change_id; // for invalidation check
	Py_ssize_t size; // for invalidation check
	Py_ssize_t remaining_size; // for length_hint
	PyObject *pair; // cached result tuple for iteritems

	static PyTypeObject KeyType;
	static PyTypeObject ValueType;
	static PyTypeObject ItemType;

private:
	static inline bool _check_valid(SparseDictIter *self);
	
public:
	static SparseDictIter *create_new(SparseDict *sdict, PyTypeObject *subtype);
	static PyObject *length_hint(SparseDictIter *self, PyObject *);

	static void tp_dealloc(SparseDictIter *self);
	template <MetaElement ME>
	static PyObject *tp_iternext(SparseDictIter *self);
	static int tp_traverse(SparseDictIter *self, visitproc visit, void *arg);
};

#endif