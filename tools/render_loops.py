"""Rendert de Jarvis-lussen (losse draden) offline naar JPEG-frames.

Port van prototypes/losse-draden.html. Elke toestand wordt een naadloze lus: alle bewegingen
zijn periodiek in de looptijd. Gebruik:  python render_loops.py [--preview] [--out out]
"""
import argparse, math, os, struct, sys
import numpy as np
from PIL import Image

W = 466
M = W / 2
TAU = math.pi * 2
BG = np.array([3, 5, 9], np.float32) / 255.0

# speed, energy, spread, brightness  (zoals in het prototype)
PRESETS = {
    "rust":      (0.70, 0.65, 0.92, 0.80),
    "luisteren": (1.15, 1.35, 1.00, 1.25),
    "denken":    (1.45, 1.10, 0.78, 1.00),
    "praten":    (1.25, 1.55, 1.08, 1.40),
    "maken":     (1.70, 1.70, 1.25, 1.30),
}
# morph (0 = golf, 1 = orb): vast per toestand; rust doorloopt de hele cyclus
MORPH = {"luisteren": 0.0, "praten": 0.0, "denken": 1.0, "maken": 1.0}
# looptijd in seconden en fps per toestand
CLIP = {"rust": (8.0, 20), "luisteren": (4.0, 20), "denken": (4.0, 20),
        "praten": (4.0, 20), "maken": (4.0, 20)}
STRANDS = 38
SAMPLES = 1100


def ease(x):
    return x * x * (3 - 2 * x)


def idle_morph(phase):
    if phase < 0.30: return ease(phase / 0.30)
    if phase < 0.67: return 1.0
    if phase < 0.95: return 1.0 - ease((phase - 0.67) / 0.28)
    return 0.0


def blur(img, sigma):
    """Gaussische vervaging (separabel) met numpy."""
    r = int(sigma * 3)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = np.apply_along_axis(lambda v: np.convolve(v, k, mode="same"), 0, img)
    return np.apply_along_axis(lambda v: np.convolve(v, k, mode="same"), 1, out)


