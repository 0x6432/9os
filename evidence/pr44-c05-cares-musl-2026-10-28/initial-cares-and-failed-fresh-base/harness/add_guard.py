from pathlib import Path
import hashlib,json
r=Path('/data/cares-maintenance');p=r/'product';paths=['scripts/alpine-host-tests.py','userland/alpine/locks/x86_64.json','userland/alpine/locks/x86_64-desktop.json','userland/ports/host-qsb-lock.json','userland/alpine/build.py']
orig={n:{'size':(p/n).stat().st_size,'sha256':hashlib.sha256((p/n).read_bytes()).hexdigest()}for n in paths}
(r/'evidence/before-inputs.json').write_text(json.dumps({'base':'c05a3fcd851297161f3981b38dd04900c49e8467','files':orig},indent=2)+'\n')
(r/'evidence/alpine-host-tests.before.py').write_bytes((p/paths[0]).read_bytes())
text='''    def assert_cares_locks(self, locks):
        expected_url = m.BASE + '/v3.22/main/x86_64/c-ares-1.34.9-r0.apk'
        expected_sha = '9b23aa07901ed64bde0be7ec544f8d2b29de80d996277dc861218c54fff8bc97'
        for lock in locks:
            records = [entry for entry in lock['packages']
                       if entry['url'].rsplit('/', 1)[-1].startswith('c-ares-')]
            self.assertEqual(len(records), 1, 'Expected exactly one c-ares package')
            self.assertEqual(records[0]['url'], expected_url)
            self.assertEqual(records[0]['sha256'], expected_sha)

    def test_x86_cares_locks_match_reviewed_candidate(self):
        top = p.parents[2]
        paths = ['userland/alpine/locks/x86_64.json',
                 'userland/alpine/locks/x86_64-desktop.json',
                 'userland/ports/host-qsb-lock.json']
        for name in paths:
            with self.subTest(path=name):
                self.assert_cares_locks([json.loads((top / name).read_text())])

    def test_cares_guard_rejects_url_hash_missing_and_duplicate(self):
        entry = {'url': m.BASE + '/v3.22/main/x86_64/c-ares-1.34.9-r0.apk',
                 'sha256': '9b23aa07901ed64bde0be7ec544f8d2b29de80d996277dc861218c54fff8bc97'}
        good = {'packages': [dict(entry)]}
        self.assert_cares_locks([good] * 3)
        bad_sets = [[], [dict(entry), dict(entry)],
                    [dict(entry, url=entry['url'].replace('/main/', '/community/'))],
                    [dict(entry, sha256='0' * 64)]]
        for position in range(3):
            for bad in bad_sets:
                with self.subTest(position=position, bad=bad):
                    locks = copy.deepcopy([good] * 3)
                    locks[position] = {'packages': bad}
                    with self.assertRaises(AssertionError):
                        self.assert_cares_locks(locks)

'''
f=p/'scripts/alpine-host-tests.py';s=f.read_text();marker='    def certificate_pin(self, lock):\n';assert s.count(marker)==1;f.write_text(s.replace(marker,text+marker));print('guard added; production pins remain old')
