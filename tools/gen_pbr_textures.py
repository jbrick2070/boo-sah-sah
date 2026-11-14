"""Deterministic PBR Texture Array & GGX IBL Environment Builder for BooSahSah.

Run via Blender 5.2 Python (for native OpenImageIO PNG/JPG/EXR linear & sRGB decoding + NumPy 2.3):
  blender --background --factory-startup --python tools/gen_pbr_textures.py

Outputs:
  assets/textures/pbr_albedo_alpha.btex  (1024x1024x40, 11 mips, DXGI_FORMAT_BC7_UNORM_SRGB = 99)
  assets/textures/pbr_normal.btex        (1024x1024x40, 11 mips, DXGI_FORMAT_BC5_UNORM      = 83)
  assets/textures/pbr_ormt.btex          (1024x1024x40, 11 mips, DXGI_FORMAT_BC7_UNORM      = 98)
  assets/textures/pbr_ibl_env.btex       (512x256x3,     9 mips, DXGI_FORMAT_R16G16B16A16_FLOAT = 10)
  build/pbr_cycles_maps/mat_XX_{albedo,normal,ormt}.png  (for exact Cycles reference parity)
"""
from pathlib import Path
import json
import struct
import time
import zlib
import bpy
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
CC0_DIR = ROOT / "third_party" / "polyhaven_cc0"
OUT_TEX_DIR = ROOT / "assets" / "textures"
CYCLES_MAP_DIR = ROOT / "build" / "pbr_cycles_maps"

RES = 1024
MIPS = 11  # 1024 -> 1
NUM_MATS = 42

DXGI_FORMAT_R16G16B16A16_FLOAT = 10
DXGI_FORMAT_BC5_UNORM = 83
DXGI_FORMAT_BC7_UNORM = 98
DXGI_FORMAT_BC7_UNORM_SRGB = 99


def linear_to_srgb(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(np.maximum(x, 1e-8), 1.0 / 2.4) - 0.055)


def srgb_to_linear(x: np.ndarray) -> np.ndarray:
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.04045, x / 12.92, np.power(np.maximum((x + 0.055) / 1.055, 0.0), 2.4))


def load_image_topdown(path: Path, non_color: bool, target_hw: tuple[int, int] = (RES, RES)) -> np.ndarray:
    img = bpy.data.images.load(str(path))
    img.colorspace_settings.name = "Non-Color" if non_color else "sRGB"
    w, h = img.size
    ch = img.channels
    arr = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, ch)
    bpy.data.images.remove(img)
    # Flip OpenGL bottom-up rows to Direct3D top-down row order
    arr = arr[::-1].copy()
    th, tw = target_hw
    if (h, w) != (th, tw):
        # Clean bilinear / crop resize for non-square scans (e.g., 1026 -> 1024)
        ys = np.linspace(0, h - 1, th, dtype=np.float32)
        xs = np.linspace(0, w - 1, tw, dtype=np.float32)
        y0 = np.floor(ys).astype(np.int32)
        x0 = np.floor(xs).astype(np.int32)
        y1 = np.minimum(y0 + 1, h - 1)
        x1 = np.minimum(x0 + 1, w - 1)
        wy = (ys - y0)[:, None, None]
        wx = (xs - x0)[None, :, None]
        arr = (
            arr[y0[:, None], x0[None, :]] * (1.0 - wy) * (1.0 - wx)
            + arr[y0[:, None], x1[None, :]] * (1.0 - wy) * wx
            + arr[y1[:, None], x0[None, :]] * wy * (1.0 - wx)
            + arr[y1[:, None], x1[None, :]] * wy * wx
        )
    if arr.shape[-1] == 1:
        arr = np.repeat(arr, 4, axis=-1)
    elif arr.shape[-1] == 3:
        arr = np.concatenate([arr, np.ones((th, tw, 1), dtype=np.float32)], axis=-1)
    return arr.astype(np.float32)


