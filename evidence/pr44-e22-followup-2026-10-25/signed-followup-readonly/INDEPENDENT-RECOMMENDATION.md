# Independent bounded review: signed x86 musl-dev fetch

Verdict: **no product repair is justified by current evidence**. Keep the signed failure recorded and wait for source completion. Do not change pins, expand retries, rerun old attempts, weaken hash/signature checks, mask failure, or claim cold reliability.

## Exact original failure

Signed run40175112595 attempt1, job121113986043 at PR heade22f0ebc5b201eb1a57e318605a4df2465ae1834. Complete original job log saved before selective reading:114,981 bytes, SHA2569393b3c9f9c362efc57799259a0c1511c8fe3aa95cb6926468bb61bc9426bdb2.

Original lines96/123/126 prove actual checkout1ff01c82ae684ed0c0d5990f38e466c491baf3ce. Its REST tree equals reviewed e22tree3dd5e9909b9cdcef781b08e4ed0ff04978fea7fc; commit IDs remain distinct. Source run's actual checkout remains unconfirmed while its logs are unavailable; do not substitute this signed observation for source checkout proof.

Original line441 records x86 runtime download-cache MISS. Completed host controls include the new APT2 methods and download6 tests. Signed runtime installation starts08:06:56Z and exits1 at08:07:00Z. Final exception is HTTP404 and the new note prints exact public URL:
https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/musl-dev-1.2.5-r12.apk

The URL-context change is directly proven functional in CI. This is a fetch failure before APK signature/install/guest stages, not a compiler or rendering failure.

## Independent current full-byte probe

One bounded full GET of that exact official URL now returned HTTP200,3,427,570 bytes. SHA2561c2068d910cfdbbcb4eb107a5a478f8b48edf3a6311953c8fa5180c5190efab3 exactly matches the published e22 x86 lock. Raw response headers, APK bytes and hash manifest preserved.

APK .PKGINFO identifies musl-dev1.2.5-r12,x86_64, exact dependency musl=1.2.5-r12. No new cryptographic signature execution was performed in this bounded probe; matching SHA is against the already reviewed lock. Do not describe tar metadata inspection as signature verification.

The historical404 is real. Current matching bytes disprove an assertion that this exact pin is currently unavailable; they do not establish continuous availability or identify the historical CDN/server cause. Transient CDN/cache variation is a hypothesis only.

## Exact downloader semantics and recommendation

The helper at exacte22 still performs three attempts,60s per request, sleeps1 then2, cleans partial files after every caught exception, SHA-checks bytes before atomic replacement, preserves existing cache on failure and fails closed after the final exception. New add_note executes only on the third/final catch before the unchanged bare raise. The original final traceback proves the third terminal exception was404; earlier attempt error types were not logged, so do not assert all three were404.

No offline fallback or authentication/signature bypass is involved. Increasing retries or changing mirrors/pins cannot be justified as a proven repair from this observation. A musl-dev-only revision bump would also violate its exact matching-musl dependency unless the coupled set were reviewed; no such revision change is presently necessary.

Source QSB uses musl-r12, not musl-dev; its exact URL separately returned200 in the source investigation. Source jobs remain unfinished, not validated. No source failure was present in the preserved state snapshot. Wait for original completed source logs before proposing any implementation.

No API write, code edit, commit, push, source/library compilation, VM, cancel, rerun, merge or cold dispatch. All existing holds remain.
