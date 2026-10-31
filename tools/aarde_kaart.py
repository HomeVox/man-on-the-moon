"""Zet de NASA-kaart om voor de modus Aarde (1024x512 JPEG, baseline, voor TJpgDec in M5GFX).

Gestileerd: diepblauwe zee, groen land met reliëf (blauwe zweem), alleen poolijs en Groenland blauwwit.
Bronnen (publiek domein, NASA Visible Earth):
  dag:   Blue Marble Next Generation, world.topo.bathy.200412.3x5400x2700.jpg
Gebruik: python tools/aarde_kaart.py <dag.jpg> [voorbeeld.png]
  met een tweede pad komt er ook een voorbeeld van de bol (zoals op het scherm) bij.
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageFilter

W, H = 1024, 512
uit = Path(__file__).resolve().parent.parent / "firmware" / "main" / "assets" / "earth"
uit.mkdir(parents=True, exist_ok=True)


def blur(a, r):
    return np.asarray(Image.fromarray((np.clip(a, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(r)), np.float32) / 255


src = np.asarray(Image.open(sys.argv[1]).convert("RGB").resize((W, H), Image.LANCZOS), dtype=np.float32) / 255
r, g, b = src[..., 0], src[..., 1], src[..., 2]
lum = 0.30 * r + 0.55 * g + 0.15 * b
zee = np.clip((b / (np.maximum(r, g) + 0.02) - 1.05) * 4, 0, 1)   # 1 = water (blauw overheerst, ook in diepe donkere zee)
zee = blur(zee, 0.5)

# zee: diepte uit de bathymetrie -> van diep marine naar helder blauw (bodemribbels licht verzacht)
t = blur(np.clip((b - 0.15) / 0.35, 0, 1), 1.2)[..., None]
zeekl = np.array([0.03, 0.13, 0.46]) * (1 - t) + np.array([0.08, 0.30, 0.74]) * t
# land: groen met een blauwe zweem; reliëf (bergen, rivierdalen) uit het fijne detail van de helderheid
u = np.clip((lum - 0.10) / 0.70, 0, 1)[..., None]
landkl = np.array([0.16, 0.40, 0.30]) * (1 - u) + np.array([0.48, 0.66, 0.50]) * u
reliëf = (lum - blur(lum, 3.0))[..., None]
landkl = landkl * (1 + 1.5 * reliëf)
ijs = np.clip((lum - 0.55) * 5, 0, 1) * np.clip((b - r * 0.92) * 12 + 0.5, 0, 1)   # licht en niet geel (geen woestijn)
breedte = np.abs(90 - (np.arange(H) + 0.5) / H * 180)[:, None]   # graden van de evenaar
ijs = ijs * np.clip((breedte - 60) / 8, 0, 1)                      # alleen de poolstreken (en Groenland)
ijs = blur(ijs, 0.6)[..., None]
landkl = landkl * (1 - ijs) + np.array([0.74, 0.84, 1.00]) * ijs
kl = zeekl * zee[..., None] + landkl * (1 - zee[..., None])
kaart = Image.fromarray((np.clip(kl, 0, 1) * 255).astype(np.uint8))
kaart = kaart.filter(ImageFilter.UnsharpMask(radius=1.2, percent=160, threshold=1))   # scherper op het kleine scherm
kaart.save(uit / "aarde_dag.jpg", quality=92, progressive=False, optimize=True)
print("aarde_dag.jpg", (uit / "aarde_dag.jpg").stat().st_size, "bytes")

if len(sys.argv) > 2:   # voorbeeld van de bol, met dezelfde belichting als jarvis_earth.cpp
    k = np.asarray(kaart, np.float32) / 255
    S, R, tilt = 466, 135, 0.30
    y, x = np.mgrid[0:S, 0:S] + 0.5
    nx, ny = (x - S / 2) / R, -(y - 233) / R
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    ey, ez = ny * np.cos(tilt) + nz * np.sin(tilt), -ny * np.sin(tilt) + nz * np.cos(tilt)
    lon = np.arctan2(nx, ez) + np.pi + 5 / 360 * 2 * np.pi
    col = ((lon / (2 * np.pi) * W).astype(int)) % W
    row = np.clip(((np.pi / 2 - np.arcsin(np.clip(ey, -1, 1))) / np.pi * H).astype(int), 0, H - 1)
    L = np.array([-0.62, 0.40, 0.68]); L = L / np.linalg.norm(L)    # zijlicht van linksboven
    dif = nx * L[0] + ny * L[1] + nz * L[2] + 0.06
    sm = np.clip(dif / 0.20, 0, 1); sm = sm * sm * (3 - 2 * sm)
    lit = 0.05 + 0.95 * sm * np.clip(dif, 0, 1) ** 0.6
    hz = (1 - nz) ** 2 * 0.45 * sm                                   # dunne atmosfeer, alleen aan de dagkant
    h5 = np.floor(hz * 31)[..., None]
    img = k[row, col] * lit[..., None] * 264 / 256 + h5 * np.array([1 / 8 / 31, 0.75 / 63, 1 / 31])
    img = np.clip(img, 0, 1)
    img[nx * nx + ny * ny > 1] = 0
    Image.fromarray((img * 255).astype(np.uint8)).save(sys.argv[2])
