/*
	TODO:
	* check if end() caching improves performance
	* check if delayed GC tracking affects performance
	* check performance impact of Py_ErrOccured
*/

#include "sparsedict.h"

using namespace std;
using namespace google;

/* Set KeyError exception. */
static inline void _key_error(PyObject *key)
{
	PyObject *args = PyTuple_Pack(1, key);
    if (args != NULL) {
		PyErr_SetObject(PyExc_KeyError, args);
		Py_DECREF(args);
	}
}

// SparseDict

int SparseDict::_resize(SparseDict *self, size_t size)
{
	++self->change_id;
	self->map.resize(size);
	if (PyErr_Occurred())
		return -1;
	return 0;
}

int SparseDict::_insert(SparseDict *self, PyObject *key, PyObject *value)
{
	pair<pysparse_hash_map::iterator, bool> result = self->map.insert(pysparse_hash_map::value_type(key, value));
	if (PyErr_Occurred()) {
		if (result.second) {
			++self->change_id;
			self->map.erase(result.first); // undo insert
		}
		return -1;
	}

	Py_INCREF(value);
	if (result.second) {
		// new entry was inserted
		++self->change_id;
		Py_INCREF(key);
	}
	else {
		// result.first points to an existing entry, old key is reused
		PyObject *prev_value = result.first->second;
		result.first->second = value;
		Py_DECREF(prev_value);
	}
	return 0;
}

int SparseDict::_erase(SparseDict *self, PyObject *key)
{
	pysparse_hash_map::iterator it = self->map.find(key);
	if (PyErr_Occurred())
		return -1;
	if (it == self->map.end()) {
		_key_error(key);
		return -1;
	}
	key = it->first;
	PyObject *value = it->second;
	self->map.erase(it);
	Py_DECREF(key);
	Py_DECREF(value);
	return 0;
}

int SparseDict::_insert_pair(SparseDict *self, PyObject *item)
{
	PyObject *pair = PySequence_Fast(item, "");
	if (pair == NULL)
		return -1;
	if (PySequence_Fast_GET_SIZE(pair) != 2) {
		Py_DECREF(pair);
		PyErr_Format(PyExc_ValueError, "update sequence elements must be pairs");
		return -1;
	}
	PyObject *key = PySequence_Fast_GET_ITEM(pair, 0);
	PyObject *value = PySequence_Fast_GET_ITEM(pair, 1);
	int result = _insert(self, key, value);
	Py_DECREF(pair);
	return result;
}

int SparseDict::_update_from_sparsedict(SparseDict *self, SparseDict *other)
{
	if (self->map.empty()) {
		++self->change_id;
		self->map = other->map;
		for (pysparse_hash_map::const_iterator it = self->map.begin(); it != self->map.end(); ++it) {
			Py_INCREF(it->first);
			Py_INCREF(it->second);
		}
	}
	else {
		// keys may overlap therefore we presize to conservative minimum
		if (self->map.size() < other->map.size())
			if (_resize(self, other->map.size()) < 0)
				return -1;
		
		for (pysparse_hash_map::const_iterator it = other->map.begin(); it != other->map.end(); ++it)
			if (_insert(self, it->first, it->second) < 0)
				return -1;
	}
	return 0;
}

int SparseDict::_update_from_dict(SparseDict *self, PyObject *dict)
{
	Py_ssize_t size = PyDict_Size(dict);
	if (size <= 0)
		return (int) size;

	if (_resize(self, self->map.size() + size) < 0)
		return -1;

	Py_ssize_t pos = 0;
	PyObject *key, *value;
	while (PyDict_Next(dict, &pos, &key, &value))
		if (_insert(self, key, value) < 0)
			return -1;
	return 0;
}

