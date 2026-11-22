#!/usr/bin/env python3
"""Bake the scattering sky table embedded in assets/shaders/m2_mesh.hlsl.

The city shader's scene constants are root constants with no room for a per-frame table, and the
shader has no spare texture slot, so the sky is baked here for a handful of sun elevations and
written into the shader as constant arrays between the <sky-table> markers.

Model: spectral (16 wavelengths) Rayleigh + Mie + ozone single scattering, plus multiple scattering after Hillaire 2020
("A Scalable and Production Ready Sky and Atmosphere Rendering Technique"): an isotropic
second-order term scaled by the infinite-bounce transfer factor. Lengths are in kilometres and the
sun's irradiance is 1; each sun elevation is then scaled to an art-directed exposure, the way a
camera opens up as the light goes (EXPOSURE_TARGET).

For each sun elevation the bake writes
  kSkyRayleigh  Rayleigh single scattering (phase included) + multiple scattering
  kSkyMie       Mie single scattering WITHOUT its phase function (the shader applies the phase,
                so the aureole round the sun stays sharp at any table resolution)
over view elevation x azimuth from the sun, and the irradiance (divided by pi) the sky puts on an
up-facing surface and on walls facing towards and away from the sun, plus the radiance of a white
surface facing the sun at the ground and at cloud height.

Usage:
  python tools/gen_sky_table.py                # rewrite the table in m2_mesh.hlsl
  python tools/gen_sky_table.py --check        # fail if the shader's table is stale
  python tools/gen_sky_table.py --preview DIR  # also write tone-mapped panoramas to DIR
then run tools/refresh_asset_hashes.py.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
SHADER = ROOT / "assets" / "shaders" / "m2_mesh.hlsl"
BEGIN = "// <sky-table>"
END = "// </sky-table>"

# Atmosphere (km, 1/km), computed per wavelength and converted to linear sRGB at the end. Three
# colour bands are not enough at twilight: ozone absorbs across 500-650 nm, and sampled only at
# 680/550/440 nm the blue hour comes out purple.
RG, RT = 6360.0, 6460.0
H_R, H_M = 8.0, 1.2
WAVELENGTHS = np.arange(400.0, 701.0, 20.0)                      # nm
BETA_R = 1.24062e-3 * (WAVELENGTHS * 1e-3) ** -4.0                # Rayleigh, Bruneton 2017
MIE_SCALE = 1.2                      # a little more aerosol than clear mountain air; the city's own
                                     # haze is the shader's fog, lit by this sky
MIE_ANGSTROM = 0.6                   # haze scatters blue a little more than red
BETA_MS = 3.996e-3 * MIE_SCALE * (WAVELENGTHS / 550.0) ** -MIE_ANGSTROM
BETA_ME = BETA_MS * (4.440 / 3.996)
# Ozone absorption cross-section (m^2), 360-830 nm in 10 nm steps (Bruneton 2017, after Serdyuchenko
# et al.), for a 300 Dobson-unit column in a 30 km-wide layer centred on 25 km.
_OZONE = np.array([
    1.18e-27, 2.182e-28, 2.818e-28, 6.636e-28, 1.527e-27, 2.763e-27, 5.52e-27, 8.451e-27, 1.582e-26,
    2.316e-26, 3.669e-26, 4.924e-26, 7.752e-26, 9.016e-26, 1.48e-25, 1.602e-25, 2.139e-25, 2.755e-25,
    3.091e-25, 3.5e-25, 4.266e-25, 4.672e-25, 4.398e-25, 4.701e-25, 5.019e-25, 4.305e-25, 3.74e-25,
    3.215e-25, 2.662e-25, 2.238e-25, 1.852e-25, 1.473e-25, 1.209e-25, 9.423e-26, 7.455e-26, 6.566e-26,
    5.105e-26, 4.15e-26, 4.228e-26, 3.237e-26, 2.451e-26, 2.801e-26, 2.534e-26, 1.624e-26, 1.465e-26,
    2.078e-26, 1.383e-26, 7.105e-27])
_OZONE_PEAK = 300.0 * 2.687e20 / 15000.0                          # molecules / m^3 at 25 km
BETA_O = np.interp(WAVELENGTHS, 360.0 + 10.0 * np.arange(len(_OZONE)), _OZONE) * _OZONE_PEAK * 1e3
GROUND_ALBEDO = 0.22
OBSERVER_KM = 0.2
CLOUD_KM = 6.0                       # cloud tops keep the sun for a while after it has set below
MIE_G = 0.78                         # must match kSkyMieG in the shader

# Table axes. View elevation is sampled on a square-root scale and azimuth from the sun on a
# power-1.5 scale: dense at the horizon and round the sun, where the sky changes fastest. The darkest
# sky is the blue hour (sun 6.5 degrees down); a lower sun keeps that sky, so nights are never black.
SUN_ELEVATIONS = [-6.5, -4.5, -3.0, -1.5, 0.0, 2.0, 5.0, 11.0, 25.0, 60.0]
N_ELEV, N_AZIM = 8, 9
AZIM_POWER = 1.5
# Sky irradiance on an up-facing surface divided by pi, per sun elevation, after exposure.
EXPOSURE_TARGET = [0.085, 0.110, 0.135, 0.155, 0.170, 0.180, 0.185, 0.195, 0.230, 0.280]

LUMA = np.array([0.2126, 0.7152, 0.0722])


def _lobe(x, mu, s1, s2):
    return np.exp(-0.5 * ((x - mu) / np.where(x < mu, s1, s2)) ** 2)


def _spectral_to_rgb():
    """Matrix from a spectrum (per unit sun irradiance) to linear sRGB, white-balanced to the sun."""
    w = WAVELENGTHS
    # CIE 1931 colour-matching functions, Wyman, Sloan and Shirley's 2013 fit.
    x = 1.056 * _lobe(w, 599.8, 37.9, 31.0) + 0.362 * _lobe(w, 442.0, 16.0, 26.7) - 0.065 * _lobe(w, 501.1, 20.4, 26.2)
    y = 0.821 * _lobe(w, 568.8, 46.9, 40.5) + 0.286 * _lobe(w, 530.9, 16.3, 31.1)
    z = 1.217 * _lobe(w, 437.0, 11.8, 36.0) + 0.681 * _lobe(w, 459.0, 26.0, 13.8)
    sun = 1.0 / (w ** 5 * (np.exp(1.4388e7 / (w * 5778.0)) - 1.0))   # a 5778 K black body
    xyz = np.stack([x, y, z]) * sun
    to_srgb = np.array([[3.2406, -1.5372, -0.4986], [-0.9689, 1.8758, 0.0415], [0.0557, -0.2040, 1.0570]])
    rgb = to_srgb @ xyz
    return rgb / rgb.sum(axis=1, keepdims=True)


SPECTRAL_TO_RGB = _spectral_to_rgb()


def to_rgb(spectrum):
    """Linear sRGB of a spectrum; colours outside the gamut are desaturated at constant luminance."""
    rgb = np.asarray(spectrum) @ SPECTRAL_TO_RGB.T
    luma = np.clip(rgb @ LUMA, 0.0, None)[..., None]
    low = rgb.min(axis=-1, keepdims=True)
    keep = np.where(low < 0.0, luma / np.maximum(luma - low, 1e-20), 1.0)
    return np.clip(luma + (rgb - luma) * keep, 0.0, None)


def densities(h):
    return np.exp(-h / H_R), np.exp(-h / H_M), np.clip(1.0 - np.abs(h - 25.0) / 15.0, 0.0, None)


def extinction(h):
    d_r, d_m, d_o = densities(h)
    return d_r[..., None] * BETA_R + d_m[..., None] * BETA_ME + d_o[..., None] * BETA_O


def scattering(h):
    d_r, d_m, _ = densities(h)
    return d_r[..., None] * BETA_R + d_m[..., None] * BETA_MS


def ray_end(r, mu):
    """Distance along a ray (radius r, cosine of zenith angle mu) to the ground or the top of the air."""
    ground_disc = r * r * (mu * mu - 1.0) + RG * RG
    hits = (mu < 0.0) & (ground_disc >= 0.0)
    t_top = -r * mu + np.sqrt(np.maximum(r * r * (mu * mu - 1.0) + RT * RT, 0.0))
    t_ground = -r * mu - np.sqrt(np.maximum(ground_disc, 0.0))
    return np.where(hits, t_ground, t_top), hits


class Transmittance:
    """Transmittance from altitude h along zenith angle theta to space (0 if the ray meets the ground)."""
    NH, STEP_DEG, MAX_DEG, STEPS = 64, 0.2, 102.0, 96

    def __init__(self):
        u = np.linspace(0.0, 1.0, self.NH)
        h = (RT - RG) * u * u
        theta = np.arange(0.0, self.MAX_DEG + 1e-6, self.STEP_DEG)
        self.nt = len(theta)
        mu = np.cos(np.radians(theta))
        s = (np.arange(self.STEPS) + 0.5) / self.STEPS
        self.table = np.zeros((self.NH, self.nt, len(WAVELENGTHS)))
        for row, altitude in enumerate(h):
            r = RG + altitude
            t_end, hits = ray_end(np.full(self.nt, r), mu)
            t = t_end[:, None] * s
            radius = np.sqrt(r * r + t * t + 2.0 * r * mu[:, None] * t)
            tau = (extinction(radius - RG) * (t_end / self.STEPS)[:, None, None]).sum(axis=1)
            self.table[row] = np.where(hits[:, None], 0.0, np.exp(-tau))

    def __call__(self, h, mu):
        u = np.sqrt(np.clip(h, 0.0, RT - RG) / (RT - RG)) * (self.NH - 1)
        v = np.degrees(np.arccos(np.clip(mu, -1.0, 1.0))) / self.STEP_DEG
        v = np.clip(v, 0.0, self.nt - 1)
        u0 = np.clip(np.floor(u).astype(int), 0, self.NH - 2)
        v0 = np.clip(np.floor(v).astype(int), 0, self.nt - 2)
        fu = (u - u0)[..., None]
        fv = (v - v0)[..., None]
        tab = self.table
        return ((tab[u0, v0] * (1 - fv) + tab[u0, v0 + 1] * fv) * (1 - fu)
                + (tab[u0 + 1, v0] * (1 - fv) + tab[u0 + 1, v0 + 1] * fv) * fu)


class MultiScatter:
    """Hillaire's multiple-scattering term: radiance per unit sun irradiance, isotropic, at (h, sun angle)."""
    NH, MAX_DEG, DIRS, STEPS = 20, 106, 8, 32

    def __init__(self, trans: Transmittance):
        u = np.linspace(0.0, 1.0, self.NH)
        self.h = (RT - RG - 0.5) * u * u
        sun_theta = np.radians(np.arange(0.0, self.MAX_DEG + 1e-6, 1.0))
        self.ns = len(sun_theta)
        sun = np.stack([np.sin(sun_theta), np.cos(sun_theta), np.zeros_like(sun_theta)], axis=-1)
        # Stratified directions over the whole sphere.
        cos_p = 1.0 - 2.0 * (np.arange(self.DIRS) + 0.5) / self.DIRS
        az = 2.0 * np.pi * (np.arange(self.DIRS) + 0.5) / self.DIRS
        cp, aa = np.meshgrid(cos_p, az, indexing="ij")
        sp = np.sqrt(1.0 - cp * cp)
        dirs = np.stack([sp * np.cos(aa), cp, sp * np.sin(aa)], axis=-1).reshape(-1, 3)
        self.table = np.zeros((self.NH, self.ns, len(WAVELENGTHS)))
        s = (np.arange(self.STEPS) + 0.5) / self.STEPS
        for ih, h in enumerate(self.h):
            r = RG + h
            origin = np.array([0.0, r, 0.0])
            t_end, hits = ray_end(np.full(len(dirs), r), dirs[:, 1])
            t = t_end[:, None] * s
            dt = (t_end / self.STEPS)[:, None, None]
            pos = origin + t[..., None] * dirs[:, None, :]
            radius = np.linalg.norm(pos, axis=-1)
            alt = radius - RG
            ext = extinction(alt)
            tau = np.cumsum(ext * dt, axis=1) - 0.5 * ext * dt
            t_view = np.exp(-tau)
            sig = scattering(alt)
            mu_s = np.einsum("dkc,sc->sdk", pos, sun) / radius[None]
            t_sun = trans(np.broadcast_to(alt, mu_s.shape), mu_s)
            l2 = (t_view[None] * sig[None] * t_sun * dt[None]).sum(axis=2) / (4.0 * np.pi)
            f = (t_view * sig * dt).sum(axis=1)
            # Sunlit ground seen along rays that reach it.
            ground = origin + t_end[:, None] * dirs
            normal = ground / np.linalg.norm(ground, axis=-1, keepdims=True)
            mu_g = normal @ sun.T                                         # [dirs, sun]
            t_ground = np.exp(-(ext * dt).sum(axis=1))                    # [dirs, 3]
            lit = trans(np.zeros_like(mu_g), mu_g) * np.clip(mu_g, 0.0, None)[..., None]
            l2 += np.where(hits[None, :, None],
                           t_ground[None] * lit.transpose(1, 0, 2) * (GROUND_ALBEDO / np.pi), 0.0)
            second = l2.mean(axis=1)
            transfer = f.mean(axis=0)
            self.table[ih] = second / (1.0 - transfer)

    def __call__(self, h, mu_s):
        u = np.sqrt(np.clip(h, 0.0, self.h[-1]) / self.h[-1]) * (self.NH - 1)
        v = np.clip(np.degrees(np.arccos(np.clip(mu_s, -1.0, 1.0))), 0.0, self.ns - 1)
        u0 = np.clip(np.floor(u).astype(int), 0, self.NH - 2)
        v0 = np.clip(np.floor(v).astype(int), 0, self.ns - 2)
        fu = (u - u0)[..., None]
        fv = (v - v0)[..., None]
        tab = self.table
        return ((tab[u0, v0] * (1 - fv) + tab[u0, v0 + 1] * fv) * (1 - fu)
                + (tab[u0 + 1, v0] * (1 - fv) + tab[u0 + 1, v0 + 1] * fv) * fu)


