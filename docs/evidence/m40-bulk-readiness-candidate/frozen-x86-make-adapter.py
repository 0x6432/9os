#!/usr/bin/env python3
"""Run an existing x86 ISO only; never allow boot harness to rebuild fixtures."""
import os,sys
from pathlib import Path
a=sys.argv[1:];v={x.split('=',1)[0]:x.split('=',1)[1]for x in a if '='in x}
if 'run'not in a or v.get('ARCH')!='x86_64' or v.get('USERLAND')!='alpine':sys.exit('frozen adapter refuses builds/non-x86/non-Alpine')
iso='build/x86_64-alpine/9os.iso'
if not Path(iso).is_file():sys.exit('frozen adapter requires existing ISO')
os.execv('/usr/bin/make',['/usr/bin/make','--old-file='+iso,'--old-file=firmware',*a])
