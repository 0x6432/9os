# M40 release-path batch — patch review only

**Draft evidence; do not merge this PR as-is.** This branch stores patches for review and does not apply them to the source tree. CI on this branch tests the unpatched base, not the candidate. No guest, reliability, security, merge, or release approval is implied.

Base: PR #44 head `36933be034d41b762c98f2ab2f2902f0755fde40`. Apply 01a, 01b, and then 02 through 05 in order to a clean copy of that exact tree, checking each before applying. 01a and 01b concatenate byte-for-byte to the original blocker patch. These patches preserve intended file modes and are not independently approved as a combined product change.

- 01a and 01b: scratch blocker and opt-in input diagnostics candidate.
- 02: Alpine unpack-mode correction and CI ordering.
- 03: runner failure/skip handling and fake-only tests.
- 04: wire fake-only regression into x86 host CI.
- 05: parallelize fake regression controls; timing/flakiness needs review.

Local validation: fresh reapplication reproduced all 18 changed files; ten related host suites passed and two repeat fake-runner batches each passed 14 controls. Original runner negative controls reproduced false passes. Workflow YAML parsed locally; patches have not run in GitHub Actions. Host/fake tests do not verify real guest behavior.

**Still blocked:** historical duplicate typing, terminal and Dolphin failures lack validated runtime fixes; image security metadata and initramfs semantics remain unresolved; no approved immutable real launcher or agreed cold-test protocol exists. Original Alpine locks remain unchanged in this patch set. Sway and mouse reports are deliberately deferred.
