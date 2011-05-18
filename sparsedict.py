
from _sparsedict import SparseDict

try:
    from collections import MutableMapping
except ImportError:
    pass
else:
    MutableMapping.register(SparseDict)
    del MutableMapping
