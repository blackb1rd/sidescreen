#!/usr/bin/env python3
"""Generate the macOS app icon from assets/icon-source.jpeg.

The source is a white rounded tile on an off-white background, so the artwork
(laptop, tablet, arrow) is cut out and placed on clean, correctly proportioned
tiles for each platform:

  assets/AppIcon.icns          macOS app icon (824/1024 tile + shadow)
  assets/mac-icon-1024.png     the same, as a PNG (README, website)

Usage: python3 scripts/make-icons.py   (needs Pillow and macOS iconutil)
"""
import os
import shutil
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "icon-source.jpeg")
ARTWORK_BOX = (330, 575, 1715, 1470)  # artwork bounds in the 2048px source, with a little padding


def artwork():
    """The artwork with its white surroundings made transparent (soft edge, no halo)."""
    im = Image.open(SRC).convert("RGB").crop(ARTWORK_BOX)
    # Alpha from "distance from white": pure white -> transparent, anything darker -> opaque.
    g = im.convert("L")
    # White areas inside the artwork become transparent too, which is fine: every icon
    # background below is white.
    alpha = g.point(lambda v: 0 if v >= 250 else min(255, (250 - v) * 12))
    out = im.convert("RGBA")
    out.putalpha(alpha)
    return out


def fit(img, width):
    h = round(img.height * width / img.width)
    return img.resize((width, h), Image.LANCZOS)


def rounded_tile(size, tile, radius, shadow=True, border=True):
    """Transparent canvas with a white rounded tile of side `tile` centred in it."""
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    off = (size - tile) // 2
    box = (off, off, off + tile, off + tile)
    if shadow:
        sh = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle(
            (box[0], box[1] + size * 0.012, box[2], box[3] + size * 0.012), radius, fill=(0, 0, 0, 70))
        canvas = Image.alpha_composite(canvas, sh.filter(ImageFilter.GaussianBlur(size * 0.018)))
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle(box, radius, fill=(255, 255, 255, 255),
                        outline=(222, 222, 222, 255) if border else None, width=max(1, size // 512))
    return canvas


def place(canvas, art, width, dy=0):
    a = fit(art, width)
    x = (canvas.width - a.width) // 2
    y = (canvas.height - a.height) // 2 + dy
    canvas.alpha_composite(a, (x, y))
    return canvas


def mac_icon(art):
    base = place(rounded_tile(1024, 824, 185), art, 660)
    iconset = tempfile.mkdtemp(suffix=".iconset")
    for pt in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = pt * scale
            name = f"icon_{pt}x{pt}{'@2x' if scale == 2 else ''}.png"
            base.resize((px, px), Image.LANCZOS).save(os.path.join(iconset, name))
    out = os.path.join(ROOT, "assets", "AppIcon.icns")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)
    shutil.rmtree(iconset)
    base.save(os.path.join(ROOT, "assets", "mac-icon-1024.png"))
    print("wrote", out)


if __name__ == "__main__":
    mac_icon(artwork())
