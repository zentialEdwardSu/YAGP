"""Dependency-free GIF generator + reference compositor for gif_dump.

The reference composites from the frame *specs* this script encodes (palette
indices, rects, disposal, transparency), so it shares no code with the GIF
decoder in gif_stream.c. Rules follow gif_stream.h: disposal 2 clears the
previous frame's rect to transparent, disposal 3 restores the canvas from
before the previous frame, palette indices past the colour table are skipped,
keyed colours become transparent, a 0/1 cs delay means 100 ms.

  python3 gifgen.py check [--quick] <gif_dump>   decoder + frame cache vs reference
  python3 gifgen.py diff <old gif_dump> <new>    byte-identical outputs on all cases
  python3 gifgen.py bench <gif_dump> [...]       time a cold frame-cache build
"""
import os
import random
import struct
import subprocess
import sys
import tempfile
import time

# ---- encoder -----------------------------------------------------------------


def lzw_encode(indices, min_size):
    """GIF LZW (same scheme as omggif): variable code size, reset at 4096."""
    clear, eoi = 1 << min_size, (1 << min_size) + 1
    out = bytearray()
    acc = nbits = 0

    def emit(code, size):
        nonlocal acc, nbits
        acc |= code << nbits
        nbits += size
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8

    size = min_size + 1
    emit(clear, size)
    table, nxt = {}, eoi + 1
    cur = indices[0]
    for k in indices[1:]:
        key = (cur, k)
        if key in table:
            cur = table[key]
            continue
        emit(cur, size)
        if nxt == 4096:
            emit(clear, size)
            table, nxt, size = {}, eoi + 1, min_size + 1
        else:
            if nxt >= (1 << size):
                size += 1
            table[key] = nxt
            nxt += 1
        cur = k
    emit(cur, size)
    emit(eoi, size)
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


def table_bits(n):
    bits = 1
    while (1 << bits) < n:
        bits += 1
    return bits


def pack_palette(pal):
    bits = table_bits(len(pal))
    pal = list(pal) + [(0, 0, 0)] * ((1 << bits) - len(pal))
    return bits, b"".join(bytes(c) for c in pal)


def interlace_order(h):
    rows = []
    for start, step in ((0, 8), (4, 8), (2, 4), (1, 2)):
        rows += list(range(start, h, step))
    return rows


class Frame:
    def __init__(self, x, y, w, h, pixels, palette=None, transparent=None, disposal=0,
                 delay_cs=10, interlace=False, gce=True, min_code=None):
        self.x, self.y, self.w, self.h = x, y, w, h
        self.pixels = pixels            # w*h palette indices, row-major
        self.palette = palette          # local colour table or None
        self.transparent = transparent
        self.disposal = disposal
        self.delay_cs = delay_cs
        self.interlace = interlace
        self.gce = gce
        self.min_code = min_code        # LZW code size; may exceed the table (indices past it)


