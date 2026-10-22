# Minimal x86 pinned-runtime restoration

The original checked-in x86 base lock could download 159 of its 160 pinned APKs
on 2026-10-22. libexpat-2.8.5-r0.apk returned HTTP404. The original failed full
installer output and exact missing URL/hash are retained in evidence; this is
not a signature or consumer-code failure, and cached old packages may still
permit an old CI job to run.

Both checked-in x86 base/desktop locks change exactly one entry, with every
other package and all bootstrap/profile metadata unchanged:

- New official stable package: libexpat-2.9.0-r0.apk, Alpine v3.22/main/x86_64.
- SHA256: 4c9fb621d455d12475767e261821a4be7b4e91e2ba19c20a02e7fa99189b0d6d.
- Discovery used the signature-verified repository index and pinned3.22.6 mini
  keyring; the downloaded exact APK then passed native apk signature verification.
- It provides so:libexpat.so.1=1.13.0 and depends on the existing musl soname.
  No repository-wide resolution, signature bypass or checksum bypass was used.

An intermediate `apk fetch libexpat=2.9.0-r0` invocation could not select the
package. That original failure is retained, not claimed successful. Direct
HTTPS download of the discovered exact URL followed by strict signature
verification succeeded before the new SHA was pinned.

The updated base full160-package closure assembled successfully, with target
install scripts disabled and no Qt/Mesa/LLVM/GLib library compilation. Exact
offline replay in a SEPARATE root produced an identical manifest; the canonical
root used by active protocol tests was not replaced during those tests.

Desktop lock validation here verifies the same single-entry update, not local
assembly/boot of the desktop profile. Non-x86, fresh full desktop/source CI and
all cold-start/integration holds remain independent and pending. This does not
repair desktop cold-start reliability or establish indefinite upstream archive
availability. Raw command logs are gzip-preserved without altering their bytes.
