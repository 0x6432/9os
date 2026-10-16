#!/usr/bin/env python3
"""Exclude optional GUI host-tool imports in the pinned Quick cross build.

The native generators remain required and genuine; qmlscene is a target app.
Qt's headless host QmlTools cannot export qmltime, qmlplugindump or
qmltestrunner, which require host Quick/QuickTest. None is a target generator.
"""
import hashlib
from pathlib import Path
import sys

ORIGINAL_SHA256 = '0a5832353e38a3f68d02de7405529bbf695f4fb6f3fa0db86634c138c48de917'


TOOLS = ('qmltime', 'qmlplugindump', 'qmltestrunner')
LEGACY_TOOLS = TOOLS[:2]


def patched(original, names=TOOLS):
    for name in names:
        old = f'            add_subdirectory({name})\n'
        if original.count(old) != 1:
            raise ValueError(f'Expected exactly one upstream {name} directory')
        new = ('            # 9os: headless native Qt has no optional Quick GUI tools.\n'
               '            if(NOT CMAKE_CROSSCOMPILING)\n'
               f'                add_subdirectory({name})\n'
               '            endif()\n')
        original = original.replace(old, new)
    return original


def apply(path):
    text = path.read_text()
    # Undo only our exact insertion to recognize a prior successful application.
    original = text
    for name in TOOLS:
        new = ('            # 9os: headless native Qt has no optional Quick GUI tools.\n'
               '            if(NOT CMAKE_CROSSCOMPILING)\n'
               f'                add_subdirectory({name})\n'
               '            endif()\n')
        original = original.replace(new, f'            add_subdirectory({name})\n')
    if hashlib.sha256(original.encode()).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Unexpected Qt 6.8.3 tools/CMakeLists.txt; refusing patch')
    result = patched(original)
    if text not in (original, patched(original, LEGACY_TOOLS), result):
        raise ValueError('Partial Qt cross-tools patch; refusing update')
    if text != result:
        path.write_text(result)


if __name__ == '__main__':
    apply(Path(sys.argv[1]) / 'tools/CMakeLists.txt')
