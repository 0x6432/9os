# PR44 source CI failure review — exact head 4140e542ec1043818e7c5ee0d2c8fbd6dcc511f2

## Outcome and limits

Confirmed source ARM failure is a BUILD-HOST signed QSB runtime APK fetch HTTP404, not a target C/C++ compiler error or GIO/AT-SPI assertion. The original traceback does **not** include the failing URL. Thus the precise historical APK cannot be uniquely identified. Today the exact host-only lock has three unavailable pinned URLs; a narrowly scoped local repair updates only those three URL/SHA256 pairs. This removes three confirmed current cold-download blockers, not proof of a complete source ARM build or all CI passing.

Three reviewed local product commits were explicitly authorized after cross-review and combined native tests. No push, merge, rerun, workflow dispatch, acceptance weakening, deadline extension, chat-token use, local non-x86 compilation, VM run, or Qt/Mesa/LLVM/GLib library compilation was performed. Publication awaits separate authorization/credential. Entire PR18–44 cold/integration holds remain. M40 remains unfinished.

## Isolation and ownership

- Bash confirmed working on this new computer.
- Public repo clone: `/data/9os`; branch `investigate-pr44-aarch64`; base exactly `4140e542ec1043818e7c5ee0d2c8fbd6dcc511f2`.
- Original source-only repair: `userland/ports/host-qsb-lock.json` (6 insertions, 6 deletions). Parent later explicitly approved two matching PCRE updates in each signed x86 lock, final-failure URL notes and their native tests, plus independently reviewed host-fixture/APT patches. Final product scope is exactly nine files; see final freeze below.
- Portable patch: `/data/qsb-pin-review/source-host-qsb-pins.patch`.
- Initial source-only patch did not overlap other ownership. Later shared-helper/three-lock and integration changes were explicitly authorized. No QEMU installation script, production terminal probe, readiness/deadline/acceptance logic, or guest code was changed.

## Original public evidence, preserved before selective reading

- Run REST: `https://api.github.com/repos/0x6432/9os/actions/runs/39802683577/attempts/1` → `ci-original/run.json`.
- ARM job REST: `https://api.github.com/repos/0x6432/9os/actions/jobs/119907512734` → `ci-original/aarch64-job.json`.
- RISC-V job REST: `https://api.github.com/repos/0x6432/9os/actions/jobs/119907512820` → `ci-original/riscv64-job.json`.
- Full original ARM log: `https://nightly.link/0x6432/9os/runs/119907512734.txt` → `ci-original/aarch64-original.log` (1,243 lines, 110,894 bytes).
- Full original RISC-V log: `https://nightly.link/0x6432/9os/runs/119907512820.txt` → `ci-original/riscv64-original.log` (15,114 lines, 1,315,229 bytes).
- Original RISC-V test artifact: `https://nightly.link/0x6432/9os/actions/runs/39802683577/test-log-riscv64.zip` → `ci-original/test-log-riscv64-original.zip` (105,082 bytes, matches REST artifact size).
- Artifact listing saved: exactly `iso-riscv64` (12445535318), `test-log-riscv64` (12445530256). No ARM artifact exists because boot stage was never reached.
- Direct unauthenticated REST job-log download returned403 `Must have admin rights to Repository`; response and headers retained. Public nightly.link successfully retrieved actual complete original text logs, without a token.
- HTML/public route failures retained as access provenance; `run-logs-original.zip` is a 404 HTML response, **not** a successful log ZIP. Only the two `*-original.log` text files are original job logs.
- SHA256 manifest: `/data/ci-original/SHA256SUMS` (excludes itself).

The run REST confirms event `pull_request`, run_attempt1, branch `notion/m40-remote-error-evidence`, exact head above, created2026-10-22T15:12:52Z. Both job metadata files identify the same PR head. Actual actions/checkout used synthetic merge `3b5116bc00bd87653d41c5bb78f56061fb9e96a5`, visible in both full logs at lines97,124,127. GitHub Git-commit REST proves its tree `b4e10d1e690b2e22476fe7ce279b213adb574bf5` equals the reviewed4140 tree; parents are `e0521e7d5b36540c5b0806d11cc8cae81a476d09` and4140. Local git verifies e052 ancestry. These are distinct commit IDs with identical source trees, not interchangeable IDs. Full REST proof is retained as `source-synthetic-merge-commit.json`. Complete ARM setup and userland logs are available, not just annotations.