def splat(acc, x, y, w):
    ix = np.floor(x).astype(int); iy = np.floor(y).astype(int)
    fx = x - ix; fy = y - iy
    ok = (ix >= 0) & (iy >= 0) & (ix < W - 1) & (iy < W - 1)
    ix, iy, fx, fy, w = ix[ok], iy[ok], fx[ok], fy[ok], w[ok]
    for dx, dy, ww in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)),
                       (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        np.add.at(acc, (iy + dy, ix + dx), ww * w)


def hsl_to_rgb(h, s, l):
    import colorsys
    r, g, b = colorsys.hls_to_rgb((h % 360) / 360.0, l, s)
    return np.array([r, g, b], np.float32)


def render(state, s, D):
    """Een frame op tijd s (seconden) in een lus van D seconden."""
    speed, energy, spread, bright = PRESETS[state]
    tt = 0.48 * speed * D            # t-bereik over de hele lus
    t = tt * (s / D)

    def w(omega):                     # frequentie afronden zodat de lus naadloos sluit
        n = max(1, round(omega * tt / TAU))
        return n * TAU / tt

    if state == "rust":
        morph = idle_morph(s / D)
    else:
        morph = MORPH[state]

    u = np.linspace(0, 1, SAMPLES)
    theta = u * TAU
    env = np.clip(1 - (2 * u - 1) ** 2, 0, None) ** 1.3
    core = np.zeros((W, W), np.float32)   # per kanaal apart
    acc = [np.zeros((W, W), np.float32) for _ in range(3)]
    glow = [np.zeros((W, W), np.float32) for _ in range(3)]
    gl2 = [np.zeros((W, W), np.float32) for _ in range(3)]

    for j in range(STRANDS):
        strand = j / (STRANDS - 1)
        off = (strand - 0.5) * 2
        hue = 180 + strand * 95
        xw = 35 + u * 396
        yw = (M + off * 65 * env * spread
              + np.sin(u * 19 + w(2.3) * t / 1 * 1 + j * 0.22) * 16 * env * energy
              + np.sin(u * 31 - w(1.7) * t + j * 0.4) * 8 * env * energy)
        r = ((92 + off * 29) * spread
             + np.sin(theta * 3 + w(1.8) * t + j * 0.27) * 13 * energy
             + np.sin(theta * 5 - w(1.3) * t + j * 0.15) * 9 * energy)
        a = theta + j * 0.065
        xo = M + np.cos(a) * r
        yo = M + np.sin(a) * r * 0.9
        drift = np.sin(w(1.2) * t + j * 0.67 + u * 6) * 5 * energy
        x = xw * (1 - morph) + xo * morph + drift
        y = yw * (1 - morph) + yo * morph

        alpha = min(0.75, (0.13 + 0.18 * math.sin(j * 2.7) ** 2) * bright)
        col = hsl_to_rgb(hue, 1.0, 0.68)
        lw = 0.7 + (0.7 if j % 5 == 0 else 0)
        # stapgrootte langs de lijn: gewicht = dichtheid * lijnbreedte
        seg = np.hypot(np.diff(x, prepend=x[0]), np.diff(y, prepend=y[0]))
        seg[0] = seg[1]
        wt = np.minimum(seg, 3.0) * lw * alpha * 1.6
        big = (j % 4 == 0)
        for c in range(3):
            splat(acc[c], x, y, wt * col[c])
        for c in range(3):
            splat((glow if big else gl2)[c], x, y, wt * col[c] * (1.0 if big else 0.6) * bright)

    img = np.zeros((W, W, 3), np.float32)
    for c in range(3):
        img[..., c] = acc[c] + blur(glow[c], 5.0) * 2.6 + blur(gl2[c], 2.0) * 1.2
    img = BG + img
    img = 1 - np.exp(-img * 1.35)          # zachte begrenzing in plaats van harde clip
    img = np.clip(img, 0, 1)
    # rond masker
    yy, xx = np.mgrid[0:W, 0:W]
    img[(xx - M) ** 2 + (yy - M) ** 2 > (M) ** 2] = 0
    return (img * 255).astype(np.uint8)


def _frame_job(job):
    import io
    st, i, n, D, q = job
    im = Image.fromarray(render(st, D * i / n, D))
    b = io.BytesIO()
    im.save(b, format="JPEG", quality=q, subsampling=2, optimize=True)
    return st, i, b.getvalue()


def build(args):
    """Pakt alle lussen in een bestand: JVAN-header, clip-tabel, frame-tabel, JPEG-blobs."""
    from concurrent.futures import ProcessPoolExecutor
    jobs = []
    for st in PRESETS:
        D, fps = CLIP[st]
        n = int(D * fps)
        jobs += [(st, i, n, D, args.quality) for i in range(n)]
    frames = {st: {} for st in PRESETS}
    with ProcessPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 2)) as ex:
        for k, (st, i, data) in enumerate(ex.map(_frame_job, jobs, chunksize=4)):
            frames[st][i] = data
            if k % 40 == 0:
                print(f"{k}/{len(jobs)}", flush=True)
    names = list(PRESETS)
    head = 8 + len(names) * 24
    table_size = sum(len(frames[s]) * 8 for s in names)
    blob_pos = head + table_size
    clip_hdr, table, blobs = b"", b"", b""
    tpos = head
    for st in names:
        D, fps = CLIP[st]
        n = len(frames[st])
        clip_hdr += struct.pack("<12sHHII", st.encode(), n, fps, tpos, 0)
        for i in range(n):
            d = frames[st][i]
            table += struct.pack("<II", blob_pos + len(blobs), len(d))
            blobs += d
        tpos += n * 8
    out = struct.pack("<4sHH", b"JVAN", len(names), W) + clip_hdr + table + blobs
    path = os.path.join(args.out, "animations.bin")
    open(path, "wb").write(out)
    print("klaar:", path, len(out), "bytes,", sum(len(v) for v in frames.values()), "frames")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="out")
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--quality", type=int, default=72)
    ap.add_argument("--build", action="store_true", help="alle lussen renderen en inpakken in animations.bin")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.build:
        build(args)
        return
    if args.preview:
        sizes = []
        for st in PRESETS:
            D, fps = CLIP[st]
            for k, frac in enumerate((0.0, 0.5)):
                im = Image.fromarray(render(st, frac * D, D))
                path = os.path.join(args.out, f"preview_{st}_{k}.jpg")
                im.save(path, quality=args.quality, subsampling=2)
                sizes.append(os.path.getsize(path))
                im.save(os.path.join(args.out, f"preview_{st}_{k}.png"))
        print("JPEG-grootte (bytes):", sizes, "gem", sum(sizes) // len(sizes))
        return


if __name__ == "__main__":
    main()
