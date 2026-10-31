"""Zet de NASA-maankaart om voor de modus Maan (1024x512 JPEG, baseline, voor TJpgDec in M5GFX).

Grijs met een koele blauwe zweem, passend bij de gestileerde aarde.
Bron (publiek domein, NASA Scientific Visualization Studio, CGI Moon Kit):
  lroc_color_poles_2k.tif (LRO/LROC, 2048x1024)
Gebruik: python tools/maan_kaart.py <maan.tif> [voorbeeld.png] [schijngestalte 0..1, 0,5 = vol]
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageFilter

W, H = 1024, 512
uit = Path(__file__).resolve().parent.parent / "firmware" / "main" / "assets" / "moon"
uit.mkdir(parents=True, exist_ok=True)

src = Image.open(sys.argv[1]).convert("RGB").resize((W, H), Image.LANCZOS)
g = np.asarray(src.convert("L"), np.float32) / 255
g = np.clip((g - 0.12) / 0.70, 0, 1) ** 0.95                  # meer contrast: maria donkerder, hooglanden lichter
kl = np.array([0.10, 0.12, 0.18]) * (1 - g[..., None]) + np.array([0.86, 0.90, 0.98]) * g[..., None]
kaart = Image.fromarray((np.clip(kl, 0, 1) * 255).astype(np.uint8))
kaart = kaart.filter(ImageFilter.UnsharpMask(radius=1.4, percent=110, threshold=1))
kaart.save(uit / "maan.jpg", quality=90, progressive=False, optimize=True)
print("maan.jpg", (uit / "maan.jpg").stat().st_size, "bytes")

if len(sys.argv) > 2:   # voorbeeld van de bol met dezelfde belichting als jarvis_earth.cpp (Maan)
    fase = float(sys.argv[3]) if len(sys.argv) > 3 else 0.35
    e = fase * 2 * np.pi
    Lx, Lz = np.sin(e), -np.cos(e)
    k = np.asarray(kaart, np.float32) / 255
    S, R = 466, 135
    y, x = np.mgrid[0:S, 0:S] + 0.5
    nx, ny = (x - S / 2) / R, -(y - 233) / R
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    lon = np.arctan2(nx, nz)
    col = ((lon + np.pi) / (2 * np.pi) * W).astype(int) % W
    row = np.clip(((np.pi / 2 - np.arcsin(np.clip(ny, -1, 1))) / np.pi * H).astype(int), 0, H - 1)
    dif = nx * Lx + nz * Lz
    sm = np.clip((dif + 0.06) / 0.18, 0, 1); sm = sm * sm * (3 - 2 * sm)
    lit = 0.05 + 0.95 * sm * np.clip(dif + 0.06, 0, 1) ** 0.6
    img = np.clip(k[row, col] * lit[..., None] * 1.03, 0, 1)
    img[nx * nx + ny * ny > 1] = 0
    Image.fromarray((img * 255).astype(np.uint8)).save(sys.argv[2])
