from pathlib import Path
import json,subprocess,hashlib
r=Path('/data/cares-maintenance');p=r/'product';e=r/'expanded-evidence';e.mkdir(exist_ok=False)
t=p/'scripts/alpine-host-tests.py';old=t.read_text();(e/'before-expansion-host-tests.py.txt').write_text(old)
insert='''    def assert_musl_pair(self, lock):
        expected = {
            'musl': '359890bc7c4dc5fee7a85192de60a471c987e4664767288578beca60a44db2c8',
            'musl-dev': '2ca496dd68c0b340b50cf9940bc2f3312eac8b56ec2d3284a621be6f0c09ffb1',
        }
        for name, sha in expected.items():
            records = [entry for entry in lock['packages']
                       if entry['url'].rsplit('/', 1)[-1].rsplit('-', 2)[0] == name]
            self.assertEqual(len(records), 1, 'Expected exactly one musl pair member')
            self.assertEqual(records[0]['url'], m.BASE + '/v3.22/main/x86_64/' + name + '-1.2.5-r13.apk')
            self.assertEqual(records[0]['sha256'], sha)

    def test_base_desktop_musl_pair_matches_reviewed_candidate(self):
        top = p.parents[2]
        for name in ['x86_64.json', 'x86_64-desktop.json']:
            with self.subTest(profile=name):
                self.assert_musl_pair(json.loads((top / 'userland/alpine/locks' / name).read_text()))

    def test_musl_pair_guard_rejects_missing_duplicate_url_hash_and_mixed_revision(self):
        good = {'packages': [
            {'url': m.BASE + '/v3.22/main/x86_64/musl-1.2.5-r13.apk',
             'sha256': '359890bc7c4dc5fee7a85192de60a471c987e4664767288578beca60a44db2c8'},
            {'url': m.BASE + '/v3.22/main/x86_64/musl-dev-1.2.5-r13.apk',
             'sha256': '2ca496dd68c0b340b50cf9940bc2f3312eac8b56ec2d3284a621be6f0c09ffb1'}]}
        self.assert_musl_pair(good)
        for member in range(2):
            for fault in ['missing', 'duplicate', 'url', 'sha', 'mixed_revision']:
                with self.subTest(member=member, fault=fault):
                    bad = copy.deepcopy(good)
                    if fault == 'missing': bad['packages'].pop(member)
                    elif fault == 'duplicate': bad['packages'].append(dict(bad['packages'][member]))
                    elif fault == 'url': bad['packages'][member]['url'] = bad['packages'][member]['url'].replace('/main/', '/community/')
                    elif fault == 'sha': bad['packages'][member]['sha256'] = '0' * 64
                    else: bad['packages'][member]['url'] = bad['packages'][member]['url'].replace('-r13.apk', '-r12.apk')
                    with self.assertRaises(AssertionError): self.assert_musl_pair(bad)

'''
t.write_text(old.replace('    def certificate_pin(self, lock):',insert+'    def certificate_pin(self, lock):'))
x=subprocess.run(['python3',str(t)],capture_output=True);(e/'baseline.stdout').write_bytes(x.stdout);(e/'baseline.stderr').write_bytes(x.stderr);assert x.returncode==1
assert b'FAILED (failures=2)' in x.stderr and b'errors=' not in x.stderr,x.stderr
for name in ['x86_64.json','x86_64-desktop.json']:
 f=p/'userland/alpine/locks'/name;raw=f.read_text();(e/('before-'+name)).write_text(raw)
 for pkg,sha in [('musl','359890bc7c4dc5fee7a85192de60a471c987e4664767288578beca60a44db2c8'),('musl-dev','2ca496dd68c0b340b50cf9940bc2f3312eac8b56ec2d3284a621be6f0c09ffb1')]:
  o=json.loads(raw);entry=next(a for a in o['packages'] if a['url'].endswith('/'+pkg+'-1.2.5-r12.apk'));raw=raw.replace(entry['url'],entry['url'].replace('-r12.apk','-r13.apk')).replace(entry['sha256'],sha)
 f.write_text(raw)
for name in ['alpine-host-tests.py','alpine-download-host-tests.py','alpine-host-qsb-tests.py','evdev-linux-oracle-packages-host-tests.py']:
 f=p/'scripts'/name
 if not f.exists() and name=='alpine-host-qsb-tests.py':
  f=p/'scripts/host-qsb-host-tests.py'
 x=subprocess.run(['python3',str(f)],capture_output=True);(e/(name+'.stdout')).write_bytes(x.stdout);(e/(name+'.stderr')).write_bytes(x.stderr);print(name,x.returncode,x.stderr.decode()[-130:]);assert x.returncode==0
