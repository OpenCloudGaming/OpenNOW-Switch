#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cxxflags=(-std=c++20 -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer
    -DUSE_GL_RENDERER -Iapp/src -Itests/stream_stubs
    -Iextern/borealis/library/include/borealis/extern
    -Iextern/borealis/library/include/borealis/extern/nanovg)
if [[ -n "${OPENNOW_SANITIZERS:-}" ]]; then
    cxxflags+=("-fsanitize=$OPENNOW_SANITIZERS")
fi
"${CC:-cc}" -Iextern/borealis/library/include/borealis/extern \
    -c extern/borealis/library/lib/extern/glad/glad.c -o "$out/glad.o"
"${CXX:-g++}" "${cxxflags[@]}" tests/gl_renderer_reconfiguration_test.cpp \
    app/src/stream/OpenGL/GLVideoRenderer.cpp "$out/glad.o" \
    -lavcodec -lavutil -lEGL -ldl -o "$out/test"
for scenario in resize color p010; do
    "$out/test" "$scenario"
done