def view_dirs(elev_deg, azim_deg):
    e = np.radians(elev_deg)[:, None]
    a = np.radians(azim_deg)[None, :]
    return np.stack([np.cos(e) * np.cos(a), np.sin(e) * np.ones_like(a), np.cos(e) * np.sin(a)], axis=-1)


def sky(trans, multi, sun_elev_deg, dirs, steps=160):
    """Returns (rayleigh + multiple scattering, mie without phase) for unit sun irradiance."""
    se = math.radians(sun_elev_deg)
    sun = np.array([math.cos(se), math.sin(se), 0.0])
    flat = dirs.reshape(-1, 3)
    origin = np.array([0.0, RG + OBSERVER_KM, 0.0])
    t_end, _ = ray_end(np.full(len(flat), RG + OBSERVER_KM), flat[:, 1])
    edges = (np.arange(steps + 1) / steps) ** 2          # short steps in the dense air near the eye
    mid = 0.5 * (edges[1:] + edges[:-1])
    t = t_end[:, None] * mid
    dt = (t_end[:, None] * (edges[1:] - edges[:-1]))[..., None]
    pos = origin + t[..., None] * flat[:, None, :]
    radius = np.linalg.norm(pos, axis=-1)
    alt = radius - RG
    ext = extinction(alt)
    tau = np.cumsum(ext * dt, axis=1) - 0.5 * ext * dt
    t_view = np.exp(-tau)
    d_r, d_m, _ = densities(alt)
    mu_s = (pos @ sun) / radius
    t_sun = trans(alt, mu_s)
    cos_g = flat @ sun
    phase_r = (3.0 / (16.0 * np.pi)) * (1.0 + cos_g * cos_g)
    rayleigh = (t_view * t_sun * d_r[..., None] * BETA_R * dt).sum(axis=1) * phase_r[:, None]
    multiple = (t_view * scattering(alt) * multi(alt, mu_s) * dt).sum(axis=1)
    mie = (t_view * t_sun * d_m[..., None] * BETA_MS * dt).sum(axis=1)
    shape = dirs.shape[:-1] + (len(WAVELENGTHS),)
    return (rayleigh + multiple).reshape(shape), mie.reshape(shape)


