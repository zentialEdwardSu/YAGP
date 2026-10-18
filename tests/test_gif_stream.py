"""Compares gif_stream's composited frames with Pillow's reference compositor.

Usage: python tests/test_gif_stream.py <path-to-gif_dump.exe>
"""
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

W, H = 64, 48


def frame(color, box, mode="P"):
    """Palette frame: index 0 transparent background, index 1 = color."""
    im = Image.new("P", (W, H), 0)
    im.putpalette([0, 0, 0] + list(color) + [0, 0, 0] * 254)
    ImageDraw.Draw(im).rectangle(box, fill=1)
    return im


def make_cases(d):
    cases = {}
    boxes = [(4, 4, 20, 20), (24, 10, 40, 30), (44, 20, 60, 44)]
    colors = [(255, 0, 0), (0, 200, 0), (0, 0, 255)]
    for disposal in (1, 2, 3):
        path = os.path.join(d, f"disposal{disposal}.gif")
        frames = [frame(c, b) for c, b in zip(colors, boxes)]
        frames[0].save(path, save_all=True, append_images=frames[1:], duration=[30, 70, 0],
                       loop=0, transparency=0, disposal=disposal, optimize=False)
        cases[path] = [30, 70, 100]
    path = os.path.join(d, "interlaced.gif")
    im = Image.new("P", (W, H))
    im.putpalette([(i * 7) % 256 for i in range(768)])
    im.putdata([(x * 3 + y * 5) % 256 for y in range(H) for x in range(W)])
    im.save(path, interlace=True)
    cases[path] = [100]
    return cases


def decode(exe, gif, out, key=None):
    subprocess.run([exe, gif, out] + (list(key) if key else []), check=True)
    with open(out, "rb") as f:
        w, h, n = struct.unpack("<3i", f.read(12))
        frames = []
        for _ in range(n):
            (delay,) = struct.unpack("<i", f.read(4))
            frames.append((delay, f.read(w * h * 4)))
    return w, h, frames


def reference(gif, key=None):
    """Pillow frames; with key=(rgb, tol) opaque pixels near rgb become transparent."""
    im = Image.open(gif)
    out = []
    for i in range(getattr(im, "n_frames", 1)):
        im.seek(i)
        px = bytearray(im.convert("RGBA").tobytes())
        if key:
            (kr, kg, kb), tol = key
            for p in range(0, len(px), 4):
                if px[p + 3] and abs(px[p] - kr) <= tol and abs(px[p + 1] - kg) <= tol and abs(px[p + 2] - kb) <= tol:
                    px[p:p + 4] = bytes(4)
        out.append(bytes(px))
    return out


def make_key_case(d):
    """Opaque green-screen animation: pure green bg, near-green fringe, red sprite."""
    path = os.path.join(d, "greenscreen.gif")
    frames = []
    for i in range(3):
        im = Image.new("P", (W, H), 0)
        im.putpalette([0, 255, 0, 10, 240, 12, 255, 0, 0, 40, 200, 40] + [0, 0, 0] * 252)
        dr = ImageDraw.Draw(im)
        dr.rectangle((8 + i * 10, 8, 30 + i * 10, 30), fill=1)   # within tolerance 16
        dr.rectangle((12 + i * 10, 12, 26 + i * 10, 26), fill=2)  # sprite, kept
        dr.rectangle((40, 30, 60, 44), fill=3)                    # outside tolerance, kept
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=50, loop=0, optimize=False)
    return path


def make_full_case(d):
    """Mixed keyframes: opaque full frames between small transparent
    disposal-1 and disposal-3 sprites, so seeks start mid-file."""
    path = os.path.join(d, "mixed.gif")
    frames, disposal = [], []
    for i in range(8):
        im = Image.new("P", (W, H), 2 if i % 3 == 0 else 0)
        im.putpalette([0, 0, 0, 255, 0, 0, 30, 60, 90, 200, 200, 0] + [0, 0, 0] * 252)
        ImageDraw.Draw(im).rectangle((4 + i * 6, 4 + i * 3, 14 + i * 6, 14 + i * 3), fill=1 + (i % 2) * 2)
        frames.append(im)
        disposal.append(3 if i % 3 == 1 else 1)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=40, loop=0,
                   transparency=0, disposal=disposal, optimize=False)
    return path


