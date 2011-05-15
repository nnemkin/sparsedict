/* Hash table (dictionary) with low memory overhead.

   Uses the same idea as the google-sparsehash (http://code.google.com/p/google-sparsehash/).
   It's also very similar (and borrows portions of code from) python's native dict.

   TODO:
	* try dict() collision resolution scheme
    * cleanup error messages
*/

#include "Python.h"
//#pragma warning(4 : 4710 4711 4714)

/* Python versions compatibility */
#if PY_VERSION_HEX < 0x02060000
#define Py_TYPE(ob) (((PyObject*)(ob))->ob_type)
#define PyVarObject_HEAD_INIT(type, size) PyObject_HEAD_INIT(type) size,
#define PyBytesObject                PyStringObject
#define PyBytes_AS_STRING            PyString_AS_STRING
#define PyBytes_GET_SIZE             PyString_GET_SIZE
#define PyBytes_CheckExact           PyString_CheckExact
#define PyObject_HashNotImplemented  0
#endif
#if PY_VERSION_HEX < 0x02070000
/* XXX: this is totally private API */
#define _PyObject_GC_IS_TRACKED(o) \
    ((_Py_AS_GC(o))->gc.gc_refs != _PyGC_REFS_UNTRACKED)
#define _PyObject_GC_MAY_BE_TRACKED(obj) \
    (PyObject_IS_GC(obj) && (!PyTuple_CheckExact(obj) || _PyObject_GC_IS_TRACKED(obj)))
#endif
#if PY_VERSION_HEX < 0x03020000
typedef long Py_hash_t;
#define PyArg_ValidateKeywordArguments(kwds) 1
#endif 
#if PY_MAJOR_VERSION < 3
#define PyDict_GetItemWithError PyDict_GetItem
#else
#define PyInt_FromSsize_t            PyLong_FromSsize_t
#define PyInt_AsSsize_t              PyLong_AsSsize_t
#define PyString_FromString          PyUnicode_FromString
#define PyString_FromFormat          PyUnicode_FromFormat
#define PyString_Concat              PyUnicode_Append
#define _PyString_Join               PyUnicode_Join
#endif

/* Behavioral con */

#define SPARSEBLOCK_SIZE 48
#define INITIAL_ITEMS 32 /* Largest power of 2 that fits in one sparseblock. */

/* Single dictionary entry. */
typedef struct {
    PyObject *key; /* NULL key signifies deleted entry */
    PyObject *value;
} dictentry;

/* Sparse chunk of hash space holding SPARSEBLOCK_SIZE items.
   Each item is allocated on demand, item allocation status is marked in the bitmap. */
typedef struct {
    dictentry *items;
    unsigned short num_items;
    unsigned char bitmap[(SPARSEBLOCK_SIZE + 7) / 8];
} sparseblock;

typedef struct _sparsedictobject SparseDictObject;
struct _sparsedictobject {
    PyObject_HEAD

    dictentry *(*lookup)(SparseDictObject *self, PyObject *key, Py_hash_t hash, int insert);

    Py_ssize_t num_blocks;  /* Chunks of hash space by SPARSEBLOCK_SIZE items. */
    Py_ssize_t num_items;   /* Total number of alocated items in all blocks. */
    Py_ssize_t num_deleted; /* Number of deleted items (allocated, but have NULL key). */
    Py_ssize_t max_items;   /* Max items possible without resizing the blocks array. */
    Py_ssize_t next_index;  /* Index in hash space to resume search for nondeleted items. Used by popitem. */
    sparseblock *blocks;
    sparseblock static_blocks[1]; /* Spare block to avoid allocations for "empty" state. */
};

/* sparseblock methods */

#define BIT_TEST(bitmap, i)  (bitmap[i / 8] &   (1 << (i % 8)))
#define BIT_SET(bitmap, i)   (bitmap[i / 8] |=  (1 << (i % 8)))
#define BIT_RESET(bitmap, i) (bitmap[i / 8] &= ~(1 << (i % 8)))