class Gif:
    def __init__(self, w, h, palette, frames):
        self.w, self.h, self.palette, self.frames = w, h, palette, frames

    def encode(self, truncate_at=None):
        b = bytearray(b"GIF89a")
        packed = 0
        gpal = b""
        if self.palette:
            bits, gpal = pack_palette(self.palette)
            packed = 0x80 | (7 << 4) | (bits - 1)
        b += struct.pack("<HHBBB", self.w, self.h, packed, 0, 0) + gpal
        b += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"
        for f in self.frames:
            if f.gce:
                flags = (f.disposal << 2) | (1 if f.transparent is not None else 0)
                b += struct.pack("<BBBBHBB", 0x21, 0xF9, 4, flags, f.delay_cs, f.transparent or 0, 0)
            packed = 0x40 if f.interlace else 0
            lpal = b""
            pal = f.palette or self.palette
            if f.palette:
                bits, lpal = pack_palette(f.palette)
                packed |= 0x80 | (bits - 1)
            b += struct.pack("<BHHHHB", 0x2C, f.x, f.y, f.w, f.h, packed) + lpal
            rows = interlace_order(f.h) if f.interlace else range(f.h)
            data = [f.pixels[r * f.w + c] for r in rows for c in range(f.w)]
            min_code = f.min_code or max(2, table_bits(len(pal)))
            lz = lzw_encode(data, min_code)
            b.append(min_code)
            for i in range(0, len(lz), 255):
                chunk = lz[i:i + 255]
                b.append(len(chunk))
                b += chunk
            b.append(0)
        b.append(0x3B)
        return bytes(b if truncate_at is None else b[:truncate_at])

    def reference(self, key=None, frame_limit=None):
        """Premultiplied BGRA canvases after each frame."""
        w, h = self.w, self.h
        canvas = bytearray(w * h * 4)
        out = []
        prev_disposal, prev_rect, snapshot = 0, None, None
        frames = self.frames if frame_limit is None else self.frames[:frame_limit]
        for f in frames:
            if prev_disposal == 2:
                x, y, fw, fh = prev_rect
                for r in range(y, y + fh):
                    canvas[(r * w + x) * 4:(r * w + x + fw) * 4] = bytes(fw * 4)
            elif prev_disposal == 3:
                canvas = bytearray(snapshot)
            snapshot = bytes(canvas)
            pal = f.palette or self.palette
            _, padded = pack_palette(pal)
            ncolors = len(padded) // 3
            colors = [tuple(padded[i * 3:i * 3 + 3]) for i in range(ncolors)]
            keyed = [False] * ncolors
            if key:
                (kr, kg, kb), tol = key
                keyed = [abs(c[0] - kr) <= tol and abs(c[1] - kg) <= tol and abs(c[2] - kb) <= tol
                         for c in colors]
            tr = f.transparent if f.gce else None
            for r in range(f.h):
                for c in range(f.w):
                    idx = f.pixels[r * f.w + c]
                    if idx == tr or idx >= ncolors:
                        continue
                    o = ((f.y + r) * w + f.x + c) * 4
                    if keyed[idx]:
                        canvas[o:o + 4] = bytes(4)
                    else:
                        cr, cg, cb = colors[idx]
                        canvas[o:o + 4] = bytes((cb, cg, cr, 255))
            delay = f.delay_cs * 10 if f.gce and f.delay_cs > 1 else 100
            out.append((delay, bytes(canvas)))
            prev_disposal = f.disposal if f.gce else 0
            prev_rect = (f.x, f.y, f.w, f.h)
        return out


# ---- cases -------------------------------------------------------------------

PAL = [(0, 0, 0), (230, 90, 40), (40, 160, 230), (0, 255, 0), (10, 240, 12), (255, 255, 255),
       (128, 0, 128), (255, 0, 0)]


def rect_pixels(w, h, fill, boxes=()):
    px = [fill] * (w * h)
    for (x0, y0, x1, y1, v) in boxes:
        for y in range(y0, y1):
            for x in range(x0, x1):
                px[y * w + x] = v
    return px


def random_gif(rng, w, h, nframes):
    gpal = [tuple(rng.randrange(256) for _ in range(3)) for _ in range(rng.choice((2, 4, 8, 16)))]
    frames = []
    for i in range(nframes):
        fw, fh = rng.randint(1, w), rng.randint(1, h)
        x, y = rng.randint(0, w - fw), rng.randint(0, h - fh)
        pal = None
        if rng.random() < 0.3:
            pal = [tuple(rng.randrange(256) for _ in range(3)) for _ in range(rng.choice((2, 4, 32)))]
        n = len(pal or gpal)
        transparent = rng.randrange(n) if rng.random() < 0.6 else None
        style = rng.random()
        if style < 0.4:      # flat with a few blocks (compresses, like real sprites)
            blocks = []
            for _ in range(3):
                x0, x1 = sorted((rng.randint(0, fw), rng.randint(0, fw)))
                y0, y1 = sorted((rng.randint(0, fh), rng.randint(0, fh)))
                blocks.append((x0, y0, x1, y1, rng.randrange(n)))
            px = rect_pixels(fw, fh, rng.randrange(n), blocks)
        elif style < 0.8:    # noise
            px = [rng.randrange(n) for _ in range(fw * fh)]
        else:                # mostly the transparent index
            t = transparent if transparent is not None else 0
            px = [t if rng.random() < 0.8 else rng.randrange(n) for _ in range(fw * fh)]
        min_code = None
        if rng.random() < 0.1:   # indices beyond the colour table: must be skipped
            min_code = 8
            px = [p if rng.random() < 0.7 else rng.randrange(n, 256) for p in px]
        frames.append(Frame(x, y, fw, fh, px, palette=pal, transparent=transparent,
                            disposal=rng.choice((0, 1, 2, 3, 3)), delay_cs=rng.choice((0, 1, 2, 5, 7)),
                            interlace=rng.random() < 0.3, gce=rng.random() < 0.9, min_code=min_code))
    return Gif(w, h, gpal, frames)


