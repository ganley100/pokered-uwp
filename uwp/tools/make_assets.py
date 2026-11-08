#!/usr/bin/env python3
"""Generate the tile, logo and splash images for both apps.

Usage: python3 uwp/tools/make_assets.py   (needs Pillow)
Writes PokemonRed/Assets and PokemonBlue/Assets next to this folder.

If uwp/art/red.png or uwp/art/blue.png exists, that cover art is used:
square images show the whole cover, and wide images centre it over a
blurred copy of itself. Otherwise a plain emblem is drawn.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
UWP = os.path.dirname(HERE)

GAMES = {
    "PokemonRed": {"label": "RED", "art": "red.png", "bg": (184, 40, 44), "accent": (232, 72, 64)},
    "PokemonBlue": {"label": "BLUE", "art": "blue.png", "bg": (40, 80, 184), "accent": (72, 120, 232)},
}

# name: (width, height, draw_label)
IMAGES = {
    "Square44x44Logo": (44, 44, False),
    "Square150x150Logo": (150, 150, False),
    "Wide310x150Logo": (310, 150, True),
    "StoreLogo": (50, 50, False),
    "SplashScreen": (620, 300, True),
}
SCALES = (100, 200)

FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
)


def load_font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def draw_ball(draw, cx, cy, r, top, outline=(16, 16, 16)):
    """A simple two-tone capsule emblem with a centre button."""
    band = max(1, round(r * 0.14))
    draw.pieslice((cx - r, cy - r, cx + r, cy + r), 180, 360, fill=top)
    draw.pieslice((cx - r, cy - r, cx + r, cy + r), 0, 180, fill=(245, 245, 245))
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), outline=outline, width=band)
    draw.rectangle((cx - r, cy - band // 2, cx + r, cy + (band + 1) // 2), fill=outline)
    br = r * 0.32
    draw.ellipse((cx - br, cy - br, cx + br, cy + br), fill=outline)
    ir = br - band
    draw.ellipse((cx - ir, cy - ir, cx + ir, cy + ir), fill=(245, 245, 245))


def render(game, width, height, label, scale):
    # Draw at 4x and downsample for smooth edges.
    ss = 4
    w, h = width * scale // 100, height * scale // 100
    img = Image.new("RGBA", (w * ss, h * ss), game["bg"] + (255,))
    draw = ImageDraw.Draw(img)
    if label:
        r = h * ss * 0.30
        cx = w * ss * 0.30 if w > h * 1.5 else w * ss / 2
        draw_ball(draw, cx, h * ss / 2, r, game["accent"])
        font = load_font(int(h * ss * 0.26))
        text = game["label"]
        box = draw.textbbox((0, 0), text, font=font)
        tx = w * ss * 0.52
        ty = (h * ss - (box[3] - box[1])) / 2 - box[1]
        draw.text((tx, ty), text, font=font, fill=(255, 255, 255))
    else:
        draw_ball(draw, w * ss / 2, h * ss / 2, min(w, h) * ss * 0.36, game["accent"])
    return img.resize((w, h), Image.LANCZOS)


def render_art(art, width, height, scale):
    w, h = width * scale // 100, height * scale // 100
    if w == h:
        return art.resize((w, h), Image.LANCZOS)

    # Background: the cover stretched to cover the image and blurred.
    cover = max(w / art.width, h / art.height)
    bw, bh = round(art.width * cover), round(art.height * cover)
    back = art.resize((bw, bh), Image.LANCZOS)
    back = back.crop(((bw - w) // 2, (bh - h) // 2, (bw - w) // 2 + w, (bh - h) // 2 + h))
    back = back.filter(ImageFilter.GaussianBlur(radius=max(w, h) / 25))

    # Foreground: the whole cover, as large as fits.
    side = min(w, h)
    front = art.resize((side, side), Image.LANCZOS)
    back.paste(front, ((w - side) // 2, (h - side) // 2))
    return back


def main():
    for project, game in GAMES.items():
        out = os.path.join(UWP, project, "Assets")
        os.makedirs(out, exist_ok=True)
        art_path = os.path.join(UWP, "art", game["art"])
        art = Image.open(art_path).convert("RGB") if os.path.exists(art_path) else None
        for name, (width, height, label) in IMAGES.items():
            for scale in SCALES:
                if art is not None:
                    img = render_art(art, width, height, scale)
                else:
                    img = render(game, width, height, label, scale)
                img.save(os.path.join(out, f"{name}.scale-{scale}.png"), optimize=True)
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
