
from collections import MutableMapping
from _sparsedict import SparseDict

MutableMapping.register(SparseDict)
del MutableMapping
