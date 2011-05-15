
try:
	from setuptools import setup
except ImportError:
	from distutils.core import setup, Extension
from distutils.core import Extension

ext_modules = [
    Extension('_sparsedict',
              sources=['_sparsedict.c'],
              extra_compile_args = ['/Oi', '/Oy'])]

setup(name='sparsedict',
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
      py_modules=['sparsedict'],
      ext_modules=ext_modules)
