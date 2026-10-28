# Independent c-ares maintenance review

## Decision: PASS — narrowly scoped four-path maintenance contract
Independent review of B's retained raw metadata packets found no observed compatibility blocker to implementing the three identical c-ares URL/SHA replacements plus the existing host-test guard. This is implementation clearance, not full frozen-closure, boot, desktop or host-QSB integration clearance.

## Evidence inspected
- Original metadata packet SHA256 45e4d04fc881d806f43f023dc4d57eb6b83176bccf0d0f48d074e5da49c0449c; supplement SHA256 7dc04590a7e6b9ce4a27bc20a28b0830403ec82cc0d48360bdb7041483f3a12f. Both independently recomputed.
- Raw HTTP status/header/provenance records, verifier/key manifest, native verification commands and results, selected signed-index record, native generated-index record, PKGINFO, ELF readelf, members/symlink inventory, dynamic symbols and current-main reverse-dependency records.
- B native index and candidate verification each rc0 with pinned private apk and five-key trust store, --no-network, no allow-untrusted. These executions are attributed to B. Packets exclude full signed-index/APK bytes; A did not cryptographically reverify or independently rescan the full index.
- Selected signed-index and generated candidate index records match exactly after trailing whitespace: name c-ares; 1.34.9-r0; x86_64; 111865 bytes; checksum Q11WDonk2bTpU63hLS3JdpXivBAcU=; musl SONAME dependency; libcares.so.2=2.19.8 provider. PKGINFO agrees on identity, dependency and provider.
- Candidate URL https://dl-cdn.alpinelinux.org/alpine/v3.22/main/x86_64/c-ares-1.34.9-r0.apk; SHA256 9b23aa07901ed64bde0be7ec544f8d2b29de80d996277dc861218c54fff8bc97.
- ELF metadata: ELF64 little-endian x86-64 DYN, SONAME libcares.so.2, only NEEDED libc.musl-x86_64.so.1. Relative libcares.so.2 symlink resolves to packaged libcares.so.2.19.8.
- At product c05a3fcd851297161f3981b38dd04900c49e8467 exactly one old c-ares pin in each of base160, desktop593 and host-QSB134 locks. None includes c-ares-dev or curl-dev. All contain frozen libcurl8.14.1-r3 and musl1.2.5-r12.
- Current-main reverse metadata requests libcares.so.2 for runtime consumers. Exact c-ares version coupling is in current c-ares-dev, absent from the three locks. This does not independently establish requirements of older frozen consumers.
- Existing host-QSB raw-lock identity invalidates the cached tool stage when its lock changes; no cache-helper change is required.

## Limits preserved
Exact locked older libcurl/musl are absent from current index; old package metadata and full frozen reverse closure unavailable. No assertion that old/new ABI symbol sets are identical. Single-candidate index warning about missing musl provider is preserved and is not solver success. Desktop593 and QSB134 integration remain pending. Original old-pin HTTP404 evidence and unknown historical CDN cause remain unchanged. All milestone/cold/typing/Dolphin holds remain.

## Authorized implementation contract
Only userland/alpine/locks/x86_64.json, userland/alpine/locks/x86_64-desktop.json and userland/ports/host-qsb-lock.json: replace the one c-ares URL/SHA pair in each with reviewed candidate. Only scripts/alpine-host-tests.py: specific three-lock reviewed candidate consistency guard, with negative URL/hash/missing/duplicate cases across positions. No companion pins, oracle lock, cache helper, deadlines, acceptance changes or verification bypass. B's separately authorized single strict fresh BASE160 assembly must stop on additional missing package or verification failure; A will not repeat it. Review actual diff and tests/mutants independently before any publication.