int SparseDict::_update_from_mapping(SparseDict *self, PyObject *mapping)
{
	PyObject *keys = PyMapping_Keys(mapping);
	if (keys == NULL)
		return -1;

	int result = 0;
	if (PyList_CheckExact(keys)) {
		// keys is a list, which means known size and fast iteration
		Py_ssize_t size = PyList_GET_SIZE(keys);
		if (_resize(self, self->map.size() + size) < 0)
			return -1;

		for (Py_ssize_t i = 0; i < size; ++i) {
			PyObject *key = PyList_GET_ITEM(keys, i);
			PyObject *value = PyObject_GetItem(mapping, key);
			if (value == NULL) {
				result = -1;
				break;
			}
			result = _insert(self, key, value);
			Py_DECREF(value);
			if (result < 0)
				break;
		}
		Py_DECREF(keys);
	}
	else {
		// keys is an arbitrary sequence, use iteration protocol
		PyObject *key_iter = PyObject_GetIter(keys);
		Py_DECREF(keys);
		if (key_iter == NULL)
			return -1;

		PyObject *key;
		while ((key = PyIter_Next(key_iter)) != NULL) {
			PyObject *value = PyObject_GetItem(mapping, key);
			if (value == NULL)
				result = -1;
			else
				result = _insert(self, key, value);
			Py_DECREF(key);
			Py_XDECREF(value);
			if (result < 0)
				break;
		}
		Py_DECREF(key_iter);
		if (PyErr_Occurred()) // any exception in the above loop
			result = -1; 
	}
	return result;
}

int SparseDict::_update_from_sequence(SparseDict *self, PyObject *seq)
{
	if (PyList_CheckExact(seq)) {
		Py_ssize_t size = PyList_GET_SIZE(seq);
		if (_resize(self, self->map.size() + size) < 0)
			return -1;

		for (Py_ssize_t i = 0; i < size; ++i)
			if (_insert_pair(self, PyList_GET_ITEM(seq, i)) < 0)
				return -1;
	}
	else {
		PyObject *iter = PyObject_GetIter(seq);
		if (!iter)
			return -1;

		PyObject *item;
		while ((item = PyIter_Next(iter)) != NULL)
			if (_insert_pair(self, item) < 0)
				break;
		Py_DECREF(iter);
		if (PyErr_Occurred()) // exception in PyIter_Next or _insert_pair
			return -1;
	}
	return 0;
}

int SparseDict::_update(SparseDict *self, PyObject *args, PyObject *kwds)
{
	PyObject *arg = NULL;
	if (!PyArg_UnpackTuple(args, "SparseDict", 0, 1, &arg))
		return -1;

	int result = 0;
	if (arg != NULL) {
		if (PyObject_TypeCheck(arg, &Type))
			result = _update_from_sparsedict(self, static_cast<SparseDict *>(arg));
		else if (PyDict_Check(arg))
			result = _update_from_dict(self, arg);
		else if (PyObject_HasAttrString(arg, "keys"))
			result = _update_from_mapping(self, arg);
		else
			result = _update_from_sequence(self, arg);
	}
	if (result == 0 && kwds != NULL)
		if (PyArg_ValidateKeywordArguments(kwds))
			result = _update_from_dict(self, kwds);
		else
			result = -1;
	return result;
}

void SparseDict::_clear(SparseDict *self)
{
	++self->change_id;
	pysparse_hash_map prev_map;
	prev_map.swap(self->map);

	for (pysparse_hash_map::const_iterator it = prev_map.begin(); it != prev_map.end(); ++it) {
		Py_DECREF(it->first);
		Py_DECREF(it->second);
	}
}

int SparseDict::_equal(SparseDict *self, PyObject *other)
{
	if (self == other)
		return 1;

	if (PyObject_TypeCheck(other, &Type)) {
		SparseDict *other_sd = static_cast<SparseDict *>(other);
		if (self->map.size() != other_sd->map.size())
			return 0;

		Py_ssize_t change_id = self->change_id; // TODO
		for (pysparse_hash_map::const_iterator it = self->map.begin(); it != self->map.end(); ++it) {
			pysparse_hash_map::const_iterator other_it = other_sd->map.find(it->first);
			if (PyErr_Occurred())
				return -1;
			if (other_it == other_sd->map.end())
				return 0;
			if (it->second == other_it->second)
				continue;

			int cmp = PyObject_RichCompareBool(it->second, other_it->second, Py_EQ);
			if (cmp <= 0)
				return cmp;
		}
		return 1;
	}
	else if (PyDict_CheckExact(other)) {
		if (PyDict_Size(other) != (Py_ssize_t) self->map.size())
			return 0;

		for (pysparse_hash_map::const_iterator it = self->map.begin(); it != self->map.end(); ++it) {
#ifdef PyDict_GetItemWithError
			PyObject *value = PyDict_GetItemWithError(other, it->first);
			if (value == NULL)
				return PyErr_Occured() ? -1 : 0;
#else
			PyObject *value = PyDict_GetItem(other, it->first);
			if (value == NULL)
				return 0;
#endif
			if (value == it->second)
				continue;
			
			int cmp = PyObject_RichCompareBool(it->second, value, Py_EQ);
			if (cmp <= 0)
				return cmp;
		}
		return 1;
	}

	// TODO: other mapping types

	return -1; // unsupported type for other
}

