"""Zet een NASA-planeetkaart om voor de StopWatch (1024x512 JPEG, baseline, voor TJpgDec in M5GFX).

Kleuren licht opgepept en verscherpt voor het kleine scherm. Bronnen (publiek domein, via Wikimedia Commons):
  mars:    Mars_Viking_MDIM21_ClrMosaic_1km.jpg (Viking/USGS, 3840 px-versie)
  jupiter: Jupiter_Cylindrical_Map_-_Dec_2000_PIA07782.jpg (Cassini, NASA/JPL/SSI)
Gebruik: python tools/planeet_kaart.py <kaart.jpg> <naam> [voorbeeld.png] [schijngestalte 0..1, 0,5 = vol]
  schrijft firmware/main/assets/planets/<naam>.jpg
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageEnhance, ImageFilter

W, H = 1024, 512
naam = sys.argv[2]
uit = Path(__file__).resolve().parent.parent / "firmware" / "main" / "assets" / "planets"
uit.mkdir(parents=True, exist_ok=True)

kaart = Image.open(sys.argv[1]).convert("RGB").resize((W, H), Image.LANCZOS)
kaart = ImageEnhance.Color(kaart).enhance(1.35)
kaart = ImageEnhance.Contrast(kaart).enhance(1.15)
kaart = kaart.filter(ImageFilter.UnsharpMask(radius=1.3, percent=140, threshold=1))
kaart.save(uit / f"{naam}.jpg", quality=90, progressive=False, optimize=True)
print(f"{naam}.jpg", (uit / f"{naam}.jpg").stat().st_size, "bytes")

if len(sys.argv) > 3:   # voorbeeld van de bol, zelfde belichting als de maan in jarvis_earth.cpp
    fase = float(sys.argv[4]) if len(sys.argv) > 4 else 0.35
    e = fase * 2 * np.pi
    k = np.asarray(kaart, np.float32) / 255
    S, R = 466, 135
    y, x = np.mgrid[0:S, 0:S] + 0.5
    nx, ny = (x - S / 2) / R, -(y - 233) / R
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    col = (((np.arctan2(nx, nz) + np.pi) / (2 * np.pi) * W).astype(int)) % W
    row = np.clip(((np.pi / 2 - np.arcsin(np.clip(ny, -1, 1))) / np.pi * H).astype(int), 0, H - 1)
    dif = nx * np.sin(e) - nz * np.cos(e) + 0.06
    sm = np.clip(dif / 0.18, 0, 1); sm = sm * sm * (3 - 2 * sm)
    lit = 0.05 + 0.95 * sm * np.clip(dif, 0, 1) ** 0.6
    img = np.clip(k[row, col] * lit[..., None] * 1.03, 0, 1)
    img[nx * nx + ny * ny > 1] = 0
    Image.fromarray((img * 255).astype(np.uint8)).save(sys.argv[3])
