# M39 audio bring-up

The first audio driver uses modern **virtio-snd over PCI** on all three guest architectures.
Only x86_64 is tested locally; ARM and RISC-V runtime verification belongs to PR CI.

## Implemented subset

- One exclusive playback device: `/dev/snd/pcmC0D0p` (116:16), with a sound-class sysfs node.
- Linux 64-bit ALSA PCM wire ABI: protocol/version, info, fixed hardware refinement/setup,
  restricted software parameters, prepare/start/drop/drain/free, status/delay, sync-pointer
  query, interleaved write and `WRITEI_FRAMES`.
- **Stereo S16_LE, 48 kHz**, 256-frame periods, eight periods (2048-frame buffer).
- MSI-X/INTx completion or polling fallback. Bounded control/playback waits, kernel-owned
  DMA storage, exclusive open, queue backpressure, nonblocking EAGAIN and poll readiness.
- Prepared samples are staged in kernel-owned slots until START. DROP discards unsent
  samples without exposing their storage to the device; submitted slots are only reused
  after device completion. A timeout marks the device failed and quarantines its DMA pages.

This is **not full ALSA compatibility**: no control/mixer device, capture, mmap, pause,
rewind, format/rate conversion, underrun recovery, or audio server. No mmap/pause capabilities
are advertised. alsa-lib/aplay, applications, and audible real-hardware playback remain
unverified. Software timestamp mode is disabled; stop threshold is fixed to the buffer size.

## QEMU and tests

`make run` includes virtio-snd with a headless `none` backend. Override `QEMU_SOUND` to use a
host audio backend that your QEMU actually supports; `QEMU_SOUND=''` removes the card entirely.

```sh
scripts/snd-host-tests.sh
scripts/snd-qemu-tests.sh x86_64 --boot-timeout 300
```

The first test compares private ABI sizes, field offsets and ioctl numbers with independent
Linux UAPI headers. The guest `sndtest` exercises PCM setup, unsupported constraints, user
pointer failures, exclusive open, writes/ioctl writes, frame counters, drain, explicit start,
nonblocking backpressure, poll, drop, close and reopen. It generates opposite-channel 500 Hz
square waves. A host WAV check verifies the first **16384 stereo frames byte-for-byte**; it
does not merely accept successful ioctls or a nonempty capture. A separate boot without a
sound card checks that no fake PCM device is registered.

The QEMU harness can forcibly terminate the emulator after guest shutdown. WAV RIFF/data
length fields can therefore remain zero; the checker validates the fixed PCM header and
actual payload bytes without modifying the capture. This is a test-artifact limitation,
not an invented PCM result.
