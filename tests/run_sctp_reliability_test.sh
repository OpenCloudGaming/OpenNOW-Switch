#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp -R "$root/extern/libpeer/third_party/usrsctp" "$work/usrsctp"
python3 - "$work/usrsctp" <<'PY'
from pathlib import Path
import sys

for name in ("user_malloc.h", "user_socketvar.h"):
    path = Path(sys.argv[1]) / "usrsctplib" / name
    source = path.read_text()
    assert source.count("#define __SWITCH__ 1\n") == 1
    path.write_text(source.replace("#define __SWITCH__ 1\n", ""))
PY

flags=(-g -fno-omit-frame-pointer)
test_source="${1:-sctp_reliability_test.c}"
link_flags=()
if [[ "$test_source" == sctp_setup_diagnostic_test.c ]]; then
  link_flags+=(-Wl,--wrap=usrsctp_setsockopt,--wrap=usrsctp_connect)
fi
if [[ -n "${SCTP_TEST_SANITIZERS:-}" ]]; then
  flags+=("-fsanitize=$SCTP_TEST_SANITIZERS")
fi

cmake -S "$work/usrsctp" -B "$work/build" \
  -Dsctp_build_programs=OFF -Dsctp_debug=OFF -Dsctp_werror=OFF \
  -Dsctp_inet6=OFF -DCMAKE_BUILD_TYPE=Debug \
  "-DCMAKE_C_FLAGS=${flags[*]}"
cmake --build "$work/build" -j "${SCTP_TEST_JOBS:-4}"
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror "${flags[@]}" \
  -DCONFIG_USE_USRSCTP=1 \
  -I"$root/extern/libpeer/src" -I"$work/usrsctp/usrsctplib" \
  "$root/tests/$test_source" "$root/extern/libpeer/src/sctp.c" \
  "$work/build/usrsctplib/libusrsctp.a" -pthread -Wl,--wrap=gettimeofday "${link_flags[@]}" \
  -o "$work/sctp_reliability_test"
"$work/sctp_reliability_test"
