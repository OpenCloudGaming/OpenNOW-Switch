from pathlib import Path
import re
import runpy

root = Path(__file__).resolve().parents[2]
asset_check = runpy.run_path(str(root / "tests/ui_font_assets_test.py"))
tables = asset_check["tables"]
has_glyph = asset_check["has_glyph"]
font_dir = root / "resources/font"
fallback = tables(font_dir / "OpenNOW-CJK.ttf")
source = (root / "app/src/localization.cpp").read_text()
literals = re.findall(r'"((?:\\.|[^"\\])*)"', source)
characters = set("".join(literals))
characters.update("OpenNOW Жї 玩家设置游戏 123.45%")
characters = sorted(character for character in characters if ord(character) > 32)
for filename in ("Nunito-Medium.ttf", "Nunito-SemiBold.ttf", "Nunito-Bold.ttf", "Nunito-ExtraBold.ttf", "IBMPlexMono-Medium.ttf"):
    base = tables(font_dir / filename)
    missing = [character for character in characters
               if not has_glyph(base, ord(character)) and not has_glyph(fallback, ord(character))]
    assert not missing, f"{filename} direct bundled fallback misses: {missing}"
    print(f"PASS {filename} + direct OpenNOW-CJK: {len(characters)} UI codepoints")
