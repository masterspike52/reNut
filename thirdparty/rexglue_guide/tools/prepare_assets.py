"""Builds thirdparty/rexglue_guide/assets from their sources.

Gamerpics: each <n>.jpeg (or .jpg / .png) in the gamerpic sources dir becomes
assets/gamerpics/<n>.png, cropped square and made 256 px. Fonts are copied with
their license. Icons (assets/icons/*_n.png) and sounds (assets/sounds/, see
the README) are used as they are. Everything in assets/ is compiled into the
binary.

usage: python -I prepare_assets.py <gamerpic sources dir> <Selawik release dir> <Selawik LICENSE.txt>
"""
import shutil
import sys
from pathlib import Path

from PIL import Image

GAMERPIC_SIZE = 256

FONTS = [("selawk.ttf", "selawik_regular.ttf"), ("selawksb.ttf", "selawik_semibold.ttf"),
         ("selawkl.ttf", "selawik_light.ttf")]


def square(img: Image.Image, size: int) -> Image.Image:
    side = min(img.size)
    left, top = (img.width - side) // 2, (img.height - side) // 2
    return img.crop((left, top, left + side, top + side)).resize((size, size), Image.LANCZOS)


def main() -> None:
    gamerpics, selawik, license_file = (Path(a) for a in sys.argv[1:4])
    out = Path(__file__).resolve().parent.parent / "assets"
    for sub in ("gamerpics", "fonts"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    for src in sorted(gamerpics.iterdir()):
        if src.stem.isdigit() and src.suffix.lower() in (".jpeg", ".jpg", ".png"):
            square(Image.open(src).convert("RGB"), GAMERPIC_SIZE).save(out / "gamerpics" / f"{src.stem}.png", optimize=True)
    for src, name in FONTS:
        shutil.copyfile(selawik / src, out / "fonts" / name)
    shutil.copyfile(license_file, out / "fonts" / "OFL.txt")


if __name__ == "__main__":
    main()
