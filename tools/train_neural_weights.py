"""Train the offline neural networks used by the neural rendering suite.

Writes assets/neural/neural_networks.bin, which src/render/neural_rendering.cpp loads at startup.

Two networks are trained here. The third, the neural radiance cache, trains online on the GPU
every frame and has no offline weights.

1. Neural shading: a 16 -> 32 -> 32 -> 3 ReLU MLP that learns the analytic film-emulation grade
   `film_reference` below (dye crosstalk, shadow tint, highlight shoulder, distance haze and
   vignette). The same function is written in assets/shaders/neural_rendering.hlsl so the GPU
   measures how closely the network matches it on every frame.
2. Neural texture compression: a 64 x 64 x 8 grid of 8-bit latents plus a 16 -> 32 -> 32 -> 3
   decoder MLP that reconstructs a 512 x 512 crop of the film's own Glass Orchid frame
   (packaging/store-screenshots/02-mirrors.png): glass petals, the pink heart and the city behind.

Deterministic: fixed seeds, float32, no GPU. Usage:
    python tools/train_neural_weights.py [--steps-scale 1.0]
"""
import argparse
import struct
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets/neural/neural_networks.bin"
TEXTURE = ROOT / "packaging/store-screenshots/02-mirrors.png"
TEXTURE_CROP = (800, 40, 1920, 1160)  # the Glass Orchid, square

IN, HID, OUTC = 16, 32, 3
TEX = 512
LAT = 64
LATC = 8
MAGIC = b"BNRL"
VERSION = 1


def tmap(x):
    return x / (1.0 + x)


def film_reference(rgb, d01, u, v):
    """Analytic grade the shading network learns. Mirrors filmReference() in the HLSL."""
    m = np.array([[0.92, 0.06, 0.02], [0.05, 0.89, 0.06], [0.02, 0.07, 0.91]], np.float32)
    c = rgb @ m.T
    lum = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    c = c + 0.020 * np.array([1.00, 0.62, 0.80], np.float32) * np.exp(-6.0 * lum)[:, None]
    c = c / (1.0 + 0.04 * c)
    lum = c @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    haze = (0.10 * d01 * d01)[:, None]
    cool = lum[:, None] * np.array([0.85, 0.95, 1.15], np.float32)
    c = c + (cool - c) * haze
    r2 = (u - 0.5) ** 2 + (v - 0.5) ** 2
    return (c * (1.0 - 0.22 * r2)[:, None]).astype(np.float32)


def shade_features(rgb, d01, u, v):
    """Network inputs. Mirrors shadeFeatures() in the HLSL."""
    lum = rgb @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    r2 = (u - 0.5) ** 2 + (v - 0.5) ** 2
    pi = np.float32(np.pi)
    cols = [tmap(rgb[:, 0]), tmap(rgb[:, 1]), tmap(rgb[:, 2]), tmap(lum),
            np.log2(lum + 1e-4) / 12.0, d01, d01 * d01, u, v, r2,
            np.sin(pi * u), np.cos(pi * u), np.sin(pi * v), np.cos(pi * v),
            np.sin(2 * pi * u), np.sin(2 * pi * v)]
    return np.stack(cols, axis=1).astype(np.float32)


class Mlp:
    """16 -> 32 -> 32 -> 3 ReLU MLP with Adam. Row-major W[out][in], as the HLSL reads it."""

    def __init__(self, rng, nin=IN, nout=OUTC):
        def init(o, i):
            return (rng.standard_normal((o, i)) * np.sqrt(2.0 / i)).astype(np.float32)
        self.p = [init(HID, nin), np.zeros(HID, np.float32), init(HID, HID), np.zeros(HID, np.float32),
                  init(nout, HID) * 0.1, np.zeros(nout, np.float32)]
        self.m = [np.zeros_like(a) for a in self.p]
        self.v = [np.zeros_like(a) for a in self.p]
        self.t = 0

    def forward(self, x):
        w1, b1, w2, b2, w3, b3 = self.p
        z1 = x @ w1.T + b1
        h1 = np.maximum(z1, 0)
        z2 = h1 @ w2.T + b2
        h2 = np.maximum(z2, 0)
        return h2 @ w3.T + b3, (x, z1, h1, z2, h2)

    def backward(self, cache, dy):
        x, z1, h1, z2, h2 = cache
        w1, b1, w2, b2, w3, b3 = self.p
        g3 = dy.T @ h2
        gb3 = dy.sum(0)
        d2 = (dy @ w3) * (z2 > 0)
        g2 = d2.T @ h1
        gb2 = d2.sum(0)
        d1 = (d2 @ w2) * (z1 > 0)
        g1 = d1.T @ x
        gb1 = d1.sum(0)
        dx = d1 @ w1
        return [g1, gb1, g2, gb2, g3, gb3], dx

    def step(self, grads, lr, b1=0.9, b2=0.999, eps=1e-8):
        self.t += 1
        for i, g in enumerate(grads):
            self.m[i] = b1 * self.m[i] + (1 - b1) * g
            self.v[i] = b2 * self.v[i] + (1 - b2) * g * g
            mh = self.m[i] / (1 - b1 ** self.t)
            vh = self.v[i] / (1 - b2 ** self.t)
            self.p[i] -= lr * mh / (np.sqrt(vh) + eps)

    def flat(self):
        return np.concatenate([a.ravel() for a in self.p]).astype(np.float32)


