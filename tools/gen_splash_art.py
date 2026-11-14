#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 JazzMasterT
"""The splash screen's art: Lucca (the dog Kurzhaar is named for) under the KURZHAAR logo; three layers for
firmware/src/splash_art.h, and previews.

  background  240 x 240, RGB565: a synth-sunset sky (indigo -> magenta -> amber), a striped retro sun behind her head,
              a perspective grid on the ground, a glowing sawtooth on the horizon, a few stars
  dog         Lucca, cut out of a photo (OpenCV GrabCut), colour-graded, an amber rim light along her top edges;
              RGB565 + 8-bit alpha (smooth edges). Its source in this tree: assets/splash/dog.png (RGBA), the layer
              itself (the photo stays private)
  logo        NAME in Audiowide (SIL OFL 1.1, assets/fonts/), a sunset gradient, outline, shadow, slanted, fitted to
              the width; and SUBTITLE; RGB565 + alpha

  .venv/bin/python tools/gen_splash_art.py [--preview DIR]                  from assets/splash/dog.png (the usual way)
  .venv/bin/python tools/gen_splash_art.py --photo PHOTO.jpg [--preview DIR] a new dog layer from a photo (and saves it)"""
import argparse
from pathlib import Path
import numpy as np
import cv2
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "assets" / "fonts" / "Audiowide-Regular.ttf"
S = 240
NAME, SUBTITLE = "KURZHAAR", "der Basshund"
DOG_PNG = ROOT / "assets" / "splash" / "dog.png"

# where she goes: the photo's crop (head, ears, the ticked throat, the collar), its size and place on the screen
CROP = (88, 108, 704, 694)
DOG_W = 186
DOG_X, DOG_Y = 36, 74                    # top left on the screen (her head under the logo)


