"""Loads the app's scripts (which have no .py suffix) as modules for the tests."""

import importlib.machinery
import importlib.util
import os
import sys

APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, APP_DIR)

_cache = {}


def load_script(name):
    """The script app/NAME as a module, loaded once."""
    if name not in _cache:
        path = os.path.join(APP_DIR, name)
        loader = importlib.machinery.SourceFileLoader(name.replace("-", "_"), path)
        spec = importlib.util.spec_from_loader(loader.name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        _cache[name] = module
    return _cache[name]