PyObject *SparseDict::update(SparseDict *self, PyObject *args, PyObject *kwds)
{
	if (_update(self, args, kwds) == -1)
		return NULL;
	Py_RETURN_NONE;
}

PyObject *SparseDict::clear(SparseDict *self)
{
	_clear(self);
	Py_RETURN_NONE;
}

PyObject *SparseDict::copy(SparseDict *self)
{
	SparseDict *new_dict = static_cast<SparseDict *>(Py_TYPE(self)->tp_new(Py_TYPE(self), NULL, NULL));
	_update_from_sparsedict(new_dict, self);
	return new_dict;
}

PyObject *SparseDict::sizeof_(SparseDict *self)
{
	// Note: is not precise, but pretty close
	Py_ssize_t result = sizeof(SparseDict);
	result += self->map.size() * sizeof(pysparse_hash_map::value_type);
	result += (self->map.size() * 5) / 8; // assume 5 bit/entry overhead
	return PyInt_FromSsize_t(result);
}

PyObject *SparseDict::fromkeys(PyObject *cls, PyObject *args)
{
	PyObject *keys, *value = Py_None;
	if (!PyArg_UnpackTuple(args, "fromkeys", 1, 2, &keys, &value))
		return NULL;

	PyObject *self = PyObject_CallObject(cls, NULL);
	if (self == NULL)
		return NULL;
	
	// TODO: special cases on keys (list, dict, set)

	PyObject *key_iter = PyObject_GetIter(keys);
	if (key_iter == NULL){
		Py_DECREF(self);
		return NULL;
	}

	PyObject *key;
	while ((key = PyIter_Next(key_iter)) != NULL) {
		int status = PyObject_SetItem(self, key, value);
		Py_DECREF(key);
		if (status < 0)
			break;
	}
	Py_DECREF(key_iter);
	if (PyErr_Occurred()) {
		Py_DECREF(self);
		return NULL;
	}
	return self;
}

PyObject *SparseDict::pop(SparseDict *self, PyObject *args)
{
	PyObject *key, *value = NULL;
	if (!PyArg_UnpackTuple(args, "pop", 1, 2, &key, &value))
		return NULL;

	pysparse_hash_map::iterator it = self->map.find(key);
	if (PyErr_Occurred())
		return NULL;
	if (it != self->map.end()) {
		key = it->first;
		value = it->second;
		self->map.erase(it);
		Py_DECREF(key);
	}
	else if (value == NULL) {
		_key_error(key);
	}
	else {
		Py_INCREF(value);
	}
	return value;
}

PyObject *SparseDict::get(SparseDict *self, PyObject *args)
{
	PyObject *key, *value = Py_None;
	if (!PyArg_UnpackTuple(args, "get", 1, 2, &key, &value))
		return NULL;

	pysparse_hash_map::iterator it = self->map.find(key);
	if (PyErr_Occurred())
		return NULL;
	if (it != self->map.end())
		value = it->second;
	Py_INCREF(value);
	return value;
}

PyObject *SparseDict::setdefault(SparseDict *self, PyObject *args)
{
	PyObject *key, *value = Py_None;
	if (!PyArg_UnpackTuple(args, "setdefault", 1, 2, &key, &value))
		return NULL;

	pair<pysparse_hash_map::iterator, bool> result = self->map.insert(pysparse_hash_map::value_type(key, value));
	if (PyErr_Occurred()) {
		if (result.second) {
			++self->change_id;
			self->map.erase(result.first); // undo insert
		}
		return NULL;
	}
	if (result.second) {
		++self->change_id;
		Py_INCREF(key);
		Py_INCREF(value);
	}
	else {
		value = result.first->second;
	}
	Py_INCREF(value);
	return value;
}

