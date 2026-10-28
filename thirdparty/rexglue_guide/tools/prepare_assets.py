"""Builds thirdparty/rexglue_guide/assets from the source packs.

Icons are downscaled (the pack ships some at 1254 px), sounds are converted to
16-bit 48 kHz stereo WAV (no decoder needed at runtime), and the fonts are
copied with their license. Everything in assets/ is compiled into the binary.

usage: python -I prepare_assets.py <DashX360 Assets dir> <Selawik release dir> <Selawik LICENSE.txt>
"""
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

ICON_SIZE = 128

# (source relative to the DashX360 Assets dir, output name)
ICONS = [
    ("Misc/SettingsIcons/account.png", "account"),
    ("Misc/SettingsIcons/family.png", "friends"),
    ("Misc/SettingsIcons/preferences.png", "preferences"),
    ("Misc/SettingsIcons/privacy.png", "privacy"),
    ("Misc/SettingsIcons/system.png", "system"),
    ("Misc/SettingsIcons/turnoff.png", "turnoff"),
    ("Misc/myapps.png", "apps"),
    ("Misc/pin.png", "pin"),
    ("Misc/music.png", "music"),
    ("Misc/SearchMenuIcons/accents.png", "key_rb"),
    ("Misc/SearchMenuIcons/caps.png", "key_ls"),
    ("Misc/SearchMenuIcons/cursor-lb.png", "key_cursor_lb"),
    ("Misc/SearchMenuIcons/cursor-rb.png", "key_cursor_rb"),
    ("Misc/SearchMenuIcons/done.png", "key_start"),
    ("Misc/SearchMenuIcons/symbols.png", "key_lt"),
]

GAMERPICS = [
    # profilepicture.jpg is a larger copy of FriendPool/20002.png.
    "Profile/profilepicture.jpg",
    "Profile/FriendPool/20003.png",
    "Profile/FriendPool/20006.png",
    "Profile/FriendPool/20007.png",
    "Profile/FriendPool/20008.png",
    "Profile/FriendPool/20009.png",
    "Profile/FriendPool/2000a.png",
    "Profile/FriendPool/2000c.png",
]

SOUNDS = [
    ("Audio/Sounds/08. Page Left.mp3", "page_left"),
    ("Audio/Sounds/09. Page Right.mp3", "page_right"),
    ("Audio/Sounds/10. Select A.mp3", "select"),
    ("Audio/Sounds/11. Select A (Alt).mp3", "open"),
    ("Audio/Sounds/12. Select B.mp3", "notify"),
    ("Audio/Sounds/13. Select.mp3", "move"),
    ("Audio/Sounds/14. Back.mp3", "back"),
    ("Audio/Sounds/15. Back 2.mp3", "close"),
]

FONTS = [("selawk.ttf", "selawik_regular.ttf"), ("selawksb.ttf", "selawik_semibold.ttf"),
         ("selawkl.ttf", "selawik_light.ttf")]


def square(img: Image.Image, size: int) -> Image.Image:
    img = img.convert("RGBA")
    img.thumbnail((size, size), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.paste(img, ((size - img.width) // 2, (size - img.height) // 2), img)
    return canvas


def main() -> None:
    dash, selawik, license_file = (Path(a) for a in sys.argv[1:4])
    out = Path(__file__).resolve().parent.parent / "assets"
    for sub in ("icons", "gamerpics", "sounds", "fonts"):
        (out / sub).mkdir(parents=True, exist_ok=True)

    for src, name in ICONS:
        square(Image.open(dash / src), ICON_SIZE).save(out / "icons" / f"{name}.png", optimize=True)
    for i, src in enumerate(GAMERPICS):
        img = Image.open(dash / src).convert("RGBA")
        img.resize((128, 128), Image.LANCZOS).save(out / "gamerpics" / f"{i}.png", optimize=True)
    for src, name in SOUNDS:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(dash / src), "-ac", "2",
                        "-ar", "48000", "-sample_fmt", "s16", str(out / "sounds" / f"{name}.wav")],
                       check=True)
    for src, name in FONTS:
        shutil.copyfile(selawik / src, out / "fonts" / name)
    shutil.copyfile(license_file, out / "fonts" / "OFL.txt")


if __name__ == "__main__":
    main()
