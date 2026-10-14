"""Builds app.ico (rounded corners, 16-256 px) from source.png.

    python tools/make_icon.py [source.png] [out.ico]

Each size is resized from the source and masked separately, so small icons keep
crisp, anti-aliased corners instead of being shrunk from the 256 px image.
"""
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
RADIUS = 0.22      # Corner radius as a fraction of the side (Windows 11 tiles ~0.2).
SUPERSAMPLE = 8    # Mask is drawn this much larger, then downsampled for anti-aliasing.


def rounded_mask(size: int) -> Image.Image:
    big = size * SUPERSAMPLE
    mask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big - 1, big - 1), radius=round(big * RADIUS), fill=255)
    return mask.resize((size, size), Image.LANCZOS)


def make(src: Image.Image, size: int) -> Image.Image:
    im = src.resize((size, size), Image.LANCZOS)
    im.putalpha(ImageChops.multiply(im.getchannel("A"), rounded_mask(size)))
    return im


def main() -> None:
    src_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "main.png"
    out_path = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "app.ico"
    src = Image.open(src_path).convert("RGBA")
    side = min(src.size)  # Centre-crop to a square if needed.
    left, top = (src.width - side) // 2, (src.height - side) // 2
    src = src.crop((left, top, left + side, top + side))
    images = [make(src, s) for s in SIZES]
    largest = images[-1]
    largest.save(out_path, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print(f"wrote {out_path} ({', '.join(map(str, SIZES))} px)")


if __name__ == "__main__":
    main()
