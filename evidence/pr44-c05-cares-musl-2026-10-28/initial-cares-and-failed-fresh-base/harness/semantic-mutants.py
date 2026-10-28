from pathlib import Path
import unittest,io,json
r=Path('/data/cares-maintenance');p=r/'product/scripts/alpine-host-tests.py';s=p.read_text();guard="            self.assertEqual(len(records), 1, 'Expected exactly one c-ares package')"
mutants={
 'missing_allowed':(guard,"            if not records: continue\n"+guard),
 'duplicate_allowed':(guard,"            self.assertGreaterEqual(len(records), 1, 'Expected a c-ares package')"),
 'url_check_removed':("            self.assertEqual(records[0]['url'], expected_url)","            pass  # mutation: URL check removed"),
 'sha_check_removed':("            self.assertEqual(records[0]['sha256'], expected_sha)","            pass  # mutation: SHA check removed")}
results=[]
for name,(old,new)in mutants.items():
 assert s.count(old)==1;code=compile(s.replace(old,new),str(p),'exec');ns={'__name__':'carestest_mutant','__file__':str(p)};exec(code,ns)
 suite=unittest.defaultTestLoader.loadTestsFromTestCase(ns['TrustTests']);out=io.StringIO();res=unittest.TextTestRunner(stream=out,verbosity=2).run(suite)
 (r/'evidence'/('mutant-'+name+'.log')).write_text(out.getvalue());d={'name':name,'tests':res.testsRun,'assertion_failures':len(res.failures),'errors':len(res.errors),'killed_semantically':bool(res.failures)and not res.errors};results.append(d);assert d['killed_semantically'],d
(r/'evidence/semantic-mutants.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
