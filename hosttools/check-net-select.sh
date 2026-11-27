#!/usr/bin/env bash
# check-net-select.sh [BUILD_DIR]: rev 38 network-wait and crash-log checks, offline (host + devkit image).
#  H1 host: SelectPoll.h (__wrap_select) against the host's poll(): fd 63, nfds 64, no-fd sleep, errors,
#     POLLERR readable/writable, failed non-blocking connect wakes the waiter, closed fd -> EBADF.
#  H2 host: ThreadRecords.inc (rev 38f thread records) on host pthreads: pthread_t reuse after an out-of-wrapper
#     join, self-detach with a stale record, detach of an unrecorded thread never joined, ordinary reaping,
#     two launches in one process (reaper stopped + joined at exit, the next launch's own reaper reaps).
#  S1 libounce.a socket.o select still refuses nfds > 63 (`cmp w0, #0x3f; b.hi` -> EINVAL): the cause.
#  S2 libcurl.a curl_multi_fdset still drops sockets > 63 (`cmp w6, #0x3f`): why Kodi must not select.
#  S3 <build>/.../CurlFile.cpp.obj calls curl_multi_wait and no select.
#  S4 kodi.bin links --wrap=select and defines __wrap_select.
#  S5 CrashHandler.cpp.obj sends its own IPC (svcSendSyncRequest: crash.txt session cloned from fsGetServiceSession).
# Exit 0 = all PASS.
set -u -o pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
B="${1:-build}"
S="${TMPDIR:-/tmp}/kodi-netcheck.$$"
mkdir -p "$S"
trap 'rm -rf "$S"' EXIT
fail=0
ok() { echo "PASS $*"; }
no() { echo "FAIL $*"; fail=1; }

if g++ -std=c++17 -Wall -Wextra -O1 -o "$S/selpoll" "$HERE/hosttools/tests/select_poll_test.cpp" && "$S/selpoll" > "$S/h1.txt"; then
  ok "H1 host select-over-poll test ($(grep -c '^PASS' "$S/h1.txt") cases)"
else
  cat "$S/h1.txt" 2>/dev/null; no "H1 host select-over-poll test"
fi

if g++ -std=c++17 -Wall -Wextra -O1 -pthread -o "$S/threc" "$HERE/hosttools/tests/thread_records_test.cpp" && timeout 60 "$S/threc" > "$S/h2.txt"; then
  ok "H2 host thread-records test ($(grep -c '^PASS T' "$S/h2.txt") cases)"
else
  cat "$S/h2.txt" 2>/dev/null; no "H2 host thread-records test"
fi

out=$(podman run --rm --security-opt label=disable -v "$HERE:/k:ro" -v "$S:/w" -w /w kodi-switch2-devkit bash -lc "
  set -e; T=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
  \$T-ar x /opt/devkitpro/libounce/lib/libounce.a socket.o
  \$T-objdump -d --no-show-raw-insn -j .text.select socket.o | sed -n '/<select>:/,/b.hi/p' | grep -c 'cmp\s*w0, #0x3f' || true
  echo ---
  \$T-ar x /opt/devkitpro/portlibs/switch/lib/libcurl.a libcurl_la-multi.o
  \$T-objdump -d --no-show-raw-insn libcurl_la-multi.o | awk '/<curl_multi_fdset>:/,/^\$/' | grep -c 'cmp\s*w6, #0x3f' || true
  echo ---
  \$T-nm -u /k/$B/build/filesystem/CMakeFiles/filesystem.dir/CurlFile.cpp.obj | awk '{print \$2}' | grep -E '^(select|curl_multi_wait|curl_multi_fdset)\$' || true
  echo ---
  \$T-nm /k/$B/kodi.bin | awk '\$3==\"__wrap_select\"{print \$2}'
  grep -c -- '--wrap=select' /k/$B/build.ninja || true
  echo ---
  \$T-nm -u /k/$B/build/platform/switch2/CMakeFiles/platform_switch2.dir/CrashHandler.cpp.obj | awk '{print \$2}' | grep -E '^(svcSendSyncRequest|fsGetServiceSession)\$' || true
" 2>&1) || { echo "$out"; no "devkit container run"; }
IFS=$'\x1d' read -r -d '' s1 s2 s3 s4 s5 < <(printf '%s' "$out" | awk 'BEGIN{RS="---\n"; ORS="\x1d"} {print}'; printf '\0')
[ "$(echo "$s1" | tr -d '[:space:]')" = 1 ] && ok "S1 libounce select refuses nfds > 63 (cmp w0,#0x3f)" || no "S1 libounce select bound not found: '$s1'"
[ "$(echo "$s2" | tr -d '[:space:]')" -ge 1 ] 2>/dev/null && ok "S2 curl_multi_fdset drops sockets > 63 (cmp w6,#0x3f)" || no "S2 curl_multi_fdset bound not found: '$s2'"
if echo "$s3" | grep -qx curl_multi_wait && ! echo "$s3" | grep -qx select && ! echo "$s3" | grep -qx curl_multi_fdset; then
  ok "S3 CurlFile.cpp.obj: curl_multi_wait, no select/curl_multi_fdset"
else no "S3 CurlFile.cpp.obj undefined symbols: $(echo $s3)"; fi
if echo "$s4" | grep -qx T && [ "$(echo "$s4" | grep -E "^[0-9]+$" | head -1)" -ge 1 ] 2>/dev/null; then
  ok "S4 kodi.bin: __wrap_select defined, --wrap=select on the link line"
else no "S4 __wrap_select/--wrap=select: $(echo $s4)"; fi
if echo "$s5" | grep -qx svcSendSyncRequest && echo "$s5" | grep -qx fsGetServiceSession; then
  ok "S5 CrashHandler.cpp.obj: own IPC (svcSendSyncRequest) on a session cloned from fsGetServiceSession()"
else no "S5 CrashHandler.cpp.obj: $(echo $s5)"; fi
exit $fail
