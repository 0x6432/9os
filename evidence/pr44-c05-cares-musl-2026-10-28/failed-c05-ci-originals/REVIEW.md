# PR44 exact-head CI — one bounded snapshot, original failures preserved

Head: `c05a3fcd851297161f3981b38dd04900c49e8467`. Both automatic pull_request runs attempt1, completedfailure. Original failed-job logs explicitly check out PR merge `2cafec0b9652bae58521d68f09e871e7877c2c83` (“Merge c05a3fc into e0521e7”), not a direct head checkout. No tests/builds/boots/edits/reruns/pushes performed by this review. No polling loop, running-log request, credential reuse, package or ISO GET.

## Signed run40572840020
https://github.com/0x6432/9os/actions/runs/40572840020
- Failed alpine(x86_64) job122431334416: https://github.com/0x6432/9os/actions/runs/40572840020/job/122431334416
- Metadata says other five jobs succeeded: alpine ARM122431334026/RISC122431334247, desktop ARM122431333778/RISC122431334234/x86122431334248. Successful-job test counts/screenshots were NOT independently reviewed here; do not infer full cold-reliability clearance.
- Failed step6 Install signed prebuilt runtime. Log1317–1318 at2026-10-27T19:55:20Z identifies exact c-ares URL below and three recorded404 attempts at19:55:15/17/19Z. Python traceback `userland/alpine/build.py:40` via fetch:216 and main:219.
- Host lock/checksum negative controls step5 metadataSUCCESS. Offline replay7, ownership8, tiny compile9, Linux reference10, following x86 kernel/image11, Qt12, UP13 andSMP8 step14 all **SKIPPED**, not pending or failed consumer runs. Therefore no current Linux-reference90s/RC0 evidence and no following x86-base guest results.
- No failed-x86 diagnostic ZIP exists: raw log1339–1341 says no files/artifacts; artifact map contains only the five successful lanes. No artifact substituted.

## Source run40572839971
https://github.com/0x6432/9os/actions/runs/40572839971
- Failed build(x86_64) job122431399357: https://github.com/0x6432/9os/actions/runs/40572839971/job/122431399357
- ARMjob122431399434 metadataSUCCESS. RISCjob122431399346 metadataCANCELLED during Builduserland; cancellation cause not established, NOT automatically a product failure. StampSUCCESS, PR releaseSKIPPED.
- Failed step8 Builduserland. LLVM/Mesa/Qtbase/QtWayland completion messages precede qtdeclarative. `userland/ports/host-qsb.py:75/78` fetch calls common `userland/alpine/build.py:40` downloader; HTTP404 at2026-10-27T21:14:59Z, same exactURL. Raw log1869–1870 records404 attempts21:14:56/57/59Z. These are existing CI attempts, not reviewer retries.
- Kernel/ISO build and all subsequent x86 boot/UP/MLFQ/SMP gatesSKIPPED. No compiler or guest assertion exception identified as the terminating failure.
- No failed-x86 test ZIP: raw log1887 says no files/no artifacts. Artifact map contains only successful ARM test-log and ARM ISO. Neither downloaded.

## Shared acquisition blocker and exact retained scope
URL: `https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/c-ares-1.34.8-r0.apk`
Pinned SHA256: `1397ec9682ff6153e5d037965c76408e570ae6535ce479cc2af37436fdea52ce`.
Identical entry in productc05a3fc:
1. `userland/alpine/locks/x86_64.json:151`
2. `userland/alpine/locks/x86_64-desktop.json:1372`
3. `userland/ports/host-qsb-lock.json:116`

Both recorded failures identify a missing pinned APK, NOT the reviewed Linux-oracle cache path. Successful desktop lane may have acquired/used its input differently; no cache-causality claim made. No live package availability or replacement version was probed by this review.

## Narrow scope proposal — no approval assumed
Authorize separate bounded official stable replacement/provenance review for ONEc-ares pin, consistently across the three locks if evidence supports it. Preserve original404s; verify actual replacement signature/hash, package/dependency compatibility and host-qsb scope before proposing edits. No resolver-wide refresh, edge substitution, automatic retry/cache/timeout change, acceptance relaxation or unreviewed dependency update. SourceRISC cancellation needs separate evidence before classifying cause. Guest90sRC0 and historical cold/typing/Dolphin/M40 holds remain unresolved even if acquisition is repaired.

## Original preservation
- Failed signed log129969B SHA256 `2d3204e988d6d06d295ae2abd6d79933a949afc306090b053440a89f975d8e8a`.
- Failed source log183488B SHA256 `72b0fb6355d9b71123094debe6ebc7dd35a2c980f55f458b7e5662649e613756`.
Both complete originals saved BEFORE analysis. Metadata, raw response bytes, status/headers/UTC/SHA provenance included. No relevant failed-job artifact present to download; successful artifacts/ISO not fetched.
