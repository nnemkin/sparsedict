
#include <Python.h>

#ifdef _MSC_VER
// turn off "conditional is constant" and "stack unwinding is not enabled"
#pragma warning(disable : 4127 4530)
#define _SCL_SECURE_NO_WARNINGS
#endif

#if PY_VERSION_HEX < 0x03020000
typedef long Py_hash_t;
#endif 

#ifndef PyArg_ValidateKeywordArguments
#define PyArg_ValidateKeywordArguments(kwds) true
#endif