/* 1-bit count (aka population count) in a byte. */
static const unsigned char popcnt8[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

/* Check that sparseblock is healthy. */
#define SPARSEBLOCK_INVARIANT(block, index) \
    do { \
        Py_ssize_t num_items = 0; \
        assert(index >= 0 && index < SPARSEBLOCK_SIZE); \
        assert((block) != NULL); \
        assert((block)->num_items == 0 || (block)->items != NULL); \
        assert((block)->num_items >= 0 && (block)->num_items <= SPARSEBLOCK_SIZE); \
        for (i = 0; i < SPARSEBLOCK_SIZE / 8; ++i) \
            num_items += popcnt8[(block)->bitmap[i]]; \
        assert(num_items == (block)->num_items); \
    } while (0)

/* Find the item at index. If the item is not allocated, return NULL. */
Py_LOCAL_INLINE(dictentry *)
sparseblock_find(sparseblock *block, Py_ssize_t index)
{
    int i, offset = 0;

    SPARSEBLOCK_INVARIANT(block, index);

    if (!BIT_TEST(block->bitmap, index))
        return NULL;

    for (i = 0; index > 8; ++i, index -= 8)
        offset += popcnt8[block->bitmap[i]];
    offset += popcnt8[block->bitmap[i] & ((1 << index)-1)];

    return &block->items[offset];
}

/* Allocate new item at the previously unallocated index. Returns NULL on failure. */
Py_LOCAL_INLINE(dictentry *)
sparseblock_insert(sparseblock *block, Py_ssize_t index)
{
    int i, num_items, offset = 0;
    dictentry *items;

    SPARSEBLOCK_INVARIANT(block, index);
    assert(!BIT_TEST(block->bitmap, index)); /* not allocated yet? */
    assert(block->num_items < SPARSEBLOCK_SIZE); /* enough space for another element? */

    items = block->items;
    num_items = block->num_items + 1;
    /* Realloc only every other insert */
    if (num_items & 1) {
        items = PyMem_RESIZE(items, dictentry, num_items + 1);
        if (items == NULL) {
            PyErr_NoMemory();
            return NULL;
        }
        block->items = items;
    }
    block->num_items = (unsigned short) num_items;
    BIT_SET(block->bitmap, index);

    /* Count allocated nodes (bitmap bits) upto index. */
    for (i = 0; index > 8; ++i, index -= 8)
        offset += popcnt8[block->bitmap[i]];
    offset += popcnt8[block->bitmap[i] & ((1 << index)-1)];

    /* Shift to make place for new item. */
    for (i = num_items - 1; i > offset; --i)
        items[i] = items[i-1];
   
    return &items[offset];
}

/* SparseDict macros */

PyTypeObject SparseDict_Type;
PyTypeObject SparseDictIterKey_Type;
PyTypeObject SparseDictIterValue_Type;
PyTypeObject SparseDictIterItem_Type;
PyTypeObject SparseDictKeys_Type;
PyTypeObject SparseDictValues_Type;
PyTypeObject SparseDictItems_Type;

#define SparseDict_Check(op) PyObject_TypeCheck(op, &SparseDict_Type)
#define SparseDict_CheckExact(op) (Py_TYPE(op) == &SparseDict_Type)
#define SparseDictViewSet_Check(op) \
    (Py_TYPE(op) == &SparseDictKeys_Type || Py_TYPE(op) == &SparseDictItems_Type)

#define SparseDict_INIT_NONZERO(sdict) \
    do { \
        (sdict)->num_blocks = 1; \
        (sdict)->max_items = INITIAL_ITEMS; \
        (sdict)->blocks = (sdict)->static_blocks; \
    } while (0)

#define SparseDict_INIT(sdict) \
    do { \
        (sdict)->num_items = 0; \
        (sdict)->num_deleted = 0; \
        (sdict)->next_index = 0; \
        memset((sdict)->static_blocks, 0, sizeof(sparseblock)); \
        SparseDict_INIT_NONZERO(sdict); \
    } while (0)

/* A bunch of asserts for basic SparseDictObject invariants. */
#define SparseDict_INVARIANT(sdict) \
    do { \
        assert((sdict) != NULL); \
        assert(SparseDict_Check(sdict)); \
        assert((sdict)->lookup != NULL); \
        assert((sdict)->blocks != NULL); \
        assert((sdict)->max_items >= INITIAL_ITEMS); \
        assert(((sdict)->max_items & ((sdict)->max_items - 1)) == 0); /* power of 2 */ \
        assert((sdict)->num_items <= (sdict)->max_items); \
        assert((sdict)->num_deleted <= (sdict)->num_items); \
    } while (0)

#define SparseDict_SIZE(sdict) ((sdict)->num_items - (sdict)->num_deleted)

#define SparseDict_FOR(sdict, entry) \
    { \
        Py_ssize_t i__; \
        int j__, num_items__; \
        dictentry *items__, entry; \
        for (i__ = 0; i__ < (sdict)->num_blocks; ++i__) { \
            items__ = (sdict)->blocks[i__].items; \
            num_items__ = (sdict)->blocks[i__].num_items; \
            for (j__ = 0; j__ < num_items__; ++j__) { \
                entry = items__[j__]; \
                if (entry.key != NULL) {

#define SparseDict_ENDFOR(sdict, destructive) \
                } \
            } \
            if (destructive) PyMem_FREE(items__); \
        } \
        if (destructive && ((sdict)->blocks != (sdict)->static_blocks)) \
            PyMem_FREE((sdict)->blocks); \
    }

/* Forward */
static PyObject *dictiter_new(SparseDictObject *dict, PyTypeObject *type);
static PyObject *dictview_new(SparseDictObject *dict, PyTypeObject *type);

/* Dummy "deleted" entry used by dict_lookup to distinguish "not found" from error (NULL). */
static dictentry entry_not_found = {NULL, NULL};

/* SparseDict method helpers */

Py_LOCAL(int) dict_resize(SparseDictObject *self, Py_ssize_t new_max_items);
Py_LOCAL(int) dict_resize_delta(SparseDictObject *self, Py_ssize_t delta);

/* Bit scrambling borrowed from msvcrt's xhash header.
   XXX: 64 bit version. */
Py_LOCAL_INLINE(size_t)
hash_mix(size_t hash)
{
	long quot = (long)((hash ^ 0xdeadbeef) & LONG_MAX);
	ldiv_t qrem = ldiv(quot, 127773);

	qrem.rem = 16807 * qrem.rem - 2836 * qrem.quot;
	if (qrem.rem < 0)
		qrem.rem += LONG_MAX;
	return (size_t)qrem.rem;
}

/* Same as _PyString_Equal but using the public API. */
Py_LOCAL_INLINE(int)
string_equal(PyObject *arg1, PyObject *arg2)
{
    PyBytesObject *s1 = (PyBytesObject *)arg1;
    PyBytesObject *s2 = (PyBytesObject *)arg2;

    assert(PyBytes_CheckExact(arg1));
    assert(PyBytes_CheckExact(arg2));

    if (PyBytes_GET_SIZE(s1) != PyBytes_GET_SIZE(s2))
        return 0;
    if (PyBytes_AS_STRING(s1)[0] != PyBytes_AS_STRING(s2)[0])
        return 0;
    return memcmp(PyBytes_AS_STRING(s1),
                  PyBytes_AS_STRING(s2),
                  PyBytes_GET_SIZE(s1)) == 0;
}

/* Set a key error with the specified argument, wrapping it in a
 * tuple automatically so that tuple keys are not unpacked as the
 * exception arguments. */
Py_LOCAL_INLINE(void)
set_key_error(PyObject *arg)
{
    PyObject *tuple;
    tuple = PyTuple_Pack(1, arg);
    if (!tuple)
        return; /* caller will expect error to be set anyway */
    PyErr_SetObject(PyExc_KeyError, tuple);
    Py_DECREF(tuple);
}

/* Next nondeleted item search. Used in popitem() and iterators. */
Py_LOCAL_INLINE(dictentry *)
dict_next(SparseDictObject *self, Py_ssize_t *index, int wrap)
{
    dictentry *entry = NULL;
    int j = (int)*index & 0x3f;
    Py_ssize_t i = *index >> 6;
    do {
        for (; i < self->num_blocks; ++i) {
            int num_items = self->blocks[i].num_items;
            dictentry *items = self->blocks[i].items;

            for (; j < num_items; ++j) {
                entry = &items[j];
                if (entry->key != NULL)
                    goto Found;
            }
            j = 0;
        }
        i = 0;
    } while (wrap);
Found:
    *index = (i << 6) | (j+1);
    return entry;
}

/* Search for an entry with the specified key.
   If the key is not found and insert = 1, new entry is inserted and returned,
   otherwise a dummy deleted entry is returned. if hash parameter is -1, it's recalculated
   from the key. NULL return return means error. */
static dictentry *
dict_lookup(SparseDictObject *self, PyObject *key, Py_hash_t hash, int insert)
{
    /* NULL key will make it look like deleted entry. */
    size_t i, num_probes = 0;
    size_t max_items_mask = (size_t)self->max_items - 1;
    dictentry *entry, *freeslot = NULL;
    sparseblock *blocks = self->blocks;
    int cmp;
    PyObject *old_key;

    if (hash == -1) {
        if (PyBytes_CheckExact(key)) {
            hash = ((PyBytesObject *)key)->ob_shash;
            if (hash == -1)
                hash = PyObject_Hash(key);
        }
        else {
            hash = PyObject_Hash(key);
            if (hash == -1)
                return NULL;
        }
    }
    i = hash_mix((size_t)hash) & max_items_mask;
    for (;;) {
        entry = sparseblock_find(&blocks[i / SPARSEBLOCK_SIZE], i % SPARSEBLOCK_SIZE);
        if (entry == NULL) {
            if (freeslot != NULL)
                return freeslot;
            if (!insert)
                return &entry_not_found;

            entry = sparseblock_insert(&blocks[i / SPARSEBLOCK_SIZE], i % SPARSEBLOCK_SIZE);
            if (entry != NULL)
                /* Mark as deleted to distinguish newly inserved from existing. */
                entry->key = NULL;
            return entry;
        }
        if (entry->key == key)
            return entry;
        if (entry->key != NULL) {
            old_key = entry->key;
            Py_INCREF(old_key);
            cmp = PyObject_RichCompareBool(old_key, key, Py_EQ);
            Py_DECREF(old_key);
            if (cmp < 0)
                return NULL;
            if (self->blocks == blocks && entry->key == old_key) {
                if (cmp > 0)
                    return entry;
            }
            else {
                /* richcmp has changed the dict, restart */
                return dict_lookup(self, key, hash, insert);
            }
        }
        else if (freeslot == NULL) 
            /* entry->key == NULL, deleted entry */
            freeslot = entry;

        /* Quadratic probing */
        ++num_probes;
        i = (i + num_probes) & max_items_mask;
    }
    assert(0); /* NOT REACHED */
}

/* Special-case dict_lookup that assumes all keys in the dictionary are PyStringObjects. */
static dictentry *
dict_lookup_string(SparseDictObject *self, PyObject *key, Py_hash_t  hash, int insert)
{
    dictentry *entry, *freeslot = NULL;
    sparseblock *blocks = self->blocks;
    size_t i, num_probes = 0;
    size_t max_items_mask = (size_t)self->max_items - 1;

    if (!PyBytes_CheckExact(key)) {
        /* First non-string key, revert to universal lookup. */
        self->lookup = dict_lookup;
        return dict_lookup(self, key, hash, insert);
    }
    if (hash == -1) {
        hash = ((PyBytesObject *)key)->ob_shash;
        if (hash == -1)
            hash = PyObject_Hash(key);
    }

    i = hash_mix((size_t)hash) & max_items_mask;
    for (;;) {
        entry = sparseblock_find(&blocks[i / SPARSEBLOCK_SIZE], i % SPARSEBLOCK_SIZE);
        if (entry == NULL) {
            if (freeslot != NULL)
                return freeslot;
            if (!insert)
                return &entry_not_found;

            entry = sparseblock_insert(&blocks[i / SPARSEBLOCK_SIZE], i % SPARSEBLOCK_SIZE);
            if (entry != NULL)
                entry->key = NULL;
            return entry;
        }
        else if (entry->key == key || (entry->key != NULL && string_equal(entry->key, key)))
            return entry;
        else if (entry->key == NULL && freeslot == NULL)
            /* Deleted entry */
            freeslot = entry;

        /* Quadratic probing */
        ++num_probes;
        i = (i + num_probes) & max_items_mask;
    }
    assert(0); /* NOT REACHED */
}

/* Insert an item into the dictionary. Same semantics as PyDict_SetItem. */
Py_LOCAL(int)
dict_insert(SparseDictObject *self, PyObject *key, PyObject *value)
{
    PyObject *old_value;
    dictentry *entry;

    if (dict_resize_delta(self, 1) != 0)
        return -1;

    Py_INCREF(value);
    Py_INCREF(key);
    entry = (self->lookup)(self, key, -1, 1);
    if (entry == NULL) {
        Py_DECREF(key);
        Py_DECREF(value);
        return -1;
    }

    /* Delay GC tracking until the first trackable item is inserted. */
    if (!_PyObject_GC_IS_TRACKED(self))
        if (_PyObject_GC_MAY_BE_TRACKED(key) || _PyObject_GC_MAY_BE_TRACKED(value))
            PyObject_GC_Track(self);

    if (entry->key != NULL) {
        old_value = entry->value;
        entry->value = value;
        Py_DECREF(old_value); /* which **CAN** re-enter */
        Py_DECREF(key);
    }
    else {
        entry->key = key;
        entry->value = value;
        ++self->num_items;
    }
    return 0;
}

/* Delete an item from the dictionary. Same semantics as PyDict_DelItem. */
Py_LOCAL(int)
dict_delete(SparseDictObject *self, PyObject *key)
{
    dictentry *entry;
    PyObject *old_key, *old_value;

    entry = (self->lookup)(self, key, -1, 0);
    if (entry == NULL)
        return -1;
    if (entry->key == NULL) {
        set_key_error(key);
        return -1;
    }
    old_key = entry->key;
    old_value = entry->value;
    entry->key = NULL;
    ++self->num_deleted;
    Py_DECREF(old_value);
    Py_DECREF(old_key);
    return 0;
}

/* This is caled to preallocate space for at least delta elements. */
Py_LOCAL(int)
dict_resize_delta(SparseDictObject *self, Py_ssize_t delta) {
    /* Growth factor us 3/4 = 0.75, shrink factor is 5/16 = 0.3125 */

    Py_ssize_t new_max_items = self->max_items;

    if (delta <= 0)
        return 0;

    if (/*SparseDict_SIZE(self) > new_max_items * 5 / 16 &&*/
        self->num_items + delta <= new_max_items * 3 / 4)
        return 0;

    /* Find the size which fits nondeleted items below enlarge threshold. */
    new_max_items = INITIAL_ITEMS;
    while (SparseDict_SIZE(self) + delta > new_max_items * 3 / 4)
        new_max_items *= 2;
    if (new_max_items < self->max_items) {
        /* We're actually shrinking due to lots of deleted elements. Try to re-grow. */
        if (SparseDict_SIZE(self) + delta >= new_max_items * 2 * 5 / 16)
            /* Doubling the size won't hit shrink limit. */
            new_max_items *= 2;
    }

    return dict_resize(self, new_max_items);
}

/* Resize the hashtable by allocating a new sparseblock array and reinserting
   all non-deleted items. Returns 0 on success, -1 on error. */
Py_LOCAL(int)
dict_resize(SparseDictObject *self, Py_ssize_t new_max_items)
{
    Py_ssize_t max_items_mask = new_max_items - 1;
    Py_ssize_t num_blocks = (new_max_items + SPARSEBLOCK_SIZE - 1) / SPARSEBLOCK_SIZE;
    sparseblock *new_blocks;

    SparseDict_INVARIANT(self);
    assert(self->max_items != new_max_items || self->num_deleted != 0);

    new_blocks = PyMem_NEW(sparseblock, num_blocks);
    if (new_blocks == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    memset(new_blocks, 0, num_blocks * sizeof(sparseblock));

    SparseDict_FOR(self, entry)
        dictentry *new_entry;
        Py_hash_t hash;
        size_t i, num_probes = 0;
        PyObject *key = entry.key;

        if (PyBytes_CheckExact(key)) {
            hash = ((PyBytesObject *)key)->ob_shash;
            if (hash == -1)
                hash = PyObject_Hash(key);
        }
        else {
            hash = PyObject_Hash(key);
            if (hash == -1)
                goto Failed;
        }

        i = hash_mix((size_t)hash) & max_items_mask;
        while (BIT_TEST(new_blocks[i / SPARSEBLOCK_SIZE].bitmap, i % SPARSEBLOCK_SIZE)) {
            ++num_probes;
            i = (i + num_probes) & max_items_mask;
        }

        new_entry = sparseblock_insert(&new_blocks[i / SPARSEBLOCK_SIZE], i % SPARSEBLOCK_SIZE);
        if (new_entry == NULL)
            goto Failed;
        *new_entry = entry;
    SparseDict_ENDFOR(self, 1)

    self->blocks = new_blocks;
    self->num_blocks = num_blocks;
    self->num_items -= self->num_deleted;
    self->num_deleted = 0;
    self->max_items = new_max_items;

    SparseDict_INVARIANT(self);
    return 0;

Failed:
    /* Recovery is impossible. */
    PyMem_FREE(new_blocks);
    assert(0); // TODO XXXXXXXXXXXXXX
    return -1;
}

Py_LOCAL(int)
dict_merge_seq2(SparseDictObject *self, PyObject *seq2)
{
    PyObject *it;       /* iter(seq2) */
    Py_ssize_t i;       /* index into seq2 of current element */
    PyObject *item;     /* seq2[i] */
    PyObject *fast;     /* item as a 2-tuple or 2-list */

    SparseDict_INVARIANT(self);
    assert(seq2 != NULL);

    it = PyObject_GetIter(seq2);
    if (it == NULL)
        return -1;

    for (i = 0; ; ++i) {
        PyObject *key, *value;
        Py_ssize_t n;

        fast = NULL;
        item = PyIter_Next(it);
        if (item == NULL) {
            if (PyErr_Occurred())
                goto Fail;
            break;
        }

        /* Convert item to sequence, and verify length 2. */
        fast = PySequence_Fast(item, "");
        if (fast == NULL) {
            if (PyErr_ExceptionMatches(PyExc_TypeError))
                PyErr_Format(PyExc_TypeError,
                             "cannot convert dictionary update sequence element #%zd to a sequence",
                             i);
            goto Fail;
        }
        n = PySequence_Fast_GET_SIZE(fast);
        if (n != 2) {
            PyErr_Format(PyExc_ValueError,
                         "dictionary update sequence element #%zd has length %zd; 2 is required",
                         i, n);
            goto Fail;
        }

        /* Update/merge with this (key, value) pair. */
        key = PySequence_Fast_GET_ITEM(fast, 0);
        value = PySequence_Fast_GET_ITEM(fast, 1);
        if (dict_insert(self, key, value) < 0)
            goto Fail;
        Py_DECREF(fast);
        Py_DECREF(item);
    }

    i = 0;
    goto Return;
Fail:
    Py_XDECREF(item);
    Py_XDECREF(fast);
    i = -1;
Return:
    Py_DECREF(it);
    return Py_SAFE_DOWNCAST(i, Py_ssize_t, int);
}

Py_LOCAL(int)
dict_merge(SparseDictObject *self, PyObject *arg)
{
    SparseDict_INVARIANT(self);

    if (SparseDict_Check(arg)) {
        SparseDictObject *other = (SparseDictObject *)arg;
        if (other == self || SparseDict_SIZE(other) == 0)
            return 0;

        if (dict_resize_delta(self, SparseDict_SIZE(other) - SparseDict_SIZE(self)) != 0)
            return -1;
    
        SparseDict_FOR(other, entry)
            Py_INCREF(entry.key);
            Py_INCREF(entry.value);
            if (dict_insert(self, entry.key, entry.value) != 0)
                return -1;
        SparseDict_ENDFOR(other, 0)
    }
    else if (PyDict_Check(arg)) {
        Py_ssize_t other_size = PyDict_Size(arg);
        Py_ssize_t pos = 0;
        PyObject *key, *value;

        if (other_size == 0)
            return 0;
        if (dict_resize_delta(self, other_size - SparseDict_SIZE(self)) != 0)
            return -1;

        while (PyDict_Next(arg, &pos, &key, &value)) {
            Py_INCREF(key);
            Py_INCREF(value);
            if (dict_insert(self, key, value) != 0)
                return -1;
        }
    }
    else {
        /* Do it the generic, slower way */
        PyObject *keys = PyMapping_Keys(arg);
        PyObject *iter;
        PyObject *key, *value;
        int status;

        if (keys == NULL)
            return -1;

        iter = PyObject_GetIter(keys);
        Py_DECREF(keys);
        if (iter == NULL)
            return -1;

        while ((key = PyIter_Next(iter)) != NULL) {
            value = PyObject_GetItem(arg, key);
            if (value == NULL) {
                Py_DECREF(iter);
                Py_DECREF(key);
                return -1;
            }
            status = dict_insert(self, key, value);
            Py_DECREF(key);
            Py_DECREF(value);
            if (status < 0) {
                Py_DECREF(iter);
                return -1;
            }
        }
        Py_DECREF(iter);
        if (PyErr_Occurred())
            /* Iterator completed, via error */
            return -1;
    }
    return 0;
}

Py_LOCAL_INLINE(int)
dict_update_common(SparseDictObject *self, PyObject *args, PyObject *kwds, char *methname)
{
    PyObject *arg = NULL;
    int result = 0;

    if (!PyArg_UnpackTuple(args, methname, 0, 1, &arg))
        return -1;

    if (arg != NULL) {
        if (PyObject_HasAttrString(arg, "keys"))
            result = dict_merge(self, arg);
        else
            result = dict_merge_seq2(self, arg);
    }

    if (result == 0 && kwds != NULL) {
        if (PyArg_ValidateKeywordArguments(kwds))
            result = dict_merge(self, kwds);
        else
            result = -1;
    }
    return result;
}

/* Return 1 if dicts equal, 0 if not, -1 if error.
 * Gets out as soon as any difference is detected.
 * Uses only Py_EQ comparison.
 */
Py_LOCAL(int)
dict_equal(SparseDictObject *self, PyObject *arg)
{
    SparseDictObject *other = NULL;

    SparseDict_INVARIANT(self);

    if (SparseDict_Check(arg)) {
        other = (SparseDictObject *)arg;
        if (SparseDict_SIZE(self) != SparseDict_SIZE(other))
            return 0;
    }
    else if (PyDict_Check(arg)) {
        if (SparseDict_SIZE(self) != PyDict_Size(arg))
            return 0;
    }
    else {
        /* Unsupported type. */
        return 0;
    }

    // XXX: protect from modification?

    SparseDict_FOR(self, entry)
        PyObject *key = entry.key;
        PyObject *value = entry.value;
        PyObject *value2;
        int cmp;

        Py_INCREF(key);
        Py_INCREF(value);
        if (other != NULL) {
            /* comparing with another SparseDict */
            dictentry *entry2 = (other->lookup)(other, key, -1, 0);
            Py_DECREF(key);
            if (entry2 == NULL)
                return -1;
            if (entry2->key == NULL)
                return 0;
            value2 = entry2->value;
        }
        else {
            /* comparing with PyDictObject */
            value2 = PyDict_GetItemWithError(arg, key);
            if (value2 == NULL) {
                Py_DECREF(value);
                return PyErr_Occurred() ? -1 : 0;
            }
        }

        cmp = PyObject_RichCompareBool(value, value2, Py_EQ);
        Py_DECREF(value);
        if (cmp <= 0)  /* error or not equal */
            return cmp;
    SparseDict_ENDFOR(self, 0)
    return 1;
}

/* SparseDict type methods */

static SparseDictObject *
dict_tp_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    SparseDictObject *self;
    
    self = (SparseDictObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        /* tp_alloc zero-initialized out struct */
        SparseDict_INIT_NONZERO(self);
        self->lookup = dict_lookup_string;
        /* The object has been implicitely tracked by tp_alloc */
        if (type == &SparseDict_Type)
            PyObject_GC_UnTrack(self);
    }
    return self;
}

static int
dict_tp_init(SparseDictObject *self, PyObject *args, PyObject *kwds)
{
    return dict_update_common(self, args, kwds, "SparseDict");
}

static void
dict_tp_dealloc(SparseDictObject *self)
{
    SparseDict_INVARIANT(self);

    if (_PyObject_GC_IS_TRACKED(self))
        PyObject_GC_UnTrack(self);
    /* XXX: Py_TRASHCAN_SAFE_BEGIN ? */

    /* with refcnt of 0 we don't need to protect from modifications. */
    SparseDict_FOR(self, entry)
        Py_DECREF(entry.key);
        Py_DECREF(entry.value);
        /* destructive FOR frees the blocks for us */
    SparseDict_ENDFOR(self, 1) 

    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *
dict_tp_repr(SparseDictObject *self)
{
    Py_ssize_t i;
    PyObject *s, *temp, *colon = NULL;
    PyObject *pieces = NULL, *result = NULL;

    i = Py_ReprEnter((PyObject *)self);
    if (i != 0)
        return i > 0 ? PyString_FromString("{...}") : NULL;

    if (SparseDict_SIZE(self) == 0) {
        result = PyString_FromString("{}");
        goto Done;
    }

    pieces = PyList_New(0);
    if (pieces == NULL)
        goto Done;

    colon = PyString_FromString(": ");
    if (colon == NULL)
        goto Done;

    /* Do repr() on each key+value pair, and insert ": " between them.
       Note that repr may mutate the dict. */
    SparseDict_FOR(self, entry)
        int status;
        /* Prevent repr from deleting value during key format. */
        Py_INCREF(entry.value);
        s = PyObject_Repr(entry.key);
        PyString_Concat(&s, colon);
        temp = PyObject_Repr(entry.value);
        PyString_Concat(&s, temp);
        Py_XDECREF(temp);
        Py_DECREF(entry.value);
        if (s == NULL)
            goto Done;
        status = PyList_Append(pieces, s);
        Py_DECREF(s);  /* append created a new ref */
        if (status < 0)
            goto Done;
    SparseDict_ENDFOR(self, 0)

    /* Add "{}" decorations to the first and last items. */
    assert(PyList_GET_SIZE(pieces) > 0);
    s = PyString_FromString("{");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, 0);
    PyString_Concat(&s, temp);
    Py_XDECREF(temp);
    PyList_SET_ITEM(pieces, 0, s);
    if (s == NULL)
        goto Done;

    s = PyString_FromString("}");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1);
    PyString_Concat(&temp, s);
    Py_XDECREF(s);
    PyList_SET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1, temp);
    if (temp == NULL)
        goto Done;

    /* Paste them all together with ", " between. */
    s = PyString_FromString(", ");
    if (s == NULL)
        goto Done;
    result = _PyString_Join(s, pieces);
    Py_DECREF(s);

