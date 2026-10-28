from pathlib import Path
import unittest,io,json
r=Path('/data/cares-maintenance');p=r/'product/scripts/alpine-host-tests.py';s=p.read_text();e=r/'expanded-evidence';mutants={}
for kind,guard,url,sha in [('cares',"            self.assertEqual(len(records), 1, 'Expected exactly one c-ares package')","            self.assertEqual(records[0]['url'], expected_url)","            self.assertEqual(records[0]['sha256'], expected_sha)"),('musl',"            self.assertEqual(len(records), 1, 'Expected exactly one musl pair member')","            self.assertEqual(records[0]['url'], m.BASE + '/v3.22/main/x86_64/' + name + '-1.2.5-r13.apk')","            self.assertEqual(records[0]['sha256'], sha)")]:
 mutants[kind+'-missing']=(guard,'            if not records: continue\n'+guard)
 mutants[kind+'-duplicate']=(guard,'            self.assertGreaterEqual(len(records), 1)')
 mutants[kind+'-url']=(url,'            pass # mutant URL removed')
 mutants[kind+'-sha']=(sha,'            pass # mutant SHA removed')
results=[]
for name,(a,b) in mutants.items():
 assert s.count(a)==1
 code=compile(s.replace(a,b),str(p),'exec');ns={'__name__':'mutant','__file__':str(p)};exec(code,ns)
 out=io.StringIO();res=unittest.TextTestRunner(stream=out,verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ns['TrustTests']))
 (e/('mutant-'+name+'.log')).write_text(out.getvalue());v={'name':name,'tests':res.testsRun,'failures':len(res.failures),'errors':len(res.errors)};results.append(v);assert res.failures and not res.errors,v
(e/'semantic-mutants.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results))
