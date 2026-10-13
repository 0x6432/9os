#!/usr/bin/env python3
"""Pin Mesa 24.3 ORC to LLVM 18's no-RTTI-compatible simple compiler."""
import pathlib
import sys
p = pathlib.Path(sys.argv[1]) / 'src/gallium/auxiliary/gallivm/lp_bld_init_orc.cpp'
s = p.read_text()
changes = {
    'auto &sc = dynamic_cast<llvm::orc::SimpleCompiler &>(irc);':
    'auto &sc = static_cast<llvm::orc::SimpleCompiler &>(irc);',
    'LLJITBuilder()\n         .setJITTargetMachineBuilder':
    'LLJITBuilder()\n         .setNumCompileThreads(0)\n         .setJITTargetMachineBuilder',
}
for old, new in changes.items():
    if s.count(old) == 1:
        s = s.replace(old, new)
    elif s.count(new) != 1:
        sys.exit(f'mesa ORC no-RTTI patch: unexpected source near {old!r}')
p.write_text(s)