Done:
    Py_XDECREF(pieces);
    Py_XDECREF(colon);
    Py_ReprLeave((PyObject *)self);
    return result;
}

static PyObject *
dict_tp_richcompare(PyObject *arg1, PyObject *arg2, int op)
{
    int cmp;
    PyObject *result;

	if (op != Py_EQ && op != Py_NE) {
		PyErr_SetString(PyExc_TypeError, "SparseDict only support equality and inequality");
		return NULL;
	}

	if (SparseDict_Check(arg1))
        cmp = dict_equal((SparseDictObject *)arg1, arg2);
	else if (SparseDict_Check(arg2))
		cmp = dict_equal((SparseDictObject *)arg2, arg1);
    else
        cmp = -1;

	if (cmp != -1)
		result = (cmp ^ (op == Py_NE)) ? Py_True : Py_False;
	else
		result = Py_NotImplemented;
	Py_INCREF(result);
	return result;
}

static int
dict_tp_traverse(SparseDictObject *self, visitproc visit, void *arg)
{
    SparseDict_FOR(self, entry)
        Py_VISIT(entry.key);
        Py_VISIT(entry.value);
    SparseDict_ENDFOR(self, 0)
    return 0;
}

static int
dict_tp_clear(SparseDictObject *self)
{
    /* Actually we only need blocks and num_blocks. */
    SparseDictObject old_self = *self;
    SparseDict_INIT(self);

    SparseDict_FOR(&old_self, entry)
        Py_DECREF(entry.key);
        Py_DECREF(entry.value);
    SparseDict_ENDFOR(self, 1)
    return 0;
}

