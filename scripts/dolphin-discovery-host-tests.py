#!/usr/bin/env python3
"""Mock-only controls for bounded introspection; no desktop evidence."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parent.parent
source=(ROOT/'userland/alpine/desktop-overlay/usr/bin/alpine-dolphin-probe').read_text()
function=source[source.index('find_window() {'):source.index('service=;object=;deadline=')]
with tempfile.TemporaryDirectory(prefix='9os-dolphin-discovery-') as tmp:
    d=Path(tmp);b=d/'bin';b.mkdir()
    (b/'timeout').write_text('#!/bin/sh\nshift 3\nexec "$@"\n')
    (b/'dbus-send').write_text('''#!/usr/bin/env python3
import os,sys
p=sys.argv[-2];m=os.environ['MOCK']
if m=='error':sys.exit(1)
if m=='oversize':
 if p=='/':print('<node>\\n'+'\\n'.join('<node name="n%d"/>'%i for i in range(40))+'\\n</node>')
 else:print('<node/>')
elif m=='deep':print('<node>\\n<node name="next"/>\\n</node>')
elif p=='/':print('<node>\\n<node name="MainApplication"/>\\n<node name="dolphin"/>\\n<node name="bad;name"/>\\n</node>')
elif p=='/dolphin':print('<node>\\n<node name="Dolphin_29"/>\\n</node>')
elif p=='/dolphin/Dolphin_29' and m=='valid':print('<node>\\n<interface name="org.kde.dolphin.MainWindow">\\n</interface>\\n</node>')
else:print('<node/>')
''')
    for x in b.iterdir():x.chmod(0o755)
    (d/'test.sh').write_text('set -eu\nD="$1"\n'+function+'\nfind_window org.kde.dolphin-42\n')
    for mode in ['valid','missing','error','oversize','deep']:
        p=subprocess.run(['sh',str(d/'test.sh'),tmp],env={**os.environ,'PATH':str(b)+':'+os.environ['PATH'],'MOCK':mode},capture_output=True,text=True,timeout=5)
        assert p.returncode==(0 if mode=='valid' else 1),(mode,p.stdout,p.stderr)
        assert p.stdout==('/dolphin/Dolphin_29\n' if mode=='valid' else ''),(mode,p.stdout)
        print('PASS',mode)
    print('5/5 bounded-discovery controls (mock only; no GUI claim)')
