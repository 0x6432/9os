#!/usr/bin/env python3
"""Verify actual QEMU WAV PCM against sndtest's first 64 generated periods.
The normal QEMU harness may kill the emulator after guest shutdown; QEMU's WAV
backend leaves RIFF/data lengths zero until cleanup. Check payload length directly
without rewriting the capture or treating unfinalized header sizes as audio data.
"""
import array, struct, sys
b = open(sys.argv[1], 'rb').read()
assert len(b) >= 44 and b[:4] == b'RIFF' and b[8:20] == b'WAVEfmt \x10\x00\x00\x00', 'WAV header'
assert struct.unpack_from('<HHIIHH', b, 20) == (1, 2, 48000, 192000, 4, 16), 'stereo 48k S16 PCM'
assert b[36:40] == b'data' and (len(b) - 44) % 4 == 0, 'PCM payload alignment'
samples = array.array('h', b[44:])
if sys.byteorder != 'little': samples.byteswap()
assert len(samples) >= 2 * 64 * 256, 'complete primary playback sequence'
for i in range(64 * 256):
    v = -10000 if i // 48 % 2 else 10000
    assert samples[2*i] == v and samples[2*i+1] == -v, f'frame {i}: expected opposite stereo square wave'
print('snd-wav: 16384 stereo frames match generated 500 Hz waveform exactly')