static PyObject *
dict_tp_iter(SparseDictObject *dict)
{
    return dictiter_new(dict, &SparseDictIterKey_Type);
}

static Py_ssize_t
dict_mp_length(SparseDictObject *self)
{
    return SparseDict_SIZE(self);
}

static PyObject *
dict_mp_subscript(SparseDictObject *self, PyObject *key)
{
    dictentry *entry = (self->lookup)(self, key, -1, 0);

    if (entry == NULL)
        return NULL;
    if (entry->key == NULL) {
        set_key_error(key);
        return NULL;
    }
    Py_INCREF(entry->value);
    return entry->value;
}

static int
dict_mp_ass_subscript(SparseDictObject *self, PyObject *key, PyObject *value)
{
    if (value != NULL)
        return dict_insert(self, key, value);
    else
        return dict_delete(self, key);
}

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
int
dict_sq_contains(SparseDictObject *self, PyObject *key)
{
    dictentry *entry = (self->lookup)(self, key, -1, 0);
    if (entry == NULL)
        return -1;
    return (entry->key != NULL);
}

/* SparseDict public methods */

static PyObject *
dict_py_keys(SparseDictObject *self)
{
    PyObject *list;
    Py_ssize_t num, i;

Again:
    num = SparseDict_SIZE(self);
    list = PyList_New(num);
    if (list == NULL)
        return NULL;
    if (num != SparseDict_SIZE(self)) {
        /* Durnit. The allocations caused the dict to resize. */
        Py_DECREF(list);
        goto Again;
    }

    i = 0;
    SparseDict_FOR(self, entry)
        Py_INCREF(entry.key);
        PyList_SET_ITEM(list, i, entry.key);
        ++i;
    SparseDict_ENDFOR(self, 0)

    assert(i == num);
    return list;
}

