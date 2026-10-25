# Independent reverse review: HOST26893 and FINALAPT5700

Reviewed decoded patches before application. Both gzip payloads verified against independently supplied full SHA256. `git apply --check` clean on integration checkout with the separately approved source changes. No file overlap.

HOST: exact one guard match required per od/tr utility. Removing explicit guard via OR `:` leaves bytes intact; existing output decoder and genuine semantic assertion kill mutants. No production probe edited; SHA256 remains c34251ccea482ebf23283e712ac08a0abbb63d032bb07beb6eb8c33a1170ab1a. Independent integration Bash/Bash POSIX8+8 tests and adjacent13+13+2 tests PASS. Dash not installed here; peer's dash8 result remains attributed to peer.

APT: actual workflow diff alters only options on6 apt commands and adds2 tests once to each existing x86 host gate. Package arguments, order, QEMU/fetch scripts and other steps unchanged. HTTP/HTTPS30s and retries2 are per-request bounds, not overall setup bounds. Strict update Error-Modeany prevents partial-index success from masking refresh failure. New harness extracts real literal run blocks; final interpreter matches `/bin/bash -e -o pipefail -c`. Recording mocks prove update100 and partial-index100 prevent install/QEMU/fetch, install101 prevents QEMU/fetch, success keeps sequence.2 methods/12 scenarios PASS here. Peer reports baseline exact4140 produces14 intended failures; this review does not convert mocks into a real network-recovery claim.

Verdict PASS, bounded scope only. No merge, old-attempt rerun, acceptance weakening, deadline change or cold/guest proof. All prior holds retained.