PyObject *SparseDict::popitem(SparseDict *self, PyObject *)
{
	// NB: any allocation can trigger GC (tp_clear) and invalidate iterators
	PyObject *pair = PyTuple_New(2);
	if (pair == NULL)
		return NULL;

	if (self->map.empty()) {
		Py_DECREF(pair);
		PyErr_SetString(PyExc_KeyError, "popitem(): SparseDict is empty");
		return NULL;
	}

	// TODO: map.begin() is O(N) when iterating destructively (it must skip deleted elements)
	// to make popitem() constant time we need to store "next element to delete" iterator

	pysparse_hash_map::iterator it = self->map.begin();
	PyTuple_SET_ITEM(pair, 0, it->first);
	PyTuple_SET_ITEM(pair, 1, it->second);
	self->map.erase(it);
	return pair;
}

PyObject *SparseDict::contains(SparseDict *self, PyObject *arg)
{
	return PyBool_FromLong(self->map.find(arg) != self->map.end());
}

PyObject *SparseDict::getitem(SparseDict *self, PyObject *arg)
{
	pysparse_hash_map::const_iterator it = self->map.find(arg);
	if (PyErr_Occurred())
		return NULL;
	if (it != self->map.end()) {
		Py_INCREF(it->second);
		return it->second;
	}
	else {
		_key_error(arg);
		return NULL;
	}
}

template<MetaElement ME>
PyObject *SparseDict::list_any(SparseDict *self, PyObject *)
{
	PyObject *list;
	for (;;) {
		size_t size = self->map.size();
		list = PyList_New((Py_ssize_t) size);
		if (list == NULL)
			return NULL;
		if (size == self->map.size())
			break;
		Py_DECREF(list);
	}

	Py_ssize_t idx = 0;
	for (pysparse_hash_map::const_iterator it = self->map.begin(); it != self->map.end(); ++it, ++idx) {
		PyObject *obj = (ME == ME_KEYS) ? it->first : it->second;
		Py_INCREF(obj);
		PyList_SET_ITEM(list, idx, obj);
	}
	assert(idx == self->map.size());
	return list;
}

PyObject *SparseDict::items(SparseDict *self, PyObject *)
{
	PyObject *list;
	for (;;) {
		Py_ssize_t size = (Py_ssize_t) self->map.size();
		list = PyList_New(size);
		if (list == NULL)
			return NULL;
		for (Py_ssize_t i = 0; i < size; ++i) {
			PyObject *pair = PyTuple_New(2);
			if (pair == NULL) {
				Py_DECREF(list);
				return NULL;
			}
			PyList_SET_ITEM(list, i, pair);
		}
		if (size == (Py_ssize_t) self->map.size())
			break;
		Py_DECREF(list);
	}

	Py_ssize_t idx = 0;
	for (pysparse_hash_map::const_iterator it = self->map.begin(); it != self->map.end(); ++it, ++idx) {
		PyObject *key = it->first;
		PyObject *value = it->second;
		PyObject *pair = PyList_GET_ITEM(list, idx);
		
		Py_INCREF(key);
		PyTuple_SET_ITEM(pair, 0, key);
		Py_INCREF(value);
		PyTuple_SET_ITEM(pair, 1, value);
	}
	return list;
}

template <PyTypeObject *ITER_TYPE>
PyObject *SparseDict::iter_any(SparseDict *self, PyObject *)
{
	return SparseDictIter::create_new(self, ITER_TYPE);
}

PyObject *SparseDict::resize(SparseDict *self, PyObject *arg)
{
	Py_ssize_t size = PyInt_AsSsize_t(arg);
	if (size == -1) {
		PyErr_SetString(PyExc_ValueError, "Argument to resize() must be an integer.");
		return NULL;
	}
	if (_resize(self, (size_t) size) < 0)
		return NULL;
	Py_RETURN_NONE;
}

