"""Checks frame_cache against values computed independently from Pillow frames.

Usage: python tests/test_frame_cache.py <path-to-gif_dump.exe>
"""
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

W, H = 64, 48


def pal(im):
    im.putpalette([0, 0, 0, 230, 90, 40, 40, 160, 230, 0, 255, 0] + [0] * 756)
    return im


def make(d):
    cases = {}
    # Moving box over a static disc. Frame 3 equals frame 2 except one pixel,
    # so the box for that step must be exactly 1x1.
    fr = []
    for x in (4, 14, 24, 24, 34):
        im = pal(Image.new("P", (W, H), 0))
        dr = ImageDraw.Draw(im)
        dr.ellipse((40, 4, 60, 24), fill=1)
        dr.rectangle((x, 28, x + 10, 40), fill=2)
        fr.append(im)
    fr[3].putpixel((0, 0), 1)
    p = os.path.join(d, "moving.gif")
    fr[0].save(p, save_all=True, append_images=fr[1:], duration=40, loop=0,
               transparency=0, disposal=2, optimize=False)
    cases["moving"] = (p, None)
    # Opaque green screen, with and without a colour key.
    fr = []
    for i in range(3):
        im = pal(Image.new("P", (W, H), 3))
        ImageDraw.Draw(im).rectangle((5 + i * 15, 10, 20 + i * 15, 30), fill=1)
        fr.append(im)
    p = os.path.join(d, "green.gif")
    fr[0].save(p, save_all=True, append_images=fr[1:], duration=40, loop=0, optimize=False)
    cases["green+key"] = (p, ((0, 255, 0), 16))
    cases["green nokey"] = (p, None)
    # Fully transparent: no visible area at all.
    fr = [pal(Image.new("P", (W, H), 0)) for _ in range(2)]
    p = os.path.join(d, "empty.gif")
    fr[0].save(p, save_all=True, append_images=fr[1:], duration=40, loop=0,
               transparency=0, disposal=2, optimize=False)
    cases["transparent"] = (p, None)
    return cases


def ref_frames(path, key):
    """Pillow RGBA frames; keyed and fully transparent pixels are zeroed like
    the premultiplied canvas."""
    im = Image.open(path)
    out = []
    for i in range(im.n_frames):
        im.seek(i)
        px = bytearray(im.convert("RGBA").tobytes())
        for p in range(0, len(px), 4):
            keyed = False
            if key and px[p + 3]:
                (kr, kg, kb), tol = key
                keyed = abs(px[p] - kr) <= tol and abs(px[p + 1] - kg) <= tol and abs(px[p + 2] - kb) <= tol
            if keyed or px[p + 3] == 0:
                px[p:p + 4] = bytes(4)
        out.append(bytes(px))
    return out


def box(a, b):
    l, t, r, btm = W, H, 0, 0
    for y in range(H):
        for x in range(W):
            o = (y * W + x) * 4
            if a[o:o + 4] != b[o:o + 4]:
                l, t, r, btm = min(l, x), min(t, y), max(r, x + 1), max(btm, y + 1)
    return (0, 0, 0, 0) if r <= l else (l, t, r, btm)


def opaque_box(frames):
    """Union over all frames of the pixels with alpha != 0, as (l, t, r, b)."""
    l, t, r, b = W, H, 0, 0
    for px in frames:
        bb = Image.frombytes("RGBA", (W, H), px).getchannel("A").getbbox()
        if bb:
            l, t, r, b = min(l, bb[0]), min(t, bb[1]), max(r, bb[2]), max(b, bb[3])
    return (0, 0, 0, 0) if r <= l else (l, t, r, b)


def run(exe, gif, cache_dir, key, out):
    args = [exe, "--cache", gif, out, cache_dir]
    if key:
        (r, g, b), tol = key
        args += [f"#{r:02X}{g:02X}{b:02X}", str(tol)]
    subprocess.run(args, check=True)
    with open(out, "rb") as f:
        w, h, n, disk = struct.unpack("<4i", f.read(16))
        dirty = [struct.unpack("<4H", f.read(8)) for _ in range(n)]
        opaque = struct.unpack("<4H", f.read(8))
    return w, h, n, disk, dirty, opaque


def main():
    exe = sys.argv[1]
    failures = 0

    def report(ok, label, errors=()):
        nonlocal failures
        print(("ok   " if ok else "FAIL ") + label)
        for e in list(errors)[:4]:
            print("     " + e)
        failures += not ok

    with tempfile.TemporaryDirectory() as d:
        cache = os.path.join(d, "cache")
        out = os.path.join(d, "out.bin")
        for name, (gif, key) in make(d).items():
            frames = ref_frames(gif, key)
            n = len(frames)
            want_dirty = [box(frames[-1], frames[0])] + [box(frames[i - 1], frames[i]) for i in range(1, n)]
            want_opaque = opaque_box(frames)
            errors = []
            for attempt, want_disk in ((1, 0), (2, 1)):  # build, then load from disk
                w, h, cn, disk, dirty, opaque = run(exe, gif, cache, key, out)
                if tuple(opaque) != want_opaque:
                    errors.append(f"run {attempt}: opaque {opaque} != {want_opaque}")
                if (w, h, cn) != (W, H, n):
                    errors.append(f"run {attempt}: size/frames {w}x{h}/{cn}")
                if disk != want_disk:
                    errors.append(f"run {attempt}: from_disk={disk}, want {want_disk}")
                if [tuple(x) for x in dirty] != want_dirty:
                    errors.append(f"run {attempt}: dirty {dirty} != {want_dirty}")
            report(not errors, f"{name}: {n} frames, opaque={want_opaque}", errors)

        gif = os.path.join(d, "moving.gif")
        st = os.stat(gif)
        os.utime(gif, (st.st_atime, st.st_mtime + 5))
        report(run(exe, gif, cache, None, out)[3] == 0, "rebuilds after GIF mtime changes")
        report(run(exe, gif, cache, None, out)[3] == 1, "reloads rebuilt cache")
        # An older cache version (here: v2) must be rebuilt, not trusted.
        for f in os.listdir(cache):
            with open(os.path.join(cache, f), "r+b") as fh:
                fh.seek(4)
                fh.write(struct.pack("<I", 2))
        report(run(exe, gif, cache, None, out)[3] == 0, "rebuilds an older cache version")
        report(run(exe, gif, cache, None, out)[3] == 1, "reloads the upgraded cache")
        for f in os.listdir(cache):
            with open(os.path.join(cache, f), "r+b") as fh:
                fh.seek(-3, 2)
                fh.truncate()
        report(run(exe, gif, cache, None, out)[3] == 0, "rejects truncated cache file")
        names = os.listdir(cache)
        report(not [f for f in names if f.endswith(".tmp")],
               f"no temp files left ({len(names)} cache files)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
