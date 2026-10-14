#!/bin/sh
# Real DMA playback (host WAV capture) and absent-device regression, on CI's ARCH.
set -eu
ARCH=${1:-x86_64}; shift
cd "$(dirname "$0")/.."
mkdir -p build/ci-$ARCH
WAV=$(pwd)/build/ci-$ARCH/snd.wav
rm -f "$WAV"
QEMU_SOUND="-audiodev wav,id=snd0,path=$WAV,out.frequency=48000,out.channels=2,out.format=s16 -device virtio-sound-pci,audiodev=snd0,streams=1" \
    python3 scripts/qemu-test.py "$ARCH" --log "build/snd-$ARCH.log" "$@" sndtest
python3 scripts/snd-wav-check.py "$WAV"
QEMU_SOUND='' python3 scripts/qemu-test.py "$ARCH" --log "build/snd-absent-$ARCH.log" "$@" 'sndtest --absent'
