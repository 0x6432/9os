from pathlib import Path
import re,struct,json,hashlib,subprocess
root=Path('/data/matched-elf');out=Path('/data/pr40-pc-attribution/results');out.mkdir(exist_ok=True)
paths={'/usr/lib/libQt6Core.so.6':'usr/lib/libQt6Core.so.6.8.3','/usr/lib/libkonsoleprivate.so.25.04.2':'usr/lib/libkonsoleprivate.so.25.04.2','/usr/lib/libicuuc.so.76':'usr/lib/libicuuc.so.76.1','/usr/lib/qt6/plugins/styles/breeze6.so':'usr/lib/qt6/plugins/styles/breeze6.so','/lib/ld-musl-x86_64.so.1':'lib/ld-musl-x86_64.so.1'}
results=[]
for guest in ['guest1']:
 text=Path(f'/data/pr40-guest1-review/{guest}.out').read_text()
 for sample,body in re.findall(r'KONSOLE_FAILURE_SNAPSHOT_BEGIN sample=(\d+).*?\n(.*?)KONSOLE_FAILURE_SNAPSHOT_END',text,re.S):
  assert 'selected=2 pc=1 sp=1 truncated=0 error=0 malformed=0'in body
  assert 'TRACE_SNAPSHOT_DETACHED signal=0'in body and 'TRACE_SNAPSHOT_END result=0'in body
  pc=int(re.search(r'TRACE_SNAPSHOT_REGS pc=([0-9a-f]+)',body)[1],16)
  maps=re.findall(r'^MAP ([0-9a-f]+)-([0-9a-f]+) (....) ([0-9a-f]+) \S+ \d+ (.*)$',body,re.M)
  m=[m for m in maps if int(m[0],16)<=pc<int(m[1],16)];assert len(m)==1
  start,end,perm,offset,name=m[0];assert 'x'in perm
  member=paths[name];elf=root/member;b=elf.read_bytes();assert b[:6]==b'\x7fELF\x02\x01'and struct.unpack_from('<H',b,18)[0]==62
  phoff=struct.unpack_from('<Q',b,32)[0];ents,num=struct.unpack_from('<HH',b,54);assert ents==56
  loads=[]
  for i in range(num):
   typ,flags,po,va,pa,fs,ms,al=struct.unpack_from('<IIQQQQQQ',b,phoff+i*ents)
   off=int(offset,16)
   if typ!=1 or not(flags&1) or not(po&~4095)<=off<((po+fs+4095)&~4095):continue
   bias=int(start,16)-(va-po+off);vpc=pc-bias
   if va<=vpc<va+fs:loads.append((vpc,po+vpc-va,bias))
  assert len(loads)==1,(name,loads)
  vpc,file_pc,bias=loads[0];assert b[file_pc:file_pc+16]
  syms=subprocess.check_output(['nm','-D','-S','--defined-only','--demangle',str(elf)],text=True)
  covered=[]
  for line in syms.splitlines():
   sm=re.match(r'([0-9a-f]+) ([0-9a-f]+) \S (.*)',line)
   if sm and int(sm[1],16)<=vpc<int(sm[1],16)+int(sm[2],16):covered.append(sm[3])
  nameout=f'{guest}-sample{sample}'
  dis=subprocess.check_output(['objdump','-d','-C','--start-address='+hex(vpc),'--stop-address='+hex(vpc+32),str(elf)],text=True)
  (out/(nameout+'.disassembly.txt')).write_text(dis)
  item={'guest':guest,'sample':int(sample),'runtime_pc':hex(pc),'mapped_path':name,'mapped_start':start,'mapped_offset':offset,'exact_locked_member':member,'elf_sha256':hashlib.sha256(b).hexdigest(),'elf_vaddr':hex(vpc),'elf_file_offset':hex(file_pc),'load_bias':hex(bias),'containing_dynamic_symbols':covered,'scope':'single stopped thread AFTER irreversible failure; NO unwind, causal or pre-failure state claim'}
  results.append(item);print(guest,sample,name,hex(vpc),covered or 'NO containing sized dynamic symbol')
(out/'attribution.json').write_text(json.dumps(results,indent=2)+'\n')