static PyObject *
dict_py_values(SparseDictObject *self)
{
    PyObject *list;
    Py_ssize_t num, i;

Again:
    num = SparseDict_SIZE(self);
    list = PyList_New(num);
    if (list == NULL)
        return NULL;
    if (num != SparseDict_SIZE(self)) {
        /* Durnit. The allocations caused the dict to resize. */
        Py_DECREF(list);
        goto Again;
    }

    i = 0;
    SparseDict_FOR(self, entry)
        Py_INCREF(entry.value);
        PyList_SET_ITEM(list, i, entry.value);
        ++i;
    SparseDict_ENDFOR(self, 0)

    assert(i == num);
    return list;
}

static PyObject *
dict_py_items(SparseDictObject *self)
{
    PyObject *list, *pair;
    Py_ssize_t i, num;

    /* Preallocate the list of tuples, to avoid GC during the loop. */
Again:
    num = SparseDict_SIZE(self);
    list = PyList_New(num);
    if (list == NULL)
        return NULL;
    for (i = 0; i < num; i++) {
        pair = PyTuple_New(2);
        if (pair == NULL) {
            Py_DECREF(list);
            return NULL;
        }
        PyList_SET_ITEM(list, i, pair);
    }
    if (num != SparseDict_SIZE(self)) {
        Py_DECREF(list);
        goto Again;
    }
    /* Nothing we do below makes any function calls. */
    i = 0;
    SparseDict_FOR(self, entry)
        pair = PyList_GET_ITEM(list, i);
        Py_INCREF(entry.key);
        Py_INCREF(entry.value);
        PyTuple_SET_ITEM(pair, 0, entry.key);
        PyTuple_SET_ITEM(pair, 1, entry.value);
        i++;
    SparseDict_ENDFOR(self, 0)

    assert(i == num);
    return list;
}

static PyObject *
dict_py_fromkeys(PyObject *cls, PyObject *args)
{
    /* TODO: prealloc */
    SparseDictObject *self;
    PyObject *seq, *it, *key, *value = Py_None;

    if (!PyArg_UnpackTuple(args, "fromkeys", 1, 2, &seq, &value))
        return NULL;

    self = (SparseDictObject *)PyObject_CallObject(cls, NULL);
    if (self == NULL)
        return NULL;

    it = PyObject_GetIter(seq);
    if (it == NULL){
        Py_DECREF(self);
        return NULL;
    }
    while ((key = PyIter_Next(it)) != NULL) {
        int status = PyObject_SetItem((PyObject *)self, key, value);
        Py_DECREF(key);
        if (status < 0)
            break;
    }
    if (PyErr_Occurred()) {
        Py_DECREF(self);
        Py_DECREF(it);
        return NULL;
    }
    Py_DECREF(it);
    return (PyObject *)self;
}

static PyObject *
dict_py_update(SparseDictObject *self, PyObject *args, PyObject *kwds)
{
    if (dict_update_common(self, args, kwds, "update") != -1)
        Py_RETURN_NONE;
    return NULL;
}

static PyObject *
dict_py_copy(SparseDictObject *self)
{
    /* XXX: this is probably wrong */
    SparseDictObject *copy = (SparseDictObject *)Py_TYPE(self)->tp_new(Py_TYPE(self), NULL, NULL);
    if (copy == NULL)
        return NULL;

    copy->lookup = self->lookup;
    if (dict_merge(copy, (PyObject *)self) != 0) {
        Py_DECREF(copy);
        return NULL;
    }
    return (PyObject *) copy;
}

static PyObject *
dict_py_contains(SparseDictObject *self, PyObject *key)
{
    dictentry *entry = (self->lookup)(self, key, -1, 0);
    if (entry == NULL)
        return NULL;
    return PyBool_FromLong(entry->key != NULL);
}

static PyObject *
dict_py_get(SparseDictObject *self, PyObject *args)
{
    PyObject *key;
    PyObject *value = Py_None;
    dictentry *entry;

    if (!PyArg_UnpackTuple(args, "get", 1, 2, &key, &value))
        return NULL;

    entry = (self->lookup)(self, key, -1, 0);
    if (entry == NULL)
        return NULL;
    if (entry->key != NULL)
        value = entry->value;
    Py_INCREF(value);
    return value;
}

static PyObject *
dict_py_setdefault(SparseDictObject *self, PyObject *args)
{
    PyObject *key, *value = Py_None;
    dictentry *entry;

    if (!PyArg_UnpackTuple(args, "setdefault", 1, 2, &key, &value))
        return NULL;

    entry = (self->lookup)(self, key, -1, 1);
    if (entry == NULL)
        return NULL;
    if (entry->key == NULL) {
        /* Insert new */
        Py_INCREF(key);
        Py_INCREF(value);
        entry->key = key;
        entry->value = value;
    }
    else {
        /* Return existing */
        value = entry->value;
    }
    Py_INCREF(value);
    return value;
}

static PyObject *
dict_py_clear(SparseDictObject *self)
{
    dict_tp_clear(self);
    Py_RETURN_NONE;
}