def mie_phase(cos_g, g=MIE_G):
    return (3.0 / (8.0 * np.pi)) * ((1.0 - g * g) * (1.0 + cos_g * cos_g)) / (
        (2.0 + g * g) * np.power(1.0 + g * g - 2.0 * g * cos_g, 1.5))


def irradiance(trans, multi, sun_elev_deg):
    """Sky irradiance / pi on an up-facing surface and on walls facing towards / away from the sun."""
    ne, na = 40, 64
    elev = (np.arange(ne) + 0.5) / ne * 90.0
    azim = (np.arange(na) + 0.5) / na * 180.0           # the sky is symmetric about the sun's plane
    dirs = view_dirs(elev, azim)
    a, b = sky(trans, multi, sun_elev_deg, dirs, steps=96)
    se = math.radians(sun_elev_deg)
    cos_g = dirs @ np.array([math.cos(se), math.sin(se), 0.0])
    radiance = a + b * mie_phase(cos_g)[..., None]
    e = np.radians(elev)[:, None]
    az = np.radians(azim)[None, :]
    d_omega = np.cos(e) * (np.pi / 2 / ne) * (np.pi / na) * 2.0   # both halves of the sky
    up = (radiance * (np.sin(e) * d_omega)[..., None]).sum(axis=(0, 1)) / np.pi
    wall = np.cos(e) * np.cos(az)
    sunward = (radiance * (np.clip(wall, 0, None) * d_omega)[..., None]).sum(axis=(0, 1)) / np.pi
    away = (radiance * (np.clip(-wall, 0, None) * d_omega)[..., None]).sum(axis=(0, 1)) / np.pi
    # A wall receives each half of the sky once; the factor 2 above counted its own half twice.
    return up, sunward * 0.5, away * 0.5


