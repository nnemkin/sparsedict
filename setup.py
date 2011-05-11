
try:
	from setuptools import setup
except ImportError:
	from distutils.core import setup, Extension
from distutils.core import Extension

ext_modules = [
    Extension('sparsecoll',
              sources=['sparsecoll.cpp', 'sparsedict.cpp'],
              depends=['sparsecoll.h', 'sparsedict.h'],
              extra_compile_args = ['/Oi', '/Oy'])]

setup(name='sparsecoll',
      version='1.0',
      description='Memory-efficient python collections based on Google\'s sparsehash library.',
      author='Nikita Nemkin',
      author_email='nikita@nemkin.ru',
      keywords=['google', 'sparsehash', 'map', 'dict'],
      url='https://github.com/nnemkin/sparsecoll',
      license='BSD',
      classifiers=[
          'Development Status :: 2 - Pre-Alpha',
          'Intended Audience :: Developers',
          'License :: OSI Approved :: BSD License',
          'Operating System :: OS Independent',
          'Programming Language :: Python',
          'Topic :: Software Development :: Libraries :: Python Modules'],
      ext_modules=ext_modules)