PyObject *SparseDict::get_load_factor(SparseDict *self, size_t closure)
{
	float result;
	if (closure == LF_MIN)
		result = self->map.min_load_factor();
	else if (closure == LF_MAX)
		result = self->map.max_load_factor();
	else
		result = self->map.load_factor();
	return PyFloat_FromDouble(result);
}

int SparseDict::set_load_factor(SparseDict *self, PyObject *arg, size_t closure)
{
	float value = (float) PyFloat_AsDouble(arg);
	if (PyErr_Occurred())
		return -1;
	if (closure == LF_MIN)
		self->map.min_load_factor(value);
	else
		self->map.max_load_factor(value);
	return 0;
}

PyObject *SparseDict::tp_new(PyTypeObject *type, PyObject *, PyObject *)
{
	// NB: tp_new is also called from copy()
	SparseDict *self = static_cast<SparseDict *>(type->tp_alloc(type, 0));
	if (self != NULL) {
		new (&self->map) pysparse_hash_map;
		self->map.set_deleted_key(NULL);
		// XXX PyObject_GC_Track(self);
	}
	return self;
}

void SparseDict::tp_dealloc(SparseDict *self) {
	// XXX PyObject_GC_UnTrack(self);

	for (pysparse_hash_map::const_iterator it = self->map.begin(); it != self->map.end(); ++it) {
		Py_DECREF(it->first);
		Py_DECREF(it->second);
	}

	self->map.~pysparse_hash_map();
	Py_TYPE(self)->tp_free(self);
}

int SparseDict::tp_init(SparseDict *self, PyObject *args, PyObject *kwds)
{
	// Note: similar to dict, we delay tracking until the first non-scalar item is inserted
	return _update(self, args, kwds);
}

PyObject *SparseDict::tp_repr(SparseDict *self)
{
	Py_ssize_t i = Py_ReprEnter(self);
	if (i != 0)
		return i > 0 ? PyString_FromString("SparseDict(...)") : NULL;

	if (self->map.empty()) {
		Py_ReprLeave(self);
		return PyString_FromString("SparseDict()");
	}

	Py_ReprLeave(self);
	return PyString_FromString("SparseDict(TODO)"); 
}

PyObject *SparseDict::tp_iter(SparseDict *self)
{
	return SparseDictIter::create_new(self, &SparseDictIter::KeyType);
}

int SparseDict::tp_compare(SparseDict *o1, SparseDict *o2)
{
	// equality is defined by richcompare
	// order is by size, then by address
	if (o1->map.size() < o2->map.size())
		return -1;
	else if (o1->map.size() > o2->map.size())
		return 1;
	if (_equal(o1, o2))
		return 0;
	if ((size_t) o1 < (size_t) o2)
		return -1;
	else
		return 1;
}

PyObject *SparseDict::tp_richcompare(PyObject *o1, PyObject *o2, int op)
{
	if (op != Py_EQ && op != Py_NE) {
		PyErr_SetString(PyExc_TypeError, "SparseDict instances only support equality and inequality");
		return NULL;
	}

	int eq_cmp = -1;
	if (PyObject_TypeCheck(o1, &Type))
		eq_cmp = _equal(static_cast<SparseDict *>(o1), o2);
	else if (PyObject_TypeCheck(o2, &Type))
		eq_cmp = _equal(static_cast<SparseDict *>(o2), o1);

	PyObject *result;
	if (eq_cmp != -1)
		result = (eq_cmp ^ (op == Py_NE)) ? Py_True : Py_False;
	else
		result = Py_NotImplemented;
	Py_INCREF(result);
	return result;
}

int SparseDict::tp_traverse(SparseDict *self, visitproc visit, void *arg)
{
	/*for (pysparse_hash_map::const_iterator it = self->map.begin(); it != self->map.end(); ++it) {
		Py_VISIT(it->first);
		Py_VISIT(it->second);
	}*/
	return 0;
}

int SparseDict::tp_clear(SparseDict *self)
{
	_clear(self);
	return 0;
}

PyObject *SparseDict::mp_subscript(SparseDict *self, PyObject *key)
{
	return getitem(self, key);
}

int SparseDict::mp_ass_subscript(SparseDict *self, PyObject *key, PyObject *value)
{
	if (value != NULL)
		return _insert(self, key, value);
	else
		return _erase(self, key);
}