def sample_shading_batch(rng, n):
    lum = np.exp2(rng.uniform(-9.0, 5.0, n)).astype(np.float32)
    chroma = np.clip(1.0 + 0.6 * rng.standard_normal((n, 3)), 0.0, None).astype(np.float32)
    rgb = lum[:, None] * chroma
    pick = rng.uniform(size=n)
    rgb[pick < 0.08] = 0.0
    sat = pick > 0.95
    rgb[sat] = (rng.uniform(size=(sat.sum(), 3)) ** 4 * np.exp2(rng.uniform(-4, 4, (sat.sum(), 1)))).astype(np.float32)
    d01 = rng.uniform(size=n).astype(np.float32)
    u = rng.uniform(size=n).astype(np.float32)
    v = rng.uniform(size=n).astype(np.float32)
    x = shade_features(rgb, d01, u, v)
    target = tmap(film_reference(rgb, d01, u, v))
    base = tmap(rgb)
    return x, target - base, base, target


def psnr(mse):
    return float(10.0 * np.log10(1.0 / max(mse, 1e-12)))


def train_shading(steps, rng):
    net = Mlp(rng)
    for i in range(steps):
        x, resid, _, _ = sample_shading_batch(rng, 4096)
        y, cache = net.forward(x)
        dy = 2.0 * (y - resid) / len(x)
        grads, _ = net.backward(cache, dy)
        lr = 3e-3 * (0.5 * (1 + np.cos(np.pi * i / steps))) + 5e-5
        net.step(grads, lr)
        if i % 1000 == 0:
            print(f"shading step {i:6d} mse {np.mean((y - resid) ** 2):.3e}", flush=True)
    x, resid, base, target = sample_shading_batch(np.random.default_rng(99), 200000)
    y, _ = net.forward(x)
    pred = np.clip(base + y, 0.0, 0.999)
    score = psnr(np.mean((pred - target) ** 2))
    print(f"shading validation PSNR {score:.2f} dB (tone-mapped space)")
    return net, score


def ntc_positional(u, v):
    """Sub-cell position features. Mirrors ntcFeatures() in the HLSL."""
    two_pi = np.float32(2 * np.pi)
    cols = []
    for k in (1.0, 2.0):
        cols += [np.sin(two_pi * k * LAT * u), np.cos(two_pi * k * LAT * u),
                 np.sin(two_pi * k * LAT * v), np.cos(two_pi * k * LAT * v)]
    return np.stack(cols, axis=1).astype(np.float32)


