"""CPU-only integrity/geometry gate; run with Python + numpy after Blender export."""
from pathlib import Path
import hashlib
import json
import struct
import zlib
import numpy as np

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / 'assets/manifest.json').read_text(encoding='utf-8'))
report = json.loads((root / 'assets/meshes/generation_report.json').read_text(encoding='utf-8'))
for asset in report['assets']:
    name = asset['name']
    data = (root / 'assets/meshes' / (name + '.bmesh')).read_bytes()
    magic, version, header, stride, nv, ni, ns, *tail = struct.unpack_from('<8s6I6f2I', data)
    assert (magic, version, header, stride) == (b'BMESH001', 1, 64, 40)
    bounds = np.array(tail[:6]).reshape(2, 3)
    assert len(data) == 64 + tail[7] == 64 + ns * 24 + nv * 40 + ni * 4
    assert zlib.crc32(data[64:]) == tail[6]
    dtype = np.dtype([('p', '<f4', 3), ('n', '<i2', 4), ('t', '<i2', 4), ('uvtr', '<f2', 4), ('ids', '<u2', 2)])
    vertices = np.frombuffer(data, dtype=dtype, count=nv, offset=64 + ns * 24)
    indices = np.frombuffer(data, dtype='<u4', count=ni, offset=64 + ns * 24 + nv * 40)
    assert np.isfinite(vertices['p']).all() and np.isfinite(vertices['uvtr']).all()
    assert np.array_equal(vertices['p'].min(axis=0), bounds[0])
    assert np.array_equal(vertices['p'].max(axis=0), bounds[1])
    normal = vertices['n'][:, :3].astype(np.float64) / 32767
    tangent = vertices['t'][:, :3].astype(np.float64) / 32767
    assert np.max(np.abs(np.linalg.norm(normal, axis=1) - 1)) < .001
    assert np.max(np.abs(np.linalg.norm(tangent, axis=1) - 1)) < .001
    assert np.max(np.abs((normal * tangent).sum(axis=1))) < .001
    assert indices.max() < nv and ni % 3 == 0
    triangles = vertices['p'][indices.reshape(-1, 3)].astype(np.float64)
    area = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0]), axis=1)
    assert (area > 1e-12).all(), (name, 'degenerate triangle')
    sha = hashlib.sha256(data).hexdigest()
    entry = next(a for a in manifest['assets'] if a['id'] == 'mesh_' + name)
    assert sha == entry['sha256'] == asset['sha256']
    assert asset['nonmanifold_edges'] == 0
    print(f'[PASS] {name}: {ni // 3} triangles; CRC32, SHA256, tight bounds, normals, tangents, nondegenerate faces')
