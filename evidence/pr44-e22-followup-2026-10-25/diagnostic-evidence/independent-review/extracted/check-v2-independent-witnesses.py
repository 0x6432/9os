from pathlib import Path
import importlib.util,hashlib,tempfile,json,io,ast,contextlib
from unittest import mock
r=Path('/data/diagnostic-v2-independent-review');orig=Path('/data/source-review-e22/originals/build.py').read_text();new=(r/'userland/alpine/build.py').read_text()
# Outside download is byte-identical; original acquisition try-body remains exact AST.
a=orig.index('def download(');z=orig.index('def validate_entries(');aa=new.index('def download(');zz=new.index('def validate_entries(');assert orig[:a]==new[:aa] and orig[z:]==new[zz:]
oldfn=next(n for n in ast.parse(orig).body if isinstance(n,ast.FunctionDef) and n.name=='download');newfn=next(n for n in ast.parse(new).body if isinstance(n,ast.FunctionDef) and n.name=='download')
oldfor=next(n for n in oldfn.body if isinstance(n,ast.For));newfor=next(n for n in newfn.body if isinstance(n,ast.For));assert ast.dump(oldfor.iter)==ast.dump(newfor.iter)
oldtry=next(n for n in oldfor.body if isinstance(n,ast.Try));newtry=next(n for n in newfor.body if isinstance(n,ast.Try) and any(isinstance(x,ast.With) for x in n.body));assert [ast.dump(x) for x in oldtry.body]==[ast.dump(x) for x in newtry.body]
assert [ast.dump(x) for x in oldfn.body[:2]]==[ast.dump(x) for x in newfn.body[:2]]
s=importlib.util.spec_from_file_location('v2',r/'userland/alpine/build.py');b=importlib.util.module_from_spec(s);s.loader.exec_module(b)
u='https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/ca-certificates-bundle-20260909-r0.apk';payload=b'correct';sha=hashlib.sha256(payload).hexdigest();results=[]
with tempfile.TemporaryDirectory() as td:
 p=Path(td)/'test.apk'
 with mock.patch.object(b.urllib.request,'urlopen',return_value=io.BytesIO(payload)) as opened,mock.patch.object(b.time,'time',side_effect=OSError('clock unavailable')):
  b.download(u,p,sha);assert opened.call_count==1;assert p.read_bytes()==payload
 results.append({'witness':'identical start-clock-failure now preserves successful download','pass':True})
 p.write_bytes(b'old');error=b.urllib.error.HTTPError(u,404,'Not Found',{},None)
 with mock.patch.object(b.urllib.request,'urlopen',side_effect=error) as opened,mock.patch.object(b.time,'monotonic',side_effect=[1,OSError('diagnostic end clock failed')]),mock.patch.object(b.time,'sleep') as slept:
  try:b.download(u,p,sha)
  except Exception as ex:assert ex is error
  else:raise AssertionError('network failure hidden')
  assert opened.call_args_list==[mock.call(u,timeout=60)]*3;assert slept.call_args_list==[mock.call(1),mock.call(2)]
 results.append({'witness':'identical end-clock-failure preserves terminal HTTP404 and all retries','pass':True})
 error=b.urllib.error.HTTPError(u,503,'Unavailable',{},None)
 with mock.patch.object(b.urllib.request,'urlopen',side_effect=error) as opened,mock.patch.object(b.time,'sleep') as slept,mock.patch.object(b.json,'dumps',side_effect=RuntimeError('diagnostic serializer failed')):
  try:b.download(u,p,sha)
  except Exception as ex:assert ex is error
  else:raise AssertionError('network failure hidden')
  assert opened.call_count==3;assert slept.call_args_list==[mock.call(1),mock.call(2)];assert error.__notes__==['Download failed: '+u]
 results.append({'witness':'identical serialization failure preserves terminal HTTP503, URL-only note','pass':True})
 class NoteFails(b.urllib.error.HTTPError):
  def add_note(self,n):
   if n.startswith('Download attempts: '):raise RuntimeError('attachment failed')
   super().add_note(n)
 for case in ['attachment','multibyte-oversize','nonfinite']:
  error=(NoteFails if case=='attachment' else b.urllib.error.HTTPError)(u,503,'secret',{},None)
  with contextlib.ExitStack() as stack:
   if case=='multibyte-oversize':stack.enter_context(mock.patch.object(b.json,'dumps',return_value='雪'*400))
   if case=='nonfinite':stack.enter_context(mock.patch.object(b.time,'monotonic',return_value=float('nan')))
   opened=stack.enter_context(mock.patch.object(b.urllib.request,'urlopen',side_effect=error));slept=stack.enter_context(mock.patch.object(b.time,'sleep'))
   out=stack.enter_context(contextlib.redirect_stdout(io.StringIO()));err=stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
   try:b.download(u,p,sha)
   except Exception as ex:assert ex is error
   else:raise AssertionError('network failure hidden')
   assert error.__notes__==['Download failed: '+u];assert opened.call_count==3;assert slept.call_args_list==[mock.call(1),mock.call(2)];assert out.getvalue()==err.getvalue()==''
   assert p.read_bytes()==b'old';assert not p.with_suffix('.apk.part').exists()
  results.append({'witness':case+' preserves original terminal error, URL-only note, retry/cache/cleanup/quiet','pass':True})
report={'outside_download_bytes_identical':True,'original_fetch_read_hash_replace_body_ast_identical':True,'original_cache_offline_ast_identical':True,'retry_iterator_ast_identical':True,'independent_witnesses':results}
Path('/data/source-review-e22/independent-v2-witnesses.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
