#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp -R "$root/extern/libpeer/third_party/usrsctp" "$work/usrsctp"
sed -i '/^#define __SWITCH__ 1$/d' "$work/usrsctp/usrsctplib/user_malloc.h" \
  "$work/usrsctp/usrsctplib/user_socketvar.h"

flags=(-g -fno-omit-frame-pointer)
if [[ -n "${SCTP_TEST_SANITIZERS:-}" ]]; then
  flags+=("-fsanitize=$SCTP_TEST_SANITIZERS")
fi

"${CC:-cc}" -std=c99 "${flags[@]}" -include "$root/tests/usrsctp_no_byte_order.h" \
  -DINET -DHAVE_STDATOMIC_H -DHAVE_SYS_QUEUE_H -DSCTP_PROCESS_LEVEL_LOCKS \
  -DSCTP_SIMPLE_ALLOCATOR -D__Userspace__ -ffunction-sections -fdata-sections \
  -I"$work/usrsctp/usrsctplib" -c "$work/usrsctp/usrsctplib/netinet/sctp_crc32.c" \
  -o "$work/sctp_crc32.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "${flags[@]}" "$root/tests/usrsctp_crc32c_test.c" \
  "$work/sctp_crc32.o" -Wl,--gc-sections -o "$work/usrsctp_crc32c_test"
"$work/usrsctp_crc32c_test"
