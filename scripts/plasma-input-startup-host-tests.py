#!/usr/bin/env python3
"""Execute the actual shell readiness loop against deterministic IPC controls.

No compositor, physical typing, screenshots or kernel validation is claimed.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

PROBE=Path(__file__).resolve().parents[1]/'userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe'
SOURCE=PROBE.read_text()
LOOP=SOURCE[SOURCE.index(' input_ready=0\n'):SOURCE.index('\nfi\nparent=',SOURCE.index(' input_ready=0\n'))]


class Startup(unittest.TestCase):
    def run_loop(self, mode, pid=None, original=False):
        with tempfile.TemporaryDirectory() as directory:
            d=Path(directory)
            script=d/'swaymsg'
            script.write_text('''#!/bin/sh
n=0;[ ! -f "$D/count" ] || n=$(cat "$D/count")
n=$((n+1));echo "$n" > "$D/count"
case "$MODE:$n" in
 fail-first:1) exit 7 ;;
 empty-first:1) echo '[]';exit 0 ;;
 never:*) echo '[]';exit 0 ;;
esac
echo '[{"type":"keyboard"}]'
''')
            script.chmod(0o755)
            loop=LOOP
            if original:
                first=loop.index('  # Socket creation')
                last=loop.index('  sleep .1',first)
                loop=loop[:first]+'''  swaymsg -t get_inputs > "$D/inputs.json"
  if jq -e '[.[]|select(.type=="keyboard")]|length>0' "$D/inputs.json" >/dev/null;then input_ready=1;break;fi
'''+loop[last:]
            body='set -eu\nswaypid=${TARGET_PID:-$$}\nsleep() { :; }\n'+loop
            env=dict(os.environ,D=directory,MODE=mode,PATH=directory+':'+os.environ['PATH'])
            if pid:env['TARGET_PID']=str(pid)
            result=subprocess.run(['/bin/sh','-c',body],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=10)
            return result,int((d/'count').read_text())

    def test_transient_ipc_failure_remains_not_ready(self):
        result,count=self.run_loop('fail-first');self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(count,2)
    def test_empty_input_poll_still_requires_actual_keyboard(self):
        result,count=self.run_loop('empty-first');self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(count,2)
    def test_missing_keyboard_fails_original_bound(self):
        result,count=self.run_loop('never');self.assertEqual(result.returncode,1);self.assertEqual(count,100);self.assertIn('FAIL no physical keyboard',result.stdout)
    def test_dead_compositor_stops_without_false_ready(self):
        result,count=self.run_loop('never',99999999);self.assertEqual(result.returncode,1);self.assertEqual(count,1)
    def test_original_set_e_aborts_first_failed_ipc(self):
        result,count=self.run_loop('fail-first',original=True);self.assertEqual(result.returncode,7);self.assertEqual(count,1)


if __name__=='__main__':unittest.main()
