"""Bake six original room interiors for the city window shader, in linear RGBA8."""
from pathlib import Path
import struct
import zlib

SIZE = 64
# Back wall, floor, and lamp colors. One slice per interior face keeps all mips independent.
PALETTES = [
    ((103, 64, 45), (60, 39, 28), (250, 183, 82)),   # apartment
    ((66, 89, 102), (36, 49, 54), (191, 230, 244)), # office
    ((73, 84, 70), (43, 44, 33), (215, 210, 136)),  # bookshop
    ((93, 61, 77), (55, 38, 52), (242, 147, 107)),  # hotel
    ((49, 72, 74), (27, 40, 41), (88, 190, 166)),   # monitor room
    ((107, 83, 55), (61, 46, 32), (242, 204, 122)), # cafe
]


def pixel(room, face, u, v):
    wall, floor, lamp = PALETTES[room]
    color = wall
    light = 0.62 + 0.34 * (1 - v)
    if face == 3:  # Floorboards, receding toward the back.
        color = floor
        light = 0.76 if int(u * 8) % 2 else 0.9
        if (u * 8) % 1 < 0.035:
            light *= 0.5
    elif face == 4:  # Ceiling and practical fixtures.
        light = 0.9
        if 0.44 < v < 0.52 and 0.19 < u < 0.81:
            color, light = lamp, 1
    elif face == 0:  # Back wall.
        if room in (0, 3):
            if 0.12 < u < 0.49 and 0.22 < v < 0.50:
                color = (37, 51, 52) if min(u-0.12, 0.49-u, v-0.22, 0.50-v) > 0.035 else floor
            if v > 0.64 and 0.18 < u < 0.88:
                color = (53, 69, 72) if room == 0 else (143, 109, 80)
        elif room in (1, 4):
            if 0.22 < u < 0.72 and 0.40 < v < 0.64:
                color = lamp if min(u-0.22, 0.72-u, v-0.40, 0.64-v) > 0.026 else floor
            if 0.64 < v < 0.68 or (v >= 0.68 and min(abs(u-0.15), abs(u-0.84)) < 0.026):
                color = floor
        else:
            if 0.14 < u < 0.87 and 0.20 < v < 0.66:
                shelf = (v * 7.0) % 1
                color = floor if shelf < 0.24 else tuple(c * (0.46 + ((int(u * 19) * 7) % 11) * 0.05) for c in lamp)
            if v > 0.76:
                color = floor
    else:  # Side walls, half-open blinds or a corridor door.
        light *= 0.65 + 0.28 * u
        if face == 1 and 0.20 < u < 0.72 and 0.28 < v < 0.67:
            color = lamp if (v * 17) % 1 < 0.32 else floor
        if face == 2 and 0.51 < u < 0.88 and 0.23 < v < 0.96:
            color = floor
    return tuple(round(min(255, max(0, c * light))) for c in color) + (255,)


def bake():
    payload = bytearray()
    for room in range(len(PALETTES)):
        for face in range(5):
            side = SIZE
            pixels = [pixel(room, face, (x + 0.5) / side, (y + 0.5) / side)
                      for y in range(side) for x in range(side)]
            while True:
                payload.extend(bytes(channel for rgba in pixels for channel in rgba))
                if side == 1:
                    break
                half = side // 2
                pixels = [tuple((sum(pixels[(2*y+dy)*side + 2*x+dx][c] for dy in (0,1) for dx in (0,1))+2)//4
                                for c in range(4)) for y in range(half) for x in range(half)]
                side = half
    header = struct.pack('<8s8IQ4I', b'BTEX001\0', 1, 64, 28, SIZE, SIZE, len(PALETTES)*5,
                         SIZE.bit_length(), zlib.crc32(payload), len(payload), 0, 0, 0, 0)
    return header + payload


if __name__ == '__main__':
    (Path(__file__).resolve().parents[1] / 'assets/textures/city_rooms.btex').write_bytes(bake())