## ARM precise causal chain

Source: original ARM log lines1090–1211, plus exact checked-out scripts.

1. Userland step began2026-10-22T15:13:48Z, failed16:08:05Z with exit1. Host regression step7 was skipped for ARM, so no claim that ARM ran those host tests.
2. Target source `expat2.6.4` installed15:16:06Z, target `glib2.82.4` installed15:18:13Z, LLVM18.1.8 installed15:52:01Z, Mesa24.3.4 installed15:54:18Z, Qtbase6.8.3 installed16:06:10Z, Qtwayland6.8.3 installed16:07:56Z.
3. At16:07:56Z port `qtdeclarative` begins; source archive SHA512 passes16:07:58Z.
4. At16:08:05Z traceback: `host-qsb.py:78 list(pool.map(fetch, entries))` → `host-qsb.py:75 b.download(e['url'],...)` → `userland/alpine/build.py:30 urllib.request.urlopen` → `urllib.error.HTTPError: HTTP Error404: Not Found`.
5. `entries` are the **x86_64 BUILD-HOST** APK list from `userland/ports/host-qsb-lock.json`, not ARM target packages. Metadata fetch occurs only after this map, so the failure is in runtime entries, not `tool_metadata`.
6. `qtdeclarative.sh:36` obtains `QSB_CMAKE` from this installer before configuring target QtQuick. The kernel/ISO and all ARM boot tests were skipped. No target failure or GIO regression can be inferred.
7. Downloader already attempts3 times with60s per-request timeout; this is a received404, not an overall step timeout. The existing exception loses URL identity. No retry-count/deadline change is justified.

## Current exact-URL diagnosis and minimal repair

Official CDN only. All134 runtime URLs plus1 metadata URL were probed with HEAD, independently via urllib and curl. Each method found exactly the same3 HTTP404s; all132 other URLs returned200. Full result files retained. Live observation date2026-10-25 is distinct from original run2026-10-22; neither current HEAD nor the old stack uniquely identifies the original first failing package.

Official index: `https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/APKINDEX.tar.gz`, complete saved original in `ci-original/main-x86_64-APKINDEX-original.tar.gz`.

| Old pin (404) | Current official replacement | SHA256 verified APK |
| --- | --- | --- |
| libexpat-2.8.5-r0.apk | libexpat-2.9.0-r0.apk | 4c9fb621d455d12475767e261821a4be7b4e91e2ba19c20a02e7fa99189b0d6d |
| libpcre2-16-10.46-r0.apk | libpcre2-16-10.46-r1.apk | 63fb6e08815d6b72bc2792cce9fb8dd5b1c5952e0624915ca8324919dd50e497 |
| pcre2-10.46-r0.apk | pcre2-10.46-r1.apk | e0e0e3c5f3b4a37a77799a7367bb40dd38596be55f56f3bc60f2cb4b584f842c |

All URLs retain `https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/`. No mirror guessing, resolver run, unpinned package, or package-set expansion. All bootstrap/Qt/QSB6.8.3/tool_metadata fields stay byte-for-byte semantically unchanged; the raw lock hash changes and invalidates existing installed marker naturally.

The previous4140 minimal expat fix covered signed x86_64 base+desktop locks but not this separate host-QSB lock. PCRE2-r0 remains present in BOTH signed x86 locks too; reported to parent for separate owner/review. After this finding, parent explicitly approved matching PCRE updates in both locks; exactly those two URL/hash pairs per lock are changed, their already-reviewed expat2.9 pin unchanged.

## Replacement verification and native-only tests

Files under `/data/qsb-pin-review`:

