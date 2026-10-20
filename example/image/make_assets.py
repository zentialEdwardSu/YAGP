"""Generate the image-demo PNG/JPG. Not part of the normal build.

Run from the repository root:
    python example/image/make_assets.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent


def make_pet():
    im = Image.new("RGBA", (160, 160), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # Tail, body, ears and head leave a transparent margin (roughly 25 px).
    d.ellipse((108, 76, 138, 112), fill=(45, 118, 190, 255))
    d.ellipse((48, 72, 132, 136), fill=(70, 160, 225, 255))
    d.polygon([(48, 56), (58, 22), (78, 50)], fill=(70, 160, 225, 255))
    d.polygon([(102, 50), (122, 22), (132, 56)], fill=(70, 160, 225, 255))
    d.ellipse((44, 40, 136, 112), fill=(90, 178, 238, 255))
    d.ellipse((70, 62, 84, 80), fill=(24, 34, 52, 255))
    d.ellipse((96, 62, 110, 80), fill=(24, 34, 52, 255))
    d.ellipse((77, 68, 81, 73), fill=(255, 255, 255, 255))
    d.ellipse((103, 68, 107, 73), fill=(255, 255, 255, 255))
    d.arc((78, 78, 104, 94), 20, 160, fill=(24, 34, 52, 255), width=3)
    d.ellipse((66, 86, 76, 96), fill=(255, 150, 160, 190))
    d.ellipse((108, 86, 118, 96), fill=(255, 150, 160, 190))
    return im


def make_photo(pet):
    im = Image.new("RGB", (320, 200), (22, 30, 44))
    d = ImageDraw.Draw(im)
    for y in range(200):
        d.line((0, y, 320, y), fill=(22 + y // 8, 30 + y // 6, 44 + y // 4))
    big = pet.resize((180, 180), Image.Resampling.LANCZOS)
    im.paste(big, (70, 10), big)
    return im


def main():
    pet = make_pet()
    pet.save(HERE / "pet.png")
    make_photo(pet).save(HERE / "photo.jpg", quality=90)


if __name__ == "__main__":
    main()