def soften_arch(rgb, sun_elev):
    """Roll off the twilight arch. With the exposure opened for the blue zenith, the glow over the
    set sun is a hundred times brighter and would print as a white patch; a camera's highlight
    roll-off keeps its colour. Applies in full with the sun 3 degrees down, not at all from 2 up."""
    weight = float(np.clip((2.0 - sun_elev) / 5.0, 0.0, 1.0))
    knee, span = 0.5, 1.1
    luma = rgb @ LUMA
    over = np.clip(luma - knee, 0.0, None)
    soft = np.where(luma > knee, knee + over / (1.0 + over / span), luma)
    scale = np.where(luma > 1e-9, soft / np.maximum(luma, 1e-9), 1.0)
    return rgb * (1.0 + weight * (scale - 1.0))[..., None]


def bake():
    trans = Transmittance()
    multi = MultiScatter(trans)
    elev = 90.0 * (np.arange(N_ELEV) / (N_ELEV - 1)) ** 2
    azim = 180.0 * (np.arange(N_AZIM) / (N_AZIM - 1)) ** AZIM_POWER
    dirs = view_dirs(elev, azim)
    out = {k: [] for k in ("rayleigh", "mie", "up", "sunward", "away", "sun", "cloud", "exposure")}
    for sun_elev, target in zip(SUN_ELEVATIONS, EXPOSURE_TARGET):
        a, b = sky(trans, multi, sun_elev, dirs)
        up, sunward, away = (to_rgb(v) for v in irradiance(trans, multi, sun_elev))
        exposure = target / float(up @ LUMA)
        mu = math.sin(math.radians(sun_elev))
        out["rayleigh"].append(soften_arch(to_rgb(a) * exposure, sun_elev))
        out["mie"].append(to_rgb(b) * exposure)
        out["up"].append(up * exposure)
        out["sunward"].append(sunward * exposure)
        out["away"].append(away * exposure)
        # A white surface facing the sun, at the ground and at cloud height: irradiance * transmittance
        # / pi, under this exposure.
        out["sun"].append(to_rgb(trans(np.array(OBSERVER_KM), np.array(mu))) * exposure / np.pi)
        out["cloud"].append(to_rgb(trans(np.array(CLOUD_KM), np.array(mu))) * exposure / np.pi)
        out["exposure"].append(exposure)
    return {k: np.array(v) for k, v in out.items()}, trans, multi


