from pathlib import Path
import struct


def tables(path):
    data = path.read_bytes()
    count = struct.unpack_from(">H", data, 4)[0]
    result = {}
    for index in range(count):
        tag, _, offset, size = struct.unpack_from(">4sIII", data, 12 + index * 16)
        result[tag] = data[offset:offset + size]
    return result


def has_glyph(font, codepoint):
    cmap = font[b"cmap"]
    count = struct.unpack_from(">H", cmap, 2)[0]
    for index in range(count):
        _, _, offset = struct.unpack_from(">HHI", cmap, 4 + index * 8)
        format_id = struct.unpack_from(">H", cmap, offset)[0]
        if format_id == 12:
            groups = struct.unpack_from(">I", cmap, offset + 12)[0]
            for group in range(groups):
                start, end, glyph = struct.unpack_from(">III", cmap, offset + 16 + group * 12)
                if start <= codepoint <= end and glyph + codepoint - start != 0:
                    return True
        if format_id != 4 or codepoint > 0xFFFF:
            continue
        segments = struct.unpack_from(">H", cmap, offset + 6)[0] // 2
        ends = offset + 14
        starts = ends + segments * 2 + 2
        deltas = starts + segments * 2
        ranges = deltas + segments * 2
        for segment in range(segments):
            end = struct.unpack_from(">H", cmap, ends + segment * 2)[0]
            start = struct.unpack_from(">H", cmap, starts + segment * 2)[0]
            if not start <= codepoint <= end:
                continue
            delta = struct.unpack_from(">h", cmap, deltas + segment * 2)[0]
            distance = struct.unpack_from(">H", cmap, ranges + segment * 2)[0]
            if distance == 0:
                glyph = (codepoint + delta) & 0xFFFF
            else:
                address = ranges + segment * 2 + distance + (codepoint - start) * 2
                glyph = struct.unpack_from(">H", cmap, address)[0]
                if glyph:
                    glyph = (glyph + delta) & 0xFFFF
            if glyph:
                return True
    return False


root = Path(__file__).resolve().parents[1] / "resources" / "font"
for style, weight in (("Medium", 500), ("SemiBold", 600), ("Bold", 700), ("ExtraBold", 800)):
    font = tables(root / f"Nunito-{style}.ttf")
    assert b"fvar" not in font, f"Nunito {style} must be static for NanoVG"
    assert struct.unpack_from(">H", font[b"OS/2"], 4)[0] == weight
    for character in "OpenNOWéЖїŁ":
        assert has_glyph(font, ord(character)), f"Nunito {style} lacks {character}"
    print(f"PASS Nunito {style}: static weight {weight}, Latin/Cyrillic glyphs")

mono = tables(root / "IBMPlexMono-Medium.ttf")
assert b"fvar" not in mono
assert struct.unpack_from(">H", mono[b"OS/2"], 4)[0] == 500
assert all(has_glyph(mono, ord(character)) for character in "0123456789./:%")
print("PASS IBM Plex Mono Medium: static weight 500, metric glyphs")

fallback = tables(root / "OpenNOW-CJK.ttf")
assert b"fvar" not in fallback
assert struct.unpack_from(">H", fallback[b"OS/2"], 4)[0] == 500
assert all(has_glyph(fallback, ord(character)) for character in "玩家设置游戏")
print("PASS bundled fallback: Chinese UI glyphs")

for name in ("Nunito-OFL.txt", "IBMPlexMono-OFL.txt", "OpenNOW-CJK-OFL.txt"):
    assert "SIL OPEN FONT LICENSE" in (root / name).read_text()
print("PASS bundled font licenses")