- Native x86 Alpine3.22.6 minirootfs SHA256 checked against exact existing trusted bootstrap constant `27694aaa55fd7a9e3ef596e0ad4eb66802308bb20172b17030cd5f4d8ae9bac2`.
- Its native musl loader ran packaged `apk --keys-dir .../etc/apk/keys verify` on only the three replacement APKs: all `0 - OK`. Log `apk-signature-verification.log`.
- SHA256 computed from those downloaded and signature-verified files before editing.
- Existing `python3 scripts/host-qsb-host-tests.py`: baseline7/7 pass; patched7/7 pass. These are negative/offline installer controls, not a complete signed installation, shader compilation, guest rendering, or source build.
- Minimality check asserts unchanged package count, unchanged non-package fields, exactly3 changed entries, only `{url,sha256}` keys in each change. `git diff --check` passes.
- No hardcoded-pin acceptance rule was added. Parent approved a narrow diagnostic helper change: on the existing third/final exception, `error.add_note("Download failed: " + url)` immediately precedes the unchanged bare raise. Six mocked native controls assert same exception object/type, three attempts, sleeps1+2, timeout60, partial cleanup, preserved old-cache bytes, failure/offline behavior and recovery without a failure note. Exact baseline has three expected missing-context failures; patched six pass. No networking or package trust acceptance was loosened.

## RISC-V success scope and cache caveat

Original job119907512820 succeeded at the exact same head/run attempt. Metadata records success for source userland, kernel/ISO, boot tests, uniprocessorM25, MLFQM25; x86-only host regressions and eight-CPU steps skipped. Original log1469 records source QML/Quick candidate installation; line4228 `qttest: PASSED (31 checks,0 failures)`.

Original line1007 restores `dl-riscv64-ff4c08f054458979d8f275cb90c8672bd772a4b5b479cf66387cd71503189f1f`. ARM separately restored `dl-aarch64-ff4c08f054458979d8f275cb90c8672bd772a4b5b479cf66387cd71503189f1f` (about299MiB). RISC-V lines1228,1240,1283 verify the exact old expat/PCRE pins, and installer completes. The generic downloader bypasses networking for an existing matching cached file. This makes cache masking a supported explanation, but original logs do not distinguish each cached file from fresh downloads: do **not** claim definitively that any specific APK was cached. RISC-V success does not establish current cold download availability or explain away ARM failure.

No all-CI, desktop M40, native/signed/cold integration, readiness, accessibility, terminal evidence, or merged acceptance claim is made.

## Parent review handoff

Review one-file patch and the three replacement trust proofs. Independent reviewer should reproduce official URLs/hashes/signatures and check `git diff --check`, exactly3 entry changes, Qt6.8.3+metadata unchanged, native7 controls. Parent approved local commits only after independent reviews; separate final approval/credential is still required for API publication. A future full remote source build would be required to validate the repaired cold path, but no run was dispatched here. Independent x86 host-control investigator retains their own failure ownership.


## Final integrated native freeze and independent reviews

Source full patch SHA256 `08c298b82fe6d1df63a1d8f348d142130e683551817c2d283c2a3124d37a5107` independently reviewed by peer: exact3+2+2 JSON changes, official replacement bytes/hashes, helper semantics and native6+7+10 controls all PASS. Peer did not repeat this computer's signed-install/shader smoke, correctly attributed separately.

Reverse independent review here accepted HOST patch SHA256 `26893a52bf59da0a3def62843e731dcf4b4b8197baac4f2c131ba0caa2547304` and final APT patch `5700ed053b0d5019e56180b30e14788ae4881344a7ab59690cd78e64ea951e75`. HOST removes only exact explicit od/tr OR-list status handlers in mutants, preserves utility bytes and demonstrates genuine semantic rejection; no production edit. APT options are request timeouts30s (HTTP+HTTPS), retries2 on update/install, strict update Error-Mode=any. Exactly six APT command lines in three existing setup blocks change options; existing package arguments and downstream sequences remain. Two x86 host gates each wire both new tests exactly once. Final APT harness executes extracted workflow commands under `/bin/bash -e -o pipefail -c` with recording mocks. These are per-request transport limits, NOT an absolute setup deadline or demonstrated real network recovery.

Actual native signed QSB bootstrap completed successfully: all135 runtime/metadata APK signature checks logged OK, signed package installation succeeded, `qsb --version` is6.8.3, two tiny real vertex/fragment GLSL shaders baked with `--qt6` and dumped successfully (vertex12 variants). Installed `host-qsb.py --offline` reuse succeeded. This is prebuilt native host tooling only, no Qt library compilation or target/guest proof.

