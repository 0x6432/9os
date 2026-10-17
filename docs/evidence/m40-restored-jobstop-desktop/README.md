# Restored x86 signed-prebuilt desktop validation

33/33, including root/user synthetic snapshot controls, expanded ptracestoptest,
Quick/Wayland pixels, system bus/eudev/UDisks, KWin/genuine Konsole QtWidgets PTY,
Plasma render check and lockdep0. Only tiny consumers and kernel compiled locally;
Qt/Mesa/LLVM/GLib were signed prebuilts. Restored profile contains593 signed APKs.

Scope: candidate job-stop kernel and plain non-setuid target. The copied selftest
predates the final exact target-PID/mode fork-exec handshake added to935bd74;
that script is included verbatim. Do NOT call this validation of the final
handshake, unchanged original kernel, cold physical input, or a startup cure.
The final fixed-scope Linux/guest physical experiment and current-head CI are
separate. All previous failures and integration holds remain; M40 incomplete.

The full log includes genuine rendering PNG evidence and every result. Evidence
only: do not merge archives into product. No retries were needed for this suite.