Py_ssize_t SparseDict::mp_length(SparseDict *self)
{
	return (Py_ssize_t) self->map.size();
}

int SparseDict::sq_contains(SparseDict *self, PyObject *key)
{
	return self->map.find(key) != self->map.end();
}


// SparseDictIter

SparseDictIter *SparseDictIter::create_new(SparseDict *sdict, PyTypeObject *subtype)
{
	SparseDictIter *self = PyObject_GC_New(SparseDictIter, subtype);
	if (self == NULL)
		return NULL;
	Py_INCREF(sdict);
	self->sdict = sdict;
	self->change_id = sdict->change_id;
	self->size = (Py_ssize_t) sdict->map.size();
	self->remaining_size = (Py_ssize_t) self->size;
	new (&self->iter) pysparse_hash_map::const_iterator(sdict->map.begin());
	self->pair = NULL;
	return self;
}

void SparseDictIter::tp_dealloc(SparseDictIter *self)
{
	self->iter.~const_iterator();
	Py_XDECREF(self->sdict);
	PyObject_GC_Del(self);
}

bool SparseDictIter::_check_valid(SparseDictIter *self) {
	if (self->change_id != self->sdict->change_id // invalidating insert
			|| self->size < (Py_ssize_t) self->sdict->map.size()  // non-invalidating insert
			|| self->iter->first == NULL) // delete of the element we point to
	{
		// this iterator is is invalid
		self->size = -1;
		return false;
	}
	return true;
}

template <MetaElement ME>
PyObject *SparseDictIter::tp_iternext(SparseDictIter *self) 
{
	if (self->sdict == NULL)
		return NULL;

	if (!_check_valid(self)) {
		PyErr_SetString(PyExc_RuntimeError, "iterator is invalid");
		return NULL;
	}
	if (self->iter == self->sdict->map.end()) {
		Py_DECREF(self->sdict);
		self->sdict = NULL;
		return NULL;
	}

	pysparse_hash_map::value_type pair = *self->iter;
	++self->iter;
	--self->remaining_size;

	PyObject *result;
	if (ME == ME_KEYS) {
		result = pair.first;
		Py_INCREF(result);
	}
	else if (ME == ME_VALUES) {
		result = pair.second;
		Py_INCREF(result);
	}
	else {
		result = PyTuple_Pack(2, pair.first, pair.second);
	}
	return result;
}

PyObject *SparseDictIter::length_hint(SparseDictIter *self, PyObject *)
{
	Py_ssize_t result;
	if (self->sdict != NULL && _check_valid(self))
		result = self->remaining_size;
	else
		result = 0;
	return PyLong_FromSsize_t(result);
}

int SparseDictIter::tp_traverse(SparseDictIter *self, visitproc visit, void *arg)
{
	Py_VISIT(self->sdict);
	return 0;
}

static PyMethodDef SparseDict_methods[] = {
	// common dict methods
    { "__sizeof__", (PyCFunction) SparseDict::sizeof_, METH_NOARGS }, // sys.getsizeof support
	{ "__contains__", (PyCFunction) SparseDict::contains, METH_O | METH_COEXIST }, // optimized shortcut
    { "has_key", (PyCFunction) SparseDict::contains, METH_O },
	{ "__getitem__", (PyCFunction) SparseDict::getitem, METH_O | METH_COEXIST }, // optimized shortcut
    { "get", (PyCFunction) SparseDict::get, METH_VARARGS },
    { "pop", (PyCFunction) SparseDict::pop, METH_VARARGS },
	{ "popitem", (PyCFunction) SparseDict::popitem, METH_NOARGS },
	{ "setdefault", (PyCFunction) SparseDict::setdefault, METH_VARARGS },
    { "fromkeys", (PyCFunction) SparseDict::fromkeys, METH_VARARGS | METH_CLASS },
	{ "update", (PyCFunction) SparseDict::update, METH_VARARGS | METH_KEYWORDS },
    { "copy", (PyCFunction) SparseDict::copy, METH_NOARGS },
    { "clear", (PyCFunction) SparseDict::clear, METH_NOARGS },
    { "keys", (PyCFunction) SparseDict::list_any<ME_KEYS>, METH_NOARGS },
    { "values", (PyCFunction) SparseDict::list_any<ME_VALUES>, METH_NOARGS },
	{ "items", (PyCFunction) SparseDict::items, METH_NOARGS },
	{ "iterkeys", (PyCFunction) SparseDict::iter_any<&SparseDictIter::KeyType>, METH_NOARGS },
	{ "itervalues", (PyCFunction) SparseDict::iter_any<&SparseDictIter::ValueType>, METH_NOARGS },
	{ "iteritems", (PyCFunction) SparseDict::iter_any<&SparseDictIter::ItemType>, METH_NOARGS },

	// SparseDict specific methods
	{ "resize", (PyCFunction) &SparseDict::resize, METH_O },
    { NULL }
};

