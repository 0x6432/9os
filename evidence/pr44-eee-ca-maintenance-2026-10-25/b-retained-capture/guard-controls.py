import pathlib,json,unittest,io,hashlib
r=pathlib.Path('/data/ca-maintenance-product');e=pathlib.Path(__file__).parent;f=r/'scripts/alpine-host-tests.py';src=f.read_text();ns={'__file__':str(f),'__name__':'ca_guard_review'};exec(compile(src,str(f),'exec'),ns);cls=ns['TrustTests'];case=cls();ref=json.loads((r/'userland/ports/linux-oracle-lock.json').read_text());baseline=[]
for name in ['userland/alpine/locks/x86_64.json','userland/alpine/locks/x86_64-desktop.json','userland/ports/host-qsb-lock.json']:
 old=json.loads((e/'before'/name.replace('/','_')).read_text())
 try:case.assert_certificate_locks([old],ref)
 except AssertionError as error:baseline.append({'path':name,'semantic_assertion':str(error)})
 else:raise RuntimeError('old pin wrongly accepted')
(e/'baseline-each-old-lock-semantic-rejections.json').write_text(json.dumps(baseline,indent=2)+'\n')
mutants={'ignore_url':('            self.assertEqual(url, expected_url)','            pass  # mutant ignores URL'),'ignore_sha':('            self.assertEqual(sha, expected_sha)','            pass  # mutant ignores SHA'),'accept_duplicate':("        self.assertEqual(len(records), 1, 'Expected exactly one certificate bundle')","        self.assertGreaterEqual(len(records), 1, 'mutant permits duplicates')")};out=[]
for name,(old,new)in mutants.items():
 assert src.count(old)==1
 changed=src.replace(old,new);scope={'__file__':str(f),'__name__':'mutant_'+name}
 # Compilation, module setup/import and test construction are OUTSIDE result assertions.
 exec(compile(changed,str(f),'exec'),scope);suite=unittest.TestSuite([scope['TrustTests']('test_certificate_guard_rejects_url_hash_missing_and_duplicate')]);stream=io.StringIO();result=unittest.TextTestRunner(stream=stream).run(suite);(e/('mutant-'+name+'.log')).write_text(stream.getvalue());assert result.failures and not result.errors
 out.append({'mutant':name,'testsRun':result.testsRun,'assertion_failures':len(result.failures),'errors':len(result.errors),'source_sha256':hashlib.sha256(changed.encode()).hexdigest()})
(e/'guard-semantic-mutants.json').write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(out,indent=2))
# Exact scope and data-only change validation.
for name in ['userland/alpine/locks/x86_64.json','userland/alpine/locks/x86_64-desktop.json','userland/ports/host-qsb-lock.json']:
 before=(e/'before'/name.replace('/','_')).read_text();after=(r/name).read_text();assert before.replace('ca-certificates-bundle-20260909-r0.apk','ca-certificates-bundle-20261020-r0.apk').replace('9cbf2fd3bcbad4a73d6e2c5a2d5cfea176ce01790d8ae8bb1796ba4e64d6c48b','d35216a0a9adde79031df246759bad03f235cf201bf0aff8436b88bff59594eb')==after
for name in ['userland/alpine/build.py','userland/ports/linux-oracle-lock.json']:
 assert (e/'before'/name.replace('/','_')).read_bytes()==(r/name).read_bytes()
print('exact_three_pair_only_changes_and_unchanged_builder_reference_PASS')