def bilinear_setup(u, v):
    """Texel-centred bilinear taps on the LAT x LAT latent grid, clamped at the edges."""
    fx = u * LAT - 0.5
    fy = v * LAT - 0.5
    x0 = np.floor(fx).astype(np.int64)
    y0 = np.floor(fy).astype(np.int64)
    tx = (fx - x0).astype(np.float32)
    ty = (fy - y0).astype(np.float32)
    xs = [np.clip(x0, 0, LAT - 1), np.clip(x0 + 1, 0, LAT - 1)]
    ys = [np.clip(y0, 0, LAT - 1), np.clip(y0 + 1, 0, LAT - 1)]
    idx = [ys[0] * LAT + xs[0], ys[0] * LAT + xs[1], ys[1] * LAT + xs[0], ys[1] * LAT + xs[1]]
    w = [(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty]
    return idx, w


def train_ntc(steps, rng):
    img = Image.open(TEXTURE).convert("RGB").crop(TEXTURE_CROP)
    # Lanczos-filtered square crop at 512 x 512; the shipped reference is the same pixels.
    img = img.resize((TEX, TEX), Image.LANCZOS)
    ref = np.asarray(img, np.float32) / 255.0
    flat_ref = ref.reshape(-1, 3)
    py, px = np.mgrid[0:TEX, 0:TEX]
    all_u = ((px.ravel() + 0.5) / TEX).astype(np.float32)
    all_v = ((py.ravel() + 0.5) / TEX).astype(np.float32)

    latents = (rng.standard_normal((LAT * LAT, LATC)) * 0.1).astype(np.float32)
    lm = np.zeros_like(latents)
    lv = np.zeros_like(latents)
    net = Mlp(rng)
    quantized = None
    scale = bias = None
    for i in range(steps):
        # Quantization-aware tail: freeze 8-bit latents for the last 15 % and refine the decoder only.
        if quantized is None and i >= int(steps * 0.85):
            lo = latents.min(0)
            hi = latents.max(0)
            scale = np.maximum((hi - lo) / 255.0, 1e-8).astype(np.float32)
            bias = lo.astype(np.float32)
            quantized = np.clip(np.round((latents - bias) / scale), 0, 255).astype(np.uint8)
            latents = (quantized.astype(np.float32) * scale + bias).astype(np.float32)
            print("ntc: latents quantized to 8 bits, refining decoder", flush=True)
        sel = rng.integers(0, TEX * TEX, 16384)
        u, v = all_u[sel], all_v[sel]
        idx, w = bilinear_setup(u, v)
        lat = sum(latents[idx[k]] * w[k][:, None] for k in range(4))
        x = np.concatenate([lat, ntc_positional(u, v)], axis=1)
        y, cache = net.forward(x)
        target = flat_ref[sel]
        dy = 2.0 * (y - target) / len(sel)
        grads, dx = net.backward(cache, dy)
        lr = 4e-3 * (0.5 * (1 + np.cos(np.pi * i / steps))) + 5e-5
        net.step(grads, lr)
        if quantized is None:
            gl = np.zeros_like(latents)
            dlat = dx[:, :LATC]
            for k in range(4):
                np.add.at(gl, idx[k], dlat * w[k][:, None])
            lm = 0.9 * lm + 0.1 * gl
            lv = 0.999 * lv + 0.001 * gl * gl
            t = i + 1
            latents -= (lr * 2.5) * (lm / (1 - 0.9 ** t)) / (np.sqrt(lv / (1 - 0.999 ** t)) + 1e-8)
        if i % 1000 == 0:
            print(f"ntc step {i:6d} mse {np.mean((y - target) ** 2):.3e}", flush=True)

    idx, w = bilinear_setup(all_u, all_v)
    lat = sum(latents[idx[k]] * w[k][:, None] for k in range(4))
    y, _ = net.forward(np.concatenate([lat, ntc_positional(all_u, all_v)], axis=1))
    rec = np.round(np.clip(y, 0, 1) * 255.0) / 255.0
    score = psnr(np.mean((rec - flat_ref) ** 2))
    print(f"ntc full-texture PSNR {score:.2f} dB, {LAT}x{LAT}x{LATC} 8-bit latents")
    ref8 = np.concatenate([np.round(ref * 255).astype(np.uint8),
                           np.full((TEX, TEX, 1), 255, np.uint8)], axis=2)
    return net, quantized, scale, bias, ref8, score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps-scale", type=float, default=1.0)
    args = ap.parse_args()
    shade_net, shade_psnr = train_shading(int(12000 * args.steps_scale), np.random.default_rng(20261120))
    ntc_net, latents, scale, bias, ref8, ntc_psnr = train_ntc(int(14000 * args.steps_scale),
                                                              np.random.default_rng(1120))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<7I", VERSION, IN, HID, OUTC, TEX, LAT, LATC))
        f.write(struct.pack("<2f", shade_psnr, ntc_psnr))
        f.write(shade_net.flat().tobytes())
        f.write(ntc_net.flat().tobytes())
        f.write(scale.astype("<f4").tobytes())
        f.write(bias.astype("<f4").tobytes())
        f.write(latents.tobytes())   # [LAT*LAT][LATC] uint8, row-major from the top-left cell
        f.write(ref8.tobytes())      # [TEX][TEX] RGBA8 reference, for the live PSNR and the solo view
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
