# Independent musl/dev pair provenance review

## PASS: narrowly scoped expanded maintenance contract
The supplied actual raw metadata supports implementing musl+musl-dev1.2.5-r13 together in base and desktop, while leaving host-QSB muslr12 and bootstrap unchanged. No observed additional exact BASE dependency constraint requires another companion. This is not full closure, solver, installed-root, ABI-equivalence, desktop or guest integration clearance.

## Transfer/input binding
Reconstructed two supplied ASCII chunks (46544 characters each); chunkSHA256 b0243100072eb39b6ba8d0abf0ec91deb305b9eb027e0d62350e443e824f5912 and b7c2e687c5748bae526b418f8cbbc08f16dfad6b4041f08352d40c16b444f549 independently matched. Whole69816-byte metadata archive SHA25622e9dbb36de98ef4bcdac5c9debd5dd6546e6d5883597e92f78fc4d34a0e90fc matched. Safe extracted32 records, no APK/minirootfs/ELF payload. No repeat downloads/native verification by A.

## 159 retained BASE inputs
Independently matched all159 unique supplied package SHA256 to exact c05 base lock with already reviewed c-ares replacement; missing only musl-dev1.2.5-r12. Reparsed every full raw PKGINFO and confirmed supplied parsed fields. Native verification record has159 corresponding filenames,159 :0-OK outputs,rc0,empty stderr with reviewed private verifier/key manifest. Executions attributed B, not A cryptographic re-verification from metadata-only packet.

Independent scan of complete159 dependency ledgers found no musl/musl-dev version-qualified dependency; libbsd has unversioned 'musl', other musl edges use SONAME. Old cached muslr12 PKGINFO provides so:libc.musl-x86_64.so.1=1. Old musl-dev PKGINFO absent; this absence is not a blank or inferred constraint. The159 includes old musl to be replaced: an intended new160 closure uses158 retained package inputs plus2 verified new candidates, not159+2.

## Pair provenance and identity
Existing stable-main index SHA86cfe402b7968736024b67ca1d1334ae5824371822ae803e271af9675d280f3b selected pair records exactly match complete native-generated records (including C/P/V/A/S/I/D/p and other fields). Same build commit ddaf59b84bc6983a37d6e24d2210c2c4791642bb, version1.2.5-r13, x86_64.

- https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/musl-1.2.5-r13.apk:412387B, SHA359890bc7c4dc5fee7a85192de60a471c987e4664767288578beca60a44db2c8; HTTP200 oneGET2026-10-28T06:27:44.323749Z to.346284Z, effective official URL unchanged. PKGINFO no dependencies, provides so:libc.musl-x86_64.so.1=1. Readelf ELF64 x86-64 DYN with SONAME libc.musl-x86_64.so.1 and no NEEDED; relative libc symlink resolves packaged loader. Same old/new SONAME/provider is not whole ABI-equivalence proof.
- https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/musl-dev-1.2.5-r13.apk:3428787B, SHA2ca496dd68c0b340b50cf9940bc2f3312eac8b56ec2d3284a621be6f0c09ffb1; HTTP200 oneGET06:27:44.398366Z to.425243Z. PKGINFO EXACT requires musl=1.2.5-r13, provides libc-dev=1.2.5-r13. Five crtELF files are x86-64 REL objects; relative usr/lib/libc.so resolves ../../lib/ld-musl-x86_64.so.1.

Two-candidate native verify rc0,both0-OK,empty stderr; generated index rc0,empty stderr, no allow-untrusted. Reviewed five-key/private apk manifest exactly matches prior c-ares packet. Native execution and full-index selection/companion scan attributed B: A has selected index records, not full signed-index/APK bytes for cryptographic re-verification or independent full-main scan. Current MAIN exact companion scan reports only dev->muslr13. This does not cover all desktop frozen packages/community or establish solver success.

## Conditional clearance issued to B
Exactly same4productpaths as reviewed c-ares change. Three c-ares pairs unchanged from prior review; add only four musl/dev URL/SHA pairs across base+desktop (seven pair replacements total). Host-QSB muslr12/bootstrap unchanged. Existing host suite guard enforces intended3c-ares entries and2pairedprofiles, missing/duplicate/URL/SHA/version-relation negative controls. No helper/workflow/security/deadline changes.

Parent separately authorizes B only ONE new strict OFFLINE BASE160 assembly from retained verified assets/fresh absent target, no newGET. Stop any new constraint/other pin need/metadata failure. Review actual expanded diff, raw tests/mutants and native proof before commit; no publication clearance here. A performs no duplicate install. Full desktop/QSB integration and milestone holds remain. Historical olddevHTTP200 matching proof preserved alongside current404; neither determines permanent retirement cause. Current possession of olddevbytes not established.