# ---------------------------------------------------------------------------------------------- Lucca, cut out
def cut_out(photo):
    img = np.asarray(photo)[:, :, ::-1].copy()                     # OpenCV: BGR
    h, w = img.shape[:2]
    b, g, r = [img[..., i].astype(np.int32) for i in range(3)]
    lum = (299 * r + 587 * g + 114 * b) // 1000
    mx, mn = np.maximum(np.maximum(r, g), b), np.minimum(np.minimum(r, g), b)
    sat = (mx - mn) / np.maximum(mx, 1)
    mask = np.full((h, w), cv2.GC_PR_BGD, np.uint8)
    mask[100:h, 60:720] = cv2.GC_PR_FGD                             # roughly where she is
    green = (g > r + 8) & (g > b + 12) & (lum > 55)
    mask[green] = cv2.GC_BGD                                         # grass, trees
    mask[:95, :] = cv2.GC_BGD                                        # above her head
    mask[:300, :][(lum[:300] > 150) & (sat[:300] < 0.25)] = cv2.GC_BGD   # the blurred white car
    yy, xx = np.mgrid[0:h, 0:w]
    head = (xx > 170) & (xx < 640) & (yy > 170) & (yy < 640)
    chest = (xx > 300) & (xx < 620) & (yy > 560)
    mask[head & (lum < 70) & ~green] = cv2.GC_FGD                    # her dark fur
    mask[chest & ~green & (sat < 0.25)] = cv2.GC_FGD                 # the ticked chest (black and white, not green)
    core = ((xx - 395) / 215.0) ** 2 + ((yy - 335) / 185.0) ** 2 <= 1.0   # the core of her head: certainly her (the grey
    mask[core & ~green] = cv2.GC_FGD                                 # fur by her brow looks like the blurred car)
    body = ((xx < 250) & (yy > 560)) | ((xx < 120) & (yy > 420)) | ((xx < 175) & (yy > 520))
    mask[body] = cv2.GC_BGD                                          # her body, lying behind: not in a portrait
    bgd, fgd = np.zeros((1, 65), np.float64), np.zeros((1, 65), np.float64)
    cv2.grabCut(img, mask, None, bgd, fgd, 6, cv2.GC_INIT_WITH_MASK)
    fg = ((mask == cv2.GC_FGD) | (mask == cv2.GC_PR_FGD)).astype(np.uint8)
    n, lab, stats, _ = cv2.connectedComponentsWithStats(fg, 8)
    if n > 1:
        fg = (lab == 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))).astype(np.uint8)
    fg = cv2.morphologyEx(fg, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8))
    cnts, _ = cv2.findContours(fg, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    fg = np.zeros_like(fg)
    cv2.drawContours(fg, cnts, -1, 1, thickness=cv2.FILLED)           # no holes
    alpha = cv2.GaussianBlur(fg.astype(np.float32), (0, 0), 1.6)      # a soft edge (smooth after downscaling)
    return np.clip((alpha - 0.5) * 2.2 + 0.5, 0, 1)


def lucca_layer(photo, alpha):
    x0, y0, x1, y1 = CROP
    rgb = np.asarray(photo).astype(np.float32)[y0:y1, x0:x1] / 255.0
    a = alpha[y0:y1, x0:x1]
    # grade: a little more contrast in the mids, warmer shadows (her liver tones), the whites kept clean
    lum = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    rgb = np.clip((rgb - 0.5) * 1.12 + 0.5 + 0.02, 0, 1)
    warm = np.clip(1 - lum * 1.6, 0, 1)[..., None]
    rgb = np.clip(rgb * (1 + warm * np.array([0.10, 0.02, -0.06], np.float32)), 0, 1)
    h = round((y1 - y0) * DOG_W / (x1 - x0))
    rgb = cv2.resize(rgb, (DOG_W, h), interpolation=cv2.INTER_AREA)
    a = cv2.resize(a, (DOG_W, h), interpolation=cv2.INTER_AREA)
    # the rim light: amber along the edges that face up and to the sun (behind her head), a few pixels deep
    inside = (a > 0.5).astype(np.uint8)
    dist = cv2.distanceTransform(inside, cv2.DIST_L2, 3)
    gy, gx = np.gradient(cv2.GaussianBlur(a, (0, 0), 2.0))
    facing = np.clip(-gy * 1.0 - gx * 0.25, 0, None)                # outward normal up (and a little left)
    facing = facing / max(facing.max(), 1e-6)
    rim = np.clip(1 - dist / 3.5, 0, 1) * np.clip(facing * 2.2, 0, 1) * (a > 0.05)
    amber = np.array([1.0, 0.72, 0.38], np.float32)
    rgb = rgb * (1 - rim[..., None] * 0.65) + amber * rim[..., None] * 0.95
    return np.clip(rgb, 0, 1), np.clip(a, 0, 1)


# ---------------------------------------------------------------------------------------------- the background
def background():
    y = np.linspace(0, 1, S)[:, None]
    stops = [(0.00, (14, 12, 40)), (0.30, (44, 22, 80)), (0.52, (120, 36, 104)), (0.66, (206, 62, 104)),
             (0.74, (255, 120, 86)), (0.79, (255, 192, 112)), (0.80, (255, 210, 140)), (0.81, (40, 18, 46)),
             (1.00, (16, 10, 26))]
    img = np.zeros((S, S, 3), np.float32)
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        k = ((y >= p0) & (y <= p1))[..., 0]
        f = ((y - p0) / max(p1 - p0, 1e-6))[..., 0]
        for ch in range(3):
            img[:, :, ch] = np.where(k[:, None], c0[ch] + (c1[ch] - c0[ch]) * f[:, None], img[:, :, ch])
    horizon = int(0.805 * S)
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    rng = np.random.default_rng(7)                                    # stars in the upper sky
    for _ in range(38):
        sx, sy = rng.integers(0, S), rng.integers(0, int(S * 0.42))
        img[sy, sx] = np.maximum(img[sy, sx], (200 + rng.integers(0, 55),) * 3)
    cx, cy, R = 128, 132, 60                                           # the sun behind her head: stripes in its lower half
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    t = np.clip((yy - (cy - R)) / (2 * R), 0, 1)
    sun = np.stack([255 + 0 * t, 230 - 140 * t, 120 - 30 * t], -1)
    stripe = ((yy > cy - R * 0.1) & (((yy - cy) % 9) < 2 + (yy - cy) * 0.06))
    inside = (d <= R) & (yy < horizon) & ~stripe
    img[inside] = sun[inside]
    glow = np.exp(-np.clip(d - R, 0, None) / 22.0)[..., None] * (yy < horizon)[..., None]
    img = img + glow * np.array([120, 50, 40]) * 0.55
    g = (yy > horizon)                                                 # the ground: a perspective grid, faint magenta
    vp = (cx, horizon - 30)
    for k in range(-14, 15):
        x_b = vp[0] + k * 34
        line = np.abs((xx - vp[0]) - (x_b - vp[0]) * (yy - vp[1]) / (S - vp[1])) < 0.7
        img[g & line] = img[g & line] * 0.4 + np.array([200, 60, 140]) * 0.6
    for k in range(1, 9):
        gy = horizon + int((S - horizon) * (k / 8.0) ** 1.8)
        if gy < S:
            img[gy, :] = img[gy, :] * 0.45 + np.array([200, 60, 140]) * 0.55
    wave = np.zeros((S, S), np.float32)                                # a glowing sawtooth on the horizon
    for x in range(S):
        ph = (x % 24) / 24.0
        wy = horizon - 2 - int(ph * 9)
        wave[wy:wy + 2, x] = 1.0
        if x % 24 == 23:
            wave[horizon - 11:horizon, x] = 1.0
    glow_w = cv2.GaussianBlur(wave, (0, 0), 2.5)
    img = img + glow_w[..., None] * np.array([255, 150, 60]) * 1.2 + wave[..., None] * np.array([255, 220, 150])
    return np.clip(img / 255.0, 0, 1)


# ---------------------------------------------------------------------------------------------- the logo
def logo_layer():
    W, H, k = S, 66, 4                                                 # drawn at 4x, then down: smooth edges
    big = Image.new("RGBA", (W * k, H * k), (0, 0, 0, 0))
    text = NAME
    size = 52                                                          # as big as fits (room for the slant, shadow)
    while ImageFont.truetype(str(FONT), size * k).getbbox(text)[2] > (W - 26) * k:
        size -= 1
    font = ImageFont.truetype(str(FONT), size * k)
    tw = font.getbbox(text)[2]
    tx = (W * k - tw) // 2 + 6 * k
    ty = 2 * k + (52 - size) * k                                       # smaller lettering: lower, just above the subtitle
    def layer(fill, dx=0, dy=0, stroke=0):
        im = Image.new("L", big.size, 0)
        ImageDraw.Draw(im).text((tx + dx, ty + dy), text, font=font, fill=fill, stroke_width=stroke, stroke_fill=fill)
        return im
    shadow = layer(255, 4 * k, 4 * k, 3 * k).filter(ImageFilter.GaussianBlur(2 * k))
    outline = layer(255, 0, 0, 3 * k)
    body = layer(255)
    _, bt, _, bb = font.getbbox(text)                                  # the sunset in the letters, top to bottom
    yy = np.clip((np.arange(H * k)[:, None] - (ty + bt)) / (bb - bt), 0, 1)
    stops = [(0.0, (255, 246, 214)), (0.35, (255, 196, 92)), (0.62, (255, 106, 77)), (1.0, (214, 52, 120))]
    grad = np.zeros((H * k, W * k, 3), np.float32)
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        m = (yy >= p0) & (yy <= p1)
        f = (yy - p0) / (p1 - p0)
        for ch in range(3):
            grad[..., ch] = np.where(m, c0[ch] + (c1[ch] - c0[ch]) * f, grad[..., ch])
    out = np.zeros((H * k, W * k, 4), np.float32)
    sh = np.asarray(shadow, np.float32) / 255 * 0.85
    ol = np.asarray(outline, np.float32) / 255
    bd = np.asarray(body, np.float32) / 255
    out[..., :3] = np.array([18, 8, 30]) * sh[..., None]
    out[..., 3] = sh
    a = ol; out[..., :3] = out[..., :3] * (1 - a[..., None]) + np.array([26, 10, 36]) * a[..., None]; out[..., 3] = out[..., 3] * (1 - a) + a
    a = bd; out[..., :3] = out[..., :3] * (1 - a[..., None]) + grad * a[..., None]; out[..., 3] = out[..., 3] * (1 - a) + a
    # slant (italic), then down to size
    im = Image.fromarray(np.dstack([np.clip(out[..., :3], 0, 255), np.clip(out[..., 3] * 255, 0, 255)]).astype(np.uint8))
    im = im.transform(im.size, Image.AFFINE, (1, 0.18, -0.18 * H * k * 0.5, 0, 1, 0), resample=Image.BICUBIC)
    im = im.resize((W, H), Image.LANCZOS)
    a = np.asarray(im, np.float32)
    return a[..., :3] / 255.0, a[..., 3] / 255.0


def subtitle_layer():
    W, H, k = S, 18, 4
    im = Image.new("L", (W * k, H * k), 0)
    font = ImageFont.truetype(str(FONT), 13 * k)
    text = SUBTITLE
    tw = font.getbbox(text)[2]
    ImageDraw.Draw(im).text(((W * k - tw) // 2, 0), text, font=font, fill=255)
    a = np.asarray(im.resize((W, H), Image.LANCZOS), np.float32) / 255
    return a


# ---------------------------------------------------------------------------------------------- output
def rgb565(rgb):
    c = np.clip(rgb * 255 + 0.5, 0, 255).astype(np.uint32)
    return ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)


def carr(name, ctype, data, per=24):
    flat = data.reshape(-1)
    fmt = (lambda v: f"0x{int(v):04X}u") if ctype == "uint16_t" else (lambda v: str(int(v)))
    rows = [", ".join(fmt(v) for v in flat[i:i + per]) for i in range(0, len(flat), per)]
    return f"static const {ctype} {name}[{len(flat)}] = {{\n    " + ",\n    ".join(rows) + "\n};\n"


def emit(bg, dog, dog_a, logo, logo_a, sub_a, path):
    dh, dw = dog_a.shape
    lh, lw = logo_a.shape
    sh, sw = sub_a.shape
    txt = ["/* SPDX-License-Identifier: GPL-3.0-only */", "/* Copyright (C) 2026 JazzMasterT */",
           "/* Generated by tools/gen_splash_art.py from assets/splash/dog.png: the splash's layers. The lettering is",
           " * Audiowide (c) 2012 Brian J. Bonislawsky DBA Astigmatic, SIL Open Font License 1.1 (assets/fonts/), rendered",
           " * here as an image. Do not edit. */",
           "#ifndef SPLASH_ART_H", "#define SPLASH_ART_H", "#include <stdint.h>",
           f"#define ART_DOG_W {dw}u", f"#define ART_DOG_H {dh}u", f"#define ART_DOG_X {DOG_X}", f"#define ART_DOG_Y {DOG_Y}",
           f"#define ART_LOGO_W {lw}u", f"#define ART_LOGO_H {lh}u", f"#define ART_SUB_W {sw}u", f"#define ART_SUB_H {sh}u",
           carr("ART_BG", "uint16_t", rgb565(bg)),
           carr("ART_DOG", "uint16_t", rgb565(dog)), carr("ART_DOG_A", "uint8_t", np.round(dog_a * 255)),
           carr("ART_LOGO", "uint16_t", rgb565(logo)), carr("ART_LOGO_A", "uint8_t", np.round(logo_a * 255)),
           carr("ART_SUB_A", "uint8_t", np.round(sub_a * 255)), "#endif"]
    path.write_text("\n".join(txt) + "\n")
    kb = (bg.size // 3 * 2 + dog_a.size * 3 + logo_a.size * 3 + sub_a.size) / 1024
    print(f"wrote {path}: {kb:.0f} KB of art")


def compose(bg, dog, dog_a, logo, logo_a, sub_a):
    img = bg.copy()
    dh, dw = dog_a.shape
    img[DOG_Y:DOG_Y + dh, DOG_X:DOG_X + dw] = (img[DOG_Y:DOG_Y + dh, DOG_X:DOG_X + dw] * (1 - dog_a[..., None])
                                               + dog * dog_a[..., None])[:S - DOG_Y]
    lh = logo_a.shape[0]
    img[0:lh] = img[0:lh] * (1 - logo_a[..., None]) + logo * logo_a[..., None]
    sh = sub_a.shape[0]
    y = 56
    img[y:y + sh] = img[y:y + sh] * (1 - sub_a[..., None] * 0.95) + np.array([1.0, 0.92, 0.80]) * sub_a[..., None] * 0.95
    return img


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--photo", help="make the dog layer anew from this photo (saved as assets/splash/dog.png)")
    ap.add_argument("--preview")
    a = ap.parse_args()
    alpha = None
    if a.photo:
        photo = Image.open(a.photo).convert("RGB")
        alpha = cut_out(photo)
        dog, dog_a = lucca_layer(photo, alpha)
        dog, dog_a = dog[:S - DOG_Y], dog_a[:S - DOG_Y]               # (what shows on the screen)
        DOG_PNG.parent.mkdir(parents=True, exist_ok=True)
        rgba = np.dstack([np.clip(dog * 255 + 0.5, 0, 255), np.round(dog_a * 255)]).astype(np.uint8)
        Image.fromarray(rgba, "RGBA").save(DOG_PNG)
        print("wrote", DOG_PNG)
    rgba = np.asarray(Image.open(DOG_PNG).convert("RGBA"), np.float32) / 255   # the layer, as the firmware gets it
    dog, dog_a = rgba[..., :3], rgba[..., 3]
    bg = background()
    logo, logo_a = logo_layer()
    sub_a = subtitle_layer()
    emit(bg, dog, dog_a, logo, logo_a, sub_a, ROOT / "firmware" / "src" / "splash_art.h")
    if a.preview:
        d = Path(a.preview); d.mkdir(parents=True, exist_ok=True)
        if alpha is not None: Image.fromarray((alpha * 255).astype(np.uint8)).save(d / "mask.png")
        img = compose(bg, dog, dog_a, logo, logo_a, sub_a)
        Image.fromarray((img * 255).astype(np.uint8)).resize((S * 3, S * 3), Image.NEAREST).save(d / "splash_3x.png")
        Image.fromarray((img * 255).astype(np.uint8)).save(d / "splash_1x.png")
        print("preview", d / "splash_3x.png")
