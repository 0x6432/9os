# Source e22 failures: confirmed acquisition issue, bounded diagnostic proposal

## Confirmed original evidence

Both completed source userland failures are at reviewed head e22f0ebc5b201eb1a57e318605a4df2465ae1834, actual synthetic checkout1ff01c82ae684ed0c0d5990f38e466c491baf3ce. Original checkout lines preserved. Synthetic tree3dd5e9909b9cdcef781b08e4ed0ff04978fea7fc equals reviewed e22 tree; distinct commit IDs retained.

- x86_64 job121114009931:175439 original bytes,SHA256668825fc38bf6f1147eab5ff6c809fbc5f7dda0b0d9e3191e1bfada58f416211. Final HTTP404/URL note2026-10-25T09:01:06Z.
- ARM job121114009937:111254 original bytes,SHA256d201a304d5fb71a72eff8cedbc5b0802fead81b8763f7e35be56105b16ed4965. Final HTTP404/URL note2026-10-25T09:13:55Z.
- Both exact URL: https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/ca-certificates-bundle-20260909-r0.apk
- Both chains: qtdeclarative -> host-qsb.py runtime fetch -> shared alpine/build.py downloader. LLVM/Mesa/Qtbase/Qtwayland completed before fetch failure. This is BUILD-HOST x86 APK acquisition, not target source compilation or guest failure.
- Current one full GET:HTTP200,108541 bytes,SHA2569cbf2fd3bcbad4a73d6e2c5a2d5cfea176ce01790d8ae8bb1796ba4e64d6c48b,exactly matching reviewed host-QSB lock. This does not determine historical failure cause or prove sustained availability.
- Peer independently retrieved the original logs with identical hashes and confirmed same CA URL/hash in exact reviewed signed-base lock. Earlier peer clean160 signed-assembly experiment therefore covered this package by exact recorded input identity, NOT a fresh read of missing retained cache files.
- RISC-V source job reached boot tests in the observed public snapshot. It remains unfinished; no new claim of completed boot/integration success.

## Existing exact retry contract

Cache hit with matching SHA returns before networking; offline miss fails before retry loop. For online acquisition:
1. Attempt1 passes timeout=60 to urlopen; stream/checksum/atomic replace on success. Exception -> .part cleanup -> sleep1.
2. Attempt2 uses same behavior. Exception -> .part cleanup -> sleep2.
3. Attempt3 uses same behavior. Exception -> .part cleanup -> existing exact-URL add_note -> unchanged bare raise of same final exception.

The60 argument applies to blocking I/O/socket operations; it is NOT an absolute download/job deadline. Threadpool work and response streaming mean no183-second total promise. Existing logs establish final404 only; earlier exception classes/statuses/start times are absent. No assertion that all3 attempts returned404 is justified.

## Availability vs retirement

An actual historical404 plus current matching200 demonstrates time/context-dependent availability but does not identify CDN, routing, proxy, mirror synchronization, cache content or upstream removal as the cause. Permanent retirement is not demonstrated. A retirement repair would need official version/index provenance and reproducibly unavailable old bytes rather than inferring retirement from one terminal exception. No pin/mirror/trust-policy change is justified here.

Source download cache is target-architecture keyed while QSB APKs are build-host x86 across all targets. Original x86/ARM restored older target-specific cache keys. This is a possible efficiency/reuse issue, not a demonstrated security/hash bug or direct cause. Moving prefetch earlier would fail faster but would not fix availability; it is not proposed as a proven repair.

## Proposed diagnostic-only improvement (NOT IMPLEMENTED)

Scope exactly2 files:
- userland/alpine/build.py::download
- scripts/alpine-download-host-tests.py

Collect at most3 bounded failed-attempt records: attempt1..3, UTC start/failure timing, monotonic elapsed duration, exception class, integer HTTP status where applicable. Do not record response bodies, arbitrary exception messages, headers, credentials or new URLs.

Only on the existing final failure, append a bounded summary to the SAME exception via notes, alongside the unchanged exact-URL note, then unchanged bare raise. No stdout/stderr writes during retries. Recovered downloads remain quiet and their previous exception objects unmodified. Retain exact3 attempts, timeout60 argument, sleeps1+2, cleanup/SHA/atomic cache/offline behavior and signature enforcement.

Tests before/after: distinct mocked404 -> timeout ->503 sequence with exact record order/attempt numbering/timing under fake clocks; same final exception object/type; exactly3 calls with timeout60 and sleep1,2; partial-read/checksum failures preserve old cache and delete .part; recovered success/cache hit/offline miss emit no extra attempt notes. No real network, library build, guest or VM needed.

Tradeoff: final-only notes have less operational noise and avoid failed logging I/O masking the original exception. They do NOT expose failed attempts that subsequently recover, provide real-time retry progress, prove CDN/backend cause, fix availability or improve retries. Immediate stderr events could aid live progress but add output/I/O failure behavior; not the minimum proposal. No extra HTTP probes, fallback, cache-key policy or deadline change.

Both original URLs/SHA/size records and proposal sent for independent peer review. Await explicit approval before any implementation. No source edits, commits, pushes, credentials, API writes, CI reruns/cancels, VM/library compile or cold dispatch. PR44 remains draft/unmerged; all prior holds remain.
