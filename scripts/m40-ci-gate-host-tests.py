#!/usr/bin/env python3
"""Offline drift and structure checks for the temporary M40 CI hold.

No GitHub execution semantics, branch protection, runner provisioning, other
workflows, earlier runs, or guest safety are established by these tests.
"""
import hashlib
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[1]
HASHES = {'alpine': '41703ae2597aeb1e48d5e5b842edf8cf5140164855938419e0299b8475f8d3fd',
          'release': '26cc6afa22148f84378a8eec8eed7fe241510affb235ca626c063a07767d9809'}
GATE = {'name': 'M40 independent review gate (execution blocked)',
        'run': 'echo "::error::M40 execution blocked pending independent review; tests were not run."\nexit 1\n'}

class UniqueLoader(yaml.BaseLoader):
    pass

def mapping(loader, node, deep=False):
    result = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in result: raise ValueError('duplicate YAML key: ' + str(key))
        result[key] = loader.construct_object(value_node, deep=deep)
    return result
UniqueLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, mapping)


def validate(kind, text, *, check_digest=True):
    if kind not in HASHES: raise ValueError('unknown workflow')
    try: doc = yaml.load(text, Loader=UniqueLoader)
    except yaml.YAMLError as e: raise ValueError('invalid YAML') from e
    if not isinstance(doc, dict) or 'on' not in doc: raise ValueError('missing on triggers')
    if doc.get('permissions') != {'contents': 'read'}: raise ValueError('unexpected workflow permissions')
    expected = {'alpine': {'alpine', 'desktop'}, 'release': {'stamp', 'build', 'release'}}[kind]
    jobs = doc.get('jobs')
    if not isinstance(jobs, dict) or set(jobs) != expected: raise ValueError('unexpected job inventory')
    job_keys = {'alpine': {'runs-on','timeout-minutes','strategy','env','steps'},
                'desktop': {'runs-on','timeout-minutes','strategy','env','steps'},
                'stamp': {'runs-on','outputs','steps'},
                'build': {'runs-on','timeout-minutes','strategy','env','steps'},
                'release': {'needs','if','runs-on','steps'}}
    for name, job in jobs.items():
        if set(job) != job_keys[name]: raise ValueError('unexpected job settings: '+name)
        step = dict(GATE)
        if name == 'stamp': step['id'] = 'ts'
        if job.get('steps') != [step]: raise ValueError('non-gate step or modified gate: '+name)
        if name != 'release' and ('if' in job or 'continue-on-error' in job):
            raise ValueError('conditional or bypassable job: '+name)
        if name in ('build','alpine','desktop'):
            if set(job.get('strategy',{})) != {'fail-fast','matrix'} or job['strategy']['fail-fast'] != 'false' or set(job['strategy']['matrix']) != {'arch'}:
                raise ValueError('unexpected matrix settings')
            if job.get('strategy', {}).get('matrix', {}).get('arch') != ['x86_64','aarch64','riscv64'] and name != 'build':
                raise ValueError('unexpected architecture matrix')
            if name == 'build' and job.get('strategy', {}).get('matrix', {}).get('arch') != ['x86_64','riscv64','aarch64']:
                raise ValueError('unexpected architecture matrix')
    if kind == 'release':
        if 'needs' in jobs['build']: raise ValueError('build must not be skipped after stamp failure')
        if jobs['release'].get('if') != '${{ false }}' or jobs['release'].get('needs') != ['stamp', 'build']:
            raise ValueError('release job must be disabled')
    if check_digest and hashlib.sha256(text.encode()).hexdigest() != HASHES[kind]:
        raise ValueError('whole workflow changed: independent review required')


class GateTests(unittest.TestCase):
    def text(self, kind): return (ROOT/'.github/workflows'/f'{kind}.yml').read_text()
    def test_current_workflows(self):
        for kind in HASHES:
            with self.subTest(workflow=kind): validate(kind, self.text(kind))
    def test_duplicate_yaml_keys_refused(self):
        for kind in HASHES:
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                validate(kind, self.text(kind).replace('jobs:\n', 'jobs:\n  injected: {}\njobs:\n',1),check_digest=False)
    def test_extra_actions_status_bypass_and_gate_mutation_refused(self):
        for kind in HASHES:
            text=self.text(kind)
            mutations=[
                text.replace('          exit 1','          exit 0',1),
                text.replace('        run: |','        continue-on-error: true\n        run: |',1),
                text.replace('    steps:\n','    steps:\n      - uses: actions/checkout@v4\n',1),
                text.replace('    steps:\n','    steps:\n      - name: bypass\n        if: always()\n        run: echo unsafe\n',1),
                text.replace('    runs-on: ubuntu-24.04','    if: ${{ false }}\n    runs-on: ubuntu-24.04',1),
                text.replace('jobs:\n','jobs:\n  extra:\n    runs-on: ubuntu-24.04\n    steps: []\n',1),
            ]
            for changed in mutations:
                with self.subTest(kind=kind, change=mutations.index(changed)), self.assertRaises(ValueError):
                    validate(kind,changed,check_digest=False)
    def test_matrix_dependency_and_release_condition_mutations_refused(self):
        for kind in HASHES:
            text=self.text(kind)
            changes=[text.replace('arch: [x86_64,','arch: [other,',1)]
            if kind == 'release': changes += [text.replace('  build:\n','  build:\n    needs: stamp\n',1),text.replace('if: ${{ false }}',"if: github.event_name != 'pull_request'",1)]
            for changed in changes:
                with self.subTest(kind=kind), self.assertRaises(ValueError): validate(kind,changed,check_digest=False)
    def test_job_needs_permissions_and_matrix_include_mutations_refused(self):
        for kind in HASHES:
            text=self.text(kind)
            jobs=('alpine','desktop') if kind == 'alpine' else ('stamp','build')
            for job in jobs:
                for injected in ('    needs: other\n','    permissions: write-all\n'):
                    changed=text.replace('  '+job+':\n','  '+job+':\n'+injected,1)
                    with self.subTest(kind=kind,job=job,injected=injected), self.assertRaises(ValueError):
                        validate(kind,changed,check_digest=False)
            changed=text.replace('      matrix:\n','      matrix:\n        include: []\n',1)
            with self.subTest(kind=kind), self.assertRaises(ValueError): validate(kind,changed,check_digest=False)

    def test_on_is_literal_and_digest_catches_workflow_level_change(self):
        for kind in HASHES:
            text=self.text(kind)
            self.assertIn('on',yaml.load(text,Loader=UniqueLoader))
            with self.assertRaises(ValueError): validate(kind,text+'\n# changed\n')

if __name__ == '__main__': unittest.main()
