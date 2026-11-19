"""Taipei basin relief in local metres, east/north of the Taipei 101 spire.

Rebuild the compact offline grid with python tools/taipei_relief.py RAW_TILE_DIR.
The two required N24E121 and N25E121 .hgt.gz tiles and their hashes are listed below.
"""
import bisect
import functools
import gzip
import hashlib
import json
import math
from pathlib import Path

DATA = Path(__file__).with_name('data') / 'taipei_relief.json'
ORIGIN = (25.033668, 121.564810)
TILES = {
    'N24E121': '0222394fc4f47e154d9271d82c99ca73a44e6e25a2b6e0a48c81d7bd5a169267',
    'N25E121': '84e3c158800216366fbd81ba5664a4725ea2661be3cf2d3e5d0595723438aad8',
}


def rebuild(cache):
    import numpy as np
    tiles = {}
    for name, digest in TILES.items():
        raw = (cache / (name + '.hgt.gz')).read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError(f'Unexpected source tile {name}')
        tiles[int(name[1:3])] = np.frombuffer(gzip.decompress(raw), dtype='>i2').reshape(3601, 3601)

    phi = math.radians(ORIGIN[0])
    # Local WGS84 ellipsoid tangential scale in metres per degree.
    den = 1 - 0.00669437999 * math.sin(phi)**2
    east_scale = math.radians(1) * 6378137 * math.cos(phi) / math.sqrt(den)
    north_scale = math.radians(1) * 6378137 * (1 - 0.00669437999) / den**1.5
    axis = list(range(0, 2001, 100)) + list(range(2200, 6001, 200))
    axis += list(range(6400, 12001, 400)) + list(range(12800, 25601, 800))
    axis = [-n for n in reversed(axis[1:])] + axis
    heights = []
    for north in axis:
        lat = ORIGIN[0] + north / north_scale
        tile_lat = math.floor(lat)
        for east in axis:
            lon = ORIGIN[1] + east / east_scale
            i = round((tile_lat + 1 - lat) * 3600)
            j = round((lon - 121) * 3600)
            h = tiles[tile_lat][max(0, i-1):min(3601, i+2), j-1:j+2]
            # A three-cell median suppresses isolated roof and canopy returns.
            h = h[h != -32768]
            if not h.size:
                raise ValueError(f'Empty relief at {lat}, {lon}')
            heights.append(round(float(np.median(h))))
    doc = {'origin_lat_lon': ORIGIN, 'datum': 'WGS84 / SRTM orthometric metres',
           'source': 'Mapzen terrain tiles, global SRTM, https://registry.opendata.aws/terrain-tiles/',
           'source_tiles': TILES, 'axis_m': axis, 'rows_south_to_north_m': [
               heights[i:i+len(axis)] for i in range(0, len(heights), len(axis))]}
    DATA.parent.mkdir(parents=True, exist_ok=True)
    DATA.write_text(json.dumps(doc, separators=(',', ':')), encoding='utf8')


@functools.lru_cache(maxsize=1)
def _terrain():
    doc = json.loads(DATA.read_text(encoding='utf8'))
    if tuple(doc['origin_lat_lon']) != ORIGIN:
        raise ValueError('Taipei terrain origin does not match the tower')
    axis = doc['axis_m']
    raw = doc['rows_south_to_north_m']
    reference = raw[axis.index(0)][axis.index(0)]
    def basin_level(value):
        # Low-rise rooftops also appear in this surface radar survey. Render the
        # surveyed hills and leave the low urban plain to the building map.
        height = max(0.0, float(value - reference))
        t = min(1.0, max(0.0, (height - 45.0) / 50.0))
        return height * t*t*(3 - 2*t)
    return axis, [[basin_level(h) for h in row] for row in raw]


def taipei_relief_height(east, north):
    axis, rows = _terrain()
    def locate(value):
        i = min(len(axis)-2, max(0, bisect.bisect_right(axis, value)-1))
        return i, min(1.0, max(0.0, (value-axis[i])/(axis[i+1]-axis[i])))
    i, u = locate(east)
    j, v = locate(north)
    return ((1-u)*rows[j][i] + u*rows[j][i+1])*(1-v) + ((1-u)*rows[j+1][i] + u*rows[j+1][i+1])*v


def taipei_relief_mesh(mesh):
    axis, rows = _terrain()
    n = len(axis)
    vertices = [(x,y,rows[j][i]-.35) for j,y in enumerate(axis) for i,x in enumerate(axis)]
    faces = [(v,v+1,v+1+n,v+n) for y in range(n-1) for x in range(n-1) for v in [y*n+x]]
    rim = list(range(n)) + [y*n+n-1 for y in range(1,n)]
    rim += list(range(n*n-2,(n-1)*n-1,-1)) + [y*n for y in reversed(range(1,n-1))]
    # A closed base shares its perimeter with the relief and needs only one
    # fan beneath the landscape rather than another complete sampling grid.
    base = len(vertices)
    vertices.extend((vertices[i][0], vertices[i][1], -24) for i in rim)
    center = len(vertices)
    vertices.append((0,0,-24))
    for i,a in enumerate(rim):
        j = (i+1) % len(rim)
        faces.append((rim[j],a,base+i,base+j))
        faces.append((base+j,base+i,center))
    mesh('taipei_surveyed_basin_relief', vertices, faces, 19, part=6, smooth=True, rough=.95)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cache', type=Path, help='Directory containing the two Mapzen Skadi .hgt.gz tiles')
    rebuild(parser.parse_args().cache)