static PyGetSetDef SparseDict_getset[] ={
	{ "load_factor", (getter) SparseDict::get_load_factor, NULL, NULL, (void *) LF_CURRENT },
	{ "min_load_factor", (getter) SparseDict::get_load_factor, (setter) SparseDict::set_load_factor, NULL, (void *) LF_MIN },
    { "max_load_factor", (getter) SparseDict::get_load_factor, (setter) SparseDict::set_load_factor, NULL, (void *) LF_MAX },
    { NULL },
};

static PyMappingMethods SparseDict_mapping = {
    (lenfunc)(SparseDict::mp_length), /*mp_length*/
    (binaryfunc)(SparseDict::mp_subscript), /*mp_subscript*/
	(objobjargproc)(SparseDict::mp_ass_subscript), /*mp_ass_subscript*/
};

static PySequenceMethods SparseDict_sequence = {
    0, /* sq_length */
    0, /* sq_concat */
    0, /* sq_repeat */
    0, /* sq_item */
    0, /* sq_slice */
    0, /* sq_ass_item */
    0, /* sq_ass_slice */
	(objobjproc) &SparseDict::sq_contains, /* sq_contains */
    0, /* sq_inplace_concat */
    0, /* sq_inplace_repeat */
};

PyTypeObject SparseDict::Type = {
    PyObject_HEAD_INIT(NULL)
    0, /*ob_size*/
    "sparsecoll.SparseDict", /*tp_name*/
    sizeof(SparseDict), /*tp_basicsize*/
    0, /*tp_itemsize*/
	(destructor) &SparseDict::tp_dealloc, /*tp_dealloc*/
    0, /*tp_print*/
    0, /*tp_getattr*/
    0, /*tp_setattr*/
	(cmpfunc) &SparseDict::tp_compare, /*tp_compare*/
    (reprfunc) &SparseDict::tp_repr, /*tp_repr*/
    0, /*tp_as_number*/
    &SparseDict_sequence, /*tp_as_sequence*/
    &SparseDict_mapping, /*tp_as_mapping*/
    PyObject_HashNotImplemented, /*tp_hash */
    0, /*tp_call*/
    0, /*tp_str*/
    PyObject_GenericGetAttr, /*tp_getattro*/
    0, /*tp_setattro*/
    0, /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    "SparseDict object", /* tp_doc */
	(traverseproc) &SparseDict::tp_traverse, /*tp_traverse*/
	(inquiry) &SparseDict::tp_clear, /*tp_clear*/
	&SparseDict::tp_richcompare, /*tp_richcompare*/
	0, /*tp_weaklistoffset*/
	(getiterfunc) &SparseDict::tp_iter, /*tp_iter*/
	0, /*tp_iternext*/
	SparseDict_methods, /*tp_methods*/
	0, /*tp_members*/
	SparseDict_getset, /*tp_getset*/
	0, /*tp_base*/
	0, /*tp_dict*/
	0, /*tp_descr_get*/
	0, /*tp_descr_set*/
	0, /*tp_dictoffset*/
	(initproc) &SparseDict::tp_init, /*tp_init*/
	PyType_GenericAlloc, /*tp_alloc*/
	&SparseDict::tp_new, /*tp_new*/
	PyObject_GC_Del, /*tp_free*/
};