def fixed_cases():
    W, H = 64, 48
    cases = {}
    boxes = [(4, 4, 20, 20), (24, 10, 40, 30), (44, 20, 60, 44)]
    for disposal in (0, 1, 2, 3):
        frames = []
        for i, (x0, y0, x1, y1) in enumerate(boxes):
            px = rect_pixels(W, H, 0, [(x0, y0, x1 + 1, y1 + 1, i + 1)])
            frames.append(Frame(0, 0, W, H, px, transparent=0, disposal=disposal, delay_cs=(3, 7, 0)[i]))
        cases[f"disposal{disposal}"] = (Gif(W, H, PAL, frames), None)
    # Sub-rect frames with disposal 3 over a background: restore must only touch the rect.
    bg = Frame(0, 0, W, H, rect_pixels(W, H, 2, [(30, 5, 60, 40, 5)]), delay_cs=4)
    s1 = Frame(5, 5, 20, 20, [1] * 400, disposal=3, transparent=None)
    s2 = Frame(20, 20, 30, 20, [7 if (i % 3) else 0 for i in range(600)], transparent=0, disposal=3)
    s3 = Frame(40, 10, 10, 10, [6] * 100, disposal=2)
    s4 = Frame(0, 0, 8, 8, [3] * 64, disposal=1)
    cases["subrect-disposal3"] = (Gif(W, H, PAL, [bg, s1, s2, s3, s4]), None)
    # Interlaced, offset, odd sizes.
    px = [(x * 3 + y * 5) % 8 for y in range(37) for x in range(29)]
    cases["interlaced"] = (Gif(W, H, PAL, [Frame(7, 5, 29, 37, px, interlace=True),
                                           Frame(0, 0, 13, 9, [1] * 117, interlace=True, disposal=2)]), None)
    # Moving sprite, one-pixel change, identical frame.
    frames = []
    for x in (4, 14, 24, 24, 24, 34):
        frames.append(Frame(0, 0, W, H, rect_pixels(W, H, 0, [(40, 4, 61, 25, 1), (x, 28, x + 11, 41, 2)]),
                            transparent=0, disposal=2, delay_cs=4))
    frames[3].pixels = list(frames[3].pixels)
    frames[3].pixels[0] = 1
    cases["moving"] = (Gif(W, H, PAL, frames), None)
    # Green screen with and without a key; fully transparent.
    green = [Frame(0, 0, W, H, rect_pixels(W, H, 3, [(5 + i * 15, 10, 21 + i * 15, 31, 1),
                                                     (8, 34, 30, 44, 4)]), delay_cs=4) for i in range(3)]
    cases["green+key"] = (Gif(W, H, PAL, green), ((0, 255, 0), 16))
    cases["green+key0"] = (Gif(W, H, PAL, green), ((0, 255, 0), 0))
    cases["green nokey"] = (Gif(W, H, PAL, green), None)
    empty = [Frame(0, 0, W, H, [0] * (W * H), transparent=0, disposal=2) for _ in range(2)]
    cases["transparent"] = (Gif(W, H, PAL, empty), None)
    # Single frame, and a 1x1 GIF.
    cases["single"] = (Gif(W, H, PAL, [Frame(3, 3, 10, 10, [5] * 100)]), None)
    cases["1x1"] = (Gif(1, 1, PAL, [Frame(0, 0, 1, 1, [1]), Frame(0, 0, 1, 1, [0], transparent=0)]), None)
    # Large LZW stream (code table resets at 4096).
    rng = random.Random(7)
    big = [Frame(0, 0, 200, 150, [rng.randrange(8) for _ in range(200 * 150)])]
    big.append(Frame(50, 40, 60, 50, [rng.randrange(8) for _ in range(3000)], disposal=3))
    big.append(Frame(10, 10, 30, 30, [1] * 900, disposal=2, transparent=0))
    cases["big-noise"] = (Gif(200, 150, PAL, big), None)
    for seed in range(40):
        rng = random.Random(1000 + seed)
        w, h = rng.randint(1, 70), rng.randint(1, 50)
        key = None
        if rng.random() < 0.3:
            key = (tuple(rng.randrange(256) for _ in range(3)), rng.choice((0, 30, 255)))
        cases[f"random{seed}"] = (random_gif(rng, w, h, rng.randint(1, 9)), key)
    return cases


# ---- gif_dump driver ----------------------------------------------------------


def key_args(key):
    if not key:
        return []
    (r, g, b), tol = key
    return [f"#{r:02X}{g:02X}{b:02X}", str(tol)]