def fmt(v):
    v = float(v)
    if abs(v) < 5e-7:
        return "0.0"
    text = f"{v:.4g}"
    if "e" in text:
        mantissa, exponent = text.split("e")
        if "." not in mantissa:
            mantissa += ".0"
        return f"{mantissa}e{int(exponent)}"
    return text if "." in text else text + ".0"


def vec3(v):
    return f"float3({fmt(v[0])}, {fmt(v[1])}, {fmt(v[2])})"


def pack(rgb):
    """Shared-exponent packing (the RGB9E5 layout): 9-bit mantissas, 5-bit exponent."""
    r, g, b = (max(float(c), 0.0) for c in rgb)
    top = max(r, g, b)
    if top < 2.0 ** -24:
        return 0
    exponent = max(-16, math.floor(math.log2(top))) + 1 + 15
    scale = 2.0 ** (exponent - 24)
    if round(top / scale) == 512:
        exponent += 1
        scale *= 2.0
    assert 0 <= exponent < 32, "sky radiance out of the packed range"
    mantissa = [min(511, int(round(c / scale))) for c in (r, g, b)]
    return mantissa[0] | (mantissa[1] << 9) | (mantissa[2] << 18) | (exponent << 27)


def unpack(value):
    scale = 2.0 ** ((value >> 27) - 24)
    return np.array([value & 511, (value >> 9) & 511, (value >> 18) & 511]) * scale


