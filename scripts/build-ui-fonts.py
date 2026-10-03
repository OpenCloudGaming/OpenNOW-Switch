import argparse
from pathlib import Path
import shutil

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools import subset


def build_fonts(source, mono, cjk, output):
    output.mkdir(parents=True, exist_ok=True)
    for weight, style in ((500, "Medium"), (600, "SemiBold"), (700, "Bold"), (800, "ExtraBold")):
        variable = TTFont(source, recalcTimestamp=False)
        font = instantiateVariableFont(variable, {"wght": weight}, inplace=True)
        names = {
            1: "Nunito",
            2: style,
            4: f"Nunito {style}",
            6: f"Nunito-{style}",
            16: "Nunito",
            17: style,
        }
        for record in font["name"].names:
            if record.nameID in names:
                font["name"].setName(names[record.nameID], record.nameID,
                                     record.platformID, record.platEncID, record.langID)
        font["OS/2"].usWeightClass = weight
        font.save(output / f"Nunito-{style}.ttf")
    shutil.copyfile(mono, output / "IBMPlexMono-Medium.ttf")
    font = instantiateVariableFont(TTFont(cjk, recalcTimestamp=False), {"wght": 500}, inplace=True)
    characters = set(range(32, 127))
    for lead in range(0xA1, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                characters.add(ord(bytes((lead, trail)).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    options = subset.Options()
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14, 16, 17]
    options.name_languages = ["*"]
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=characters)
    subsetter.subset(font)
    for record in font["name"].names:
        if record.nameID in (1, 4, 6, 16):
            font["name"].setName("OpenNOWCJK" if record.nameID == 6 else "OpenNOW CJK", record.nameID,
                                 record.platformID, record.platEncID, record.langID)
    font.save(output / "OpenNOW-CJK.ttf")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Build static OpenNOW UI fonts for NanoVG.")
    parser.add_argument("--nunito", type=Path, required=True)
    parser.add_argument("--mono", type=Path, required=True)
    parser.add_argument("--cjk", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("resources/font"))
    args = parser.parse_args()
    build_fonts(args.nunito, args.mono, args.cjk, args.output)
