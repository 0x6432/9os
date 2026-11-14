#!/usr/bin/env python3
"""Opt-in LP64 Qt6.8.3 platform-submission trace, NOT application consumption."""
from pathlib import Path
from importlib.machinery import SourceFileLoader
import re,json,argparse
C=SourceFileLoader('qt_observer_capture',str(Path(__file__).with_name('input-capture-check.py'))).load_module()
START='QT_PLATFORM_KEY_TRACE_BEGIN';END='QT_PLATFORM_KEY_TRACE_END'
LOADED='QT_KEY_TRACE_LOADED abi=Qt6.8.3-LP64 boundary=platform-submission'
FIELDS='seq phase pid clock_ok mono_ns source_ms type key modifiers scan sym native autorepeat count result'.split()
PAT=re.compile(r'^QT_KEY_TRACE '+r' '.join(k+'=('+('[a-z]+'if k=='phase'else'-?[0-9]+')+')'for k in FIELDS)+r'$')
def check(raw):
 if len(raw)>64*1024*1024:raise ValueError('oversize trace')
 blocks=re.findall(r'^'+START+r'\n(.*?)^'+END+r'$',raw,re.M|re.S)
 if not blocks or raw.splitlines().count(START)!=len(blocks) or raw.splitlines().count(END)!=len(blocks):raise ValueError('incomplete/nested/missing framing')
 if any(b!=blocks[0]for b in blocks):raise ValueError('nonidentical replay')
 lines=blocks[0].splitlines()
 if not lines or lines[0]!=LOADED or lines.count(LOADED)!=1:raise ValueError('missing/ambiguous hook-load witness')
 rows=[]
 for line in lines[1:]:
  m=PAT.fullmatch(line)
  if not m:raise ValueError('malformed/truncated/unexpected hook record')
  row={k:(v if k=='phase'else int(v))for k,v in zip(FIELDS,m.groups())}
  if row['phase']not in ['begin','end']or row['clock_ok']!=1 or row['pid']<=0:raise ValueError('invalid phase/clock/PID')
  if row['autorepeat']not in [0,1]or row['type']not in [6,7]or not 1<=row['count']<=65535:raise ValueError('invalid key type/repeat/count')
  for k in ['seq','scan','sym','native']:
   if not 0<=row[k]<=0xffffffff:raise ValueError('unsigned field overflow')
  for k in ['key','modifiers']:
   if not -0x80000000<=row[k]<=0x7fffffff:raise ValueError('signed field overflow')
  if not 0<=row['source_ms']<=0xffffffffffffffff or not 0<=row['mono_ns']<=0xffffffffffffffff:raise ValueError('time field overflow')
  rows.append(row)
 if not rows or len(rows)%2:raise ValueError('incomplete event pair')
 events=[];last=0
 for i in range(0,len(rows),2):
  a,b=rows[i:i+2]
  if a['phase']!='begin'or b['phase']!='end'or a['seq']!=i//2 or b['seq']!=i//2 or a['result']!=-1 or b['result']not in [0,1]:raise ValueError('incomplete/reordered/invalid result pair')
  if any(a[k]!=b[k]for k in FIELDS if k not in ['phase','mono_ns','result']):raise ValueError('changed arguments during forwarding')
  if not last<=a['mono_ns']<=b['mono_ns']:raise ValueError('nonmonotonic observer timestamps')
  last=b['mono_ns'];events.append(a)
 if len({r['pid']for r in events})!=1:raise ValueError('mixed client PID')
 physical=[(r['scan']-8,1 if r['type']==6 else 0)for r in events if not r['autorepeat']]
 expected=[e for f in C.expected_frames(C.DEFAULT_TEXT)for e in f]
 if physical!=expected:raise ValueError('non-repeat platform edges differ from intended physical sequence')
 repeats=[r for r in events if r['autorepeat']]
 return {'physical_key_edges':len(physical),'autorepeat_events':len(repeats),'autorepeat_scancodes':[r['scan']for r in repeats],'events':len(events),'pid':events[0]['pid'],'identical_replays':len(blocks),'scope':'Qt platform event submission only; not application/terminal consumption or unchanged-timing reliability proof'}
if __name__=='__main__':
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('log',type=Path);ap.add_argument('--require-no-autorepeat',action='store_true',help='reject even otherwise well-formed Qt autorepeat events');a=ap.parse_args()
 try:r=check(a.log.read_text(errors='replace'))
 except(ValueError,OSError)as e:ap.exit(1,'FAIL: '+str(e)+'\n')
 print(json.dumps(r,sort_keys=True))
 if a.require_no_autorepeat and r['autorepeat_events']:ap.exit(1,'FAIL: Qt generated autorepeat during fixed physical tap sequence\n')
