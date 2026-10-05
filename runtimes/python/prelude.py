# The prelude runs once, when the release builds python.wasm, before the
# snapshot. It imports the standard modules a mod may use, because an
# instance reads no files, and defines start, which imports the mod's main
# module from its sources, and describe, which formats an error.

# The standard modules a mod may import.
import abc  # noqa: F401
import base64  # noqa: F401
import bisect  # noqa: F401
import collections  # noqa: F401
import copy  # noqa: F401
import dataclasses  # noqa: F401
import enum  # noqa: F401
import fractions  # noqa: F401
import functools  # noqa: F401
import heapq  # noqa: F401
import importlib
import importlib.machinery
import itertools  # noqa: F401
import json  # noqa: F401
import math  # noqa: F401
import operator  # noqa: F401
import random  # noqa: F401
import re  # noqa: F401
import statistics  # noqa: F401
import string  # noqa: F401
import struct  # noqa: F401
import sys
import textwrap  # noqa: F401
import time  # noqa: F401
import traceback
import types
import typing  # noqa: F401

import _modlock

# An instance has no files, so only the mod's sources and the modules above
# import.
sys.path.clear()
sys.path_importer_cache.clear()


class Sources:
    """Sources imports modules from the mod's files: a.b is a/b.py, or
    a/b/__init__.py for a package."""

    def __init__(self, files: dict[str, bytes]) -> None:
        self.files = files

    def find_spec(self, name: str, path: object = None, target: object = None) -> importlib.machinery.ModuleSpec | None:
        base = name.replace(".", "/")
        for file, package in ((base + "/__init__.py", True), (base + ".py", False)):
            if file in self.files:
                spec = importlib.machinery.ModuleSpec(name, self, origin=file, is_package=package)
                if package:
                    spec.submodule_search_locations = [base]
                spec.has_location = True
                return spec
        return None

    def create_module(self, spec: importlib.machinery.ModuleSpec) -> None:
        return None

    def exec_module(self, module: types.ModuleType) -> None:
        file = module.__spec__.origin
        exec(compile(self.files[file], file, "exec", dont_inherit=True), module.__dict__)

    def get_source(self, name: str) -> str | None:
        spec = self.find_spec(name)
        return self.files[spec.origin].decode() if spec else None


class Log:
    """Log writes text to the server log a line at a time."""

    def __init__(self) -> None:
        self.line = ""

    def write(self, text: str) -> int:
        lines = (self.line + text).split("\n")
        self.line = lines.pop()
        for line in lines:
            _modlock.log(line)
        return len(text)

    def flush(self) -> None:
        pass


def start(files: dict[str, bytes]) -> None:
    """start sends printed text to the server log and imports the mod's main
    module from files."""
    sys.stdout = sys.stderr = Log()
    sys.meta_path.insert(0, Sources(files))
    importlib.import_module("main")


def describe(error: BaseException) -> str:
    """describe returns error with its traceback."""
    return "".join(traceback.format_exception(error)).rstrip()
