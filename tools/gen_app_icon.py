"""Original procedural glass-orchid ICO. Python standard library only; deterministic CC0 asset."""
import math
import struct
from pathlib import Path


def pixel(x, y):
    r = math.hypot(x, y)
    # Rounded midnight medallion, with five faceted jade/magenta orchid petals.
    if max(abs(x), abs(y)) > .92 or (abs(x) > .72 and abs(y) > .72 and math.hypot(abs(x)-.72, abs(y)-.72) > .20):
        return (0, 0, 0, 0)
    rgb = [10 + 8*(1-y), 18 + 10*(1-y), 30 + 12*(1-y)]
    for i in range(5):
        angle = -math.pi/2 + i*2*math.pi/5
        u = x*math.cos(angle) + y*math.sin(angle)
        v = -x*math.sin(angle) + y*math.cos(angle)
        width = .26*max(0, math.sin(math.pi*max(0, min(1, (u+.08)/.89))))
        if -.08 < u < .81 and abs(v) < width:
            edge = abs(v)/max(width, .001)
            facet = .58 + .3*(v < 0) + .2*edge
            tint = (70, 220, 210) if i in (0, 1, 4) else (206, 111, 221)
            rgb = [c*facet*(1-.28*u) for c in tint]
    if r < .13:
        rgb = [250, 206, 109]
    return (*[max(0, min(255, round(c))) for c in rgb], 255)


def image(size):
    # 4x analytic supersampling keeps 16/24/32 px shell icons legible.
    data = bytearray()
    for y in range(size-1, -1, -1):
        for x in range(size):
            samples = [pixel(2*(x+(sx+.5)/4)/size-1, 2*(y+(sy+.5)/4)/size-1) for sy in range(4) for sx in range(4)]
            alpha = sum(p[3] for p in samples)
            rgb = [round(sum(p[c]*p[3] for p in samples)/alpha) if alpha else 0 for c in range(3)]
            data += bytes((rgb[2], rgb[1], rgb[0], round(alpha/16)))
    mask = bytes(((size+31)//32)*4*size)
    header = struct.pack('<IiiHHIIiiII', 40, size, size*2, 1, 32, 0, len(data), 0, 0, 0, 0)
    return header + data + mask


def main():
    sizes = (16, 24, 32, 48, 64, 128, 256)
    images = [image(s) for s in sizes]
    offset = 6 + 16*len(sizes)
    directory = bytearray(struct.pack('<HHH', 0, 1, len(sizes)))
    for size, payload in zip(sizes, images):
        directory += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(payload), offset)
        offset += len(payload)
    target = Path(__file__).resolve().parents[1] / 'assets/icons/boo_sah_sah.ico'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(directory + b''.join(images))
    print(target)


if __name__ == '__main__':
    main()
