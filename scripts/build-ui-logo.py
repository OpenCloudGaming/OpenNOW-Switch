from pathlib import Path

from PIL import Image


root = Path(__file__).resolve().parents[1]
source = root / "resources/img/opennow-logo-mark.png"
target = root / "resources/img/opennow-logo-mark-small.png"
with Image.open(source) as image:
    height = round(image.height * 128 / image.width)
    image.convert("RGBA").resize((128, height), Image.Resampling.LANCZOS).save(
        target, optimize=True
    )
print(f"Generated {target.relative_to(root)}: 128x{height}")
