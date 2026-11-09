#!/usr/bin/env python3
"""Static host controls for temporary draft CI guest-execution gates."""
import pathlib, re, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
NAME='M40 independent review gate (guest runs blocked)'
EXPECTED='''      - name: M40 independent review gate (guest runs blocked)
        run: |
          echo "::error::M40 guest execution blocked pending independent fixture, security and launcher review; guest tests are not run."
          exit 1
'''
def verify(text,job,first_guest):
 match=re.search(r'^  '+re.escape(job)+r':\s*$',text,re.M)
 if not match:raise ValueError('missing job')
 end=re.search(r'^  [A-Za-z0-9_-]+:\s*$',text[match.end():],re.M)
 section=text[match.end():match.end()+end.start()] if end else text[match.end():]
 if section.count('      - name: '+NAME)!=1 or section.count(EXPECTED)!=1:raise ValueError('missing or modified gate')
 # Reject additional keys such as if / continue-on-error within the gate step.
 start=section.index(EXPECTED); after=section[start+len(EXPECTED):]
 next_step=re.search(r'^      - ',after,re.M)
 remainder=after[:next_step.start()] if next_step else after
 if re.search(r'^        (if|continue-on-error):',remainder,re.M):raise ValueError('conditional or bypassed gate')
 first='      - name: '+first_guest
 if section.count(first)!=1 or start>=section.index(first):raise ValueError('gate does not precede guest')

class Gates(unittest.TestCase):
 def test_release(self):verify((ROOT/'.github/workflows/release.yml').read_text(),'build','Boot tests in QEMU')
 def test_base(self):verify((ROOT/'.github/workflows/alpine.yml').read_text(),'alpine','Exact input consumer on signed x86 Linux reference kernel')
 def test_desktop(self):verify((ROOT/'.github/workflows/alpine.yml').read_text(),'desktop','Real EGL Quick pixels, KWin, Konsole and Plasma on disk root')
 def test_mutations_rejected(self):
  text=(ROOT/'.github/workflows/release.yml').read_text()
  for modified in (text.replace(EXPECTED,''),text.replace('          exit 1','          exit 0'),text.replace('          exit 1\n','          exit 1\n        continue-on-error: true\n')):
   with self.subTest(mutation=modified!=text),self.assertRaises(ValueError):verify(modified,'build','Boot tests in QEMU')
if __name__=='__main__':unittest.main()
