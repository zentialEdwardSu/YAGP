import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw


def run(exe, path, *args):
    p = subprocess.run([exe, path, *args], capture_output=True, text=True)
    return p.returncode, p.stdout.strip()


def main():
    exe = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="yagp-image-") as tmp:
        # PNG with a transparent border and a 4x4 opaque square at (5,3).
        png = os.path.join(tmp, "pet.png")
        im = Image.new("RGBA", (20, 10), (0, 0, 0, 0))
        ImageDraw.Draw(im).rectangle((5, 3, 8, 6), fill=(255, 0, 0, 255))
        im.save(png)
        rc, out = run(exe, png)
        assert rc == 0, rc
        assert out.split() == ["20", "10", "20", "10", "5", "3", "9", "7"], out
        print("ok   PNG alpha and hitbox")

        # Colour key: red background, blue square.
        key = os.path.join(tmp, "key.png")
        im = Image.new("RGBA", (20, 10), (255, 0, 0, 255))
        ImageDraw.Draw(im).rectangle((6, 2, 10, 5), fill=(0, 0, 255, 255))
        im.save(key)
        rc, out = run(exe, key, "FF0000", "0")
        assert rc == 0, rc
        assert out.split() == ["20", "10", "20", "10", "6", "2", "11", "6"], out
        print("ok   PNG colour key")

        jpg = os.path.join(tmp, "pet.jpg")
        Image.new("RGB", (20, 10), (10, 20, 30)).save(jpg, quality=90)
        rc, out = run(exe, jpg)
        assert rc == 0, rc
        parts = out.split()
        assert parts[:4] == ["20", "10", "20", "10"], out
        print("ok   JPEG decode")

        bad = os.path.join(tmp, "bad.png")
        with open(bad, "wb") as f:
            f.write(b"not an image")
        assert run(exe, bad)[0] != 0
        print("ok   rejects corrupt image")

        huge = os.path.join(tmp, "huge.png")
        Image.new("RGB", (1, 16385), (0, 0, 0)).save(huge)
        assert run(exe, huge)[0] != 0
        print("ok   rejects oversized image")


if __name__ == "__main__":
    main()
