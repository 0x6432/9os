from pathlib import Path
p=Path('/data/linux-oracle-cache-product/scripts/evdev-linux-oracle-packages-host-tests.py');s=p.read_text();s=s.replace('import subprocess\n','import subprocess\nimport sys\n',1)
needle='    def test_real_lock_exact_identity_inventory(self):\n'
new='''    def test_cli_default_cache_spaces_unset_and_explicit_override(self):
        with tempfile.TemporaryDirectory(prefix='oracle-cli-') as tmp:
            root = Path(tmp)
            cases = [({'TOOLS_DIR': str(root / 'tools with spaces')}, [],
                      root / 'tools with spaces/linux-oracle'),
                     ({}, [], ROOT / '.tools/linux-oracle'),
                     ({'TOOLS_DIR': str(root / 'unused')}, ['--cache', str(root / 'explicit cache')],
                      root / 'explicit cache')]
            for env, extra, expected in cases:
                with self.subTest(env=env, extra=extra):
                    target = root / 'target'
                    manifest = root / 'manifest.json'
                    argv = [str(HELPER), '--target', str(target), '--manifest', str(manifest),
                            '--offline', *extra]
                    with patch.dict(os.environ, env, clear=True), patch.object(sys, 'argv', argv), \\
                         patch.object(self.m, 'assemble', return_value={'cli_probe': True}) as assemble:
                        self.m.main()
                    assemble.assert_called_once_with(LOCK, expected, target, True)
                    self.assertEqual(json.loads(manifest.read_text()), {'cli_probe': True})
                    manifest.unlink()

    def cache_permission_child(self, selected):
        tmp = tempfile.TemporaryDirectory(prefix='oracle-permission-')
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        tools = root / 'tools with spaces'
        tools.mkdir()
        dl = tools / 'dl'
        dl.mkdir()
        target = root / 'target'
        target.mkdir()
        denied = root / 'denied'
        denied.mkdir()
        uid = os.geteuid() if os.geteuid() != 0 else 65534
        gid = os.getegid() if os.geteuid() != 0 else 65534
        if os.geteuid() == 0:
            for path in [root, tools, target, denied]:
                os.chown(path, uid, gid)  # Fixture paths ONLY; dl remains root-owned.
        dl.chmod(0o555)
        if selected == 'denied-default':
            tools.chmod(0o555)
        if selected == 'denied-override':
            denied.chmod(0o555)
        self.addCleanup(lambda: [path.chmod(0o700) for path in [tools, dl, denied]])
        manifest = root / 'manifest.json'
        trace = root / 'bootstrap-called.json'
        extra = []
        if selected == 'old-default':
            extra = ['--cache', str(dl / 'alpine-linux-oracle')]
        elif selected == 'denied-override':
            extra = ['--cache', str(denied / 'cache')]
        child = r"""
import importlib.util,json,os,pathlib,sys
helper,tools,target,manifest,trace=sys.argv[1:6]
print(json.dumps({'euid':os.geteuid(),'egid':os.getegid(),'tools':tools,
                  'dl_uid':pathlib.Path(tools,'dl').stat().st_uid,
                  'dl_mode':oct(pathlib.Path(tools,'dl').stat().st_mode & 0o777)}),flush=True)
assert os.geteuid()!=0 and os.getegid()!=0
spec=importlib.util.spec_from_file_location('oracle_permission_probe',helper)
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class BoundaryReached(Exception):pass
def boundary(lock,cache,work,offline,runner):
    pathlib.Path(trace).write_text(json.dumps({'selected_cache':str(cache),'euid':os.geteuid()}))
    raise BoundaryReached()
def forbidden(*args,**kwargs):
    raise AssertionError('Unexpected download/native command; fixture must stop at cache boundary')
m.bootstrap=boundary;m.BUILD.download=forbidden;m.subprocess.run=forbidden
sys.argv=[helper,'--target',target,'--manifest',manifest,'--offline',*sys.argv[6:]]
try:m.main()
except BoundaryReached:print('CACHE_BOUNDARY_REACHED')
"""
        def drop_fixture_identity():
            os.setgroups([])
            os.setgid(gid)
            os.setuid(uid)
        result = subprocess.run([sys.executable, '-c', child, str(HELPER), str(tools),
                                 str(target), str(manifest), str(trace), *extra],
                                env={**os.environ, 'TOOLS_DIR': str(tools), 'PYTHONDONTWRITEBYTECODE': '1'},
                                capture_output=True, text=True,
                                preexec_fn=drop_fixture_identity if os.geteuid() == 0 else None)
        # A setup/import failure cannot count as an expected permission rejection.
        observation = json.loads(result.stdout.splitlines()[0])
        self.assertNotEqual(observation['euid'], 0)
        self.assertNotEqual(observation['egid'], 0)
        self.assertFalse(manifest.exists())
        self.assertEqual(list(target.iterdir()), [])
        self.assertFalse((root / 'qemu-called').exists())
        print('Actual non-root cache fixture:', selected, json.dumps(observation),
              'rc=' + str(result.returncode), flush=True)
        return result, tools, trace

    def test_actual_nonroot_cache_default_avoids_readonly_dl(self):
        result, tools, trace = self.cache_permission_child('new-default')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        self.assertTrue((tools / 'linux-oracle').is_dir())
        self.assertEqual(json.loads(trace.read_text())['selected_cache'], str(tools / 'linux-oracle'))
        old, tools, trace = self.cache_permission_child('old-default')
        self.assertEqual(old.returncode, 1)
        self.assertIn('PermissionError: [Errno 13]', old.stderr)
        self.assertFalse(trace.exists())
        self.assertFalse((tools / 'linux-oracle').exists())

    def test_denied_selected_cache_fatal_without_fallback_or_bootstrap(self):
        for selected in ['denied-default', 'denied-override']:
            with self.subTest(selected=selected):
                result, tools, trace = self.cache_permission_child(selected)
                self.assertEqual(result.returncode, 1)
                self.assertIn('PermissionError: [Errno 13]', result.stderr)
                self.assertFalse(trace.exists())
                self.assertFalse((tools / 'linux-oracle').exists())

'''
assert s.count(needle)==1;s=s.replace(needle,new+needle);p.write_text(s)