def emit(t):
    n = len(SUN_ELEVATIONS)
    lines = [
        BEGIN,
        f"// Generated by tools/gen_sky_table.py; do not edit by hand. A scattering sky for {n} sun elevations,",
        "// each under its own exposure. The big tables are packed (9-bit mantissas, shared 5-bit exponent);",
        "// index = (sunKnot * kSkyElevSteps + viewElevation) * kSkyAzimSteps + azimuth, with view elevation",
        "// (0..90 degrees) on a square-root scale and azimuth from the sun (0..180) on a power scale.",
        f"static const uint kSkySunKnots = {n}u;",
        f"static const uint kSkyElevSteps = {N_ELEV}u;",
        f"static const uint kSkyAzimSteps = {N_AZIM}u;",
        f"static const float kSkyAzimPower = {fmt(AZIM_POWER)};",
        f"static const float kSkyMieG = {fmt(MIE_G)};",
        f"static const float kSkySunElev[{n}] = {{ " + ", ".join(fmt(e) for e in SUN_ELEVATIONS) + " };",
    ]

    def small(name, comment, data):
        lines.append(f"// {comment}")
        lines.append(f"static const float3 {name}[{n}] = {{")
        lines.extend("    " + vec3(row) + "," for row in data)
        lines.append("};")

    small("kSkyAmbUp", "Sky irradiance / pi on an up-facing surface.", t["up"])
    small("kSkyAmbSunward", "Sky irradiance / pi on a wall facing the sun's bearing.", t["sunward"])
    small("kSkyAmbAway", "Sky irradiance / pi on a wall facing away from the sun.", t["away"])
    small("kSkySunLight", "Radiance of a white surface facing the sun at the ground.", t["sun"])
    small("kSkyCloudSun", "Radiance of a white surface facing the sun at cloud height (6 km).", t["cloud"])

    def big(name, comment, data):
        lines.append(f"// {comment}")
        lines.append(f"static const uint {name}[{n * N_ELEV * N_AZIM}] = {{")
        for k in range(n):
            lines.append(f"    // sun {fmt(SUN_ELEVATIONS[k])} degrees")
            for i in range(N_ELEV):
                lines.append("    " + " ".join(f"0x{pack(data[k, i, j]):08X}u," for j in range(N_AZIM)))
        lines.append("};")

    big("kSkyRayleigh", "Rayleigh single scattering (phase applied) plus multiple scattering.", t["rayleigh"])
    big("kSkyMie", "Mie single scattering before its phase function.", t["mie"])
    lines.append(END)
    return "\n".join(lines)


