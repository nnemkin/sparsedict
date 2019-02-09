
Warning
=======
This project is experimental and not suitable for production use.

SparseDict: memory-efficient dict-like container
================================================

Python's native ``dict`` type is very fast but not memory-efficient.
This module provides dictionary that is slower but uses less memory.

Preliminary benchmarks indicate x2 memory usage decrease
at the cost of x3-x5 slower performance.

This module is based on the Python's builtin dict and
algorithms and data structures from ``sparse_hash_map`` of google-sparsehash_.

.. _google-sparsehash: http://code.google.com/p/google-sparsehash/


Compatibility with ``dict``
---------------------------

``SparseDict`` is intended as a drop-in replacement for ``dict``, therefore the differences are few.

* Insertion may fail if ``__hash__`` of any of the ``SparseDict`` keys fails.
  This is due to the fact that during resize, all keys are rehashed.
* Ordering is not supported: ``cmp()`` raises a ``TypeError`` if dicts are not equal,
  operators ``<``, ``<=``, ``>``, ``>=`` also raise a ``TypeError``.


Additional API
--------------

``SparseDict`` fully implements the ``dict`` API, including differences between Python 2 and 3.
It also adds the following methods:

``SparseDict(seq_or_len, **kwargs)``
    First positional argument to constructor can be an integer specifying initial size.
    Otherwise, it has the same semantics as ``dict()``.

``resize(len)``
    Resize internal dictionary structures to hold at least ``len`` entries.
    If ``len`` is smaller that the actual length, nothing happens.
    You can call ``resize(0)`` after a large batch of deletes to trigger the shrink.

``_stats()``
    Return some information about ``SparseDict`` internals: number of allocated items,
    number of deleted items, block size distribution and more.
