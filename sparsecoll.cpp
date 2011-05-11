/*
	Python wrapper for Google's sparse_hash_map.
*/

#include "sparsecoll.h"
#include "sparsedict.h"

PyDoc_STRVAR(sparsecoll_doc, "Memory efficient collections for Python.");

static PyObject *_sparsecoll_init()
{
	if (PyType_Ready(&SparseDict::Type) < 0
			|| PyType_Ready(&SparseDictIter::KeyType) < 0
			|| PyType_Ready(&SparseDictIter::ValueType) < 0
			|| PyType_Ready(&SparseDictIter::ItemType) < 0)
        return NULL;

#if PY_MAJOR_VERSION >= 3
	static PyModuleDef module_def = {
		PyModuleDef_HEAD_INIT,
		"sparsecoll",
		sparsecoll_doc
	};
	PyObject *module = PyModule_Create(&module_def);
#else
	PyObject *module = Py_InitModule3("sparsecoll", NULL, sparsecoll_doc);
#endif
	if (module == NULL)
		return NULL;

	Py_INCREF(&SparseDict::Type);
    PyModule_AddObject(module, "SparseDict", (PyObject *) &SparseDict::Type);

	// TODO: collections.MutableMapping.register(SparseDict)

	return module;
}

#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC PyInit_sparsecoll()
{
	return _sparsecoll_init();
}
#else
PyMODINIT_FUNC initsparsecoll()
{
	_sparsecoll_init();
}
#endif
