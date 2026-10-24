# FTP review fixes (jx-21)

## Finding and change

Based on main `0fc5c01836`, first cherry-picked `8d9079ee68` as `95d7cfe35d`
(the FTP server port). The follow-up fixes four issues in that server:

- Data EOF alone previously published a short `.part` over the destination.
  STOR/APPE now require a single-use ALLO byte count. The production completion
  gate requires an exact count and a live transfer, then flushes/commits/closes
  before replacement. Rejection/commit failure removes the owned partial and
  preserves the destination. Existing ABOR, stop and read-error cleanup remains.
  APPE counts only newly received bytes. SIZE is an existing-file query, not a
  trustworthy declaration; FTP has no final client acknowledgement of `226`.
  Unknown-length uploads are deliberately rejected (`503`). Default clients
  without ALLO support must be adapted; see the documented ftplib example.
- A blocked control read previously held the only session indefinitely. A
  monotonic deadline allows 30 seconds to authenticate and 300 seconds idle
  afterward, checked while pending reads wait in 250 ms intervals. Each WinRT
  read installs one Completed delegate and polls a standard future, avoiding
  repeated completion-handler registration on the same operation. Complete
  authenticated commands/data progress renew idle time; unauthenticated traffic
  and partial commands cannot renew login time. Expiry closes sockets and joins
  the transfer, using the same cleanup path as disconnect. Slow filesystem/WinRT
  cancellation can extend observed shutdown latency and needs console validation.
- The fixed session-local two-second login delay now grows per peer IP:
  1/2/4/8/16/30 seconds. State belongs to the server and survives reconnect and
  stop/start, but not app exit. Stop interrupts the delay; login expiry takes
  precedence. The map retains at most 1024 peers without eviction; new peers
  beyond capacity get 30 seconds, so churn cannot evict a penalty.
- RNTO refuses an existing target outside games (`550`). Its final move never
  replaces outside games, including a target created after the existence check.
  Explicit DELE first permits the rename. Games retain replacement behavior.

These are direct safety fixes without an environment switch, new dependency,
UI/catalog change, package version change or renderer change. No upstream work,
push, network, console, PC app or reference-repository mutation was performed.

## Host evidence (24 October 2026)

Before: standalone FTP suite **2/2 passed**; `storage-path`, `text-entry` and
`localization-catalogs` all compiled and passed on macOS. Jadyx has no
`tests/port` Python suite; the NXbox-specific unittest command does not apply.

The new `ftp-safety` test was added before the fix. With the previous EOF-only
publication condition extracted into the production gate, it compiled and
failed `assert(!published)` for an unproven upload; the original two tests passed.
After the fix the suite passes **3/3** with `-Wall -Wextra -Werror -Wconversion
-Wshadow`. CMake retains MSVC `/W4 /WX /we4456 /we4457 /we4459 /we4244`.

New coverage uses fake monotonic times (no 30/300-second sleeps), per-IP reconnect
penalties/capacity, allocation parsing through uint64 max and overflow, missing/
short/excess/zero/matching byte counts, cancellation and rename policy. Six real
host-filesystem transaction scenarios verify original/new contents and partial
removal, including clean early EOF, absent size, control loss/ABOR, commit failure,
replacement failure and successful publication. Transfer tests still exercise
12 GiB + 37 bytes with a bounded 1 MiB buffer. The safety and transfer tests also
pass AddressSanitizer/UBSan. The three pre-existing macOS gates pass again.

```sh
cmake -S tools/xbox/tests/ftp -B /tmp/jadyx-jx21-ftp-tests
cmake --build /tmp/jadyx-jx21-ftp-tests
ctest --test-dir /tmp/jadyx-jx21-ftp-tests --output-on-failure
clang++ -std=c++20 -Wall -Wextra -Werror -Wconversion -Wshadow \
  -fsanitize=address,undefined -Isrc tools/xbox/tests/ftp-safety.cpp \
  -o /tmp/jadyx-jx21-ftp-safety-asan
/tmp/jadyx-jx21-ftp-safety-asan
```

## Exact console test

Follow [FTP console gate](nxbox-ftp-server.md#exact-console-gate) and its
[review regression gate](nxbox-ftp-server.md#review-regression-gate-jx-21), including
explicit ALLO before STOR/APPE. Require matching hashes/no partial after short
clean EOF, control/data disconnect, ABOR, stop and suspend; exact-size publication;
ALLO consumption/zero/append; 31-second unauthenticated and 301-second idle session
release; authenticated keepalive and progressing-transfer renewal; per-IP
reconnect penalties; protected RNTO, explicit DELE retry and games replacement.
The documented commands build the normal `build-uwp/package/jadyx.appx` on the
Windows build machine for installation in Series Dev Mode/Game mode.

These console checks and the WinRT/MSVC build remain **pending**; portable host
policy/transaction tests do not exercise Windows socket cancellation or FromApp APIs.
