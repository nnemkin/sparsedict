
import sys
try:
    from setuptools import setup
except ImportError:
    from distutils.core import setup, Extension
from distutils.core import Extension

ext_modules = [
    Extension('_sparsedict', sources=['_sparsedict.c'])]

setup(name='sparsedict',
      version='0.1',
      description='Memory-efficient python dictionary.',
      long_description=open('README.rst').read(),
      author='Nikita Nemkin',
      author_email='nikita@nemkin.ru',
      keywords=['sparsehash', 'sparsedict', 'map', 'hash', 'dict', 'memory'],
      url='https://github.com/nnemkin/sparsedict',
      license='BSD',
      classifiers=[
          'Development Status :: 3 - Alpha',
          'Intended Audience :: Developers',
          'License :: OSI Approved :: BSD License',
          'Operating System :: OS Independent',
          'Programming Language :: C',
          'Programming Language :: Python',
          'Programming Language :: Python :: 3',
          'Topic :: Software Development :: Libraries :: Python Modules'],
      zip_safe = False,
      py_modules=['sparsedict'],
      ext_modules=ext_modules,
      test_suite='tests.collector',
      tests_require=['unittest2'])
