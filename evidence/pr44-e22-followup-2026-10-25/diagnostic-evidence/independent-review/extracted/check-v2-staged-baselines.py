from pathlib import Path
import importlib.util,types,unittest,io,json
r=Path('/data/diagnostic-v2-independent-review');out=Path('/data/source-review-e22')
s=importlib.util.spec_from_file_location('staged_tests',r/'scripts/alpine-download-host-tests.py');t=importlib.util.module_from_spec(s);s.loader.exec_module(t)
s1=importlib.util.spec_from_file_location('exact_v1_candidate','/data/diagnostic-independent-review/userland/alpine/build.py');v1=importlib.util.module_from_spec(s1);s1.loader.exec_module(v1)
reports=[]
def run(module,methods,name):
 t.b=module;suite=unittest.TestSuite(t.AlpineDownload(m) for m in methods);buf=io.StringIO();res=unittest.TextTestRunner(stream=buf,verbosity=2).run(suite);(out/name).write_text(buf.getvalue());info={'label':name,'methods':res.testsRun,'assertion_failures':len(res.failures),'errors':len(res.errors)};reports.append(info);return info
x=run(v1,['test_clock_faults_cannot_mask_terminal_network_exception','test_clock_faults_cannot_break_successful_download'],'independent-v2-clocks-against-unguarded-v1.log');assert x['assertion_failures']==10 and x['errors']==0
src=(r/'userland/alpine/build.py').read_text();start=src.index('                # This optional diagnostic must never mask the original error.');end=src.index('                raise\n',start)
clockonly=src[:start]+"                error.add_note(('Download attempts: ' + json.dumps(failures, separators=(',', ':')))[:1024])\n"+src[end:]
module=types.ModuleType('reconstructed_clock_only_review_fixture');module.__file__=str(r/'userland/alpine/build.py');exec(compile(clockonly,'<reconstructed-clock-only>', 'exec'),module.__dict__)
x=run(module,['test_summary_faults_or_oversize_only_omit_optional_note'],'independent-v2-summary-against-reconstructed-clock-only.log');assert x['assertion_failures']==4 and x['errors']==0
(out/'independent-v2-staged-baseline-results.json').write_text(json.dumps({'provenance':'Clock-only variant is explicitly reconstructed isolated control fixture, not a published source snapshot. V1 is exact earlier supplied candidate. No production branch edits.','results':reports},indent=2)+'\n');print(json.dumps(reports))
