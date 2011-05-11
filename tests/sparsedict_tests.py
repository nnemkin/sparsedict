
from tests import mapping_tests

from sparsecoll import SparseDict

class SparseDictSubclass(SparseDict):
    pass

class TestGeneralMapping(mapping_tests.TestHashMappingProtocol):
    type2test = SparseDict
    
    def _full_mapping(self, data):
        return self.type2test(data)

class TestSubclassMapping(mapping_tests.TestHashMappingProtocol):
    type2test = SparseDictSubclass
    
    def _full_mapping(self, data):
        return self.type2test(data)
