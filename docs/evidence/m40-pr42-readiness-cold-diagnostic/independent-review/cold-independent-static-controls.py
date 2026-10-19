"""Read-only cohort structure checks; NEVER invokes prepare/run/VM/CI."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,shlex,subprocess,yaml
root=Path('/data/9os-pr42-cold-validation')
old=Path('/data/9os-epoll-leaf-validation')
base='07fda66bd9a7904e77b7f3e1051df243ee1e4d21'
held='ad39a795cae2ade0083d451cdaf9edea3735ae33'
for name in ['scripts/m40-cold-driver.sh','scripts/m40-cold-run.py']:
 content=(root/name).read_bytes()
 assert content==(old/name).read_bytes()
 assert content==subprocess.check_output(['git','-C',str(old),'show',held+':'+name])
 print('BYTE-IDENTICAL held driver/evaluator:',name,hashlib.sha256(content).hexdigest())
assert subprocess.check_output(['git','-C',str(root),'diff',base,'--','kernel','userland','scripts/qemu-test.py','scripts/qemu-ready-type.py'])==b''
print('Pinned runtime paths match',base)
text=(root/'scripts/m40-cold-driver.sh').read_text()
command=shlex.split(text[text.index('python3 scripts/qemu-test.py'):].replace('\\\n',''))
ap=argparse.ArgumentParser()
ap.add_argument('arch');ap.add_argument('cmds',nargs='*')
for option in ['--userland','--memory','--disk','--cmdline','--boot-timeout','--timeout','--log']:
 ap.add_argument(option)
a=ap.parse_intermixed_args(command[2:])
# shlex parses the literal shell source; it must not invent expansion of $ARCH.
assert a.arch=='$ARCH' and a.memory=='3072M' and a.boot_timeout=='300' and a.timeout=='780'
assert '[ "$ARCH" = x86_64 ] ||' in text
assert "['scripts/m40-cold-driver.sh','x86_64']" in (root/'scripts/m40-cold-run.py').read_text()
assert len(a.cmds)==17
assert a.cmds[-1]=='grep -q "^violations 0" /proc/lockdep'
print('Exact gate count: one boot +',len(a.cmds),'commands = 18')
for i,cmd in enumerate(a.cmds,1): print(f'gate {i:02d}: {cmd}')
workflow=yaml.load((root/'.github/workflows/alpine.yml').read_text(),Loader=yaml.BaseLoader)
assert set(workflow['on'])=={'workflow_dispatch'}
assert workflow['permissions']=={'contents':'read'}
job=workflow['jobs']['fixed-x86-plan'];steps=job['steps']
assert job['runs-on']=='ubuntu-24.04' and job['timeout-minutes']=='120'
guests=[s for s in steps if s.get('run','').startswith('python3 scripts/m40-cold-run.py guest')]
assert len(guests)==3
condition="${{ !cancelled() && steps.linux.outcome == 'success' }}"
for n,step in enumerate(guests,1):
 assert step['run']=='python3 scripts/m40-cold-run.py guest'+str(n)
 assert step['if']==condition and step['timeout-minutes']=='25'
 assert 'continue-on-error' not in step
linux=[s for s in steps if s.get('id')=='linux'];assert len(linux)==1
assert linux[0]['run']=='python3 scripts/m40-cold-run.py linux1'
assert 'if' not in linux[0]
assert sum(s.get('if')=='always()' for s in steps)==5
print('One Linux sample + exactly three independent !cancelled/Linux-success guest steps; 5 always-upload steps')
for first_guest_failed in [False,True]:
 for second_guest_failed in [False,True]:
  # Neither prior-guest outcome is referenced in the literal checked condition.
  assert all('guest' not in s['if'] for s in guests)
print('Guest conditions do not depend on prior guest success; no continue-on-error waiver')
for runid,expected in [('39168427970',6),('39168428016',3)]:
 saved=json.loads(Path('/data/pr42-current/'+runid+'.json').read_text())
 assert saved['run']['head_sha']==base
 assert saved['run']['conclusion']=='success' and saved['run']['status']=='completed'
 jobs=[j for j in saved['jobs'] if j.get('name') not in ['stamp','release']]
 assert len(jobs)==expected and all(j['conclusion']=='success' for j in jobs)
 print('Saved prerequisite metadata:',runid,'current head, completed/success, relevant jobs',len(jobs))
for name in ['.github/workflows/alpine.yml','docs/M40_COLD_EXPERIMENT.md','scripts/m40-cold-prepare.py','scripts/m40-cold-host-tests.py']:
 print('Reviewed plan SHA256:',name,hashlib.sha256((root/name).read_bytes()).hexdigest())
assert not (root/'build/m40-cold').exists()
print('No prepared cohort fixture directory exists here. No prepare/run/dispatch performed by this control.')