def save_png_topdown(path: Path, rgba_float: np.ndarray, is_srgb: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    h, w, _ = rgba_float.shape
    img = bpy.data.images.new("export_tmp", width=w, height=h, alpha=True, float_buffer=False)
    img.colorspace_settings.name = "sRGB" if is_srgb else "Non-Color"
    # Blender expects bottom-up pixels; if sRGB, Blender converts linear float -> sRGB PNG on save
    bot = np.clip(rgba_float[::-1], 0.0, 1.0).astype(np.float32, copy=False)
    img.pixels.foreach_set(np.ascontiguousarray(bot).ravel())
    img.filepath_raw = str(path)
    img.file_format = "PNG"
    img.save()
    bpy.data.images.remove(img)


def periodic_fbm(res: int, base_freq: int, octaves: int, persistence: float, seed: int, aniso: tuple[float, float] = (1.0, 1.0)) -> np.ndarray:
    """100% seamlessly tileable band-limited spectral FBM on [0, 1)^2 via inverse 2D FFT."""
    rng = np.random.default_rng(seed)
    spec = np.zeros((res, res), dtype=np.complex64)
    ky = np.fft.fftfreq(res) * res
    kx = np.fft.fftfreq(res) * res
    KX, KY = np.meshgrid(kx, ky)
    R = np.sqrt((KX / max(aniso[0], 1e-3)) ** 2 + (KY / max(aniso[1], 1e-3)) ** 2)
    amp = 1.0
    freq = float(base_freq)
    for _ in range(octaves):
        band = np.exp(-0.5 * ((R - freq) / max(freq * 0.48, 0.8)) ** 2) * (R > 0.5)
        phase = rng.uniform(0.0, 2.0 * np.pi, size=(res, res)).astype(np.float32)
        mag = rng.normal(0.0, 1.0, size=(res, res)).astype(np.float32)
        spec += (amp * band * mag * (np.cos(phase) + 1j * np.sin(phase))).astype(np.complex64)
        freq *= 2.0
        amp *= persistence
    field = np.fft.ifft2(spec).real.astype(np.float32)
    lo, hi = float(np.percentile(field, 1.0)), float(np.percentile(field, 99.0))
    return np.clip((field - lo) / max(hi - lo, 1e-6), 0.0, 1.0)


def height_to_normal_gl(height: np.ndarray, strength: float) -> np.ndarray:
    """Convert periodic heightfield [0..1] into OpenGL tangent-space normal [-1..1]^3 (+X right, +Y up = -row!)."""
    # Row 0 is top, row H-1 is bottom: +u is +col (axis=1), +v_gl (up image) is -row (axis=0)
    dh_du = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5 * strength
    dh_dv_gl = (np.roll(height, 1, axis=0) - np.roll(height, -1, axis=0)) * 0.5 * strength
    nx = -dh_du
    ny = -dh_dv_gl
    nz = np.ones_like(height, dtype=np.float32)
    norm = np.sqrt(nx * nx + ny * ny + nz * nz)
    return np.stack([nx / norm, ny / norm, nz / norm], axis=-1)


def decode_normal_gl(nor_rgb01: np.ndarray, scale: float = 1.0) -> np.ndarray:
    n = nor_rgb01[..., :3] * 2.0 - 1.0
    n[..., 0] *= scale
    n[..., 1] *= scale
    n[..., 2] = np.maximum(n[..., 2], 1e-3)
    norm = np.linalg.norm(n, axis=-1, keepdims=True)
    return n / np.maximum(norm, 1e-6)


def blend_normals_rnm(n1: np.ndarray, n2: np.ndarray) -> np.ndarray:
    t = n1 + np.array([0.0, 0.0, 1.0], dtype=np.float32)
    u = n2 * np.array([-1.0, -1.0, 1.0], dtype=np.float32)
    r = t * np.sum(t * u, axis=-1, keepdims=True) / np.maximum(t[..., 2:3], 1e-4) - u
    norm = np.linalg.norm(r, axis=-1, keepdims=True)
    return r / np.maximum(norm, 1e-6)


def dilate_rgb_into_transparent(rgb: np.ndarray, alpha: np.ndarray, iterations: int = 12) -> np.ndarray:
    """Prevent dark halo bleeding in mipmaps & bilinear filtering on alpha-cutout botanical leaves."""
    out = rgb.copy()
    valid = alpha > 0.35
    for _ in range(iterations):
        if np.all(valid):
            break
        acc = np.zeros_like(out)
        cnt = np.zeros(alpha.shape, dtype=np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            v_shift = np.roll(np.roll(valid, dy, axis=0), dx, axis=1)
            c_shift = np.roll(np.roll(out, dy, axis=0), dx, axis=1)
            acc += c_shift * v_shift[..., None]
            cnt += v_shift.astype(np.float32)
        fill = (~valid) & (cnt > 0.5)
        out = np.where(fill[..., None], acc / np.maximum(cnt[..., None], 1.0), out)
        valid = valid | fill
    return out


def load_cc0_set(name: str, tint: tuple[float, float, float] = (1.0, 1.0, 1.0),
                 rough_remap: tuple[float, float] = (0.0, 1.0),
                 normal_scale: float = 1.0, metallic_val: float = 0.0, trans_val: float = 0.0):
    tdir = CC0_DIR / "textures" / name
    diff_file = next(tdir.glob("diffuse.*"))
    nor_file = next(tdir.glob("nor_gl.*"))
    arm_file = next(tdir.glob("arm.*"))
    diff = load_image_topdown(diff_file, non_color=False)[..., :3]
    nor = decode_normal_gl(load_image_topdown(nor_file, non_color=True), normal_scale)
    arm = load_image_topdown(arm_file, non_color=True)[..., :3]
    albedo = np.clip(diff * np.array(tint, dtype=np.float32), 0.005, 0.96)
    ao = np.clip(arm[..., 0], 0.15, 1.0)
    rough = np.clip(rough_remap[0] + arm[..., 1] * (rough_remap[1] - rough_remap[0]), 0.02, 0.99)
    metal = np.clip(np.maximum(arm[..., 2], metallic_val), 0.0, 1.0)
    trans = np.full((RES, RES), trans_val, dtype=np.float32)
    alpha = np.ones((RES, RES), dtype=np.float32)
    return np.concatenate([albedo, alpha[..., None]], axis=-1), nor, np.stack([ao, rough, metal, trans], axis=-1)


def make_water_droplets_mask(res: int, count: int, seed: int, radius_range: tuple[float, float]) -> tuple[np.ndarray, np.ndarray]:
    """Return periodic hemispherical droplet height [0..1] and mask [0..1]."""
    rng = np.random.default_rng(seed)
    yy, xx = np.mgrid[0:res, 0:res].astype(np.float32) / float(res)
    height = np.zeros((res, res), dtype=np.float32)
    mask = np.zeros((res, res), dtype=np.float32)
    for _ in range(count):
        cx, cy = rng.uniform(0.0, 1.0, size=2)
        rad = rng.uniform(radius_range[0], radius_range[1])
        aspect = rng.uniform(0.85, 1.22)
        dx = (xx - cx + 0.5) % 1.0 - 0.5
        dy = ((yy - cy + 0.5) % 1.0 - 0.5) / aspect
        r2 = (dx * dx + dy * dy) / (rad * rad)
        inside = r2 < 1.0
        dome = np.where(inside, np.sqrt(np.maximum(1.0 - r2, 0.0)), 0.0)
        height = np.maximum(height, dome)
        mask = np.maximum(mask, np.clip((1.0 - r2) * 3.0, 0.0, 1.0))
    return height, mask


def make_curtain_wall(res: int, xx: np.ndarray, yy: np.ndarray, glass_tint: tuple[float, float, float], frame_tint: tuple[float, float, float], spandrel_tint: tuple[float, float, float], warm_tint: tuple[float, float, float], lit_thresh: float, seed: int):
    cols_grid = (xx * 8.0) % 1.0
    rows_grid = (yy * 6.0) % 1.0
    mullion_x = (cols_grid < 0.048) | (cols_grid > 0.952)
    mullion_y = (rows_grid < 0.075) | (rows_grid > 0.925)
    spandrel = (rows_grid >= 0.075) & (rows_grid < 0.24)
    is_frame = mullion_x | mullion_y
    cell_id = np.floor(xx * 8.0) * 17.0 + np.floor(yy * 6.0) * 31.0 + seed
    room_hash = np.sin(cell_id * 12.9898) * 43758.5453
    room_lit = room_hash - np.floor(room_hash)
    blinds = 0.5 + 0.5 * np.sin(yy * 6.0 * 14.0 * np.pi)
    g_col = np.array(glass_tint, dtype=np.float32)[None, None, :] * (0.76 + 0.36 * room_lit[..., None])
    w_int = np.array(warm_tint, dtype=np.float32)[None, None, :] * ((room_lit > lit_thresh).astype(np.float32) * (0.30 + 0.45 * blinds))[..., None]
    pane_col = np.where(spandrel[..., None], np.array(spandrel_tint, dtype=np.float32), g_col + w_int * 0.35)
    f_col = np.array(frame_tint, dtype=np.float32)
    alb_rgb = np.where(is_frame[..., None], f_col, pane_col)
    h = is_frame.astype(np.float32) * 0.85 + spandrel.astype(np.float32) * 0.16 + periodic_fbm(res, 4, 2, 0.5, seed) * 0.05
    nor = height_to_normal_gl(h, 2.1)
    ao = np.where(is_frame, 0.86, 0.96 - 0.18 * ((cols_grid < 0.08) | (rows_grid < 0.11)).astype(np.float32))
    rough = np.where(is_frame, 0.34, np.where(spandrel, 0.20, 0.055))
    metal = np.where(is_frame, 0.82, 0.12)
    alb = np.concatenate([np.clip(alb_rgb, 0.008, 0.95), np.ones((res, res, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    orm = np.stack([ao, rough, metal, np.where(is_frame, 0.0, 0.85)], axis=-1).astype(np.float32)
    return alb, nor, orm


def build_all_40_materials():
    albedos = [None] * NUM_MATS
    normals = [None] * NUM_MATS
    ormts = [None] * NUM_MATS

    yy, xx = np.mgrid[0:RES, 0:RES].astype(np.float32) / float(RES)

    # 0: StructuralSteel / RivetedDarkBronzeGraphite (canonical slot 0)
    n_mill = periodic_fbm(RES, 6, 5, 0.55, 101)
    n_brush = periodic_fbm(RES, 20, 4, 0.5, 102, aniso=(8.0, 1.0))
    alb0 = np.stack([0.072 + 0.032 * n_mill, 0.084 + 0.036 * n_mill, 0.102 + 0.042 * n_mill, np.ones_like(xx)], axis=-1)
    nor0 = height_to_normal_gl(0.65 * n_mill + 0.35 * n_brush, 1.35)
    orm0 = np.stack([0.84 + 0.16 * n_mill, 0.28 + 0.22 * (1.0 - n_brush), np.full_like(xx, 0.88), np.zeros_like(xx)], axis=-1)
    albedos[0], normals[0], ormts[0] = alb0.astype(np.float32), nor0, orm0.astype(np.float32)

    # 1: WetAsphalt / BasaltSlateDeck (canonical slot 1 — CC0 asphalt_02 + damp puddle islands)
    alb1, nor1, orm1 = load_cc0_set("asphalt_02", tint=(0.22, 0.25, 0.30), rough_remap=(0.12, 0.62), normal_scale=1.15)
    puddle = np.clip((periodic_fbm(RES, 3, 4, 0.55, 100) - 0.52) * 3.2, 0.0, 1.0)
    alb1[..., :3] *= (1.0 - 0.38 * puddle[..., None])
    nor1 = np.where(puddle[..., None] > 0.01,
                    nor1 * (1.0 - 0.86 * puddle[..., None]) + np.array([0.0, 0.0, 1.0], dtype=np.float32) * (0.86 * puddle[..., None]),
                    nor1)
    nor1 /= np.linalg.norm(nor1, axis=-1, keepdims=True)
    orm1[..., 1] = orm1[..., 1] * (1.0 - 0.84 * puddle) + 0.035 * puddle
    orm1[..., 3] = 0.68 * puddle
    albedos[1], normals[1], ormts[1] = alb1.astype(np.float32), nor1.astype(np.float32), orm1.astype(np.float32)

    # 2: ArchitecturalStone / Concrete / Ballast (canonical slot 2 — CC0 concrete_pavers_02 + granite_tile_02)
    alb_c, nor_c, orm_c = load_cc0_set("concrete_pavers_02", tint=(0.76, 0.74, 0.70), rough_remap=(0.44, 0.82), normal_scale=1.15)
    alb_g, nor_g, orm_g = load_cc0_set("granite_tile_02", tint=(0.72, 0.70, 0.66), rough_remap=(0.38, 0.76), normal_scale=0.95)
    albedos[2] = np.clip(alb_c * 0.65 + alb_g * 0.35, 0.02, 0.88).astype(np.float32)
    albedos[2][..., 3] = 1.0
    normals[2] = blend_normals_rnm(nor_c, decode_normal_gl(nor_g * 0.5 + 0.5, 0.55))
    ormts[2] = (orm_c * 0.65 + orm_g * 0.35).astype(np.float32)

    # 3: LowIronBlownCrystalGlass (canonical slot 3 — Orchid wing petals & delicate blown crystal!)
    u_sym = np.abs(xx - 0.5) * 2.0
    vein_freq = u_sym * 6.0 - yy * 0.95
    fine_vein = np.exp(-18.0 * ((vein_freq % 1.0) - 0.5) ** 2) * (1.0 - 0.55 * yy) * (1.0 - 0.35 * u_sym)
    center_rib = np.exp(-140.0 * (xx - 0.5) ** 2) * (1.0 - 0.70 * yy)
    w_blow = periodic_fbm(RES, 5, 3, 0.5, 106)
    cryst3 = (
        np.array([0.84, 0.92, 0.94], dtype=np.float32)[None, None, :] * (1.0 - 0.22 * u_sym[..., None])
        + np.array([0.89, 0.86, 0.95], dtype=np.float32)[None, None, :] * (0.22 * u_sym[..., None])
    )
    nor3 = height_to_normal_gl(fine_vein * 0.14 + center_rib * 0.16 + w_blow * 0.08, 0.16)
    orm3 = np.stack([1.0 - 0.02 * fine_vein, np.clip(0.018 + 0.022 * fine_vein + 0.010 * w_blow, 0.010, 0.055), np.zeros_like(xx), np.full_like(xx, 0.96)], axis=-1)
    albedos[3] = np.concatenate([np.clip(cryst3, 0.05, 0.98), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[3], ormts[3] = nor3, orm3.astype(np.float32)

    # 4: PaleCeladonCrystalGlass (canonical slot 4 — Orchid keeled sepals, gynostemium column & celadon crystal vase!)
    w_blow4 = periodic_fbm(RES, 6, 3, 0.5, 107)
    cryst4 = np.array([0.64, 0.85, 0.80], dtype=np.float32)[None, None, :] * (0.95 + 0.06 * fine_vein[..., None])
    nor4 = height_to_normal_gl(fine_vein * 0.15 + center_rib * 0.18 + w_blow4 * 0.09, 0.18)
    orm4 = np.stack([1.0 - 0.03 * fine_vein, np.clip(0.020 + 0.024 * fine_vein + 0.010 * w_blow4, 0.012, 0.060), np.zeros_like(xx), np.full_like(xx, 0.95)], axis=-1)
    albedos[4] = np.concatenate([np.clip(cryst4, 0.05, 0.96), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[4], ormts[4] = nor4, orm4.astype(np.float32)

    # 5: Taipei101JadeCurtainWall (canonical slot 5)
    albedos[5], normals[5], ormts[5] = make_curtain_wall(
        RES, xx, yy, glass_tint=(0.045, 0.16, 0.19), frame_tint=(0.12, 0.16, 0.17),
        spandrel_tint=(0.032, 0.095, 0.115), warm_tint=(0.45, 0.56, 0.48), lit_thresh=0.56, seed=111
    )

    # 6: CyanJadeSignalAccent (canonical slot 6 — emissive architectural diffuser)
    w_sig = periodic_fbm(RES, 8, 3, 0.5, 108)
    alb6 = np.stack([0.08 + 0.04 * w_sig, 0.76 + 0.10 * w_sig, 0.58 + 0.08 * w_sig, np.ones_like(xx)], axis=-1)
    nor6 = height_to_normal_gl(w_sig, 0.30)
    orm6 = np.stack([np.ones_like(xx), np.full_like(xx, 0.16), np.zeros_like(xx), np.full_like(xx, 0.88)], axis=-1)
    albedos[6], normals[6], ormts[6] = alb6.astype(np.float32), nor6, orm6.astype(np.float32)

    # 7: RippledDarkWater (canonical slot 7 — river, bay & reflecting pool water)
    w1 = periodic_fbm(RES, 7, 4, 0.58, 114, aniso=(1.8, 1.0))
    w2 = periodic_fbm(RES, 19, 3, 0.52, 115, aniso=(1.0, 1.6))
    alb7 = np.stack([np.full_like(xx, 0.012), np.full_like(xx, 0.038), np.full_like(xx, 0.054), np.ones_like(xx)], axis=-1)
    nor7 = height_to_normal_gl(0.65 * w1 + 0.35 * w2, 0.75)
    orm7 = np.stack([np.ones_like(xx), np.full_like(xx, 0.022), np.zeros_like(xx), np.full_like(xx, 0.96)], axis=-1)
    albedos[7], normals[7], ormts[7] = alb7.astype(np.float32), nor7, orm7.astype(np.float32)

    # 8: Brushed24kGoldBrass (canonical slot 8 — TMD sphere, callus crest, ruyi coins, brass collars)
    g_fine = periodic_fbm(RES, 48, 4, 0.60, 103, aniso=(18.0, 1.0))
    g_pat = periodic_fbm(RES, 4, 4, 0.50, 104)
    base_gold = np.array([0.86, 0.60, 0.20], dtype=np.float32)
    alb8 = np.concatenate([base_gold[None, None, :] * (0.90 + 0.16 * g_fine[..., None]) * (0.93 + 0.11 * g_pat[..., None]), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1)
    nor8 = height_to_normal_gl(g_fine * 0.7 + g_pat * 0.3, 1.05)
    orm8 = np.stack([0.92 + 0.08 * g_fine, 0.14 + 0.11 * (1.0 - g_fine) + 0.05 * g_pat, np.full_like(xx, 0.96), np.zeros_like(xx)], axis=-1)
    albedos[8], normals[8], ormts[8] = np.clip(alb8, 0.0, 1.0).astype(np.float32), nor8, orm8.astype(np.float32)

    # 9: OrchidRubyLabellum / PearlSphere (canonical slot 9 — bruised ruby-plum & nectar-gold speckled Liuli crystal!)
    throat_mask = np.clip(1.0 - yy * 1.45, 0.0, 1.0) * np.clip(1.0 - np.abs(xx - 0.5) * 2.4, 0.0, 1.0)
    speck = np.clip((periodic_fbm(RES, 16, 4, 0.6, 136) - 0.58) * 4.5, 0.0, 1.0)
    papillae = periodic_fbm(RES, 64, 3, 0.65, 137)
    lip_ruby = np.array([0.56, 0.065, 0.22], dtype=np.float32)
    lip_gold = np.array([0.92, 0.62, 0.14], dtype=np.float32)
    lip_spot = np.array([0.28, 0.018, 0.075], dtype=np.float32)
    lip_rgb = lip_ruby[None, None, :] * (1.0 - throat_mask[..., None]) + lip_gold[None, None, :] * throat_mask[..., None]
    lip_rgb = lip_rgb * (1.0 - 0.50 * speck[..., None] * throat_mask[..., None]) + lip_spot[None, None, :] * (0.50 * speck[..., None] * throat_mask[..., None])
    nor9 = height_to_normal_gl(papillae * 0.6 + speck * 0.4, 0.70)
    orm9 = np.stack([0.92 + 0.08 * papillae, 0.07 + 0.13 * throat_mask, np.zeros_like(xx), np.full_like(xx, 0.92)], axis=-1)
    albedos[9] = np.concatenate([np.clip(lip_rgb, 0.01, 0.98), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[9], ormts[9] = nor9, orm9.astype(np.float32)

    # 10: BotanicalFoliageAndStems (canonical slot 10 — veined leaf/stem epidermis + subsurface translucency)
    v_leaf = np.abs(xx - 0.5) * 2.0
    lat_vein = np.exp(-42.0 * ((yy * 7.0 - v_leaf * 1.4) % 1.0 - 0.5) ** 2) * (1.0 - 0.6 * v_leaf)
    midrib = np.exp(-180.0 * (xx - 0.5) ** 2) * (1.0 - 0.5 * yy)
    cell_ep = periodic_fbm(RES, 32, 3, 0.55, 112)
    vein_mask = np.clip(midrib + lat_vein * 0.65, 0.0, 1.0)
    leaf_rgb = (np.array([0.038, 0.175, 0.065], dtype=np.float32)[None, None, :] * (0.82 + 0.35 * cell_ep[..., None])
                + np.array([0.085, 0.25, 0.075], dtype=np.float32)[None, None, :] * vein_mask[..., None] * 0.55)
    nor10 = height_to_normal_gl(vein_mask * 0.75 + cell_ep * 0.25, 1.65)
    orm10 = np.stack([0.85 + 0.15 * vein_mask, 0.28 + 0.24 * (1.0 - cell_ep), np.zeros_like(xx), 0.80 - 0.42 * vein_mask], axis=-1)
    albedos[10] = np.concatenate([np.clip(leaf_rgb, 0.01, 0.9), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[10], ormts[10] = nor10, orm10.astype(np.float32)

    # 11: AerospacePearlComposite / PorcelainAntherCap (canonical slot 11 — sleek glossy aerospace composite clearcoat)
    w_comp = periodic_fbm(RES, 10, 3, 0.5, 113)
    alb11 = np.stack([0.84 + 0.03 * w_comp, 0.86 + 0.03 * w_comp, 0.89 + 0.025 * w_comp, np.ones_like(xx)], axis=-1)
    nor11 = height_to_normal_gl(w_comp, 0.045)
    orm11 = np.stack([np.ones_like(xx), 0.10 + 0.045 * w_comp, np.full_like(xx, 0.08), np.full_like(xx, 0.84)], axis=-1)
    albedos[11], normals[11], ormts[11] = alb11.astype(np.float32), nor11, orm11.astype(np.float32)

    # 12: PolishedSteel / ObsidianMirrorStele (canonical slot 12 — optical smoked mirror with vernier border ticks)
    edge_band = ((xx < 0.032) | (xx > 0.968)) & (((yy * 60.0) % 1.0) < 0.18)
    w_obs = periodic_fbm(RES, 5, 3, 0.5, 119)
    alb12_rgb = np.where(edge_band[..., None], np.array([0.48, 0.54, 0.56], dtype=np.float32), np.array([0.22, 0.26, 0.30], dtype=np.float32))
    nor12 = height_to_normal_gl(edge_band.astype(np.float32) * 0.45 + w_obs * 0.05, 0.65)
    orm12 = np.stack([np.ones_like(xx), np.where(edge_band, 0.26, 0.020 + 0.015 * w_obs), np.full_like(xx, 0.86), np.full_like(xx, 0.92)], axis=-1)
    albedos[12] = np.concatenate([alb12_rgb, np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[12], ormts[12] = nor12, orm12.astype(np.float32)

    # 13: WarmHotelFacade (canonical slot 13 — limestone mullions + warm glass)
    albedos[13], normals[13], ormts[13] = make_curtain_wall(
        RES, xx, yy, glass_tint=(0.10, 0.09, 0.085), frame_tint=(0.28, 0.24, 0.20),
        spandrel_tint=(0.16, 0.13, 0.11), warm_tint=(0.64, 0.46, 0.26), lit_thresh=0.50, seed=213
    )

    # 14: CoolHighRiseFacade (canonical slot 14 — silver-blue glass skyscraper)
    albedos[14], normals[14], ormts[14] = make_curtain_wall(
        RES, xx, yy, glass_tint=(0.055, 0.095, 0.145), frame_tint=(0.18, 0.21, 0.24),
        spandrel_tint=(0.042, 0.068, 0.10), warm_tint=(0.42, 0.54, 0.64), lit_thresh=0.58, seed=214
    )

    # 15: WarmOpalInteriorSoffit (canonical slot 15 — warm opal architectural diffuser / cove halo)
    w_op = periodic_fbm(RES, 6, 3, 0.5, 116)
    alb15 = np.stack([0.92 - 0.05 * w_op, 0.72 - 0.06 * w_op, 0.44 - 0.05 * w_op, np.ones_like(xx)], axis=-1)
    nor15 = height_to_normal_gl(w_op, 0.22)
    orm15 = np.stack([np.ones_like(xx), np.full_like(xx, 0.24), np.zeros_like(xx), np.full_like(xx, 0.90)], axis=-1)
    albedos[15], normals[15], ormts[15] = alb15.astype(np.float32), nor15, orm15.astype(np.float32)

    # 16: WarmStreetlampBeacon (canonical slot 16)
    alb16 = np.stack([0.98 - 0.03 * w_op, 0.66 - 0.05 * w_op, 0.24 - 0.04 * w_op, np.ones_like(xx)], axis=-1)
    nor16 = height_to_normal_gl(w_op, 0.20)
    orm16 = np.stack([np.ones_like(xx), np.full_like(xx, 0.16), np.zeros_like(xx), np.full_like(xx, 0.92)], axis=-1)
    albedos[16], normals[16], ormts[16] = alb16.astype(np.float32), nor16, orm16.astype(np.float32)

    # 17: RotorMotionDisc (canonical slot 17)
    rad17 = np.sqrt((xx - 0.5) ** 2 + (yy - 0.5) ** 2)
    rings17 = 0.5 + 0.5 * np.sin(rad17 * 64.0 * np.pi)
    alb17 = np.stack([0.44 + 0.08 * rings17, 0.50 + 0.08 * rings17, 0.56 + 0.08 * rings17, np.ones_like(xx)], axis=-1)
    nor17 = height_to_normal_gl(rings17, 0.35)
    orm17 = np.stack([np.ones_like(xx), np.full_like(xx, 0.22), np.full_like(xx, 0.55), np.zeros_like(xx)], axis=-1)
    albedos[17], normals[17], ormts[17] = alb17.astype(np.float32), nor17, orm17.astype(np.float32)

    # 18: SmokedCanopyGlass (canonical slot 18 -> thinGlass)
    w_can = periodic_fbm(RES, 5, 3, 0.5, 118)
    alb18 = np.stack([np.full_like(xx, 0.08), np.full_like(xx, 0.12), np.full_like(xx, 0.17), np.ones_like(xx)], axis=-1)
    nor18 = height_to_normal_gl(w_can, 0.15)
    orm18 = np.stack([np.ones_like(xx), 0.025 + 0.015 * w_can, np.zeros_like(xx), np.full_like(xx, 0.92)], axis=-1)
    albedos[18], normals[18], ormts[18] = alb18.astype(np.float32), nor18, orm18.astype(np.float32)

    # 19: DistantMountainRidge (canonical slot 19)
    m_can = periodic_fbm(RES, 24, 4, 0.6, 119)
    alb19 = np.stack([0.018 + 0.012 * m_can, 0.032 + 0.022 * m_can, 0.040 + 0.020 * m_can, np.ones_like(xx)], axis=-1)
    nor19 = height_to_normal_gl(m_can, 1.2)
    orm19 = np.stack([0.82 + 0.18 * m_can, 0.88 + 0.08 * (1.0 - m_can), np.zeros_like(xx), np.zeros_like(xx)], axis=-1)
    albedos[19], normals[19], ormts[19] = alb19.astype(np.float32), nor19, orm19.astype(np.float32)

    # 20: HydroxyapatiteEnamel (canonical slot 20 — Hunter-Schreger bands + perikymata micro-ridges + hydrated pearlescent gloss!)
    hsb = 0.5 + 0.5 * np.sin((yy * 28.0 + 0.18 * np.sin(xx * 6.0 * np.pi)) * 2.0 * np.pi)
    peri = 0.5 + 0.5 * np.sin(yy * 96.0 * 2.0 * np.pi + periodic_fbm(RES, 5, 3, 0.5, 117) * 2.5)
    en_var = periodic_fbm(RES, 4, 4, 0.55, 220)
    alb20 = np.stack([0.87 + 0.05 * hsb + 0.03 * en_var, 0.85 + 0.05 * hsb + 0.03 * en_var, 0.79 + 0.06 * hsb + 0.02 * en_var, np.ones_like(xx)], axis=-1)
    nor20 = height_to_normal_gl(0.55 * peri + 0.45 * en_var, 0.52)
    orm20 = np.stack([0.94 + 0.06 * hsb, 0.072 + 0.060 * (1.0 - hsb) + 0.03 * en_var, np.zeros_like(xx), np.full_like(xx, 0.82)], axis=-1)
    albedos[20], normals[20], ormts[20] = alb20.astype(np.float32), nor20, orm20.astype(np.float32)

    # 21: NeuralVelvetSirenRibbon (canonical slot 21 -> thinGlass — woven iridescent silk-glass filaments)
    f1 = np.abs(np.sin((xx * 18.0 + 0.35 * np.sin(yy * 6.0 * np.pi)) * np.pi))
    f2 = np.abs(np.sin((yy * 22.0 + 0.30 * np.cos(xx * 8.0 * np.pi)) * np.pi))
    filaments = np.exp(-6.0 * np.minimum(f1, f2))
    n_vel = periodic_fbm(RES, 12, 4, 0.55, 120)
    alb21_rgb = (np.array([0.08, 0.34, 0.34], dtype=np.float32)[None, None, :] * (0.45 + 0.55 * n_vel[..., None])
                 + np.array([0.24, 0.70, 0.62], dtype=np.float32)[None, None, :] * filaments[..., None] * 0.65)
    nor21 = height_to_normal_gl(filaments * 0.7 + n_vel * 0.3, 1.45)
    orm21 = np.stack([0.78 + 0.22 * filaments, 0.16 + 0.24 * (1.0 - filaments), np.full_like(xx, 0.12), np.full_like(xx, 0.90)], axis=-1)
    albedos[21] = np.concatenate([np.clip(alb21_rgb, 0.01, 0.95), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[21], ormts[21] = nor21, orm21.astype(np.float32)

    # 22: BioluminescentMicroFlora (canonical slot 22 — translucent emerald-gold oral micro-flora dew)
    w_dew = periodic_fbm(RES, 10, 4, 0.55, 123)
    alb22 = np.stack([0.10 + 0.14 * w_dew, 0.52 + 0.22 * w_dew, 0.38 + 0.16 * w_dew, np.ones_like(xx)], axis=-1)
    nor22 = height_to_normal_gl(w_dew, 0.65)
    orm22 = np.stack([np.ones_like(xx), 0.05 + 0.07 * w_dew, np.zeros_like(xx), np.full_like(xx, 0.92)], axis=-1)
    albedos[22], normals[22], ormts[22] = alb22.astype(np.float32), nor22, orm22.astype(np.float32)

    # 23: BrushedChampagneNickelTitanium (canonical slot 23 — CNC lathe-brushed satin nickel faucet & cathedral ribs!)
    ni_brush = periodic_fbm(RES, 64, 4, 0.58, 127, aniso=(1.0, 26.0))
    ni_spot = periodic_fbm(RES, 5, 3, 0.5, 128)
    ni_rgb = np.array([0.66, 0.645, 0.615], dtype=np.float32)[None, None, :] * (0.95 + 0.08 * ni_brush[..., None])
    nor23 = height_to_normal_gl(0.75 * ni_brush + 0.25 * ni_spot, 0.32)
    rough23 = np.clip(0.13 + 0.07 * (1.0 - ni_brush) + 0.025 * ni_spot, 0.09, 0.24)
    orm23 = np.stack([0.96 + 0.04 * ni_brush, rough23, np.full_like(xx, 0.97), np.zeros_like(xx)], axis=-1)
    albedos[23] = np.concatenate([ni_rgb, np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[23], ormts[23] = nor23, orm23.astype(np.float32)

    # 24: VenetianWarmLimePlaster (canonical slot 24 — CC0 plastered_wall_02 + clay_plaster gallery lime plaster)
    alb_p1, nor_p1, orm_p1 = load_cc0_set("plastered_wall_02", tint=(1.18, 1.13, 1.04), rough_remap=(0.64, 0.88), normal_scale=0.65)
    alb_p2, nor_p2, orm_p2 = load_cc0_set("clay_plaster", tint=(1.42, 1.33, 1.20), rough_remap=(0.68, 0.90), normal_scale=0.45)
    blend_p = periodic_fbm(RES, 3, 3, 0.5, 131)[..., None]
    albedos[24] = np.clip(alb_p1 * (0.65 + 0.25 * blend_p) + alb_p2 * (0.35 - 0.25 * blend_p), 0.05, 0.86).astype(np.float32)
    albedos[24][..., 3] = 1.0
    normals[24] = blend_normals_rnm(nor_p1, decode_normal_gl(nor_p2 * 0.5 + 0.5, 0.45))
    ormts[24] = (orm_p1 * 0.7 + orm_p2 * 0.3).astype(np.float32)

    # 25: HandThrownCrazedCeladonStoneware (canonical slot 25 — celadon ceramic vessels & tumbler with throwing rings + crazing)
    c_throw = 0.5 + 0.5 * np.sin(yy * 42.0 * 2.0 * np.pi + periodic_fbm(RES, 4, 2, 0.5, 121) * 1.2)
    c_craze_a = periodic_fbm(RES, 14, 3, 0.6, 122)
    craze_line = np.exp(-320.0 * (c_craze_a - 0.5) ** 2)
    cel_rgb = (np.array([0.32, 0.48, 0.43], dtype=np.float32)[None, None, :] * (0.90 + 0.14 * c_throw[..., None])
               * (1.0 - 0.28 * craze_line[..., None]))
    nor25 = height_to_normal_gl(c_throw * 0.35 - craze_line * 0.65, 0.95)
    orm25 = np.stack([1.0 - 0.22 * craze_line, 0.08 + 0.06 * (1.0 - c_throw) + 0.28 * craze_line, np.zeros_like(xx), np.full_like(xx, 0.72)], axis=-1)
    albedos[25] = np.concatenate([np.clip(cel_rgb, 0.02, 0.95), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[25], ormts[25] = nor25, orm25.astype(np.float32)

    # 26: QuarterSawnWhiteOakVanity (canonical slot 26 — CC0 white_oak_veneer calibrated to warm architectural satin white oak!)
    albedos[26], normals[26], ormts[26] = load_cc0_set(
        "white_oak_veneer", tint=(1.08, 0.96, 0.80), rough_remap=(0.26, 0.54), normal_scale=1.18, trans_val=0.14
    )

    # 27: GlazedPorcelainBasin (calibrated 0.70 linear gallery vitreous china + subtle kiln glaze micro-waviness!)
    op_glaze = periodic_fbm(RES, 12, 3, 0.5, 124)
    wipe_glaze = periodic_fbm(RES, 4, 3, 0.52, 125)
    porc_rgb = np.array([0.705, 0.696, 0.680], dtype=np.float32)[None, None, :] * (0.985 + 0.024 * wipe_glaze[..., None])
    nor27 = height_to_normal_gl(0.65 * op_glaze + 0.35 * wipe_glaze, 0.065)
    rough27 = np.clip(0.032 + 0.026 * wipe_glaze, 0.022, 0.075)
    orm27 = np.stack([np.ones_like(xx), rough27, np.zeros_like(xx), np.full_like(xx, 0.90)], axis=-1)
    albedos[27] = np.concatenate([porc_rgb, np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[27], ormts[27] = nor27, orm27.astype(np.float32)

    # 28: HandmadeVerticalCeramicTile (CC0 long_white_tiles rotated 90 deg CCW for vertical finger-tile backsplash + warm celadon-ivory glaze!)
    alb_t, nor_t, orm_t = load_cc0_set("long_white_tiles", tint=(0.93, 0.95, 0.91), rough_remap=(0.065, 0.78), normal_scale=1.30, trans_val=0.58)
    alb_t = np.rot90(alb_t, k=1).copy()
    orm_t = np.rot90(orm_t, k=1).copy()
    nor_rot = np.rot90(nor_t, k=1).copy()
    nor_t_fixed = np.stack([-nor_rot[..., 1], nor_rot[..., 0], nor_rot[..., 2]], axis=-1)
    albedos[28], normals[28], ormts[28] = alb_t.astype(np.float32), nor_t_fixed.astype(np.float32), orm_t.astype(np.float32)

    # 29: CalatheaFoliageCutout (CC0 calathea_orbifolia_01 real scanned striped tropical leaves + alpha cutout + vein translucency!)
    cal_dir = CC0_DIR / "models" / "calathea_orbifolia_01" / "textures"
    cal_diff = load_image_topdown(next(cal_dir.glob("*diff*")), non_color=False)[..., :3]
    cal_alpha = load_image_topdown(next(cal_dir.glob("*alpha*")), non_color=True)[..., 0]
    cal_nor = decode_normal_gl(load_image_topdown(next(cal_dir.glob("*nor_gl*")), non_color=True), 1.25)
    cal_arm = load_image_topdown(next(cal_dir.glob("*arm*")), non_color=True)[..., :3]
    cal_rgb = dilate_rgb_into_transparent(np.clip(cal_diff * np.array([1.06, 1.10, 0.96], dtype=np.float32), 0.005, 0.92), cal_alpha)
    cal_luma = 0.2126 * cal_rgb[..., 0] + 0.7152 * cal_rgb[..., 1] + 0.0722 * cal_rgb[..., 2]
    cal_trans = np.clip(0.40 + 1.45 * cal_luma, 0.26, 0.92) * (cal_alpha > 0.2).astype(np.float32)
    dh29, dm29 = make_water_droplets_mask(RES, 30, 132, (0.003, 0.007))
    cal_nor = blend_normals_rnm(cal_nor, height_to_normal_gl(dh29 * (cal_alpha > 0.5), 1.05))
    albedos[29] = np.concatenate([cal_rgb, np.clip(cal_alpha, 0.0, 1.0)[..., None]], axis=-1).astype(np.float32)
    normals[29] = cal_nor
    ormts[29] = np.stack([
        np.clip(cal_arm[..., 0], 0.25, 1.0),
        np.clip(cal_arm[..., 1] * 0.76 + 0.08 - 0.14 * dm29, 0.04, 0.70),
        np.zeros_like(xx),
        cal_trans
    ], axis=-1).astype(np.float32)

    # 30: HaworthiaSucculentAtlas (CC0 potted_plant_04 photogrammetry succulent & pot atlas)
    pp_dir = CC0_DIR / "models" / "potted_plant_04" / "textures"
    pp_diff = load_image_topdown(next(pp_dir.glob("*diff*")), non_color=False)[..., :3]
    pp_nor = decode_normal_gl(load_image_topdown(next(pp_dir.glob("*nor_gl*")), non_color=True), 1.15)
    pp_arm = load_image_topdown(next(pp_dir.glob("*arm*")), non_color=True)[..., :3]
    is_green = (pp_diff[..., 1] > pp_diff[..., 0] * 1.12)
    albedos[30] = np.concatenate([np.clip(pp_diff * 1.06, 0.005, 0.94), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[30] = pp_nor
    ormts[30] = np.stack([
        np.clip(pp_arm[..., 0], 0.2, 1.0),
        np.clip(pp_arm[..., 1] * 0.88 + 0.05, 0.08, 0.92),
        np.clip(pp_arm[..., 2], 0.0, 1.0),
        np.where(is_green, 0.68, 0.05)
    ], axis=-1).astype(np.float32)

    # 31: HonedMarbleCountertop (Monolithic grout-free Honed Calacatta Borghini / Carrara warm marble slab!)
    # Pure multi-scale crystalline calcite matrix + sparse sweeping diagonal breccia mineral veins (zero tile seams!)
    calcite_micro = periodic_fbm(RES, 28, 4, 0.55, 310)
    cloud_m = periodic_fbm(RES, 2, 4, 0.52, 315)
    warp_a = periodic_fbm(RES, 2, 4, 0.58, 311, aniso=(2.5, 1.0))
    warp_b = periodic_fbm(RES, 5, 4, 0.55, 312)
    # Single sweeping diagonal ribbon coordinate with natural tectonic kinks (not curly loops)
    v_diag = (xx * 1.0 - yy * 1.0) + 0.22 * (warp_a - 0.5) + 0.07 * (warp_b - 0.5)
    d_rib = np.abs(((v_diag + 0.5) % 1.0) - 0.5) * 2.0  # [0..1], 0 along diagonal vein spine
    vein_mod = periodic_fbm(RES, 3, 3, 0.55, 313)
    vein_primary = np.exp(-95.0 * (d_rib ** 1.75)) * np.clip(vein_mod * 1.35 - 0.18, 0.0, 1.0)
    # Secondary hairline capillary vein branching subtly near main vein
    d_cap = np.abs((((xx * 1.0 - yy * 1.0) + 0.14 * (warp_b - 0.5) + 0.23) % 1.0) - 0.5) * 2.0
    vein_gold = np.exp(-380.0 * (d_cap ** 2.0)) * np.clip(periodic_fbm(RES, 4, 3, 0.55, 314) * 1.4 - 0.25, 0.0, 1.0)
    # Soft cloudy grey mineral shadow alongside main vein
    vein_halo = np.exp(-18.0 * (d_rib ** 2.0)) * vein_mod * 0.42

    marble_rgb = np.array([0.755, 0.734, 0.698], dtype=np.float32)[None, None, :] * (
        0.95 + 0.08 * cloud_m[..., None] + 0.04 * calcite_micro[..., None] - 0.14 * vein_halo[..., None]
    )
    marble_rgb = marble_rgb * (1.0 - 0.42 * vein_primary[..., None]) + np.array([0.36, 0.35, 0.34], dtype=np.float32) * (0.42 * vein_primary[..., None])
    marble_rgb = marble_rgb * (1.0 - 0.34 * vein_gold[..., None]) + np.array([0.58, 0.45, 0.28], dtype=np.float32) * (0.34 * vein_gold[..., None])

    nor31 = height_to_normal_gl(cloud_m * 0.25 + calcite_micro * 0.20 - vein_primary * 0.35 - vein_gold * 0.22, 0.18)
    rough31 = np.clip(0.135 + 0.055 * (1.0 - cloud_m) + 0.09 * vein_primary + 0.05 * vein_gold, 0.10, 0.28)
    orm31 = np.stack([1.0 - 0.04 * vein_primary, rough31, np.zeros_like(xx), np.full_like(xx, 0.32)], axis=-1)
    albedos[31] = np.concatenate([np.clip(marble_rgb, 0.04, 0.92), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[31], ormts[31] = nor31, orm31.astype(np.float32)

    # 32: HonedTerrazzoFloor (CC0 terrazzo_tiles architectural aggregate bathroom floor)
    albedos[32], normals[32], ormts[32] = load_cc0_set(
        "terrazzo_tiles", tint=(1.12, 1.08, 1.02), rough_remap=(0.24, 0.46), normal_scale=0.75, trans_val=0.10
    )

    # 33: WaffleLinenTowel (CC0 rough_linen yarn scan remapped to natural unbleached Belgian oatmeal-ecru waffle spa linen!)
    alb_l, nor_l, orm_l = load_cc0_set("rough_linen", tint=(1.0, 1.0, 1.0), rough_remap=(0.72, 0.95), normal_scale=1.35, trans_val=0.32)
    linen_luma = 0.2126 * alb_l[..., 0] + 0.7152 * alb_l[..., 1] + 0.0722 * alb_l[..., 2]
    linen_norm = np.clip(linen_luma / max(float(np.mean(linen_luma)), 1e-4), 0.55, 1.45)
    waf = np.sin(xx * 42.0 * np.pi) ** 2 * np.sin(yy * 42.0 * np.pi) ** 2
    ecru_rgb = np.array([0.655, 0.612, 0.545], dtype=np.float32)[None, None, :] * linen_norm[..., None] * (0.88 + 0.16 * waf[..., None])
    albedos[33] = np.concatenate([np.clip(ecru_rgb, 0.04, 0.92), np.ones((RES, RES, 1), dtype=np.float32)], axis=-1).astype(np.float32)
    normals[33] = blend_normals_rnm(nor_l, height_to_normal_gl(waf, 0.95))
    orm_l[..., 0] = np.clip(orm_l[..., 0] * (0.84 + 0.16 * waf), 0.2, 1.0)
    ormts[33] = orm_l.astype(np.float32)

    # 34: TrueSilveredFlatMirror (ultra-flat optical silvered bathroom mirror!)
    m_wipe = periodic_fbm(RES, 4, 3, 0.5, 130)
    alb34 = np.stack([np.full_like(xx, 0.94), np.full_like(xx, 0.95), np.full_like(xx, 0.945), np.ones_like(xx)], axis=-1)
    nor34 = np.dstack([np.zeros_like(xx), np.zeros_like(xx), np.ones_like(xx)]).astype(np.float32)
    orm34 = np.stack([np.ones_like(xx), 0.006 + 0.008 * m_wipe, np.full_like(xx, 0.99), np.zeros_like(xx)], axis=-1)
    albedos[34], normals[34], ormts[34] = alb34.astype(np.float32), nor34, orm34.astype(np.float32)

    # 35: AmberApothecaryGlass (translucent warm amber hand-blown glass soap dispenser -> thinGlass!)
    w_amb = periodic_fbm(RES, 5, 3, 0.52, 135)
    alb35 = np.stack([0.68 + 0.10 * w_amb, 0.21 + 0.05 * w_amb, 0.032 + 0.015 * w_amb, np.ones_like(xx)], axis=-1)
    nor35 = height_to_normal_gl(w_amb, 0.08)
    orm35 = np.stack([np.ones_like(xx), np.clip(0.024 + 0.020 * w_amb, 0.014, 0.055), np.zeros_like(xx), np.full_like(xx, 0.94)], axis=-1)
    albedos[35], normals[35], ormts[35] = alb35.astype(np.float32), nor35, orm35.astype(np.float32)

    # 36: FernFoliageCutout (CC0 fern_02 real scanned botanical fern fronds + alpha cutout + translucency!)
    fern_dir = CC0_DIR / "models" / "fern_02" / "textures"
    fern_diff = load_image_topdown(next(fern_dir.glob("*diff*")), non_color=False)[..., :3]
    fern_alpha = load_image_topdown(next(fern_dir.glob("*alpha*")), non_color=True)[..., 0]
    fern_nor = decode_normal_gl(load_image_topdown(next(fern_dir.glob("*nor_gl*")), non_color=True), 1.25)
    fern_arm = load_image_topdown(next(fern_dir.glob("*arm*")), non_color=True)[..., :3]
    fern_rgb = dilate_rgb_into_transparent(np.clip(fern_diff * np.array([0.96, 1.12, 0.92], dtype=np.float32), 0.005, 0.90), fern_alpha)
    albedos[36] = np.concatenate([fern_rgb, np.clip(fern_alpha, 0.0, 1.0)[..., None]], axis=-1).astype(np.float32)
    normals[36] = fern_nor
    ormts[36] = np.stack([
        np.clip(fern_arm[..., 0], 0.25, 1.0),
        np.clip(fern_arm[..., 1] * 0.80 + 0.08, 0.08, 0.78),
        np.zeros_like(xx),
        np.full_like(xx, 0.84) * (fern_alpha > 0.2).astype(np.float32)
    ], axis=-1).astype(np.float32)

    # 37: TwillCarbonFiber (3K 2x2 twill carbon weave for eVTOL blades, boom rails & aerospace struts!)
    tx = (xx * 64.0) % 4.0
    ty = (yy * 64.0) % 4.0
    twill = (((np.floor(tx) + np.floor(ty)) % 4.0) < 2.0).astype(np.float32)
    weave_h = np.where(twill > 0.5, np.sin((tx % 1.0) * np.pi), np.sin((ty % 1.0) * np.pi))
    alb37_v = 0.038 + 0.048 * weave_h + 0.015 * twill
    alb37 = np.stack([alb37_v, alb37_v * 1.04, alb37_v * 1.12, np.ones_like(xx)], axis=-1)
    nor37 = height_to_normal_gl(weave_h, 1.60)
    orm37 = np.stack([0.78 + 0.22 * weave_h, 0.16 + 0.18 * (1.0 - weave_h), np.full_like(xx, 0.35), np.full_like(xx, 0.86)], axis=-1)
    albedos[37], normals[37], ormts[37] = alb37.astype(np.float32), nor37, orm37.astype(np.float32)

    # 38: AnodizedDarkBronze (architectural window mullions, mirror frame, towel bar & ribbed stoneware trim)
    bz_brush = periodic_fbm(RES, 56, 4, 0.6, 138, aniso=(1.0, 16.0))
    bz_pat = periodic_fbm(RES, 5, 3, 0.5, 139)
    alb38 = np.stack([0.13 + 0.035 * bz_pat, 0.105 + 0.028 * bz_pat, 0.082 + 0.022 * bz_pat, np.ones_like(xx)], axis=-1)
    nor38 = height_to_normal_gl(0.65 * bz_brush + 0.35 * bz_pat, 0.90)
    orm38 = np.stack([0.92 + 0.08 * bz_brush, 0.22 + 0.11 * (1.0 - bz_brush), np.full_like(xx, 0.91), np.zeros_like(xx)], axis=-1)
    albedos[38], normals[38], ormts[38] = alb38.astype(np.float32), nor38, orm38.astype(np.float32)

    # 39: MossySanctuaryStoneAndHumus (CC0 concrete_moss wet sanctuary stone & planter humus)
    albedos[39], normals[39], ormts[39] = load_cc0_set(
        "concrete_moss", tint=(0.64, 0.74, 0.62), rough_remap=(0.28, 0.88), normal_scale=1.40, trans_val=0.22
    )

    # 40: ArchitecturalLowIronFloatGlass (optically flat low-iron penthouse curtain-wall glass & walk-in shower partition!)
    w_flt = periodic_fbm(RES, 2, 2, 0.5, 140)
    alb40 = np.stack([np.full_like(xx, 0.93), np.full_like(xx, 0.965), np.full_like(xx, 0.955), np.ones_like(xx)], axis=-1)
    nor40 = height_to_normal_gl(w_flt, 0.008)
    orm40 = np.stack([np.ones_like(xx), np.full_like(xx, 0.012), np.zeros_like(xx), np.full_like(xx, 0.98)], axis=-1)
    albedos[40], normals[40], ormts[40] = alb40.astype(np.float32), nor40, orm40.astype(np.float32)

    # 41: TravertineTerracePavers (CC0 marble_01 running-bond architectural stone pavers for outdoor penthouse balcony!)
    albedos[41], normals[41], ormts[41] = load_cc0_set(
        "marble_01", tint=(0.86, 0.82, 0.76), rough_remap=(0.36, 0.72), normal_scale=0.95, trans_val=0.05
    )

    return albedos, normals, ormts


def encode_bc5_channel(ch: np.ndarray) -> np.ndarray:
    cmax = ch.max(axis=-1)
    cmin = ch.min(axis=-1)
    flat_zero = cmax == 0
    flat_same = (cmax == cmin) & (~flat_zero)
    c0 = np.where(flat_zero, np.uint8(1), cmax)
    c1 = np.where(flat_zero, np.uint8(0), np.where(flat_same, cmax - np.uint8(1), cmin))
    span = np.maximum((c0.astype(np.float32) - c1.astype(np.float32))[..., None], 1.0)
    pos = np.clip(np.round((c0.astype(np.float32)[..., None] - ch.astype(np.float32)) * 7.0 / span), 0, 7).astype(np.uint64)
    lut = np.array([0, 2, 3, 4, 5, 6, 7, 1], dtype=np.uint64)
    codes = lut[pos]
    codes = np.where(flat_zero[..., None], np.uint64(1), np.where(flat_same[..., None], np.uint64(0), codes))
    word = c0.astype(np.uint64) | (c1.astype(np.uint64) << np.uint64(8))
    for i in range(16):
        word |= codes[..., i] << np.uint64(16 + 3 * i)
    return word


def encode_bc5_unorm(xy_uint8: np.ndarray) -> bytes:
    """xy_uint8: (H, W, 2) uint8; pads mips < 4x4 cleanly to 1x1 block."""
    h, w, _ = xy_uint8.shape
    ph = max(4, ((h + 3) // 4) * 4)
    pw = max(4, ((w + 3) // 4) * 4)
    if (h, w) != (ph, pw):
        pad = np.empty((ph, pw, 2), dtype=np.uint8)
        pad[:h, :w] = xy_uint8
        pad[h:, :w] = xy_uint8[h - 1 : h, :w]
        pad[:, w:] = pad[:, w - 1 : w]
        xy_uint8 = pad
    bh, bw = ph // 4, pw // 4
    blocks = xy_uint8.reshape(bh, 4, bw, 4, 2).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 2)
    w0 = encode_bc5_channel(blocks[..., 0])
    w1 = encode_bc5_channel(blocks[..., 1])
    return np.stack([w0, w1], axis=-1).astype("<u8").tobytes()


def encode_bc7_mode6(rgba_uint8: np.ndarray) -> bytes:
    """rgba_uint8: (H, W, 4) uint8; pads mips < 4x4 cleanly to 1x1 block."""
    h, w, _ = rgba_uint8.shape
    ph = max(4, ((h + 3) // 4) * 4)
    pw = max(4, ((w + 3) // 4) * 4)
    if (h, w) != (ph, pw):
        pad = np.empty((ph, pw, 4), dtype=np.uint8)
        pad[:h, :w] = rgba_uint8
        pad[h:, :w] = rgba_uint8[h - 1 : h, :w]
        pad[:, w:] = pad[:, w - 1 : w]
        rgba_uint8 = pad
    bh, bw = ph // 4, pw // 4
    blocks = rgba_uint8.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 4)
    f = blocks.astype(np.float32)
    mean = f.mean(axis=-2, keepdims=True)
    centered = f - mean
    axis = f.max(axis=-2) - f.min(axis=-2) + 1e-4
    axis /= np.linalg.norm(axis, axis=-1, keepdims=True)
    for _ in range(3):
        proj = np.sum(centered * axis[..., None, :], axis=-1, keepdims=True)
        axis = np.sum(centered * proj, axis=-2)
        norm = np.linalg.norm(axis, axis=-1, keepdims=True)
        axis = np.where(norm > 1e-5, axis / np.maximum(norm, 1e-5), np.array([1.0, 0.0, 0.0, 0.0], dtype=np.float32))
    dots = np.sum(centered * axis[..., None, :], axis=-1)
    dmin = dots.min(axis=-1, keepdims=True)
    dmax = dots.max(axis=-1, keepdims=True)
    e0 = np.clip(np.round(mean[..., 0, :] + dmin * axis), 0.0, 255.0)
    e1 = np.clip(np.round(mean[..., 0, :] + dmax * axis), 0.0, 255.0)
    diff = e1 - e0
    denom = np.maximum(np.sum(diff * diff, axis=-1, keepdims=True), 1e-4)
    t = np.clip(np.sum((f - e0[..., None, :]) * diff[..., None, :], axis=-1) / denom, 0.0, 1.0)
    idx = np.clip(np.round(t * 15.0), 0, 15).astype(np.uint64)
    swap = idx[..., 0] >= 8
    e0_s = np.where(swap[..., None], e1, e0).astype(np.uint8)
    e1_s = np.where(swap[..., None], e0, e1).astype(np.uint8)
    idx = np.where(swap[..., None], np.uint64(15) - idx, idx)
    p0 = (np.sum(e0_s & 1, axis=-1) >= 2).astype(np.uint64)
    p1 = (np.sum(e1_s & 1, axis=-1) >= 2).astype(np.uint64)
    q0 = (e0_s >> 1).astype(np.uint64)
    q1 = (e1_s >> 1).astype(np.uint64)
    lo = np.uint64(1 << 6)
    bit = 7
    for ch in range(4):
        lo |= q0[..., ch] << np.uint64(bit); bit += 7
        lo |= q1[..., ch] << np.uint64(bit); bit += 7
    lo |= p0 << np.uint64(63)
    hi = p1.copy()
    hi |= (idx[..., 0] & np.uint64(7)) << np.uint64(1)
    hbit = 4
    for i in range(1, 16):
        hi |= (idx[..., i] & np.uint64(15)) << np.uint64(hbit)
        hbit += 4
    return np.stack([lo, hi], axis=-1).astype("<u8").tobytes()


def write_btex(path: Path, dxgi_format: int, width: int, height: int, array_size: int, mip_count: int, chunks: list[bytes]) -> None:
    payload = b"".join(chunks)
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = struct.pack(
        "<8s8IQ4I",
        b"BTEX001\x00",
        1,
        64,
        dxgi_format,
        width,
        height,
        array_size,
        mip_count,
        crc,
        len(payload),
        0, 0, 0, 0,
    )
    assert len(header) == 64
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + payload)


def build_ibl_env_btex(path: Path) -> None:
    """Build 512x256x3 9-mip GGX-prefiltered RGBA16F equirectangular environment map array."""
    ew, eh, emips = 512, 256, 9
    # Load CC0 Qwantani Dawn PureSky HDRI (1024x512 EXR, linear Rec.709)
    exr_path = CC0_DIR / "hdris" / "qwantani_dawn_puresky_1k.exr"
    dawn_src = load_image_topdown(exr_path, non_color=False, target_hw=(eh, ew))[..., :3]
    # Rotate azimuth so warm rising sun aligns with bathroom right-side window (+X, +Y, +Z-right)
    dawn_src = np.roll(dawn_src, shift=ew // 4, axis=1)
    # Clamp direct solar disc firefly in IBL while preserving warm golden-rose sky energy
    dawn_lum = 0.2126 * dawn_src[..., 0] + 0.7152 * dawn_src[..., 1] + 0.0722 * dawn_src[..., 2]
    dawn_scale = np.minimum(dawn_lum, 6.5) / np.maximum(dawn_lum, 1e-5)
    dawn_sky = dawn_src * dawn_scale[..., None]

    v = (np.arange(eh, dtype=np.float32) + 0.5) / eh
    u = (np.arange(ew, dtype=np.float32) + 0.5) / ew
    theta = v[:, None] * np.pi          # 0 (zenith +Y) .. pi (nadir -Y)
    phi = (u[None, :] - 0.5) * 2.0 * np.pi
    dy = np.cos(theta)
    sin_t = np.sin(theta)
    dx = sin_t * np.sin(phi)
    dz = sin_t * np.cos(phi)

    # Slice 0: Taipei Monsoon Night Sanctuary (rich midnight-indigo zenith, jade/amber/orchid horizon glow, wet road bounce)
    horiz0 = np.exp(-5.5 * np.abs(dy))
    up0 = np.clip(dy, 0.0, 1.0)
    down0 = np.clip(-dy, 0.0, 1.0)
    az_jade = 0.5 + 0.5 * np.cos(phi - 0.4)
    az_warm = 0.5 + 0.5 * np.sin(phi * 2.0 + 0.9)
    slice0 = (
        np.array([0.018, 0.036, 0.072], dtype=np.float32) * (0.4 + 0.6 * up0[..., None])
        + np.array([0.06, 0.34, 0.32], dtype=np.float32) * (horiz0 * (0.45 + 0.55 * az_jade))[..., None]
        + np.array([0.42, 0.24, 0.12], dtype=np.float32) * (horiz0 * 0.45 * az_warm)[..., None]
        + np.array([0.025, 0.055, 0.065], dtype=np.float32) * (down0 * 0.65)[..., None]
    )

    # Slice 1: Shenzhen / Shanghai Coastal Twilight Harbour
    sun1 = np.maximum(dx * (-0.55) + dy * 0.28 + dz * 0.78, 0.0)
    horiz1 = np.exp(-4.2 * np.abs(dy))
    slice1 = (
        np.array([0.05, 0.11, 0.22], dtype=np.float32) * (0.35 + 0.65 * up0[..., None])
        + np.array([0.62, 0.36, 0.18], dtype=np.float32) * (horiz1 * (0.35 + 0.65 * sun1))[..., None]
        + np.array([0.95, 0.58, 0.26], dtype=np.float32) * (sun1 ** 12)[..., None] * 1.4
        + np.array([0.03, 0.07, 0.11], dtype=np.float32) * down0[..., None]
    )

    # Slice 2: Dawn Bathroom Interior + CC0 Qwantani Dawn Window Sky
    # Right hemisphere (dx > 0.25) sees the real CC0 Qwantani Dawn sky through the window;
    # Indoor directions see warm sunlit Venetian plaster walls, white-oak vanity bounce & travertine floor!
    win_mask = np.clip((dx - 0.18) / 0.38, 0.0, 1.0) * np.clip((dy + 0.10) / 0.30, 0.0, 1.0)
    plaster_bounce = (
        np.array([0.46, 0.41, 0.35], dtype=np.float32) * (0.45 + 0.35 * up0[..., None])
        + np.array([0.54, 0.38, 0.24], dtype=np.float32) * np.clip(-dx * 0.5 + 0.5, 0.0, 1.0)[..., None] * 0.42
        + np.array([0.34, 0.28, 0.22], dtype=np.float32) * down0[..., None] * 0.55
    )
    slice2 = dawn_sky * 0.85 * win_mask[..., None] + plaster_bounce * (1.0 - 0.72 * win_mask[..., None])

    base_slices = [slice0.astype(np.float32), slice1.astype(np.float32), slice2.astype(np.float32)]
    chunks = []
    for s_idx, base in enumerate(base_slices):
        cur = base.copy()
        for m in range(emips):
            mw = max(1, ew >> m)
            mh = max(1, eh >> m)
            if m > 0:
                # Separable spherical-aware Gaussian blur before 2x downsample for smooth GGX lobe prefiltering
                blur_iters = 2 if m <= 3 else 3
                tmp = cur
                for _ in range(blur_iters):
                    tmp = 0.5 * tmp + 0.25 * (np.roll(tmp, -1, axis=1) + np.roll(tmp, 1, axis=1))
                    tmp_up = np.concatenate([tmp[0:1], tmp[:-1]], axis=0)
                    tmp_dn = np.concatenate([tmp[1:], tmp[-1:]], axis=0)
                    tmp = 0.5 * tmp + 0.25 * (tmp_up + tmp_dn)
                cur = 0.25 * (tmp[0::2, 0::2] + tmp[0::2, 1::2] + tmp[1::2, 0::2] + tmp[1::2, 1::2])
            assert cur.shape[:2] == (mh, mw)
            rgba16 = np.concatenate([cur, np.ones((mh, mw, 1), dtype=np.float32)], axis=-1).astype("<f2")
            chunks.append(rgba16.tobytes())
    write_btex(path, DXGI_FORMAT_R16G16B16A16_FLOAT, ew, eh, 3, emips, chunks)
    print(f"[OK] {path.name}: {ew}x{eh}x3 ({emips} mips, RGBA16F, {path.stat().st_size / 1048576:.2f} MB)")


def main() -> None:
    t0 = time.perf_counter()
    albedos, normals, ormts = build_all_40_materials()
    t1 = time.perf_counter()
    print(f"Synthesized & calibrated {NUM_MATS} 1024x1024 PBR material sets in {t1 - t0:.2f}s")

    # Export Cycles reference PNGs for the Dawn Bathroom & Hero materials so Cycles uses the exact same maps!
    CYCLES_MAP_DIR.mkdir(parents=True, exist_ok=True)
    for mid in range(NUM_MATS):
        save_png_topdown(CYCLES_MAP_DIR / f"mat_{mid:02d}_albedo.png", albedos[mid], is_srgb=True)
        nor01 = np.concatenate([normals[mid] * 0.5 + 0.5, np.ones((RES, RES, 1), dtype=np.float32)], axis=-1)
        save_png_topdown(CYCLES_MAP_DIR / f"mat_{mid:02d}_normal.png", nor01, is_srgb=False)
        save_png_topdown(CYCLES_MAP_DIR / f"mat_{mid:02d}_ormt.png", ormts[mid], is_srgb=False)

    cutout_mats = {29, 36}
    alb_chunks, nor_chunks, orm_chunks = [], [], []

    for mid in range(NUM_MATS):
        cur_alb = albedos[mid].copy()   # linear RGB + linear Alpha
        cur_nor = normals[mid].copy()   # unit XYZ in [-1..1]
        cur_orm = ormts[mid].copy()     # linear (AO, Rough, Metal, Trans/Coat)
        is_cutout = mid in cutout_mats

        for m in range(MIPS):
            if m == 0:
                mip_alb = cur_alb
                mip_nor = cur_nor
                mip_orm = cur_orm
            else:
                # 2x2 downsample in linear space
                if is_cutout:
                    # Coverage-preserving alpha mipmap + alpha-weighted RGB average
                    a4 = cur_alb[..., 3:4]
                    w_rgb = cur_alb[..., :3] * np.maximum(a4, 1e-3)
                    sum_w = w_rgb[0::2, 0::2] + w_rgb[0::2, 1::2] + w_rgb[1::2, 0::2] + w_rgb[1::2, 1::2]
                    sum_a = a4[0::2, 0::2] + a4[0::2, 1::2] + a4[1::2, 0::2] + a4[1::2, 1::2]
                    avg_rgb = sum_w / np.maximum(sum_a, 1e-3)
                    # Boost mip alpha slightly so thin fern/calathea pinnae preserve silhouette coverage at distance
                    avg_a = np.clip((sum_a * 0.25) * 1.06, 0.0, 1.0)
                    mip_alb = np.concatenate([avg_rgb, avg_a], axis=-1)
                else:
                    mip_alb = 0.25 * (cur_alb[0::2, 0::2] + cur_alb[0::2, 1::2] + cur_alb[1::2, 0::2] + cur_alb[1::2, 1::2])

                avg_n = 0.25 * (cur_nor[0::2, 0::2] + cur_nor[0::2, 1::2] + cur_nor[1::2, 0::2] + cur_nor[1::2, 1::2])
                n_len = np.linalg.norm(avg_n, axis=-1)
                mip_nor = avg_n / np.maximum(n_len[..., None], 1e-6)

                mip_orm = 0.25 * (cur_orm[0::2, 0::2] + cur_orm[0::2, 1::2] + cur_orm[1::2, 0::2] + cur_orm[1::2, 1::2])
                # Toksvig normal-variance -> roughness specular antialiasing!
                r_tok = np.clip(n_len, 0.2, 1.0)
                sigma2 = (1.0 - r_tok) / r_tok
                mip_orm[..., 1] = np.sqrt(np.minimum(1.0, mip_orm[..., 1] ** 2 + np.minimum(0.32, 1.6 * sigma2)))

                cur_alb, cur_nor, cur_orm = mip_alb, mip_nor, mip_orm

            # Encode Albedo (linear -> IEC sRGB uint8) + Alpha (linear uint8) -> BC7_UNORM_SRGB
            srgb_u8 = np.empty(mip_alb.shape, dtype=np.uint8)
            srgb_u8[..., :3] = np.clip(np.round(linear_to_srgb(mip_alb[..., :3]) * 255.0), 0, 255).astype(np.uint8)
            srgb_u8[..., 3] = np.clip(np.round(mip_alb[..., 3] * 255.0), 0, 255).astype(np.uint8)
            alb_chunks.append(encode_bc7_mode6(srgb_u8))

            # Encode Normal XY [-1..1] -> [0..255] -> BC5_UNORM
            nor_xy_u8 = np.clip(np.round((mip_nor[..., :2] * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
            nor_chunks.append(encode_bc5_unorm(nor_xy_u8))

            # Encode ORMT linear [0..1] -> [0..255] -> BC7_UNORM
            orm_u8 = np.clip(np.round(mip_orm * 255.0), 0, 255).astype(np.uint8)
            orm_chunks.append(encode_bc7_mode6(orm_u8))

        if (mid + 1) % 10 == 0:
            print(f"  Encoded {mid + 1}/{NUM_MATS} material mip chains...")

    write_btex(OUT_TEX_DIR / "pbr_albedo_alpha.btex", DXGI_FORMAT_BC7_UNORM_SRGB, RES, RES, NUM_MATS, MIPS, alb_chunks)
    write_btex(OUT_TEX_DIR / "pbr_normal.btex", DXGI_FORMAT_BC5_UNORM, RES, RES, NUM_MATS, MIPS, nor_chunks)
    write_btex(OUT_TEX_DIR / "pbr_ormt.btex", DXGI_FORMAT_BC7_UNORM, RES, RES, NUM_MATS, MIPS, orm_chunks)
    build_ibl_env_btex(OUT_TEX_DIR / "pbr_ibl_env.btex")

    t2 = time.perf_counter()
    print(f"[DONE] All PBR BTEX001 textures generated in {t2 - t0:.2f}s")


if __name__ == "__main__":
    import sys
    import traceback
    try:
        main()
    except Exception:
        traceback.print_exc()
        sys.exit(1)