def sample_nearest(px, w, h, ow, oh):
    """RGBA bytes w x h -> ow x oh, source pixel floor((i + 0.5) * src / dst)."""
    xs = [min((2 * i + 1) * w // (2 * ow), w - 1) for i in range(ow)]
    ys = [min((2 * i + 1) * h // (2 * oh), h - 1) for i in range(oh)]
    out = bytearray()
    for y in ys:
        row = y * w * 4
        for x in xs:
            out += px[row + x * 4:row + x * 4 + 4]
    return bytes(out)


def compare(ours, ref, w, h):
    for p in range(w * h):
        b, g, r, a = ours[p * 4:p * 4 + 4]
        rr, rg, rb, ra = ref[p * 4:p * 4 + 4]
        if a != ra or (a and (r, g, b) != (rr, rg, rb)) or (not a and (r, g, b) != (0, 0, 0)):
            return f"pixel ({p % w},{p // w}) ours BGRA={b,g,r,a} ref RGBA={rr,rg,rb,ra}"
    return None


def main():
    exe = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as d:
        for gif, delays in make_cases(d).items():
            w, h, frames = decode(exe, gif, os.path.join(d, "out.bin"))
            ref = reference(gif)
            name = os.path.basename(gif)
            errors = []
            if (w, h) != Image.open(gif).size:
                errors.append(f"size {w}x{h}")
            if len(frames) != 2 * len(ref):
                errors.append(f"{len(frames)} frames over two loops, expected {2 * len(ref)}")
            for i, (delay, px) in enumerate(frames[:2 * len(ref)]):
                if delay != delays[i % len(ref)]:
                    errors.append(f"frame {i} delay {delay} != {delays[i % len(ref)]}")
                msg = compare(px, ref[i % len(ref)], w, h)
                if msg:
                    errors.append(f"frame {i}: {msg}")
            print(("FAIL " if errors else "ok   ") + name)
            for e in errors[:5]:
                print("     " + e)
            failures += bool(errors)

        # Shrunken canvas (--size): must equal a nearest-centre sample of each
        # full-size reference frame, for every disposal mode and interlacing.
        # (Not Image.resize(NEAREST): Pillow's fixed-point maths rounds centres
        # that land exactly on a pixel edge down, ours is exact.)
        for gif in make_cases(d):
            for size in ((W // 2, H // 2), (W // 5, H // 5), (37, 29), (1, 1), (W, H)):
                out = os.path.join(d, "out.bin")
                subprocess.run([exe, "--size", str(size[0]), str(size[1]), gif, out], check=True)
                with open(out, "rb") as f:
                    w, h, n = struct.unpack("<3i", f.read(12))
                    frames = []
                    for _ in range(n):
                        f.read(4)
                        frames.append(f.read(w * h * 4))
                ref = [sample_nearest(px, W, H, size[0], size[1]) for px in reference(gif)]
                errors = []
                if (w, h) != size:
                    errors.append(f"canvas {w}x{h}")
                if len(frames) != 2 * len(ref):
                    errors.append(f"{len(frames)} frames, expected {2 * len(ref)}")
                for i, px in enumerate(frames[:2 * len(ref)]):
                    msg = compare(px, ref[i % len(ref)], w, h)
                    if msg:
                        errors.append(f"frame {i}: {msg}")
                print(("FAIL " if errors else "ok   ") + f"{os.path.basename(gif)} at {size[0]}x{size[1]}")
                for e in errors[:5]:
                    print("     " + e)
                failures += bool(errors)

        # Seeking: every frame decoded from its nearest keyframe, and every
        # frame decoded after restoring a snapshot, must equal the reference.
        seek_cases = list(make_cases(d)) + [make_key_case(d), make_full_case(d)]
        for gif in seek_cases:
            cache = os.path.join(d, "cache")
            out = os.path.join(d, "out.bin")
            subprocess.run([exe, "--seek", gif, out, cache], check=True)
            with open(out, "rb") as f:
                w, h, n = struct.unpack("<3i", f.read(12))
                frames = []
                for _ in range(n):
                    f.read(4)
                    frames.append(f.read(w * h * 4))
            ref = reference(gif)
            errors = []
            if n != 2 * len(ref):
                errors.append(f"{n} frames, expected {2 * len(ref)}")
            for i, px in enumerate(frames):
                msg = compare(px, ref[i % len(ref)], w, h)
                if msg:
                    errors.append(f"{'seek' if i < len(ref) else 'snapshot'} frame {i % len(ref)}: {msg}")
            print(("FAIL " if errors else "ok   ") + f"seek {os.path.basename(gif)}")
            for e in errors[:5]:
                print("     " + e)
            failures += bool(errors)

        gif = make_key_case(d)
        for arg, rgb in (("#00FF00", (0, 255, 0)), ("0f0", (0, 255, 0)), ("#00ff00", (0, 255, 0))):
            w, h, frames = decode(exe, gif, os.path.join(d, "out.bin"), (arg, "16"))
            ref = reference(gif, (rgb, 16))
            errors = [f"frame {i}: {m}" for i, (_, px) in enumerate(frames[:len(ref)])
                      if (m := compare(px, ref[i], w, h))]
            if len(frames) != 2 * len(ref):
                errors.append(f"{len(frames)} frames, expected {2 * len(ref)}")
            keyed = sum(1 for p in range(w * h) if frames[0][1][p * 4 + 3] == 0)
            if not keyed or keyed == w * h:
                errors.append(f"implausible keyed pixel count {keyed}")
            print(("FAIL " if errors else "ok   ") + f"color key {arg}")
            for e in errors[:5]:
                print("     " + e)
            failures += bool(errors)
        for bad in ("#12345", "zzzzzz", "#1234567", ""):
            r = subprocess.run([exe, gif, os.path.join(d, "out.bin"), bad, "16"])
            ok = r.returncode == 6
            print(("ok   " if ok else "FAIL ") + f"rejects hex {bad!r}")
            failures += not ok
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
