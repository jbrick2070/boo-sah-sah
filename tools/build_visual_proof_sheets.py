"""Assemble high-resolution Before / Cycles Reference / D3D12 Application comparison
sheets and 1:1 unscaled pixel-crop inspection sheets for BooSahSah visual proof.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
BEFORE_DIR = ROOT / 'build' / 'before_visual_rebuild' / 'film_captures'
AFTER_DIR = ROOT / 'build' / 'film_captures'
PROOF_DIR = ROOT / 'build' / 'visual_proof'


def load_font(size: int):
    for cand in (
        r'C:\Windows\Fonts\segoeui.ttf',
        r'C:\Windows\Fonts\segoeuib.ttf',
        r'C:\Windows\Fonts\arial.ttf',
    ):
        if Path(cand).exists():
            return ImageFont.truetype(cand, size)
    return ImageFont.load_default()


def draw_badge(img: Image.Image, text: str, xy=(18, 16), font_size=26):
    draw = ImageDraw.Draw(img, 'RGBA')
    fnt = load_font(font_size)
    bbox = draw.textbbox((0, 0), text, font=fnt)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x, y = xy
    draw.rounded_rectangle((x, y, x + tw + 24, y + th + 16), radius=8, fill=(14, 18, 24, 212), outline=(210, 185, 145, 160), width=1)
    draw.text((x + 12, y + 6), text, fill=(246, 242, 234, 255), font=fnt)


def build_dawn_3way_comparison():
    tile_w, tile_h = 1120, 630
    pad = 18
    hdr_h = 64
    W = pad * 4 + tile_w * 3
    H = hdr_h + pad * 3 + tile_h * 2
    canvas = Image.new('RGB', (W, H), (15, 17, 21))
    draw = ImageDraw.Draw(canvas)
    title_fnt = load_font(30)
    draw.text((pad, 16), 'BooSahSah Dawn Bathroom Hero — Starting 3478b3c vs. Blender 5.2 Cycles Reference vs. Real-Time D3D12/DXR (1440p)', fill=(242, 238, 230), font=title_fnt)

    rows = [
        ('342.0s (Intimate Vanity Reveal)', BEFORE_DIR / 'film_family_342s_preferred.png', PROOF_DIR / 'dawn_cycles_342s.png', AFTER_DIR / 'film_family_342s_preferred.png'),
        ('355.0s (10s Pullback Settle)',    BEFORE_DIR / 'film_family_355s_preferred.png', PROOF_DIR / 'dawn_cycles_355s.png', AFTER_DIR / 'film_family_355s_preferred.png'),
    ]
    col_labels = ['Before (Commit 3478b3c)', 'Blender 5.2 Cycles Reference', 'After — Real-Time D3D12 / DXR']

    for r_idx, (row_tag, p_bef, p_cyc, p_aft) in enumerate(rows):
        for c_idx, src_p in enumerate((p_bef, p_cyc, p_aft)):
            im = Image.open(src_p).convert('RGB').resize((tile_w, tile_h), Image.Resampling.LANCZOS)
            draw_badge(im, f'{col_labels[c_idx]}  |  {row_tag}', xy=(14, 14), font_size=21)
            ox = pad + c_idx * (tile_w + pad)
            oy = hdr_h + pad + r_idx * (tile_h + pad)
            canvas.paste(im, (ox, oy))

    out = PROOF_DIR / 'dawn_before_cycles_after_comparison.png'
    canvas.save(out)
    print(f'[OK] Wrote {out} ({canvas.size[0]}x{canvas.size[1]})')


def build_dawn_1to1_crops():
    """Extract true 1:1 native pixel crops (zero upscaling/downscaling!) from 2560x1440 D3D12 & Cycles captures."""
    d342 = Image.open(AFTER_DIR / 'film_family_342s_preferred.png').convert('RGB')
    c342 = Image.open(PROOF_DIR / 'dawn_cycles_342s.png').convert('RGB')
    d355 = Image.open(AFTER_DIR / 'film_family_355s_preferred.png').convert('RGB')
    c355 = Image.open(PROOF_DIR / 'dawn_cycles_355s.png').convert('RGB')

    cw, ch = 640, 440
    # Crop boxes (x0, y0, x0+640, y0+440) inside 2560x1440
    specs = [
        ('1:1 Crop A (342s): Basin Undercut, CNC Faucet & 3D Water Droplets', d342, c342, (980, 620, 980 + cw, 620 + ch)),
        ('1:1 Crop B (355s): Flat Mirror — Oak Slats, Porcelain Leaf Relief & Shower Glass', d355, c355, (860, 250, 860 + cw, 250 + ch)),
        ('1:1 Crop C (342s): Scanned Calathea Veins, Translucency & Ribbed Celadon Planter', d342, c342, (210, 420, 210 + cw, 420 + ch)),
        ('1:1 Crop D (342s): Waffle Towel, Amber Soap, Bristle Brush & Keepsake Orchid', d342, c342, (1520, 560, 1520 + cw, 560 + ch)),
        ('1:1 Crop E (355s): AR Float Window, Travertine Terrace & Grounded Taipei 101', d355, c355, (1760, 210, 1760 + cw, 210 + ch)),
    ]

    pad = 18
    hdr_h = 62
    W = pad * 3 + cw * 2
    H = hdr_h + pad + len(specs) * (ch + pad)
    canvas = Image.new('RGB', (W, H), (15, 17, 21))
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, 16), 'Dawn Hero 1:1 Unscaled Pixel Crops (2560x1440 Native) — Real-Time D3D12/DXR (Left) vs. Cycles Path-Traced (Right)', fill=(242, 238, 230), font=load_font(24))

    for idx, (label, d_im, c_im, box) in enumerate(specs):
        oy = hdr_h + pad + idx * (ch + pad)
        crop_d = d_im.crop(box)
        crop_c = c_im.crop(box)
        draw_badge(crop_d, f'D3D12/DXR — {label}', xy=(12, 12), font_size=17)
        draw_badge(crop_c, f'Cycles Ref — {label}', xy=(12, 12), font_size=17)
        canvas.paste(crop_d, (pad, oy))
        canvas.paste(crop_c, (pad * 2 + cw, oy))

    out = PROOF_DIR / 'dawn_1to1_crops_sheet.png'
    canvas.save(out)
    print(f'[OK] Wrote {out} ({canvas.size[0]}x{canvas.size[1]})')


def build_full_film_before_after():
    shots = [
        ('36s — Act 1 Glass Orchid Reveal',     'film_family_36s_preferred.png'),
        ('95s — Act 2 Enamel Cathedral',        'film_family_95s_preferred.png'),
        ('175s — Act 3 Botanical Sanctuary',    'film_family_175s_preferred.png'),
        ('260s — Act 5 Shenzhen Bay eVTOL',     'film_family_260s_preferred.png'),
        ('342s — Act 7 Dawn Vanity Close',      'film_family_342s_preferred.png'),
        ('355s — Act 7 Dawn Penthouse Finale',  'film_family_355s_preferred.png'),
    ]
    tw, th = 1024, 576
    pad = 16
    hdr_h = 60
    W = pad * 3 + tw * 2
    H = hdr_h + pad + len(shots) * (th + pad)
    canvas = Image.new('RGB', (W, H), (15, 17, 21))
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, 15), 'BooSahSah Full-Film Visual Rebuild — Before (3478b3c, Left) vs. After (Real-Time D3D12/DXR, Right)', fill=(242, 238, 230), font=load_font(27))

    for idx, (label, fname) in enumerate(shots):
        oy = hdr_h + pad + idx * (th + pad)
        im_b = Image.open(BEFORE_DIR / fname).convert('RGB').resize((tw, th), Image.Resampling.LANCZOS)
        im_a = Image.open(AFTER_DIR / fname).convert('RGB').resize((tw, th), Image.Resampling.LANCZOS)
        draw_badge(im_b, f'BEFORE (3478b3c)  |  {label}', xy=(12, 12), font_size=19)
        draw_badge(im_a, f'AFTER (D3D12/DXR)  |  {label}', xy=(12, 12), font_size=19)
        canvas.paste(im_b, (pad, oy))
        canvas.paste(im_a, (pad * 2 + tw, oy))

    out = PROOF_DIR / 'full_film_before_after_sheet.png'
    canvas.save(out)
    print(f'[OK] Wrote {out} ({canvas.size[0]}x{canvas.size[1]})')


if __name__ == '__main__':
    PROOF_DIR.mkdir(parents=True, exist_ok=True)
    build_dawn_3way_comparison()
    build_dawn_1to1_crops()
    build_full_film_before_after()