Combined commands on this integration computer (all PASS; logs in `combined-tests/`):
- `python3 scripts/apt-host-setup-host-tests.py`:2 test methods,12 setup-command scenarios.
- `python3 scripts/alpine-download-host-tests.py`:6 tests.
- `python3 scripts/host-qsb-host-tests.py`:7 tests.
- `python3 scripts/alpine-host-tests.py`:10 tests.
- `HISTORY_TEST_SHELL='/bin/bash' python3 scripts/terminal-history-status-host-tests.py`:8 tests.
- `HISTORY_TEST_SHELL='/bin/bash --posix' python3 scripts/terminal-history-status-host-tests.py`:8 tests.
- `python3 scripts/terminal-history-transport-host-tests.py`:13 tests.
- `python3 scripts/terminal-attempt-history-host-tests.py`:13 tests.
- `python3 scripts/terminal-history-utility-status-host-tests.py`:2 tests; intentionally preserves historical F1 witness, not a repaired-production failure.
- `git diff --check HEAD`:clean before local commits.

Native dash is unavailable on this AmazonLinux computer; peer independently ran8 controls under dash0.5.12. Do not attribute those executions to this computer. Production `userland/alpine/desktop-overlay/usr/bin/alpine-plasma-probe` SHA256 before/after remains `c34251ccea482ebf23283e712ac08a0abbb63d032bb07beb6eb8c33a1170ab1a`.

Exactly nine product files: three locks; `userland/alpine/build.py`; `scripts/alpine-download-host-tests.py`; `scripts/terminal-history-status-host-tests.py`; both `.github/workflows/{alpine,release}.yml`; `scripts/apt-host-setup-host-tests.py`.235 insertions,29 deletions. Full original logs, ZIPs, signed APK bytes and large artifacts are NOT product changes.

Explicitly authorized local logical commits appended to4140:
1. `313a4be1216fd7a7d098dbf8e299dd8ae098202d`, tree `b836a3980d24c8b6e2218b806b7d88f2c7e997fd`:source pins/helper/native controls.
2. `0d81a28caa1b9bd2225a02015e5b2003b90f3955`, tree `9638455c44eb05f197b1fea6f7d7a4417ccabe7a`:host mutation fixture portability.
3. `e22f0ebc5b201eb1a57e318605a4df2465ae1834`, tree `3dd5e9909b9cdcef781b08e4ed0ff04978fea7fc`:APT workflow plumbing/native mock controls.

## Additional original evidence and metadata provenance

Peer supplied a public URL+SHA map for six signed-run original job logs, original source-x86 log and four signed-run artifact ZIPs. This computer independently retrieved ALL11 payloads; each is byte-for-byte identical to the peer's recorded SHA256 and size. Full originals preserved in `signed-ci-original/` with map and acquisition results. The source-x86 log belongs to source run39802683577; the other six logs and four artifacts belong to signed run39802683128.

Eight fresh REST response bodies differed bytewise in formatting from peer snapshots. Preserve each raw response under `*.new-snapshot.json`. For ALL8, compact serialization of the parsed response with `json.dumps(...,separators=(',',':'),ensure_ascii=False)` and no newline exactly reproduces the peer SHA/size. These separately saved `*.peer-minified-reconstruction.json` are **locally reconstructed equivalents**, not this computer's raw response bytes. Peer confirms their original compact bodies were saved directly from urllib responses; GitHub may vary formatting by request/client. No semantic-content difference was observed; no cross-fetch raw-byte metadata identity is claimed. Explicit mapping in `rest-serialization-provenance.json` retains both originals and reconstructions.

Separate never-merge evidence branch is based directly on4140 and will preserve diagnostic job logs/test ZIPs/REST snapshots/manifests/review logs, without merging them into product. No build ISO or657MiB native runtime is necessary for this diagnostic branch. Original failing attempts and all cold/integration holds remain unchanged. Ordinary CI on newly published code, if subsequently authorized, is distinct from rerunning old attempts.
