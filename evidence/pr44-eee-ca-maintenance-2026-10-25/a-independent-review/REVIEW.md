# A independent review — PASS

Exact reviewed patch SHA256: e283abd9c1193b52019c28de5f0f601ce03193af6b0175d5588e63d9cc5aa26e (5691 bytes).
Product parent: eee711900dde4e748e7e76ed45526669d33b96f7.
Product commit: 1fbeb9a18b3529866856a04f327ac2e37eff17c0.
Product tree: 80082c3c04353a0a982d843a698c3c13c87900df.

A independently checked the exact four-file diff against that parent. Each of three JSON locks changes only one CA URL/SHA pair. The host-test extension compares unique CA entries with the unchanged reference and tests URL, SHA, absent and duplicate entries. Builder, reference/kernel code, solvers, workflow, flags, deadlines and other package bytes are unchanged.

Already-completed independent isolated host tests: Alpine12, download18, QSB7 and NEW11418 PASS. All logs retained. The initial NEW114 log has one setup error because A's fixture omitted .github; the corrected exact-parent .github fixture passes all18 in independent-reference-tests.log. The setup error is not a product defect and is not counted as a semantic negative control. Independent ignore-URL, ignore-SHA, allow-duplicate mutants produce 3,3,4 assertion failures, zero errors. B's genuine old-pin baseline and isolated three-old-lock rejection results are retained separately.

## Supplemental recorded-evidence review

A reconstructed the installed DB from four chunks: 350136 bytes, SHA256 401f3c63a3f5b6c01882e3a313d54e6c91120cfbe78b68ecd03fd5dc1224cf30.
Five supplemental chunks reconstruct a182390-byte TAR_GZIP, SHA256 14dd3615a99c70ccfb4f7f088a834bf9a41e698d7966c37dcdef78e686dd433a.
The actual file manifest is922236 bytes, SHA256 65dd2097aed9919ba05ea311fdf1d9226406b008d40464496e063a53e707e37d, with5477 distinct paths.
Bootstrap DB15306 bytes, SHA256 74c01562aa75d8dd6dbb5c8b30f2d2d73c7d445146335962cc68930d751ddab1.

A independently parsed unique package names and exact versions: 160 locked plus8 bootstrap-only =168 installed, from16 bootstrap records with8 overlapping locked records. The5477-path manifest binds the exact installed DB SHA. All installed architectures are x86_64 or noarch. CA installed version20261020-r0, archive size107116, installed size178791, provider ca-certificates-cacert=20261020-r0 and source commit ce7bff87478cd936f5b7031ce822b02d4fdd313a agree.

A inspected both validators. Original verify-native-evidence.py is B's capture script, which reads B's native root and writes capture outputs; A did not execute it. NEW review-retained-evidence.py only reads retained evidence. A executed that portable validator once: PASS recorded160-input identities,168 exact union,5477 entries, CA payload hash/symlink, BusyBox04755, recorded Sway capability,160 recorded signature-OK count. This is internal recorded-evidence consistency, not a new root/signature/xattr observation. native-root-manifest.json is the generated160-package lock manifest, not the full file census; native-root-files-manifest.json is the actual5477-entry census.

B's ONE native BASE160 assembly and live signature/security observations remain attributed to B. A did not repeat installation, compile targets, boot a guest or run CI. Local BASE coherence passes on that recorded native evidence. Desktop593/QSB134 integration CI remains pending; unchanged strict solvers may still fail closed. CDN cause remains unknown; global retirement is not proved. Original x86 base-acquisition failure remains a failure; NEW114 guest/reference stage was not reached there. No M40, cold-boot, strace137 or guest reliability hold is lifted.

No APK, mini-root tarball or installed-root payload is vendored. Only metadata, logs, manifests, capture/validation scripts, patches and evidence archives are included. This append does not alter older evidence. The historical B implementation note saying no commit had been made describes capture time; the authorized local product commit above occurred afterward.