static PyObject *
dict_py_pop(SparseDictObject *self, PyObject *args)
{
    PyObject *key, *value = NULL;

    if (!PyArg_UnpackTuple(args, "pop", 1, 2, &key, &value))
        return NULL;

    if (SparseDict_SIZE(self) != 0) {
        dictentry *entry = (self->lookup)(self, key, -1, 0);
        if (entry == NULL)
            return NULL;
        if (entry->key != NULL) {
            PyObject *old_key = entry->key;
            PyObject *old_value = entry->value;
            entry->key = NULL;
            ++self->num_deleted;
            Py_DECREF(old_key);
            return old_value;
        }
    }
    if (value) {
        Py_INCREF(value);
        return value;
    }
    set_key_error(key);
    return NULL;

}

static PyObject *
dict_py_popitem(SparseDictObject *self)
{
    PyObject *pair;
    dictentry *entry;

    pair = PyTuple_New(2);
    if (pair == NULL)
        return NULL;
    if (SparseDict_SIZE(self) == 0) {
        Py_DECREF(pair);
        PyErr_SetString(PyExc_KeyError, "popitem(): SparseDict is empty");
        return NULL;
    }

    assert(self->next_index >= 0);
    entry = dict_next(self, &self->next_index, 1);

    PyTuple_SET_ITEM(pair, 0, entry->key);
    PyTuple_SET_ITEM(pair, 1, entry->value);
    entry->key = NULL;
    ++self->num_deleted;
    return pair;
}

static PyObject *
dict_py_resize(SparseDictObject *self, PyObject *arg)
{
    Py_ssize_t size = PyInt_AsSsize_t(arg);
	if (size < 0) {
		PyErr_SetString(PyExc_ValueError, "Argument to resize() must be a nonnegative integer.");
		return NULL;
	}
    if (dict_resize_delta(self, size - SparseDict_SIZE(self)) < 0)
		return NULL;
	Py_RETURN_NONE;
}

static PyObject *
dict_py_iterkeys(SparseDictObject *dict)
{
    return dictiter_new(dict, &SparseDictIterKey_Type);
}

static PyObject *
dict_py_itervalues(SparseDictObject *dict)
{
    return dictiter_new(dict, &SparseDictIterValue_Type);
}

static PyObject *
dict_py_iteritems(SparseDictObject *dict)
{
    return dictiter_new(dict, &SparseDictIterItem_Type);
}

static PyObject *
dict_py_viewkeys(SparseDictObject *dict)
{
    return dictview_new(dict, &SparseDictKeys_Type);
}

static PyObject *
dict_py_viewvalues(SparseDictObject *dict)
{
    return dictview_new(dict, &SparseDictValues_Type);
}

static PyObject *
dict_py_viewitems(SparseDictObject *dict)
{
    return dictview_new(dict, &SparseDictItems_Type);
}

static PyObject *
dict_py_sizeof(SparseDictObject *self)
{
    Py_ssize_t result = sizeof(SparseDictObject);
    if (self->blocks != self->static_blocks)
        result += sizeof(sparseblock) * self->num_blocks;
    result += sizeof(dictentry) * self->num_items;
    return PyInt_FromSsize_t(result);
}

static PyObject *
dict_py_dump(SparseDictObject *self)
{
    Py_ssize_t i, hist_max = 0;
    Py_ssize_t hist[SPARSEBLOCK_SIZE+1] = { 0 };
    for (i = 0; i < self->num_blocks; ++i)
        ++hist[self->blocks[i].num_items];

    for (i = 0; i <= SPARSEBLOCK_SIZE; ++i)
        if (hist[i] > hist_max)
            hist_max = hist[i];

    fprintf(stderr, "\n\nSparseDict:\nmax_items = %d\nnum_items = %d\nnum_blocks = %d\n",
        self->max_items, self->num_items, self->num_blocks);
    fprintf(stderr, "block size distribution:\n");
    for (i = 0; i <= SPARSEBLOCK_SIZE; ++i)
        fprintf(stderr, "%4d: %-4d %10d\n", i, hist[i]);

    Py_RETURN_NONE;
}

static PyMethodDef dict_methods[] = {
    {"__sizeof__",  (PyCFunction)dict_py_sizeof,       METH_NOARGS}, /* sys.getsizeof support */
    {"__contains__",(PyCFunction)dict_py_contains,     METH_O | METH_COEXIST}, /* shortcut for sq_contains */
    {"__getitem__", (PyCFunction)dict_mp_subscript,    METH_O | METH_COEXIST}, /* shortcut for mp_getitem */
    {"get",         (PyCFunction)dict_py_get,          METH_VARARGS},
    {"setdefault",  (PyCFunction)dict_py_setdefault,   METH_VARARGS},
    {"pop",         (PyCFunction)dict_py_pop,          METH_VARARGS},
    {"popitem",     (PyCFunction)dict_py_popitem,      METH_NOARGS},
    {"update",      (PyCFunction)dict_py_update,       METH_VARARGS | METH_KEYWORDS},
    {"fromkeys",    (PyCFunction)dict_py_fromkeys,     METH_VARARGS | METH_CLASS},
    {"clear",       (PyCFunction)dict_py_clear,        METH_NOARGS},
    {"copy",        (PyCFunction)dict_py_copy,         METH_NOARGS},
    {"resize",      (PyCFunction)dict_py_resize,       METH_O},
    {"dump",      (PyCFunction)dict_py_dump,       METH_NOARGS},
#if PY_MAJOR_VERSION < 3
    {"has_key",     (PyCFunction)dict_py_contains,     METH_O},
    {"keys",        (PyCFunction)dict_py_keys,         METH_NOARGS},
    {"values",      (PyCFunction)dict_py_values,       METH_NOARGS},
    {"items",       (PyCFunction)dict_py_items,        METH_NOARGS},
    {"iterkeys",    (PyCFunction)dict_py_iterkeys,     METH_NOARGS},
    {"itervalues",  (PyCFunction)dict_py_itervalues,   METH_NOARGS},
    {"iteritems",   (PyCFunction)dict_py_iteritems,    METH_NOARGS},
    {"viewkeys",    (PyCFunction)dict_py_viewkeys,     METH_NOARGS},
    {"viewvalues",  (PyCFunction)dict_py_viewvalues,   METH_NOARGS},
    {"viewitems",   (PyCFunction)dict_py_viewitems,    METH_NOARGS},
#else
    {"keys",        (PyCFunction)dict_py_viewkeys,     METH_NOARGS},
    {"values",      (PyCFunction)dict_py_viewvalues,   METH_NOARGS},
    {"items",       (PyCFunction)dict_py_viewitems,    METH_NOARGS},
#endif
    {NULL}   /* sentinel */
};

static PySequenceMethods dict_as_sequence = {
    0,                             /* sq_length */
    0,                             /* sq_concat */
    0,                             /* sq_repeat */
    0,                             /* sq_item */
    0,                             /* sq_slice */
    0,                             /* sq_ass_item */
    0,                             /* sq_ass_slice */
    (objobjproc)dict_sq_contains,  /* sq_contains */
    0,                             /* sq_inplace_concat */
    0,                             /* sq_inplace_repeat */
};

static PyMappingMethods dict_as_mapping = {
    (lenfunc)dict_mp_length,               /* mp_length */
    (binaryfunc)dict_mp_subscript,         /* mp_subscript */
    (objobjargproc)dict_mp_ass_subscript,  /* mp_ass_subscript */
};

PyTypeObject SparseDict_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "SparseDict",
    sizeof(SparseDictObject),
    0,
    (destructor)dict_tp_dealloc,                /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    (reprfunc)dict_tp_repr,                     /* tp_repr */
    0,                                          /* tp_as_number */
    &dict_as_sequence,                          /* tp_as_sequence */
    &dict_as_mapping,                           /* tp_as_mapping */
    PyObject_HashNotImplemented,                /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE, /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dict_tp_traverse,             /* tp_traverse */
    (inquiry)dict_tp_clear,                     /* tp_clear */
    (richcmpfunc)dict_tp_richcompare,           /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dict_tp_iter,                  /* tp_iter */
    0,                                          /* tp_iternext */
    dict_methods,                               /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    (initproc)dict_tp_init,                     /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    (newfunc)dict_tp_new,                       /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
};


