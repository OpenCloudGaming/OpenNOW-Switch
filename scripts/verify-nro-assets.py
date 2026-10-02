import argparse
from pathlib import Path
import struct


def verify(nro_path, resources):
    data = nro_path.read_bytes()
    if data[0x10:0x14] != b"NRO0":
        raise ValueError("Not a Nintendo Switch NRO")
    executable_size = struct.unpack_from("<I", data, 0x18)[0]
    if data[executable_size:executable_size + 4] != b"ASET":
        raise ValueError("NRO has no asset header")
    offset, size = struct.unpack_from("<QQ", data, executable_size + 40)
    romfs = data[executable_size + offset:executable_size + offset + size]
    header = struct.unpack_from("<10Q", romfs)
    if header[0] != 80:
        raise ValueError("Unexpected RomFS header")
    directory_offset, directory_size = header[3:5]
    file_offset, file_size = header[7:9]
    directories = romfs[directory_offset:directory_offset + directory_size]
    files = romfs[file_offset:file_offset + file_size]

    def directory_path(entry):
        parent, _, _, _, _, length = struct.unpack_from("<6I", directories, entry)
        name = directories[entry + 24:entry + 24 + length].decode("utf-8")
        if entry == 0:
            return ""
        prefix = directory_path(parent)
        return f"{prefix}/{name}" if prefix else name

    entries = {}
    position = 0
    while position < len(files):
        parent, _, payload, length, _, name_length = struct.unpack_from("<IIQQII", files, position)
        name = files[position + 32:position + 32 + name_length].decode("utf-8")
        prefix = directory_path(parent)
        path = f"{prefix}/{name}" if prefix else name
        entries[path] = romfs[header[9] + payload:header[9] + payload + length]
        position += 32 + (name_length + 3) // 4 * 4

    expected = sorted((resources / "font").glob("*"))
    expected.append(resources / "img" / "opennow-logo-mark.png")
    for asset in expected:
        if not asset.is_file():
            continue
        path = asset.relative_to(resources).as_posix()
        if entries.get(path) != asset.read_bytes():
            raise ValueError(f"NRO asset missing or stale: {path}")
        print(f"PASS embedded {path}: {len(entries[path])} bytes")
    print(f"PASS NRO executable and RomFS: {len(data)} bytes, {len(entries)} files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Verify the final NRO contains exact bundled UI assets.")
    parser.add_argument("nro", type=Path)
    parser.add_argument("--resources", type=Path, default=Path("resources"))
    arguments = parser.parse_args()
    verify(arguments.nro, arguments.resources)
