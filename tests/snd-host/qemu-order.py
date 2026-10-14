#!/usr/bin/env python3
"""Inspect all three QEMU command definitions without building/running non-x86 guests.
The base command must have the same device prefix with/without optional sound,
particularly the virtio-scsi boot controller whose PCI address UEFI persists.
"""
import shlex, subprocess
for arch in ('x86_64', 'riscv64', 'aarch64'):
    def cmd(sound):
        p = subprocess.run(['make', '-s', 'ARCH='+arch, 'QEMU_SOUND='+sound,
                            '--eval=audio-print:;@echo $(QEMU_$(ARCH))', 'audio-print'],
                           capture_output=True, text=True, check=True)
        return shlex.split(p.stdout.strip())
    present = cmd('-audiodev none,id=snd0 -device virtio-sound-pci,audiodev=snd0,streams=1')
    absent = cmd('')
    marker = present.index('-audiodev')
    assert present[:marker] == absent, f'{arch}: sound removal changed boot device prefix'
    if arch != 'x86_64':
        assert present.index('virtio-scsi-pci') < marker, f'{arch}: sound precedes boot controller'
    print(f'snd-host: {arch} optional audio follows unchanged boot-device prefix')