/* Key, value and item iterators. */

typedef struct {
    PyObject_HEAD
    SparseDictObject *sdict; /* set to NULL when iterator is exhausted */
    Py_ssize_t num_items; /* original size to track modifications */
    Py_ssize_t remaining_items;
    Py_ssize_t next_index;
    PyObject* pair; /* reusable result tuple for iteritems */
} dictiterobject;

static PyObject *
dictiter_new(SparseDictObject *sdict, PyTypeObject *itertype)
{
    dictiterobject *di = PyObject_GC_New(dictiterobject, itertype);
    if (di == NULL)
        return NULL;

    Py_INCREF(sdict);
    di->sdict = sdict;
    di->num_items = sdict->num_items;
    di->next_index = 0;
    di->remaining_items = SparseDict_SIZE(sdict);
    if (itertype == &SparseDictIterItem_Type) {
        di->pair = PyTuple_Pack(2, Py_None, Py_None);
        if (di->pair == NULL) {
            Py_DECREF(di);
            return NULL;
        }
    }
    else
        di->pair = NULL;
    PyObject_GC_Track(di);
    return (PyObject *)di;
}

static void
dictiter_tp_dealloc(dictiterobject *di)
{
    Py_XDECREF(di->sdict);
    Py_XDECREF(di->pair);
    PyObject_GC_Del(di);
}

static int
dictiter_tp_traverse(dictiterobject *di, visitproc visit, void *arg)
{
    Py_VISIT(di->sdict);
    Py_VISIT(di->pair);
    return 0;
}

static PyObject *
dictiter_len_hint(dictiterobject *di)
{
    Py_ssize_t len = 0;
    if (di->sdict != NULL && di->num_items == di->sdict->num_items)
        len = di->remaining_items;
    return PyInt_FromSsize_t(len);
}

static PyObject *dictiter_iternextkey(dictiterobject *di)
{
    dictentry *entry;
    SparseDictObject *sdict = di->sdict;

    if (sdict == NULL)
        return NULL;
    SparseDict_INVARIANT(sdict);

    if (di->num_items != sdict->num_items) {
        PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
        di->num_items = -1; /* Make this state sticky */
        return NULL;
    }

    entry = dict_next(sdict, &di->next_index, 0);
    if (entry == NULL) {
        Py_DECREF(sdict);
        di->sdict = NULL;
        return NULL;
    }

    Py_INCREF(entry->key);
    return entry->key;
}

static PyObject *dictiter_iternextvalue(dictiterobject *di)
{
    dictentry *entry;
    SparseDictObject *sdict = di->sdict;

    if (sdict == NULL)
        return NULL;
    SparseDict_INVARIANT(sdict);

    if (di->num_items != sdict->num_items) {
        PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
        di->num_items = -1; /* Make this state sticky */
        return NULL;
    }

    entry = dict_next(sdict, &di->next_index, 0);
    if (entry == NULL) {
        Py_DECREF(sdict);
        di->sdict = NULL;
        return NULL;
    }

    --di->remaining_items;
    Py_INCREF(entry->value);
    return entry->value;
}

static PyObject *dictiter_iternextitem(dictiterobject *di)
{
    PyObject *pair = di->pair;
    dictentry *entry;
    SparseDictObject *sdict = di->sdict;

    if (sdict == NULL)
        return NULL;
    SparseDict_INVARIANT(sdict);

     if (di->num_items != sdict->num_items) {
        PyErr_SetString(PyExc_RuntimeError, "dictionary changed size during iteration");
        di->num_items = -1; /* Make this state sticky */
        return NULL;
    }

    entry = dict_next(sdict, &di->next_index, 0);
    if (entry == NULL) {
        Py_DECREF(sdict);
        di->sdict = NULL;
        return NULL;
    }

    if (pair->ob_refcnt == 1) {
        Py_INCREF(pair);
        Py_DECREF(PyTuple_GET_ITEM(pair, 0));
        Py_DECREF(PyTuple_GET_ITEM(pair, 1));
    }
    else {
        pair = PyTuple_New(2);
        if (pair == NULL)
            return NULL;
    }
    --di->remaining_items;
    Py_INCREF(entry->key);
    Py_INCREF(entry->value);
    PyTuple_SET_ITEM(pair, 0, entry->key);
    PyTuple_SET_ITEM(pair, 1, entry->value);
    return pair;
}

static PyMethodDef dictiter_methods[] = {
    {"__length_hint__", (PyCFunction)dictiter_len_hint, METH_NOARGS}, /* undocumented, but harmless */
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject SparseDictIterKey_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "SparseDict_KeyIter",                       /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)dictiter_tp_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_tp_traverse,         /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextkey,         /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
};

PyTypeObject SparseDictIterValue_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "SparseDict_ValueIter",                     /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)dictiter_tp_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_tp_traverse,         /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextvalue,       /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
};

PyTypeObject SparseDictIterItem_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "SparseDict_ItemIter",                      /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    (destructor)dictiter_tp_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_tp_traverse,         /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextitem,        /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
};


/* Key, value and item views. */

typedef struct {
    PyObject_HEAD
    SparseDictObject *sdict;
} dictviewobject;


static void
dictview_tp_dealloc(dictviewobject *dv)
{
    Py_XDECREF(dv->sdict);
    PyObject_GC_Del(dv);
}

static int
dictview_tp_traverse(dictviewobject *dv, visitproc visit, void *arg)
{
    Py_VISIT(dv->sdict);
    return 0;
}

static PyObject *
dictview_new(SparseDictObject *dict, PyTypeObject *type)
{
    dictviewobject *dv = PyObject_GC_New(dictviewobject, type);
    if (dv == NULL)
        return NULL;
    Py_INCREF(dict);
    dv->sdict = dict;
    PyObject_GC_Track(dv);
    return (PyObject *)dv;
}

static Py_ssize_t
dictview_sq_len(dictviewobject *dv)
{
    Py_ssize_t len = 0;
    if (dv->sdict != NULL)
        len = SparseDict_SIZE(dv->sdict);
    return len;
}

/* Return 1 if self is a subset of other, iterating over self;
   0 if not; -1 if an error occurred. */
static int
all_contained_in(PyObject *self, PyObject *other)
{
    PyObject *iter = PyObject_GetIter(self);
    int ok = 1;

    if (iter == NULL)
        return -1;
    for (;;) {
        PyObject *next = PyIter_Next(iter);
        if (next == NULL) {
            if (PyErr_Occurred())
                ok = -1;
            break;
        }
        ok = PySequence_Contains(other, next);
        Py_DECREF(next);
        if (ok <= 0)
            break;
    }
    Py_DECREF(iter);
    return ok;
}