def run_decode(exe, gif, out, key=None, size=None):
    args = [exe] + (["--size", str(size[0]), str(size[1])] if size else []) + [gif, out] + key_args(key)
    rc = subprocess.run(args).returncode
    frames = []
    w = h = 0
    if os.path.exists(out):
        with open(out, "rb") as f:
            w, h, n = struct.unpack("<3i", f.read(12))
            for _ in range(n):
                (delay,) = struct.unpack("<i", f.read(4))
                frames.append((delay, f.read(w * h * 4)))
    return rc, w, h, frames


def run_cache(exe, gif, out, cache_dir, key=None):
    rc = subprocess.run([exe, "--cache", gif, out, cache_dir] + key_args(key)).returncode
    if rc:
        return rc, None
    with open(out, "rb") as f:
        w, h, n, disk = struct.unpack("<4i", f.read(16))
        dirty = [struct.unpack("<4H", f.read(8)) for _ in range(n)]
        opaque = struct.unpack("<4H", f.read(8))
    return 0, (w, h, n, disk, dirty, opaque)


def sample(px, w, h, ow, oh):
    xs = [min((2 * i + 1) * w // (2 * ow), w - 1) for i in range(ow)]
    ys = [min((2 * i + 1) * h // (2 * oh), h - 1) for i in range(oh)]
    out = bytearray()
    for y in ys:
        for x in xs:
            out += px[(y * w + x) * 4:(y * w + x) * 4 + 4]
    return bytes(out)


def diff_box(a, b, w, h):
    l, t, r, btm = w, h, 0, 0
    for y in range(h):
        row = slice(y * w * 4, (y + 1) * w * 4)
        if a[row] == b[row]:
            continue
        for x in range(w):
            o = (y * w + x) * 4
            if a[o:o + 4] != b[o:o + 4]:
                l, t, r, btm = min(l, x), min(t, y), max(r, x + 1), max(btm, y + 1)
    return (0, 0, 0, 0) if r <= l else (l, t, r, btm)


def opaque_box(frames, w, h):
    l, t, r, b = w, h, 0, 0
    for px in frames:
        for y in range(h):
            for x in range(w):
                if px[(y * w + x) * 4 + 3]:
                    l, t, r, b = min(l, x), min(t, y), max(r, x + 1), max(b, y + 1)
    return (0, 0, 0, 0) if r <= l else (l, t, r, b)


def first_mismatch(ours, ref, w):
    for p in range(len(ref) // 4):
        if ours[p * 4:p * 4 + 4] != ref[p * 4:p * 4 + 4]:
            return f"pixel ({p % w},{p // w}) ours BGRA={tuple(ours[p*4:p*4+4])} ref={tuple(ref[p*4:p*4+4])}"
    return "length differs" if len(ours) != len(ref) else None


def check(exe, quick=False):
    failures = 0

    def report(ok, label, errors=()):
        nonlocal failures
        if not ok or not quick:
            print(("ok   " if ok else "FAIL ") + label)
        for e in list(errors)[:4]:
            print("     " + e)
        failures += not ok

    cases = fixed_cases()
    if quick:
        cases = {k: v for i, (k, v) in enumerate(cases.items()) if i % 3 == 0 or not k.startswith("random")}
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "out.bin")
        cache = os.path.join(d, "cache")
        for name, (gif, key) in cases.items():
            path = os.path.join(d, name.replace(" ", "_") + ".gif")
            with open(path, "wb") as f:
                f.write(gif.encode())
            ref = gif.reference(key)
            errors = []
            rc, w, h, frames = run_decode(exe, path, out, key)
            if rc or (w, h) != (gif.w, gif.h) or len(frames) != 2 * len(ref):
                errors.append(f"rc={rc} size={w}x{h} frames={len(frames)} want {2 * len(ref)}")
            for i, (delay, px) in enumerate(frames[:2 * len(ref)]):
                rd, rpx = ref[i % len(ref)]
                if delay != rd:
                    errors.append(f"frame {i}: delay {delay} != {rd}")
                m = first_mismatch(px, rpx, w)
                if m:
                    errors.append(f"frame {i}: {m}")
            sizes = [(max(1, gif.w // 2), max(1, gif.h // 2)), (max(1, gif.w // 5), max(1, gif.h // 3)), (1, 1)]
            for size in ([] if quick else sizes):
                rc, sw, sh, sframes = run_decode(exe, path, out, key, size)
                want = [sample(px, gif.w, gif.h, *size) for _, px in ref]
                if rc or (sw, sh) != size or len(sframes) != 2 * len(want):
                    errors.append(f"--size {size}: rc={rc} canvas {sw}x{sh} frames {len(sframes)}")
                    continue
                for i, (_, px) in enumerate(sframes):
                    m = first_mismatch(px, want[i % len(want)], sw)
                    if m:
                        errors.append(f"--size {size} frame {i}: {m}")
                        break
            canv = [px for _, px in ref]
            n = len(canv)
            want_dirty = [diff_box(canv[-1], canv[0], gif.w, gif.h)] + \
                         [diff_box(canv[i - 1], canv[i], gif.w, gif.h) for i in range(1, n)]
            want_opaque = opaque_box(canv, gif.w, gif.h)
            for attempt, want_disk in ((1, 0), (2, 1)):
                rc, c = run_cache(exe, path, out, cache, key)
                if rc:
                    errors.append(f"cache run {attempt}: rc={rc}")
                    break
                cw, ch, cn, disk, dirty, opq = c
                if (cw, ch, cn, disk) != (gif.w, gif.h, n, want_disk):
                    errors.append(f"cache run {attempt}: {cw}x{ch} n={cn} disk={disk}")
                if [tuple(x) for x in dirty] != want_dirty:
                    bad = next(i for i in range(min(n, len(dirty))) if tuple(dirty[i]) != want_dirty[i]) \
                        if len(dirty) == n else -1
                    errors.append(f"cache run {attempt}: dirty[{bad}] {dirty[bad] if bad >= 0 else dirty}"
                                  f" != {want_dirty[bad] if bad >= 0 else want_dirty}")
                if tuple(opq) != want_opaque:
                    errors.append(f"cache run {attempt}: opaque {opq} != {want_opaque}")
            report(not errors, f"{name}: {gif.w}x{gif.h}, {len(gif.frames)} frames", errors)

        # Truncated file: decoding stops at the damaged frame; the cache keeps the
        # complete frames (matches playback, which loops over what decoded).
        gif = fixed_cases()["subrect-disposal3"][0]
        data = gif.encode()
        cut = data.rfind(b"\x2C")  # start of the last image descriptor
        path = os.path.join(d, "truncated.gif")
        with open(path, "wb") as f:
            f.write(data[:cut + 13])  # descriptor, code size, part of the LZW data
        ref = [px for _, px in gif.reference(frame_limit=len(gif.frames) - 1)]
        rc, c = run_cache(exe, path, out, cache)
        errors = []
        if rc or c[2] != len(ref):
            errors.append(f"rc={rc} frames={c and c[2]} want {len(ref)}")
        else:
            want = [diff_box(ref[-1], ref[0], gif.w, gif.h)] + \
                   [diff_box(ref[i - 1], ref[i], gif.w, gif.h) for i in range(1, len(ref))]
            if [tuple(x) for x in c[4]] != want:
                errors.append(f"dirty {c[4]} != {want}")
            if tuple(c[5]) != opaque_box(ref, gif.w, gif.h):
                errors.append(f"opaque {c[5]}")
        report(not errors, "truncated GIF: cache keeps complete frames", errors)

        # Cache invalidation and robustness.
        path = os.path.join(d, "moving.gif")
        st = os.stat(path)
        os.utime(path, (st.st_atime, st.st_mtime + 5))
        report(run_cache(exe, path, out, cache)[1][3] == 0, "rebuilds after GIF mtime changes")
        report(run_cache(exe, path, out, cache)[1][3] == 1, "reloads rebuilt cache")
        for f in os.listdir(cache):
            with open(os.path.join(cache, f), "r+b") as fh:
                fh.seek(4)
                fh.write(struct.pack("<I", 2))
        report(run_cache(exe, path, out, cache)[1][3] == 0, "rebuilds an older cache version")
        report(run_cache(exe, path, out, cache)[1][3] == 1, "reloads the upgraded cache")
        for f in os.listdir(cache):
            with open(os.path.join(cache, f), "r+b") as fh:
                fh.seek(-3, 2)
                fh.truncate()
        report(run_cache(exe, path, out, cache)[1][3] == 0, "rejects truncated cache file")
        names = os.listdir(cache)
        report(not [f for f in names if f.endswith(".tmp")], f"no temp files left ({len(names)} cache files)")

        green = os.path.join(d, "green+key.gif")
        for arg in ("#00FF00", "0f0", "#00ff00", "0x00FF00", " #00FF00 "):
            r = subprocess.run([exe, green, out, arg, "16"])
            report(r.returncode == 0, f"accepts hex {arg!r}")
        for bad in ("#12345", "zzzzzz", "#1234567", "", "#"):
            r = subprocess.run([exe, green, out, bad, "16"])
            report(r.returncode == 6, f"rejects hex {bad!r}")
    print(f"{'FAILED' if failures else 'passed'}: {len(cases)} GIF cases, {failures} failure(s)")
    return 1 if failures else 0


def diff(old, new):
    """Every output byte (frames, delays, cache) must match between two builds."""
    failures = 0
    with tempfile.TemporaryDirectory() as d:
        for name, (gif, key) in fixed_cases().items():
            path = os.path.join(d, name.replace(" ", "_") + ".gif")
            with open(path, "wb") as f:
                f.write(gif.encode())
            outs = []
            for exe, tag in ((old, "old"), (new, "new")):
                blobs = []
                for size in (None, (max(1, gif.w // 3), max(1, gif.h // 2))):
                    o = os.path.join(d, f"{tag}.bin")
                    args = [exe] + (["--size", str(size[0]), str(size[1])] if size else []) + [path, o] + key_args(key)
                    rc = subprocess.run(args).returncode
                    blobs.append((rc, open(o, "rb").read()))
                o = os.path.join(d, f"{tag}.cache.bin")
                rc = subprocess.run([exe, "--cache", path, o, os.path.join(d, f"cache-{tag}")] + key_args(key)).returncode
                blobs.append((rc, open(o, "rb").read() if rc == 0 else b""))
                outs.append(blobs)
            same = outs[0] == outs[1]
            failures += not same
            if not same:
                print(f"DIFF {name}")
    print(f"{'DIFFERENT' if failures else 'identical'}: {failures} case(s) differ")
    return 1 if failures else 0


# ---- benchmark -----------------------------------------------------------------


def bench_gifs(d):
    """A large mostly-static GIF with a small animated sprite (typical desktop pet),
    and a full-canvas noisy animation (worst case for box-limited scanning)."""
    rng = random.Random(3)
    out = {}
    W, H, N = 1024, 1024, 120
    bgpx = [((x // 64) + (y // 64)) % 4 + 1 for y in range(H) for x in range(W)]
    frames = [Frame(0, 0, W, H, bgpx, delay_cs=4)]
    for i in range(1, N):
        sx, sy = 100 + i * 6, 300 + (i % 20) * 8
        px = [0 if (x - 32) ** 2 + (y - 32) ** 2 > 900 else 5 + (i % 3) for y in range(64) for x in range(64)]
        frames.append(Frame(sx, sy, 64, 64, px, transparent=0, disposal=3, delay_cs=4))
    out["sprite_1024"] = Gif(W, H, PAL, frames)
    W, H, N = 400, 400, 40
    frames = [Frame(0, 0, W, H, [rng.randrange(8) for _ in range(W * H)], delay_cs=4) for _ in range(N)]
    out["noise_400"] = Gif(W, H, PAL, frames)
    paths = {}
    for name, gif in out.items():
        p = os.path.join(d, name + ".gif")
        with open(p, "wb") as f:
            f.write(gif.encode())
        paths[name] = p
    return paths


def bench(exes, reps=5):
    with tempfile.TemporaryDirectory() as d:
        print("generating benchmark GIFs ...", flush=True)
        paths = bench_gifs(d)
        for name, p in paths.items():
            print(f"{name} ({os.path.getsize(p) // 1024} KiB): cold frame-cache build, best of {reps}")
            for exe in exes:
                best = None
                for r in range(reps):
                    cache = os.path.join(d, f"c{r}-{abs(hash(exe))}")
                    t = time.perf_counter()
                    subprocess.run([exe, "--cache", p, os.path.join(d, "o.bin"), cache], check=True)
                    dt = time.perf_counter() - t
                    best = dt if best is None else min(best, dt)
                print(f"   {best * 1000:8.1f} ms  {exe}")
    return 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "check":
        args = [a for a in sys.argv[2:] if a != "--quick"]
        sys.exit(check(args[0], quick="--quick" in sys.argv))
    if cmd == "diff":
        sys.exit(diff(sys.argv[2], sys.argv[3]))
    if cmd == "bench":
        sys.exit(bench(sys.argv[2:]))
    print(__doc__)
    sys.exit(2)