def agx_preview(rgb):
    """The film's display transform, close enough to judge the bake by eye."""
    m = np.array([[0.842479062253094, 0.0784335999999992, 0.0792237451477643],
                  [0.0423282422610123, 0.878468636469772, 0.0791661274605434],
                  [0.0423756549057051, 0.0784336, 0.879142973793104]])
    v = np.clip(rgb @ m.T, 1e-10, None)
    v = np.clip((np.log2(v) + 12.47393) / 16.5, 0.0, 1.0)
    v2, v4 = v * v, v ** 4
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232
    return np.clip(np.clip(v, 0.0, 1.0) @ np.linalg.inv(m).T, 0.0, 1.0)


def preview(t, out_dir: Path):
    from PIL import Image
    out_dir.mkdir(parents=True, exist_ok=True)
    width, height = 720, 180
    az = (np.arange(width) + 0.5) / width * 360.0 - 180.0
    el = 90.0 - (np.arange(height) + 0.5) / height * 90.0
    ue = np.sqrt(el / 90.0) * (N_ELEV - 1)
    ua = (np.abs(az) / 180.0) ** (1.0 / AZIM_POWER) * (N_AZIM - 1)
    i0 = np.clip(np.floor(ue).astype(int), 0, N_ELEV - 2)
    j0 = np.clip(np.floor(ua).astype(int), 0, N_AZIM - 2)
    fe = (ue - i0)[:, None, None]
    fa = (ua - j0)[None, :, None]
    rows = []
    packed = {name: np.array([[[unpack(pack(c)) for c in row] for row in knot] for knot in t[name]])
              for name in ("rayleigh", "mie")}
    for k, sun_elev in enumerate(SUN_ELEVATIONS):
        def lookup(tab):
            g = tab[k]
            return ((g[i0][:, j0] * (1 - fa) + g[i0][:, j0 + 1] * fa) * (1 - fe)
                    + (g[i0 + 1][:, j0] * (1 - fa) + g[i0 + 1][:, j0 + 1] * fa) * fe)
        se = math.radians(sun_elev)
        d = view_dirs(el, az)
        cos_g = d @ np.array([math.cos(se), math.sin(se), 0.0])
        rgb = lookup(packed["rayleigh"]) + lookup(packed["mie"]) * mie_phase(cos_g)[..., None]
        rows.append(agx_preview(rgb))
    sheet = np.concatenate(rows, axis=0)
    Image.fromarray((np.clip(sheet, 0, 1) * 255 + 0.5).astype(np.uint8)).save(out_dir / "sky_table_preview.png")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="fail if the shader's table is out of date")
    parser.add_argument("--preview", type=Path, help="write tone-mapped panoramas to this folder")
    args = parser.parse_args()

    tables, _, _ = bake()
    # Keep the file's own line endings (a Windows checkout may hold CRLF).
    with SHADER.open("r", encoding="utf-8", newline="") as handle:
        source = handle.read()
    if BEGIN not in source or END not in source:
        print(f"{SHADER} has no {BEGIN} ... {END} block", file=sys.stderr)
        return 2
    newline = "\r\n" if "\r\n" in source else "\n"
    block = emit(tables).replace("\n", newline)
    head, rest = source.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    updated = head + block + tail
    if args.preview:
        preview(tables, args.preview)
    for k, sun_elev in enumerate(SUN_ELEVATIONS):
        up = tables["up"][k]
        print(f"sun {sun_elev:6.1f}  exposure {tables['exposure'][k]:9.2f}  up {up.round(3)}  "
              f"sunward {tables['sunward'][k].round(3)}  away {tables['away'][k].round(3)}")
    if args.check:
        if updated != source:
            print("sky table in m2_mesh.hlsl is stale; run tools/gen_sky_table.py", file=sys.stderr)
            return 1
        return 0
    if updated != source:
        with SHADER.open("w", encoding="utf-8", newline="") as handle:
            handle.write(updated)
        print(f"wrote sky table to {SHADER.relative_to(ROOT)}")
    else:
        print("sky table unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