static PyMethodDef SparseDictIter_methods[] = {
	{ "__length_hint__", (PyCFunction) SparseDictIter::length_hint, METH_NOARGS }, // undocumented
    { NULL },
};

PyTypeObject SparseDictIter::KeyType = {
    PyObject_HEAD_INIT(NULL)
    0, /*ob_size*/
    "sparsecoll.SparseDict._KeyIter", /*tp_name*/
    sizeof(SparseDictIter), /*tp_basicsize*/
    0, /*tp_itemsize*/
	(destructor) &SparseDictIter::tp_dealloc, /*tp_dealloc*/
    0, /*tp_print*/
    0, /*tp_getattr*/
    0, /*tp_setattr*/
    0, /*tp_compare*/
    0, /*tp_repr*/
    0, /*tp_as_number*/
    0, /*tp_as_sequence*/
    0, /*tp_as_mapping*/
    0, /*tp_hash */
    0, /*tp_call*/
    0, /*tp_str*/
    PyObject_GenericGetAttr, /*tp_getattro*/
    0, /*tp_setattro*/
    0, /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    0, /* tp_doc */
	(traverseproc) &SparseDictIter::tp_traverse, /*tp_traverse*/
	0, /*tp_clear*/
	0, /*tp_richcompare*/
	0, /*tp_weaklistoffset*/
	PyObject_SelfIter, /*tp_iter*/
	(iternextfunc) &SparseDictIter::tp_iternext<ME_KEYS>, /*tp_iternext*/
	SparseDictIter_methods, /*tp_methods*/
};

PyTypeObject SparseDictIter::ValueType = {
    PyObject_HEAD_INIT(NULL)
    0, /*ob_size*/
    "sparsecoll.SparseDict._ValueIter", /*tp_name*/
    sizeof(SparseDictIter), /*tp_basicsize*/
    0, /*tp_itemsize*/
	(destructor) &SparseDictIter::tp_dealloc, /*tp_dealloc*/
    0, /*tp_print*/
    0, /*tp_getattr*/
    0, /*tp_setattr*/
    0, /*tp_compare*/
    0, /*tp_repr*/
    0, /*tp_as_number*/
    0, /*tp_as_sequence*/
    0, /*tp_as_mapping*/
    0, /*tp_hash */
    0, /*tp_call*/
    0, /*tp_str*/
    PyObject_GenericGetAttr, /*tp_getattro*/
    0, /*tp_setattro*/
    0, /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    0, /* tp_doc */
	(traverseproc) &SparseDictIter::tp_traverse, /*tp_traverse*/
	0, /*tp_clear*/
	0, /*tp_richcompare*/
	0, /*tp_weaklistoffset*/
	PyObject_SelfIter, /*tp_iter*/
	(iternextfunc) &SparseDictIter::tp_iternext<ME_VALUES>, /*tp_iternext*/
	SparseDictIter_methods, /*tp_methods*/
};

PyTypeObject SparseDictIter::ItemType = {
    PyObject_HEAD_INIT(NULL)
    0, /*ob_size*/
    "sparsecoll.SparseDict._ItemIter", /*tp_name*/
    sizeof(SparseDictIter), /*tp_basicsize*/
    0, /*tp_itemsize*/
	(destructor) &SparseDictIter::tp_dealloc, /*tp_dealloc*/
    0, /*tp_print*/
    0, /*tp_getattr*/
    0, /*tp_setattr*/
    0, /*tp_compare*/
    0, /*tp_repr*/
    0, /*tp_as_number*/
    0, /*tp_as_sequence*/
    0, /*tp_as_mapping*/
    0, /*tp_hash */
    0, /*tp_call*/
    0, /*tp_str*/
    PyObject_GenericGetAttr, /*tp_getattro*/
    0, /*tp_setattro*/
    0, /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    0, /* tp_doc */
	(traverseproc) &SparseDictIter::tp_traverse, /*tp_traverse*/
	0, /*tp_clear*/
	0, /*tp_richcompare*/
	0, /*tp_weaklistoffset*/
	PyObject_SelfIter, /*tp_iter*/
	(iternextfunc) &SparseDictIter::tp_iternext<ME_ITEMS>, /*tp_iternext*/
	SparseDictIter_methods, /*tp_methods*/
};
