#!/usr/bin/env python3
"""Decode ONE framed retained-query history offline; never execute its payload.

Only the new retained-history section is decoded. Final original-query raw
replay remains outside this format and outside the transport safety guarantee.
"""
import argparse
from pathlib import Path
import re
import sys

SLOTS=('first','last','terminal-first','terminal-last','remote-first','remote-last',
       'incomplete-first','incomplete-last','witness-first','witness-last','alarm-first','alarm-last')
MAX_SLOT=5120
MAX_WIRE=128*1024
HEADER=re.compile(rb'KONSOLE_QUERY_HISTORY attempts=[0-9]+ recording_failed=[01] raw=prefix2048\+suffix2048 encoding=hex timing_perturbed=1')
BEGIN=re.compile(rb'KONSOLE_QUERY_ATTEMPT_BEGIN slot=([a-z-]+) encoding=hex bytes=([0-9]+)')


def decode_history(data):
    """Return fixed-slot-name -> exact original bytes; reject ambiguous damage.

    Other surrounding log bytes are ignored. Supply a single section when a
    collected job log repeats it; never guess which duplicate is authoritative.
    """
    lines=data.split(b'\n')
    starts=[i for i,line in enumerate(lines) if line.startswith(b'KONSOLE_QUERY_HISTORY ')]
    if len(starts)!=1:
        raise ValueError('expected exactly one history section')
    start=starts[0]
    if not HEADER.fullmatch(lines[start]):
        raise ValueError('invalid history header')
    try:
        end=lines.index(b'KONSOLE_QUERY_HISTORY_END',start+1)
    except ValueError:
        raise ValueError('incomplete history section') from None
    if sum(len(line)+1 for line in lines[start:end+1])>MAX_WIRE:
        raise ValueError('history wire bound exceeded')
    result={}
    previous=-1
    i=start+1
    while i<end:
        match=BEGIN.fullmatch(lines[i])
        if match is None or i+2>=end:
            raise ValueError('invalid slot framing')
        slot=match[1].decode('ascii')
        if slot not in SLOTS or SLOTS.index(slot)<=previous:
            raise ValueError('unknown, duplicate or reordered slot')
        size=int(match[2])
        if size>MAX_SLOT:
            raise ValueError('slot byte bound exceeded')
        payload=lines[i+1]
        if len(payload)!=size*2 or re.fullmatch(rb'[0-9a-f]*',payload) is None:
            raise ValueError('invalid lowercase hex or byte count')
        if lines[i+2]!=b'KONSOLE_QUERY_ATTEMPT_END slot='+match[1]:
            raise ValueError('mismatched slot end')
        result[slot]=bytes.fromhex(payload.decode('ascii'))
        previous=SLOTS.index(slot)
        i+=3
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log',type=Path,help='log containing exactly one framed history section')
    parser.add_argument('--slot',required=True,choices=SLOTS,help='write this slot as exact binary stdout')
    args=parser.parse_args()
    try:
        slots=decode_history(args.log.read_bytes())
        if args.slot not in slots:
            raise ValueError('requested slot absent; not an empty/zero observation')
    except (OSError,ValueError) as error:
        parser.exit(1,'history decode: '+str(error)+'\n')
    sys.stdout.buffer.write(slots[args.slot])


if __name__=='__main__':
    main()
