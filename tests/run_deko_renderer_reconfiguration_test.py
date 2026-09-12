#!/usr/bin/env python3

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
sources = (
    root / "app/src/stream/IVideoRenderer.hpp",
    root / "app/src/stream/deko3d/DKVideoRenderer.hpp",
    root / "app/src/stream/deko3d/DKVideoRenderer.cpp",
)
with tempfile.TemporaryDirectory(prefix="opennow-deko-test-") as output:
    directory = Path(output)
    header = '#include "deko_renderer_stubs.hpp"\n'
    for source in sources:
        header += f'\n#line 1 "{source}"\n'
        header += "\n".join(
            "" if line.lstrip().startswith("#include") else line
            for line in source.read_text().splitlines()
        ) + "\n"
    (directory / "deko_renderer_under_test.hpp").write_text(header)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libavutil", "libavcodec"], text=True
    ))
    command = shlex.split(os.environ.get("CXX", "g++")) + [
        "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
        "-DPLATFORM_SWITCH", "-D__SWITCH__", "-DBOREALIS_USE_DEKO3D",
        "-I" + str(directory), "-I" + str(root / "tests"),
        "-I" + str(root / "app/src"),
        str(root / "tests/deko_renderer_reconfiguration_test.cpp"),
        "-o", str(directory / "test"),
    ]
    if sanitizers := os.environ.get("OPENNOW_SANITIZERS"):
        command += ["-O1", "-fno-omit-frame-pointer", "-fsanitize=" + sanitizers]
    command += shlex.split(os.environ.get("CXXFLAGS", "")) + flags
    subprocess.run(command, check=True)
    subprocess.run([str(directory / "test")], check=True)
