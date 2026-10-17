# INVALID frozen-fixture trial: checked-in PR33 integrated diagnostic

The observed runtime log reports 18/18, but DO NOT accept this as a controlled
frozen-fixture trial. The driver invoked make run, which automatically rebuilt
initramfs.cpio and 9os.iso immediately before starting QEMU because the base
root had acquired the new tiny helper. Kernel ELF, disk, driver, probe and helper
hashes remained unchanged; the two boot-artifact mismatches are retained in
bulk-integrated-post-hash-check.txt alongside the ORIGINAL expected manifest.
The end-of-run hash gate correctly stopped the controller before acceptance.
Do not replace that original manifest with the new hashes or hide this failure.

Subsequent frozen runs use a separate x86-only make adapter that marks the
existing ISO and firmware old; dry-run proves exactly one QEMU invocation and
zero build commands. This is a harness fix, not a kernel/runtime cure.

Observed original libatspi gate and explicit post-ready bulk query succeeded,
with real Qt PID769; raw390/262edges/128taps, outer262, Qt262/0autorepeat. Those
layer observations do not repair failed fixture integrity or prove reliability.
All raw evidence/checkers/scripts/PNG files retained. No retry of this sample is
silently substituted. M40 and earlier failure/integration holds remain open.
Evidence-only branch; do not merge binary archives into product code.