static PyObject *
dictview_tp_richcompare(PyObject *self, PyObject *other, int op)
{
    Py_ssize_t len_self, len_other;
    int ok;
    PyObject *result;

    assert(self != NULL);
    assert(SparseDictViewSet_Check(self));
    assert(other != NULL);

    if (!PyAnySet_Check(other) && !SparseDictViewSet_Check(other)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    len_self = PyObject_Size(self);
    if (len_self < 0)
        return NULL;
    len_other = PyObject_Size(other);
    if (len_other < 0)
        return NULL;

    ok = 0;
    switch(op) {

    case Py_NE:
    case Py_EQ:
        if (len_self == len_other)
            ok = all_contained_in(self, other);
        if (op == Py_NE && ok >= 0)
            ok = !ok;
        break;

    case Py_LT:
        if (len_self < len_other)
            ok = all_contained_in(self, other);
        break;

      case Py_LE:
          if (len_self <= len_other)
              ok = all_contained_in(self, other);
          break;

    case Py_GT:
        if (len_self > len_other)
            ok = all_contained_in(other, self);
        break;

    case Py_GE:
        if (len_self >= len_other)
            ok = all_contained_in(other, self);
        break;

    }
    if (ok < 0)
        return NULL;
    result = ok ? Py_True : Py_False;
    Py_INCREF(result);
    return result;
}

static PyObject *
dictview_tp_repr(dictviewobject *dv)
{
    PyObject *seq;
    PyObject *result;

    seq = PySequence_List((PyObject *)dv);
    if (seq == NULL)
        return NULL;

    result = PyString_FromFormat("%s(%R)", Py_TYPE(dv)->tp_name, seq);
    Py_DECREF(seq);
    return result;
}

static PyObject *
dictkeys_tp_iter(dictviewobject *dv)
{
    if (dv->sdict == NULL)
        Py_RETURN_NONE;
    return dictiter_new(dv->sdict, &PyDictIterKey_Type);
}

static int
dictkeys_sq_contains(dictviewobject *dv, PyObject *obj)
{
    if (dv->sdict == NULL)
        return 0;
    return dict_sq_contains(dv->sdict, obj);
}

static PyObject*
dictviews_nb_sub(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "difference_update", "O", other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyObject*
dictviews_nb_and(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "intersection_update", "O", other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyObject*
dictviews_nb_or(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "update", "O", other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyObject*
dictviews_nb_xor(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "symmetric_difference_update", "O", other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyObject*
dictviews_py_isdisjoint(PyObject *self, PyObject *other)
{
    PyObject *it;
    PyObject *item = NULL;

    if (self == other)
        return PyBool_FromLong(dictview_sq_len((dictviewobject *)self) == 0);

    /* Iterate over the shorter object (only if other is a set,
     * because PySequence_Contains may be expensive otherwise): */
    if (PyAnySet_Check(other) || SparseDictViewSet_Check(other)) {
        Py_ssize_t len_self = dictview_sq_len((dictviewobject *)self);
        Py_ssize_t len_other = PyObject_Size(other);
        if (len_other == -1)
            return NULL;

        if ((len_other > len_self)) {
            PyObject *tmp = other;
            other = self;
            self = tmp;
        }
    }

    it = PyObject_GetIter(other);
    if (it == NULL)
        return NULL;

    while ((item = PyIter_Next(it)) != NULL) {
        int contains = PySequence_Contains(self, item);
        Py_DECREF(item);
        if (contains == -1) {
            Py_DECREF(it);
            return NULL;
        }
        if (contains) {
            Py_DECREF(it);
            Py_RETURN_FALSE;
        }
    }
    Py_DECREF(it);
    if (PyErr_Occurred())
        return NULL; /* PyIter_Next raised an exception. */
    Py_RETURN_TRUE;
}


static PyObject *
dictvalues_tp_iter(dictviewobject *dv)
{
    if (dv->sdict == NULL)
        Py_RETURN_NONE;
    return dictiter_new(dv->sdict, &PyDictIterValue_Type);
}

static PyObject *
dictitems_tp_iter(dictviewobject *dv)
{
    if (dv->sdict == NULL)
        Py_RETURN_NONE;
    return dictiter_new(dv->sdict, &PyDictIterItem_Type);
}

static int
dictitems_contains(dictviewobject *dv, PyObject *obj)
{
    PyObject *key, *value, *found;
    if (dv->sdict == NULL)
        return 0;
    if (!PyTuple_Check(obj) || PyTuple_GET_SIZE(obj) != 2)
        return 0;
    key = PyTuple_GET_ITEM(obj, 0);
    value = PyTuple_GET_ITEM(obj, 1);
    found = PyDict_GetItem((PyObject *)dv->sdict, key);
    if (found == NULL) {
        if (PyErr_Occurred())
            return -1;
        return 0;
    }
    return PyObject_RichCompareBool(value, found, Py_EQ);
}


static PySequenceMethods dictkeys_as_sequence = {
    (lenfunc)dictview_sq_len,           /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)dictkeys_sq_contains,   /* sq_contains */
};

static PyNumberMethods dictviews_as_number = {
    0,                                  /*nb_add*/
    (binaryfunc)dictviews_nb_sub,       /*nb_subtract*/
    0,                                  /*nb_multiply*/
    0,                                  /*nb_remainder*/
    0,                                  /*nb_divmod*/
    0,                                  /*nb_power*/
    0,                                  /*nb_negative*/
    0,                                  /*nb_positive*/
    0,                                  /*nb_absolute*/
    0,                                  /*nb_bool*/
    0,                                  /*nb_invert*/
    0,                                  /*nb_lshift*/
    0,                                  /*nb_rshift*/
    (binaryfunc)dictviews_nb_and,       /*nb_and*/
    (binaryfunc)dictviews_nb_xor,       /*nb_xor*/
    (binaryfunc)dictviews_nb_or,        /*nb_or*/
};

static PyMethodDef dictkeys_methods[] = {
    {"isdisjoint",      (PyCFunction)dictviews_py_isdisjoint,  METH_O},
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject SparseDictKeys_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "SparseDict_Keys",                          /* tp_name */
    sizeof(dictviewobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_tp_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)dictview_tp_repr,                 /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictkeys_as_sequence,                      /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_tp_traverse,         /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_tp_richcompare,                    /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictkeys_tp_iter,              /* tp_iter */
    0,                                          /* tp_iternext */
    dictkeys_methods,                           /* tp_methods */
};

static PySequenceMethods dictitems_as_sequence = {
    (lenfunc)dictview_sq_len,           /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)dictitems_contains,     /* sq_contains */
};

static PyMethodDef dictitems_methods[] = {
    {"isdisjoint",      (PyCFunction)dictviews_py_isdisjoint,  METH_O},
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject SparseDictItems_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "SparseDict_Items",                         /* tp_name */
    sizeof(dictviewobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_tp_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)dictview_tp_repr,                 /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictitems_as_sequence,                     /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_tp_traverse,         /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_tp_richcompare,                    /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictitems_tp_iter,             /* tp_iter */
    0,                                          /* tp_iternext */
    dictitems_methods,                          /* tp_methods */
};

static PySequenceMethods dictvalues_as_sequence = {
    (lenfunc)dictview_sq_len,           /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)0,                      /* sq_contains */
};

PyTypeObject SparseDictValues_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "SparseDict_Values",                        /* tp_name */
    sizeof(dictviewobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_tp_dealloc,            /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)dictview_tp_repr,                 /* tp_repr */
    0,                                          /* tp_as_number */
    &dictvalues_as_sequence,                    /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,    /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_tp_traverse,         /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictvalues_tp_iter,            /* tp_iter */
};

/*  Module initialization */

Py_LOCAL(int)
sparsedict_register(PyObject *module)
{
	if (PyType_Ready(&SparseDict_Type) != 0 ||
		PyType_Ready(&SparseDictIterKey_Type) != 0 ||
		PyType_Ready(&SparseDictIterValue_Type) != 0 ||
		PyType_Ready(&SparseDictIterItem_Type) != 0 ||
        PyType_Ready(&SparseDictKeys_Type) != 0 ||
		PyType_Ready(&SparseDictValues_Type) != 0 ||
		PyType_Ready(&SparseDictItems_Type) != 0)
        return -1;

    Py_INCREF(&SparseDict_Type);
    PyModule_AddObject(module, "SparseDict", (PyObject *)&SparseDict_Type);
    
    return 0;
}

#if PY_MAJOR_VERSION < 3
PyMODINIT_FUNC init_sparsedict(void)
{
	PyObject *module = Py_InitModule("_sparsedict", NULL);
	if (module == NULL)
		return;

    if (sparsedict_register(module) != 0)
        return;
}
#else
PyMODINIT_FUNC PyInit__sparsedict()
{
	static PyModuleDef module_def = {
		PyModuleDef_HEAD_INIT,
		"sparsecoll",
	};
	PyObject *module = PyModule_Create(&module_def);
	if (module == NULL)
		return NULL;

    if (sparsedict_register(module) != 0)
        return NULL;

    return module;
}
#endif
