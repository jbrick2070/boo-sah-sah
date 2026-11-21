"""Deterministic Blender 5.2 -> BMESH001 v1 Asset Pipeline for BooSahSah.
Combines precision procedural construction, architectural bevels/FWN normals,
metric PBR UV projection, MikkTSpace-equivalent float64 UV-derivative tangents,
and watertight solidified CC0 scanned botanical models.
"""
import bpy, bmesh, math, random, struct, zlib, hashlib, json, os, sys
import numpy as np
from pathlib import Path
from mathutils import Vector, Matrix, Euler
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from taipei_relief import taipei_relief_height, taipei_relief_mesh
from taipei_city_mesh import city_mesh as taipei_city_mesh
OUT = ROOT / 'assets' / 'meshes'
CC0_MODELS = ROOT / 'third_party' / 'polyhaven_cc0' / 'models'
OUT.mkdir(parents=True, exist_ok=True)
TAU = math.tau
objects = []
serial = 0

# Real-world metric PBR texture tile repeat sizes (meters per [0..1] UV cycle)
MAT_TILE_METERS = {
    0: 1.40, 1: 2.80, 2: 2.20, 3: 0.65, 4: 0.55, 5: 2.00, 6: 0.80, 7: 3.20,
    8: 0.45, 9: 0.45, 10: 0.50, 11: 0.55, 12: 1.50, 13: 2.00, 14: 2.00, 15: 0.80,
    16: 0.50, 17: 1.00, 18: 0.80, 19: 18.0, 20: 1.15, 21: 1.20, 22: 0.65, 23: 0.28,
    24: 1.85, 25: 0.24, 26: 0.68, 27: 0.48, 28: 0.48, 29: 1.00, 30: 1.00, 31: 1.15,
    32: 1.20, 33: 0.26, 34: 1.20, 35: 0.22, 36: 1.00, 37: 0.24, 38: 0.40, 39: 1.05,
    40: 1.50, 41: 1.35,
}

def atomic(path, data):
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)

def keep(obj, name, mat, part=0, rough=.3, thick=.03, preserve_uv=False, smooth_angle=52.0, uv_grain='auto', uv_scale=1.0):
    global serial
    serial += 1
    obj.name = '%06d_%s' % (serial, name)
    obj['mat'], obj['part'], obj['rough'], obj['thick'] = int(mat), int(part), float(rough), float(thick)
    obj['preserve_uv'] = bool(preserve_uv)
    obj['smooth_angle'] = float(smooth_angle)
    obj['uv_grain'] = str(uv_grain)
    obj['uv_scale'] = float(uv_scale)
    objects.append(obj)
    return obj

def box(name, pos, size, mat=0, part=0, bevel=0, rough=.3, rot_z=0.0, rot_euler=None, segments=2, uv_grain='auto', uv_scale=1.0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=pos)
    obj = bpy.context.object
    obj.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if rot_euler is not None:
        obj.rotation_euler = rot_euler
    elif rot_z:
        obj.rotation_euler.z = rot_z
    if bevel:
        mod = obj.modifiers.new('machined_edges', 'BEVEL')
        mod.width = bevel
        mod.segments = segments
        for f in obj.data.polygons:
            f.use_smooth = True
    return keep(obj, name, mat, part, rough=rough, smooth_angle=44.0 if bevel else 15.0, uv_grain=uv_grain, uv_scale=uv_scale)

def sphere(name, pos, scale, mat, part=0, seg=24, rings=12, rough=.25, thick=.03):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=seg, ring_count=rings, radius=1, location=pos)
    obj = bpy.context.object
    obj.scale = scale
    for f in obj.data.polygons:
        f.use_smooth = True
    return keep(obj, name, mat, part, rough=rough, thick=thick, smooth_angle=180.0)

def rod(name, a, b, radius, mat=0, part=0, verts=16, rough=.3, bevel=0.0, preserve_uv=None):
    a, b = Vector(a), Vector(b)
    d = b - a
    if bevel > 0.0 and d.length > 2.5 * bevel:
        u = d.normalized()
        bw = min(bevel, 0.35 * radius, 0.25 * d.length)
        r_in = max(radius - bw, radius * 0.5)
        pts = [tuple(a + u * (0.15 * bw)), tuple(a + u * bw), tuple(b - u * bw), tuple(b - u * (0.15 * bw))]
        return closed_tube(name, pts, [r_in, radius, radius, r_in], mat, part, n_ring=verts, rough=rough, thick=min(0.04, radius))
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=radius, depth=max(d.length, 1e-4), end_fill_type='TRIFAN', location=(a + b) * .5)
    obj = bpy.context.object
    obj.rotation_mode = 'QUATERNION'
    obj.rotation_quaternion = d.to_track_quat('Z', 'Y')
    if verts >= 12:
        for f in obj.data.polygons:
            f.use_smooth = True
    keep_cyl_uv = (int(mat) in (8, 23)) if preserve_uv is None else bool(preserve_uv)
    return keep(obj, name, mat, part, rough=rough, preserve_uv=keep_cyl_uv, smooth_angle=55.0)

def torus(name, pos, radius, tube, mat=0, part=0, seg=64, minor_seg=12, rough=.25):
    bpy.ops.mesh.primitive_torus_add(major_segments=seg, minor_segments=minor_seg, location=pos, major_radius=radius, minor_radius=tube)
    obj = bpy.context.object
    for f in obj.data.polygons:
        f.use_smooth = True
    return keep(obj, name, mat, part, rough=rough, smooth_angle=120.0)

def mesh(name, verts, faces, mat, part=0, smooth=False, rough=.3, thick=.03, preserve_uv=False, smooth_angle=65.0):
    m = bpy.data.meshes.new(name)
    m.from_pydata(verts, [], faces)
    m.update()
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    for f in m.polygons:
        f.use_smooth = smooth
    return keep(o, name, mat, part, rough=rough, thick=thick, preserve_uv=preserve_uv, smooth_angle=smooth_angle if smooth else 15.0)

def notched(name, z0, z1, w0, w1, mat=5, part=0, ox=0.0, oy=0.0, rough=.22):
    outline = [(-1,-.70),(-.86,-.70),(-.86,-.86),(-.70,-.86),(-.70,-1),(.70,-1),(.70,-.86),(.86,-.86),(.86,-.70),(1,-.70),(1,.70),(.86,.70),(.86,.86),(.70,.86),(.70,1),(-.70,1),(-.70,.86),(-.86,.86),(-.86,.70),(-1,.70)]
    n = len(outline)
    v = [(ox + x * w, oy + y * w, z) for z, w in [(z0, w0), (z1, w1)] for x, y in outline]
    f = [tuple(reversed(range(n))), tuple(range(n, 2 * n))] + [(i, (i + 1) % n, (i + 1) % n + n, i + n) for i in range(n)]
    return mesh(name, v, f, mat, part=part, rough=rough)

def closed_blade(name, nu, nv, eval_surf, mat, part, rough=.07):
    """Watertight 2-manifold botanical shell with automatic outward normals, [0..1]^2 UVs, and thickness."""
    verts = []
    thicks = []
    uvs_vert = []
    du = 1e-3
    dv = 2e-3
    grid = []
    for i in range(nu + 1):
        u = i / nu
        row = []
        for j in range(nv + 1):
            v = (j / nv) * 2.0 - 1.0
            pos, th = eval_surf(u, v)
            pu_hi, _ = eval_surf(min(1.0, u + du), v)
            pu_lo, _ = eval_surf(max(0.0, u - du), v)
            pv_hi, _ = eval_surf(u, min(1.0, v + dv))
            pv_lo, _ = eval_surf(u, max(-1.0, v - dv))
            d_u = Vector(pu_hi) - Vector(pu_lo)
            d_v = Vector(pv_hi) - Vector(pv_lo)
            nrm = d_v.cross(d_u)
            if nrm.length < 1e-9:
                nrm = Vector((0.0, -1.0, 0.0))
            else:
                nrm.normalize()
            row.append((Vector(pos), nrm, th, u, 0.5 + 0.5 * v))
        grid.append(row)
    for side in (-1.0, 1.0):
        for i in range(nu + 1):
            for j in range(nv + 1):
                pos, nrm, th, u, v01 = grid[i][j]
                p = pos + nrm * (side * 0.5 * th)
                verts.append((p.x, p.y, p.z))
                thicks.append(th)
                uvs_vert.append((u, v01))
    layer = (nu + 1) * (nv + 1)
    faces = []
    for side in range(2):
        for i in range(nu):
            for j in range(nv):
                a = side * layer + i * (nv + 1) + j
                q = (a, a + 1, a + nv + 2, a + nv + 1)
                faces.append(q if side else tuple(reversed(q)))
    boundary = (list(range(nv + 1))
                + [i * (nv + 1) + nv for i in range(1, nu + 1)]
                + [nu * (nv + 1) + j for j in range(nv - 1, -1, -1)]
                + [i * (nv + 1) for i in range(nu - 1, 0, -1)])
    for k, a in enumerate(boundary):
        b = boundary[(k + 1) % len(boundary)]
        faces.append((a, b, b + layer, a + layer))
    m = bpy.data.meshes.new(name)
    m.from_pydata(verts, [], faces)
    m.update()
    for f in m.polygons:
        f.use_smooth = True
    uv_layer = m.uv_layers.new(name='UVMap')
    for poly in m.polygons:
        for li in poly.loop_indices:
            vi = m.loops[li].vertex_index
            uv_layer.data[li].uv = uvs_vert[vi]
    th_attr = m.attributes.new(name='thick', type='FLOAT', domain='POINT')
    for vi, tv in enumerate(thicks):
        th_attr.data[vi].value = tv
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    return keep(o, name, mat, part=part, rough=rough, thick=.03, preserve_uv=True, smooth_angle=180.0)

def closed_tube(name, centers, radii, mat, part, n_ring=16, rough=.18, thick=.04, flute_amp=0.0, flute_freq=3):
    """Watertight 2-manifold swept botanical/architectural tube with end-cap poles and [0..1]^2 UVs."""
    ns = len(centers)
    verts = []
    uvs_vert = []
    thicks = []
    frames = []
    # Parallel-transport / continuous Bishop frame avoids 90-deg twist glitches along curved goosenecks!
    p0 = Vector(centers[0])
    p1 = Vector(centers[min(ns - 1, 1)])
    t_init = (p1 - p0)
    t_init = Vector((0.0, 0.0, 1.0)) if t_init.length < 1e-6 else t_init.normalized()
    up_ref = Vector((1.0, 0.0, 0.0)) if abs(t_init.x) < 0.85 else Vector((0.0, 1.0, 0.0))
    cur_n1 = t_init.cross(up_ref).normalized()
    for i in range(ns):
        p_prev = Vector(centers[max(0, i - 1)])
        p_next = Vector(centers[min(ns - 1, i + 1)])
        tang = (p_next - p_prev)
        tang = t_init if tang.length < 1e-6 else tang.normalized()
        proj = cur_n1 - tang * cur_n1.dot(tang)
        if proj.length > 1e-5:
            cur_n1 = proj.normalized()
        cur_n2 = tang.cross(cur_n1).normalized()
        frames.append((Vector(centers[i]), tang, cur_n1, cur_n2))
    c0, t0, _, _ = frames[0]
    verts.append(tuple(c0 - t0 * max(0.0015, radii[0] * 0.25)))
    uvs_vert.append((0.0, 0.5))
    thicks.append(thick)
    for i in range(ns):
        u = i / max(1, ns - 1)
        ci, _, n1, n2 = frames[i]
        r_base = max(0.0006, radii[i])
        for j in range(n_ring):
            ang = j * TAU / n_ring
            flute = 1.0 + flute_amp * math.cos(flute_freq * ang) * math.sin(math.pi * u)
            r = r_base * flute
            pt = ci + (n1 * math.cos(ang) + n2 * math.sin(ang)) * r
            verts.append(tuple(pt))
            uvs_vert.append((u, j / n_ring))
            thicks.append(thick * (0.7 + 0.3 * flute))
    cn, tn, _, _ = frames[-1]
    end_idx = 1 + ns * n_ring
    verts.append(tuple(cn + tn * max(0.0015, radii[-1] * 0.25)))
    uvs_vert.append((1.0, 0.5))
    thicks.append(thick * 0.65)
    faces = []
    for j in range(n_ring):
        jn = (j + 1) % n_ring
        faces.append((0, 1 + jn, 1 + j))
    for i in range(ns - 1):
        b0 = 1 + i * n_ring
        b1 = 1 + (i + 1) * n_ring
        for j in range(n_ring):
            jn = (j + 1) % n_ring
            faces.append((b0 + j, b0 + jn, b1 + jn, b1 + j))
    last_ring = 1 + (ns - 1) * n_ring
    for j in range(n_ring):
        jn = (j + 1) % n_ring
        faces.append((last_ring + j, last_ring + jn, end_idx))
    m = bpy.data.meshes.new(name)
    m.from_pydata(verts, [], faces)
    m.update()
    for f in m.polygons:
        f.use_smooth = True
    uv_layer = m.uv_layers.new(name='UVMap')
    for poly in m.polygons:
        for li in poly.loop_indices:
            vi = m.loops[li].vertex_index
            uv_layer.data[li].uv = uvs_vert[vi]
    th_attr = m.attributes.new(name='thick', type='FLOAT', domain='POINT')
    for vi, tv in enumerate(thicks):
        th_attr.data[vi].value = tv
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    return keep(o, name, mat, part=part, rough=rough, thick=thick, preserve_uv=True, smooth_angle=160.0)

def superellipse_vessel(name, center, profile, rx, ry, power, mat, part, seg=128, rough=.05, thick=.014):
    """Smooth watertight superellipse architectural basin/vessel (|x/rx|^p + |y/ry|^p = r^p) with seamless UVs."""
    cx, cy, cz = center
    inv_p = 2.0 / max(power, 0.5)
    verts = []
    uvs_loop = []
    arc_s = [0.0]
    for k in range(1, len(profile) + 1):
        r0, h0 = profile[k - 1]
        r1, h1 = profile[k % len(profile)]
        arc_s.append(arc_s[-1] + math.hypot((r1 - r0) * 0.5 * (rx + ry), h1 - h0))
    tot_s = max(arc_s[-1], 1e-4)
    for r, h in profile:
        for j in range(seg):
            ang = j * TAU / seg
            ca, sa = math.cos(ang), math.sin(ang)
            sx = (abs(ca) ** inv_p) * (1.0 if ca >= 0 else -1.0)
            sy = (abs(sa) ** inv_p) * (1.0 if sa >= 0 else -1.0)
            verts.append((cx + r * rx * sx, cy + r * ry * sy, cz + h))
    faces = []
    for k in range(len(profile)):
        nk = (k + 1) % len(profile)
        v0 = arc_s[k] / tot_s * 1.6
        v1 = arc_s[k + 1] / tot_s * 1.6
        for j in range(seg):
            nj = (j + 1) % seg
            u0 = j / seg * 2.0
            u1 = (j + 1) / seg * 2.0
            faces.append((k * seg + j, k * seg + nj, nk * seg + nj, nk * seg + j))
            uvs_loop.extend([(u0, v0), (u1, v0), (u1, v1), (u0, v1)])
    m = bpy.data.meshes.new(name)
    m.from_pydata(verts, [], faces)
    m.update()
    for f in m.polygons:
        f.use_smooth = True
    uv_layer = m.uv_layers.new(name='UVMap')
    for idx, uv in enumerate(uvs_loop):
        uv_layer.data[idx].uv = uv
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    return keep(o, name, mat, part=part, rough=rough, thick=thick, preserve_uv=True, smooth_angle=85.0)

def stadium_prism(name, center, half_w, half_h, y_front, depth, bevel_w, bevel_d, mat, part, seg_arc=48, rough=.01):
    """Watertight pill/stadium architectural mirror or backing plate with optically flat front plane (N=(0,-1,0) in Blender -> (0,0,-1) in Engine)."""
    cx, cy_ref, cz = center
    straight_h = max(0.0, half_h - half_w)
    # Build 2D stadium contour in (x, z)
    contour = []
    for i in range(seg_arc + 1):
        a = math.pi * i / seg_arc  # right (+x) over top (+z) to left (-x)
        contour.append((math.cos(a) * half_w, straight_h + math.sin(a) * half_w))
    for i in range(1, seg_arc):
        a = math.pi + math.pi * i / seg_arc  # left (-x) around bottom (-z) to right (+x)
        contour.append((math.cos(a) * half_w, -straight_h + math.sin(a) * half_w))
    n = len(contour)
    # Rings from front center fan? No: use convex/concave ear-clipped cap polygons on front & back so zero central star pole!
    # Ring 0: inner front flat glass (inset by bevel_w at y = y_front)
    # Ring 1: outer front edge (at y = y_front + bevel_d)
    # Ring 2: outer back edge (at y = y_front + depth)
    verts = []
    for scale_inset, yy in [
        (max(0.1, (half_w - bevel_w) / half_w), y_front),
        (1.0, y_front + bevel_d),
        (1.0, y_front + depth),
    ]:
        for px, pz in contour:
            # Uniform inset along normal of stadium
            if pz > straight_h:
                dx, dz = px, pz - straight_h
                ln = max(math.hypot(dx, dz), 1e-6)
                ix, iz = px - dx / ln * bevel_w * (1.0 if scale_inset < 1.0 else 0.0), pz - dz / ln * bevel_w * (1.0 if scale_inset < 1.0 else 0.0)
            elif pz < -straight_h:
                dx, dz = px, pz + straight_h
                ln = max(math.hypot(dx, dz), 1e-6)
                ix, iz = px - dx / ln * bevel_w * (1.0 if scale_inset < 1.0 else 0.0), pz - dz / ln * bevel_w * (1.0 if scale_inset < 1.0 else 0.0)
            else:
                ix = px - (bevel_w if px > 0 else -bevel_w) * (1.0 if scale_inset < 1.0 else 0.0)
                iz = pz
            verts.append((cx + ix, yy, cz + iz))
    faces = []
    # Front flat mirror face (outward normal -Y in Blender -> winding clockwise when viewed from -Y: reversed(0..n-1))
    faces.append(tuple(reversed(range(n))))
    # Front bevel ring (0 -> 1)
    for i in range(n):
        ni = (i + 1) % n
        faces.append((i, ni, n + ni, n + i))
    # Side rim (1 -> 2)
    for i in range(n):
        ni = (i + 1) % n
        faces.append((n + i, n + ni, 2 * n + ni, 2 * n + i))
    # Back cap (+Y normal)
    faces.append(tuple(range(2 * n, 3 * n)))
    m = bpy.data.meshes.new(name)
    m.from_pydata(verts, [], faces)
    m.update()
    # Smooth side arc & bevel laterally, but keep front mirror polygon 100% flat via smooth_angle=22 deg!
    for idx, f in enumerate(m.polygons):
        f.use_smooth = (idx != 0 and idx != len(m.polygons) - 1)
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    return keep(o, name, mat, part=part, rough=rough, smooth_angle=22.0)

def import_cc0_plant(tag, slug, mesh_names, target_pos, scale=1.0, rot_z=0.0, mat=29, part=234, rough=.36, thickness=.00075):
    """Import real scanned CC0 botanical plant sub-meshes, recenter base to target_pos, and solidify to 0 non-manifold edges."""
    gltf_path = next((CC0_MODELS / slug).glob('*.gltf'))
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(gltf_path))
    imported = [o for o in bpy.data.objects if o not in before]
    picked = [o for o in imported if o.type == 'MESH' and (not mesh_names or o.name in mesh_names)]
    unused = [o for o in imported if o not in picked]

    # Compute combined world bounding box of picked sub-meshes to recenter base cleanly at origin
    all_pts = []
    for o in picked:
        mw = o.matrix_world
        for v in o.data.vertices:
            all_pts.append(mw @ v.co)
    cx_src = 0.5 * (min(p.x for p in all_pts) + max(p.x for p in all_pts))
    cy_src = 0.5 * (min(p.y for p in all_pts) + max(p.y for p in all_pts))
    zmin_src = min(p.z for p in all_pts)

    rot_m = Euler((0.0, 0.0, rot_z), 'XYZ').to_matrix().to_4x4()
    tx, ty, tz = target_pos
    out_objs = []
    for idx, o in enumerate(picked):
        # Bake existing glTF hierarchy transform into mesh vertices first
        mw = o.matrix_world.copy()
        o.parent = None
        o.matrix_world = Matrix.Identity(4)
        me = o.data
        bm = bmesh.new()
        bm.from_mesh(me)
        for v in bm.verts:
            p = mw @ v.co
            local_p = Vector(((p.x - cx_src) * scale, (p.y - cy_src) * scale, (p.z - zmin_src) * scale))
            rp = rot_m @ local_p
            v.co = Vector((rp.x + tx, rp.y + ty, rp.z + tz))
        degen = [f for f in bm.faces if f.calc_area() <= 1e-11]
        if degen:
            bmesh.ops.delete(bm, geom=degen, context='FACES')
        multi = [e for e in bm.edges if len(e.link_faces) > 2]
        if multi:
            bmesh.ops.split_edges(bm, edges=multi)
        bm.to_mesh(me)
        bm.free()
        me.materials.clear()
        for f in me.polygons:
            f.use_smooth = True
        mod = o.modifiers.new('botanical_leaf_thickness', 'SOLIDIFY')
        mod.solidify_mode = 'NON_MANIFOLD'
        mod.thickness = thickness
        mod.offset = 0.0
        mod.use_rim = True
        out_objs.append(keep(o, '%s_%d' % (tag, idx), mat, part=part, rough=rough, thick=thickness, preserve_uv=True, smooth_angle=75.0))

    for uo in unused:
        bpy.data.objects.remove(uo, do_unlink=True)
    return out_objs

def shanghai():
    """Shanghai Huangpu River crossing: Waibaidu-inspired curved-chord riveted railway through-truss (y = -54..+6)
    opening onto a low-parapet plate-girder river span (y = +6..+56) with West Bund heritage blocks, river barges,
    and a grounded Pudong / Lujiazui waterfront embankment & landmark quartet.
    Partitioned across spatial part IDs 0..5 for tight DXR BLAS bounding boxes.
    """
    def sh_foliage(tag, bx, by, bz, rx, ry, rz, part_id):
        box('%s_curb' % tag, (bx, by, bz - rz * 0.52), (rx * 1.45, ry * 1.45, 0.24), 2, part=part_id, bevel=.04, rough=.52)
        box('%s_canopy_lo' % tag, (bx, by, bz - rz * 0.08), (rx * 1.32, ry * 1.26, rz * 0.72), 39, part=part_id, bevel=.22, segments=2, rough=.84)
        box('%s_canopy_hi' % tag, (bx + 0.12 * rx, by - 0.08 * ry, bz + rz * 0.30), (rx * 0.92, ry * 0.88, rz * 0.56), 39, part=part_id, bevel=.18, segments=2, rough=.82, rot_z=0.35)

    # Part 0: Huangpu River Basin, River Barges, West Bund Heritage Embankment & Pudong Waterfront Seawall
    box('huangpu_river', (0.0, 42.0, -1.55), (260.0, 230.0, 0.30), 7, part=0, rough=.04)

    # Pudong / Lujiazui East/North Granite Embankment, Floodwall & Tree-Lined Riverside Promenade (y >= 66)
    box('pudong_land_base', (8.0, 108.0, -0.60), (210.0, 84.0, 1.70), 2, part=0, rough=.56)
    box('pudong_seawall', (8.0, 66.2, -0.45), (195.0, 1.6, 1.85), 2, part=0, bevel=.06, rough=.42)
    box('pudong_promenade', (8.0, 68.4, 0.32), (190.0, 3.8, 0.18), 2, part=0, rough=.48)
    for q_i in range(16):
        qx = -46.0 + q_i * 7.2
        box('pudong_quay_lamp_%d' % q_i, (qx, 66.8, 0.54), (0.40, 0.10, 0.08), 16, part=0, rough=.22)
        sh_foliage('pudong_tree_%d' % q_i, qx + 2.4, 69.5, 1.08, 1.35, 1.20, 0.95, 0)

    # West / North Bund Heritage Riverfront Embankment & Neoclassical Masonry Blocks on Left Bend
    box('bund_west_land', (-52.0, 24.0, -0.65), (68.0, 92.0, 1.65), 2, part=0, rough=.56)
    box('bund_west_seawall', (-18.2, 24.0, -0.48), (1.5, 88.0, 1.80), 2, part=0, bevel=.06, rough=.42)
    bund_heritage = [
        (-26.5, 22.0,  9.5, 11.0,  9.2, 13,  0.04, True),
        (-28.0, 36.5, 10.5, 12.5, 11.4, 13, -0.02, False),
        (-29.5, 51.0, 11.0, 11.5, 13.0, 13, -0.06, True),
        (-42.0, 32.0, 11.5, 12.0, 15.5, 14,  0.02, False),
        (-44.0, 48.0, 12.0, 11.5, 17.5,  5, -0.04, False),
    ]
    for bh_i, (bx, by, bsx, bsy, bh, bmat, brot, has_cupola) in enumerate(bund_heritage):
        box('bund_masonry_base_%d' % bh_i, (bx, by, 1.3), (bsx * 1.06, bsy * 1.06, 2.6), 2, part=0, bevel=.14, rough=.45, rot_z=brot)
        box('bund_heritage_body_%d' % bh_i, (bx, by, bh * 0.5 + 0.2), (bsx, bsy, bh), bmat, part=0, bevel=.18, rough=.32, rot_z=brot)
        box('bund_cornice_glow_%d' % bh_i, (bx + bsx * 0.49, by, bh - 0.35), (0.14, bsy * 0.88, 0.16), 15, part=0, rough=.24, rot_z=brot)
        if has_cupola:
            box('bund_clock_turret_%d' % bh_i, (bx + bsx * 0.22, by, bh + 1.3), (2.5, 2.5, 2.4), 2, part=0, bevel=.10, rough=.42, rot_z=brot)
            box('bund_turret_lantern_%d' % bh_i, (bx + bsx * 0.22, by, bh + 2.55), (1.7, 1.7, 0.22), 15, part=0, rough=.24, rot_z=brot)
            sphere('bund_dome_%d' % bh_i, (bx + bsx * 0.22, by, bh + 2.95), (1.15, 1.15, 0.95), 0, part=0, seg=14, rings=8, rough=.36)

    # Two Huangpu River Cargo Barges in Middle-Distance Channel (scale & maritime context)
    for bg_i, (bgx, bgy, bg_rot) in enumerate([(-9.2, 41.0, 0.24), (17.5, 50.5, -0.18)]):
        box('river_barge_hull_%d' % bg_i, (bgx, bgy, -1.24), (3.4, 15.5, 0.62), 0, part=0, bevel=.10, rough=.36, rot_z=bg_rot)
        box('river_barge_hold_%d' % bg_i, (bgx, bgy - 0.8, -0.82), (2.6, 10.2, 0.42), 2, part=0, bevel=.06, rough=.58, rot_z=bg_rot)
        box('river_barge_cabin_%d' % bg_i, (bgx - 0.5 * math.sin(bg_rot) * 5.2, bgy - 5.4 * math.cos(bg_rot), -0.54), (2.3, 2.4, 0.96), 0, part=0, bevel=.06, rough=.34, rot_z=bg_rot)
        box('river_barge_bridge_lit_%d' % bg_i, (bgx - 0.5 * math.sin(bg_rot) * 5.2, bgy - 4.18 * math.cos(bg_rot), -0.34), (1.7, 0.12, 0.22), 15, part=0, rough=.22, rot_z=bg_rot)
        sphere('river_barge_nav_%d' % bg_i, (bgx + math.sin(bg_rot) * 7.2, bgy + 7.2 * math.cos(bg_rot), -0.78), (.09, .09, .09), 16, part=0, seg=8, rings=6, rough=.18)

    # Continuous Railway Bridge Deck Substructure, Track Ballast, Localized Wet Plates, Sleepers & Steel Rails (y = -54..+64)
    # Partitioned at y = +6 between Part 1 (approach truss) and Part 2 (open river deck girder)
    for seg_part, y0, y1 in [(1, -54.0, 6.0), (2, 6.0, 66.0)]:
        ym = 0.5 * (y0 + y1)
        ylen = y1 - y0
        # Twin longitudinal steel plate girders + matte stone ballast bed
        for sx_g in [-4.65, 4.65]:
            box('plate_girder_%d' % seg_part, (sx_g, ym, -0.58), (0.34, ylen, 0.96), 0, part=seg_part, bevel=.03, rough=.34)
        box('ballast_trough_%d' % seg_part, (0.0, ym, -0.18), (8.90, ylen, 0.28), 2, part=seg_part, rough=.72)
        # Localized rain-slicked steel inspection apron & drainage strips (realistic localized reflections, not a 12m mirror!)
        box('wet_center_apron_%d' % seg_part, (0.0, ym, -0.025), (1.18, ylen, 0.04), 1, part=seg_part, rough=.08)
        for sx_w in [-2.35, 2.35]:
            box('wet_drain_strip_%d' % seg_part, (sx_w, ym, -0.020), (0.95, ylen, 0.04), 1, part=seg_part, rough=.10)
        # Raised side maintenance walkways
        for sx_wk in [-3.65, 3.65]:
            box('side_walkway_%d' % seg_part, (sx_wk, ym, 0.08), (1.45, ylen, 0.14), 1, part=seg_part, bevel=.02, rough=.24)
        # Standard-gauge polished steel running rails + inner bridge guard/check rails
        for rx_r in [-0.76, 0.76]:
            box('rail_web_%d' % seg_part, (rx_r, ym, 0.14), (0.07, ylen, 0.11), 0, part=seg_part, rough=.36)
            box('polished_rail_head_%d' % seg_part, (rx_r, ym, 0.215), (0.085, ylen, 0.055), 12, part=seg_part, bevel=.012, rough=.07)
        for gx_r in [-0.54, 0.54]:
            box('guard_rail_%d' % seg_part, (gx_r, ym, 0.185), (0.055, ylen, 0.075), 12, part=seg_part, bevel=.008, rough=.12)

    # Sleepers & rail tie-plates along visible track zone (y = -48..+62)
    y_slp = -48.0
    while y_slp <= 62.0:
        sp_id = 1 if y_slp < 6.0 else 2
        box('sleeper', (0.0, y_slp, 0.03), (2.65, 0.26, 0.13), 2, part=sp_id, bevel=.02, rough=.62)
        for rx_p in [-0.76, 0.76]:
            box('tie_plate', (rx_p, y_slp, 0.10), (0.18, 0.22, 0.025), 0, part=sp_id, rough=.32)
        y_slp += 1.25

    # Granite/Concrete River Caisson Piers supporting Truss Portals & Open River Span
    for pier_y, pier_w, pt_id in [(-24.0, 11.2, 1), (6.0, 11.8, 1), (26.0, 10.2, 2), (46.0, 10.2, 2), (64.0, 10.6, 2)]:
        box('river_caisson_pier', (0.0, pier_y, -1.05), (pier_w, 2.2, 1.35), 2, part=pt_id, bevel=.12, rough=.48)

    # Part 1 Superstructure: Curved-Chord Waibaidu-Style Riveted Steel Through-Truss (y = -54..+6 ONLY!)
    truss_nodes = [(-54.0, 6.4), (-44.0, 7.6), (-34.0, 8.5), (-24.0, 8.9), (-14.0, 8.5), (-4.0, 7.6), (6.0, 6.4)]
    for idx_t, (ty, th) in enumerate(truss_nodes):
        is_portal = (idx_t == 0 or idx_t == len(truss_nodes) - 1)
        post_r = 0.22 if is_portal else 0.15
        for sx_t in [-4.55, 4.55]:
            # Vertical built-up steel box/chord post + lower & upper riveted gusset plates
            box('truss_vertical', (sx_t, ty, th * 0.5), (post_r * 1.5, post_r * 1.8, th), 0, part=1, bevel=.02, rough=.30)
            box('gusset_low', (sx_t - math.copysign(0.08, sx_t), ty, 0.75), (0.08, 0.95, 1.15), 0, part=1, bevel=.01, rough=.28)
            box('gusset_high', (sx_t - math.copysign(0.08, sx_t), ty, th - 0.55), (0.08, 0.95, 0.95), 0, part=1, bevel=.01, rough=.28)
            # Portal / sway knee-braces
            rod('knee_brace', (sx_t, ty, th - 1.35), (sx_t - math.copysign(1.35, sx_t), ty, th), .085, 0, part=1, verts=8, rough=.30)
        # Overhead transverse portal/sway girder + double-tier latticed portal frame at North Exit (y = +6.0)
        box('overhead_sway_beam', (0.0, ty, th), (9.30, 0.34 if is_portal else 0.22, 0.38 if is_portal else 0.24), 0, part=1, bevel=.02, rough=.30)
        if is_portal:
            box('portal_upper_arch', (0.0, ty, th + 0.78), (9.30, 0.30, 0.28), 0, part=1, bevel=.02, rough=.30)
            for px_l in [-3.2, -1.6, 0.0, 1.6, 3.2]:
                rod('portal_lattice', (px_l, ty, th), (px_l + 0.8, ty, th + 0.78), .055, 0, part=1, verts=8, rough=.30)
                rod('portal_lattice_b', (px_l, ty, th + 0.78), (px_l + 0.8, ty, th), .055, 0, part=1, verts=8, rough=.30)

        if idx_t + 1 < len(truss_nodes):
            ny, nh = truss_nodes[idx_t + 1]
            ym_b = 0.5 * (ty + ny)
            for sx_t in [-4.55, 4.55]:
                # Curved polygonal top chord + subdivided Warren diagonals + mid-bay sub-vertical
                rod('curved_top_chord', (sx_t, ty, th), (sx_t, ny, nh), .18, 0, part=1, verts=10, rough=.28)
                rod('warren_diag_up', (sx_t, ty, 0.15), (sx_t, ym_b, 0.5 * (th + nh)), .12, 0, part=1, verts=8, rough=.30)
                rod('warren_diag_dn', (sx_t, ym_b, 0.5 * (th + nh)), (sx_t, ny, 0.15), .12, 0, part=1, verts=8, rough=.30)
                rod('sub_vertical', (sx_t, ym_b, 0.10), (sx_t, ym_b, 0.5 * (th + nh)), .085, 0, part=1, verts=8, rough=.32)
                # Walkway handrail along truss
                rod('truss_handrail', (math.copysign(4.25, sx_t), ty, 1.05), (math.copysign(4.25, sx_t), ny, 1.05), .035, 0, part=1, verts=8, rough=.32)
            # Overhead X-wind-bracing
            rod('wind_brace_a', (-4.55, ty, th), (4.55, ny, nh), .065, 0, part=1, verts=8, rough=.32)
            rod('wind_brace_b', (4.55, ty, th), (-4.55, ny, nh), .065, 0, part=1, verts=8, rough=.32)

    # Inclined Batter End-Posts anchoring the North Truss Portal (y = +6 -> +10.2) down to the deck girder
    for sx_t in [-4.55, 4.55]:
        rod('north_batter_post', (sx_t, 6.0, 6.4), (sx_t, 10.2, 0.15), .19, 0, part=1, verts=10, rough=.28)

    # Structural Railway Signal Gantry mounted on the y = -14 Truss Frame (no floating ovals!)
    box('signal_gantry_beam', (0.0, -14.0, 5.15), (9.0, 0.24, 0.26), 0, part=1, bevel=.02, rough=.30)
    for sg_x, lamp_m in [(-0.85, 6), (2.45, 16)]:
        box('signal_head_hood', (sg_x, -14.12, 4.78), (0.44, 0.32, 0.88), 0, part=1, bevel=.03, rough=.32)
        sphere('signal_aspect_lens', (sg_x, -14.30, 4.66), (0.12, 0.06, 0.12), lamp_m, part=1, seg=12, rings=8, rough=.14)
    # Warm low bulkhead lamps mounted to truss posts at walkway level
    for wy_l in [-34.0, -14.0, 6.0]:
        for sx_l in [-4.38, 4.38]:
            box('truss_walk_lamp', (sx_l, wy_l, 1.42), (0.08, 0.22, 0.12), 16, part=1, rough=.20)

    # Realistic Dark-Copper Overhead Catenary terminated cleanly at the y = +6 North Truss Portal,
    # handing off to a low trackside third-rail conductor along the open river span so no wires cut the skyline!
    rod('catenary_contact_wire', (0.0, -52.0, 5.35), (0.0, 6.0, 5.35), .014, 0, part=1, verts=6, rough=.26)
    rod('catenary_messenger', (0.0, -52.0, 5.95), (0.0, 6.0, 5.95), .012, 0, part=1, verts=6, rough=.30)
    box('third_rail_conductor', (1.22, 36.0, 0.24), (0.11, 60.0, 0.09), 0, part=2, bevel=.015, rough=.26)

    # Part 2 Superstructure: Open Low-Parapet River Span (y = +6..+66 — completely unobstructed skyline reveal!)
    for sx_p in [-4.42, 4.42]:
        box('open_span_parapet_top', (sx_p, 36.0, 1.08), (0.10, 60.0, 0.08), 0, part=2, bevel=.015, rough=.28)
        box('open_span_parapet_mid', (sx_p, 36.0, 0.62), (0.06, 60.0, 0.06), 0, part=2, rough=.32)
    for py_st in range(8, 66, 3):
        for sx_p in [-4.42, 4.42]:
            box('parapet_stanchion', (sx_p, float(py_st), 0.56), (0.09, 0.09, 1.04), 0, part=2, rough=.32)
        if (py_st % 12) == 8:
            for sx_p in [-4.34, 4.34]:
                box('parapet_deck_lantern', (sx_p, float(py_st), 0.88), (0.09, 0.24, 0.10), 16, part=2, rough=.20)

    # Part 3 & Part 4: Pudong / Lujiazui Hero Landmarks & Varied Financial Skyline (grounded on y >= 68 land!)
    # 1) Oriental Pearl Tower at (-11.5, 76.5), total height 35.0 (angled tripod struts + 3 columns + spheres + antenna)
    op_x, op_y = -11.5, 76.5
    for k_s in range(3):
        ang_s = k_s * TAU / 3.0 + 0.42
        rod('pearl_tripod_strut', (op_x + 4.8 * math.cos(ang_s), op_y + 4.8 * math.sin(ang_s), -0.2), (op_x + 1.15 * math.cos(ang_s), op_y + 1.15 * math.sin(ang_s), 8.2), .42, 11, part=3, verts=10, rough=.32)
        rod('pearl_diag_brace', (op_x + 6.2 * math.cos(ang_s + 1.05), op_y + 6.2 * math.sin(ang_s + 1.05), -0.2), (op_x + 1.5 * math.cos(ang_s + 1.05), op_y + 1.5 * math.sin(ang_s + 1.05), 5.6), .26, 11, part=3, verts=8, rough=.34)
        rod('pearl_main_column', (op_x + 1.05 * math.cos(ang_s), op_y + 1.05 * math.sin(ang_s), 7.5), (op_x + 0.78 * math.cos(ang_s), op_y + 0.78 * math.sin(ang_s), 23.0), .30, 11, part=3, verts=10, rough=.30)
    rod('pearl_lift_core', (op_x, op_y, 0.0), (op_x, op_y, 29.2), .42, 2, part=3, verts=12, rough=.36)
    sphere('pearl_lower_sphere', (op_x, op_y, 9.6), (2.75, 2.75, 2.65), 9, part=3, seg=28, rings=16, rough=.18)
    torus('pearl_lower_obs_ring', (op_x, op_y, 9.6), 2.80, .12, 15, part=3, seg=36, rough=.22)
    sphere('pearl_upper_sphere', (op_x, op_y, 22.4), (1.95, 1.95, 1.88), 9, part=3, seg=24, rings=14, rough=.18)
    torus('pearl_upper_obs_ring', (op_x, op_y, 22.4), 1.98, .09, 6, part=3, seg=32, rough=.20)
    sphere('pearl_space_capsule', (op_x, op_y, 28.4), (0.98, 0.98, 0.95), 9, part=3, seg=18, rings=10, rough=.20)
    rod('pearl_antenna_spire', (op_x, op_y, 29.0), (op_x, op_y, 35.0), .14, 11, part=3, verts=10, rough=.24)
    sphere('pearl_beacon_tip', (op_x, op_y, 35.1), (.18, .18, .18), 16, part=3, seg=10, rings=6, rough=.16)

    # 2) Jin Mao Tower at (3.5, 83.0), stepped 8-tier pagoda skyscraper, total height 31.5
    jm_x, jm_y = 3.5, 83.0
    box('jinmao_podium', (jm_x, jm_y, 1.6), (7.6, 7.6, 3.2), 13, part=3, bevel=.18, rough=.30)
    for tier_j in range(8):
        zj0 = 2.8 + tier_j * 3.1
        wj = 5.4 * (0.94 ** tier_j)
        box('jinmao_tier_%d' % tier_j, (jm_x, jm_y, zj0 + 1.55), (wj, wj, 3.15), 13, part=3, bevel=.14, rough=.22)
        box('jinmao_crown_collar_%d' % tier_j, (jm_x, jm_y, zj0 + 3.12), (wj * 1.04, wj * 1.04, 0.16), 2, part=3, rough=.35)
    box('jinmao_lantern', (jm_x, jm_y, 28.5), (2.2, 2.2, 1.9), 15, part=3, bevel=.08, rough=.22)
    rod('jinmao_spire', (jm_x, jm_y, 29.4), (jm_x, jm_y, 32.0), .12, 11, part=3, verts=8, rough=.24)

    # 3) Twisted Shanghai Tower at (15.5, 88.0), total height 38.2 (uncropped helical megatall with V-notch & glowing crown)
    st_x, st_y = 15.5, 88.0
    verts_st = []
    faces_st = []
    n_st = 18
    lv_st = 30
    for k in range(lv_st + 1):
        q = k / lv_st
        twist = q * 2.10  # 120-degree helical rotation
        taper = 1.0 - 0.44 * (q ** 1.18)
        for j in range(n_st):
            ang = j * TAU / n_st
            # Rounded-triangular cam-lobes + subtle vertical sky-atrium V-notch
            tri_lobe = 1.0 + 0.11 * math.cos(3.0 * ang) - 0.08 * math.exp(-12.0 * ((ang - math.pi) ** 2))
            r = 4.15 * taper * tri_lobe
            verts_st.append((st_x + r * math.cos(ang + twist), st_y + r * math.sin(ang + twist), -0.2 + 36.8 * q))
    for k in range(lv_st):
        for j in range(n_st):
            jn = (j + 1) % n_st
            faces_st.append((k * n_st + j, k * n_st + jn, (k + 1) * n_st + jn, (k + 1) * n_st + j))
    faces_st.extend([tuple(reversed(range(n_st))), tuple(range(lv_st * n_st, (lv_st + 1) * n_st))])
    mesh('shanghai_tower_helix', verts_st, faces_st, 5, part=4, smooth=False, rough=.16)
    bpy.ops.mesh.primitive_cylinder_add(vertices=18, radius=2.15, depth=1.6, location=(st_x, st_y, 37.3))
    keep(bpy.context.object, 'shanghai_tower_crown', 15, part=4, rough=.20)

    # 4) SWFC at (27.5, 85.0), total height 34.2 (chamfered monolith with crisp trapezoidal skybridge aperture)
    sw_x, sw_y = 27.5, 85.0
    box('swfc_body_lower', (sw_x, sw_y, 14.4), (5.8, 4.4, 29.2), 14, part=4, bevel=.18, rough=.18, rot_z=-0.12)
    for side_sw in [-2.15, 2.15]:
        sx_w = sw_x + side_sw * math.cos(-0.12)
        sy_w = sw_y + side_sw * math.sin(-0.12)
        box('swfc_portal_pier', (sx_w, sy_w, 31.2), (1.45, 4.2, 4.6), 14, part=4, bevel=.10, rough=.18, rot_z=-0.12)
    box('swfc_skybridge_deck', (sw_x, sw_y, 33.6), (5.75, 4.2, 0.95), 14, part=4, bevel=.08, rough=.18, rot_z=-0.12)
    box('swfc_skybridge_soffit', (sw_x, sw_y, 33.05), (3.2, 3.8, 0.14), 15, part=4, rough=.20, rot_z=-0.12)

    # 5) Varied Lujiazui Financial Towers, IFC Twins & Waterfront Podium Pavilions (breaking any uniform grid!)
    pudong_skyline = [
        # (bx, by, sx, sy, h, mat, setback, rot, part_id)
        (-36.0, 82.0, 7.6, 6.8, 17.5, 13, 0.74,  0.08, 3),
        (-26.5, 76.0, 8.2, 7.2, 22.0, 14, 0.68, -0.05, 3),
        (-21.0, 90.0, 8.6, 7.8, 25.5,  5, 0.72,  0.03, 3),
        ( -4.5, 73.5, 7.2, 6.4, 19.5, 14, 0.76, -0.08, 3),  # IFC South
        ( -3.0, 88.0, 7.6, 6.8, 23.0, 14, 0.74, -0.06, 3),  # IFC North
        (  9.5, 75.0, 6.8, 6.2, 14.5, 13, 0.00,  0.05, 4),
        ( 21.5, 76.5, 7.2, 6.4, 18.0, 13, 0.78,  0.04, 4),
        ( 36.5, 78.0, 8.2, 7.2, 24.0,  5, 0.70, -0.06, 4),
        ( 40.0, 92.0, 8.8, 7.8, 27.0, 14, 0.68,  0.05, 4),
        ( 48.0, 82.0, 8.0, 7.0, 19.0, 13, 0.75, -0.04, 4),
        ( 57.5, 76.0, 8.6, 7.4, 15.0, 14, 0.00,  0.08, 4),
    ]
    for pd_i, (px, py, psx, psy, ph, pmat, psb, prot, ppt) in enumerate(pudong_skyline):
        box('ljz_podium_%d' % pd_i, (px, py, 1.5), (psx * 1.25, psy * 1.25, 3.0), 2 if (pd_i % 2 == 0) else 13, part=ppt, bevel=.18, rough=.36, rot_z=prot)
        box('ljz_tower_%d' % pd_i, (px, py, ph * 0.5), (psx, psy, ph), pmat, part=ppt, bevel=.22, rough=.20, rot_z=prot)
        if psb > 0.1:
            th_top = ph * 0.22
            box('ljz_setback_%d' % pd_i, (px, py, ph + th_top * 0.45), (psx * psb, psy * psb, th_top), pmat, part=ppt, bevel=.16, rough=.20, rot_z=prot)

    # Low Waterfront Exhibition & Ferry Pavilions along Pudong Promenade (y = 70.5)
    for pv_x in [-19.0, 1.5, 16.0, 32.0]:
        box('pudong_pavilion', (pv_x, 70.5, 1.45), (9.5, 4.2, 2.3), 14, part=5, bevel=.12, rough=.22)
        box('pudong_pavilion_glow', (pv_x, 68.5, 1.05), (7.6, 0.20, 0.95), 15, part=5, rough=.22)

    # The moving train sees the vertical side frames at close range. A second
    # flange and diagonal lacing give these posts depth from either direction.
    # The knees stay over the maintenance walks, outside the rail clearance.
    for frame, (y, height) in enumerate(truss_nodes):
        for side in (-1, 1):
            x = 4.55 * side
            for fy in (y-.23, y+.23):
                box('built_up_post_flange', (x, fy, height/2), (.34,.075,height), 11, part=1, rough=.41)
            for level in range(8):
                za, zb = .50 + (height-1.1)*level/8, .50 + (height-1.1)*(level+1)/8
                offset = .22 if level % 2 else -.22
                rod('post_lacing', (x-side*.13,y+offset,za), (x-side*.13,y-offset,zb),
                    .047, 0, part=1, verts=6, rough=.42)
            # This tapered plate supports an open triangular spandrel. A compact
            # hooded amber lamp marks the inside face of each alternate pier.
            profile = [(x, height-.12), (side*2.60, height-.12), (side*3.15,height-.54),
                       (side*3.70,height-.86), (side*4.27,height-1.82), (x,height-2.05)]
            ring = profile if side > 0 else list(reversed(profile))
            verts = [(px, y+dy, pz) for dy in (-.16,.16) for px,pz in ring]
            n = len(ring)
            mesh('portal_knee_plate', verts,
                 [tuple(range(n)), tuple(range(2*n-1,n-1,-1))] +
                 [(j,j+n,(j+1)%n+n,(j+1)%n) for j in range(n)], 11, 1, rough=.44)
            rod('lower_portal_flange', (side*4.27,y,height-1.82), (side*2.60,y,height-.12),
                .10, 0, part=1, verts=8, rough=.46)
            if frame % 2 == 1:
                box('walkway_lamp_bracket', (x-side*.22,y-.24,2.30), (.52,.60,.28), 0, part=1, bevel=.04, rough=.43)
                box('walkway_lamp_lens', (x-side*.23,y-.55,2.28), (.32,.055,.15), 15, part=1, bevel=.025, rough=.24)
        if frame % 2 == 0:
            # Shallow overhead Warren truss. The continuous slot between chords
            # frames the next portal, then opens on the existing river span.
            box('portal_lower_chord', (0,y,height-.54), (8.7,.20,.13), 11, part=1, rough=.40)
            for bay in range(8):
                x0 = -4.32 + 1.08 * bay
                z0,z1 = (height-.50,height-.07) if bay % 2 == 0 else (height-.07,height-.50)
                rod('portal_lattice_strap',(x0,y,z0),(x0+1.08,y,z1),.068,0,part=1,verts=6,rough=.44)

    # River-facing galleries sit in front of the existing masonry skins. The
    # recessed windows, columns and continuous stone entablature follow each
    # block's yaw, so they remain attached around the bend in the embankment.
    for bx, by, width, depth, height, _, yaw, _ in bund_heritage:
        ca, sa = math.cos(yaw), math.sin(yaw)
        def gallery_pos(u, v, z):
            return (bx+ca*u-sa*v, by+sa*u+ca*v, z)
        edge = width*.50
        box('bund_gallery_steps',gallery_pos(edge+.38,0,1.0),(.95,depth*.93,.35),2,part=0,rot_z=yaw,rough=.50)
        box('bund_gallery_entablature',gallery_pos(edge+.33,0,height*.63),
            (1.05,depth*.97,.42),2,part=0,rot_z=yaw,rough=.42)
        for bay in range(7):
            v = (bay-3)*depth*.135
            rod('bund_gallery_column',gallery_pos(edge+.56,v,1.16),gallery_pos(edge+.56,v,height*.63),
                .18,2,part=0,verts=10,rough=.48)
            if bay < 6:
                box('bund_recessed_sash',gallery_pos(edge+.07,v+depth*.0675,height*.41),
                    (.11,depth*.068,height*.23),15 if bay in (1,4) else 38,part=0,rot_z=yaw,rough=.32)

    # North-bank second row behind the four established hero landmarks. Keep the
    # centre open: these lower, occupied silhouettes step down toward the river.
    for x,y,w,d,h,mat in [(-62,113,9,8,18,14),(-51,126,10,9,31,13),(-39,120,8,8,23,14),
                           (48,119,8,10,23,13),(59,128,12,10,34,14),(72,123,10,9,19,5)]:
        part = 3 if x < 0 else 4
        box('riverbank_office_podium',(x,y,h*.17),(w*1.12,d*1.10,h*.34),2,part=part,rough=.46)
        box('riverbank_office_wing',(x-w*.14,y,h*.49),(w*.65,d*.88,h*.97),mat,part=part,rough=.28)
        box('riverbank_office_annex',(x+w*.29,y+d*.10,h*.33),(w*.31,d*.64,h*.65),mat,part=part,rough=.32)
        box('riverbank_screened_crown',(x-w*.16,y+d*.04,h*.99),(w*.47,d*.56,h*.12),11,part=part,rough=.38)

def avenue(tag, to_asset, x, depth, surface, length, width, east_west):
    # Keep local metric UVs on both axes so distant vehicle lights stay on the asphalt
    # when a district moves or changes scale for the final interior shot.
    verts, top_uv = [], []
    for z in (surface - .08, surface):
        for dx, dz in [(-1, -1), (1, -1), (1, 1), (-1, 1)]:
            world_x = dx * .5 * (length if east_west else width)
            world_z = dz * .5 * (width if east_west else length)
            verts.append(to_asset(x + world_x, z, depth + world_z))
            run, cross = (world_x, world_z) if east_west else (world_z, world_x)
            top_uv.append((run / 2.8, cross / 2.8))
    faces = [(3, 2, 1, 0), (4, 5, 6, 7)] + [(i, (i+1)%4, (i+1)%4+4, i+4) for i in range(4)]
    road = mesh(tag, verts, faces, 1, part=7 if east_west else 8, rough=.15, preserve_uv=True)
    uv = road.data.uv_layers.new(name='RoadUV')
    for poly in road.data.polygons:
        for k, li in enumerate(poly.loop_indices):
            vi = road.data.loops[li].vertex_index
            uv.data[li].uv = top_uv[vi] if poly.index < 2 else ((0, 0), (1, 0), (1, .03), (0, .03))[k]


def district_towers(city, to_asset, parcels):
    """Occupied apartments, offices and atrium towers. Dimensions are in the film's world units."""
    batches = defaultdict(lambda: ([], []))
    square = [(-.5, -.5), (.5, -.5), (.5, .5), (-.5, .5)]
    clipped = [(-.36,-.5), (.35,-.5), (.5,-.34), (.5,.37),
               (.36,.5), (-.39,.5), (-.5,.37), (-.5,-.35)]
    oval = [(.5 * math.cos(i * TAU / 12), .5 * math.sin(i * TAU / 12)) for i in range(12)]
    for x, z, width, depth, height, glazing, angle, part, style in parcels:
        ca, sa = math.cos(angle), math.sin(angle)

        # Profiles can shift, turn, taper and slope independently. Each piece ends in a
        # real roof/soffit, so setbacks, through-openings and canopies survive reflections.
        def loft(material, outline, sections, slope=0):
            verts, faces = batches[part, material]
            base, n = len(verts), len(outline)
            for level, (y, u, v, sx, sz, twist) in enumerate(sections):
                ct, st = math.cos(twist), math.sin(twist)
                for a, b in outline:
                    du = width * (u + a * sx * ct - b * sz * st)
                    dv = depth * (v + a * sx * st + b * sz * ct)
                    roof = y + (a * slope if level == len(sections) - 1 else 0)
                    verts.append(to_asset(x + ca * du - sa * dv, -.68 + roof * height, z + sa * du + ca * dv))
            faces.append(tuple(base+i for i in reversed(range(n))))
            for j in range(len(sections) - 1):
                s = base + j*n
                faces.extend((s+i, s+(i+1)%n, s+n+(i+1)%n, s+n+i) for i in range(n))
            cap = base + (len(sections)-1) * n
            faces.append(tuple(cap+i for i in range(n)))

        def slab(mat, u, v, y0, y1, sx, sz):
            loft(mat, square, [(y0,u,v,sx,sz,0), (y1,u,v,sx,sz,0)])

        # A continuous plinth with a setback at the entrance reads as an occupied block
        # from the high Taiwan camera and gives the Shenzhen pilot a strong street wall.
        loft(13 if city == 'taipei' else 5, clipped,
             [(0,0,0,1.12,1.13,0), (.13,0,0,1.12,1.13,0), (.17,0,.03,1.04,1.01,0)])
        slab(2, 0, -.08, .12, .15, 1.16, 1.17)

        if style == 0:
            # Two inhabited blades carry sky lobbies over a completely open slot.
            for side in (-1, 1):
                crown = .93 if side < 0 else 1.08
                loft(glazing, clipped, [(.15,side*.32,0,.31,.89,0),
                    (.58,side*.36,.015,.34,.86,0), (crown,side*.25,.04,.29,.68,0)])
                slab(11, side*.25, .04, crown, crown+.022, .30, .69)
            for y, span in ((.48,.50), (.82,.38)):
                slab(13, 0, .03, y, y+.071, span, .42)
                slab(11, 0, .03, y-.013, y, span+.02, .45)
                slab(15, 0, -.185, y+.009, y+.018, span*.84, .018)
        elif style == 1:
            # Residential floor plates turn the corner round a lower garden wing.
            slab(14, -.22, .07, .15, 1, .43, .76)
            slab(13, .21, .20, .15, .84, .43, .47)
            for j in range(5):
                y = .29 + j*.13
                slab(2, -.22, -.285, y, y+.019, .48, .22)
                if j < 4:
                    slab(2, .20, .025, y, y+.019, .46, .21)
            slab(2, -.22, .07, 1, 1.029, .47, .80)
            slab(0, -.26, .24, 1.029, 1.087, .20, .27)
            slab(39, .21, .16, .84, .875, .38, .33)
        elif style == 2:
            # A faceted civic office changes bearing above a recessed mechanical storey.
            loft(glazing, oval, [(.15,0,0,.95,.87,0), (.54,0,0,.98,.84,.13),
                (.65,.015,0,.83,.76,.20), (.88,.03,0,.71,.71,.35),
                (1.04,.05,0,.66,.63,.42)], slope=.13)
            for y, sx, sz, twist in ((.53,1,.88,.13), (.87,.73,.75,.35)):
                loft(11, oval, [(y,0,0,sx,sz,twist), (y+.014,0,0,sx,sz,twist)])
            slab(0, -.13, .12, .97, 1.11, .15, .19)
        elif style == 3:
            # Deep, planted terrace cuts face the camera; the service spine continues up the back.
            for u, v, y0, y1, sx, sz in ((0,0,.15,.42,.91,.98), (-.03,.06,.42,.66,.84,.76),
                (-.08,.15,.66,.85,.70,.55), (-.14,.21,.85,1,.51,.36)):
                slab(glazing, u, v, y0, y1, sx, sz)
                slab(2, u, v, y1, y1+.014, sx+.045, sz+.04)
                slab(39, u+.06, v-sz*.43, y1+.014, y1+.035, sx*.65, sz*.09)
            slab(11, -.25, .34, .16, 1.075, .14, .21)
        else:
            # Steelwork follows a sheared glass sail. The diagonals are solids outside
            # the glass and the high corner breaks the roofline in profile.
            loft(glazing, clipped, [(.15,0,0,.96,.86,0), (.55,.02,0,.90,.86,0),
                (.95,.09,0,.67,.82,0)], slope=.30)
            for side in (-1, 1):
                for j in range(3):
                    ya, yb = .21 + j*.22, .43 + j*.22
                    ua = side * (.34 if j%2 == 0 else .05)
                    ub = side * (.05 if j%2 == 0 else .34)
                    loft(11, square, [(ya,ua,-.434,.036,.053,0), (yb,ub,-.434,.036,.053,0)])
            slab(0, -.13,.02,.88,.99,.22,.38)

    for (part, mat), (verts, faces) in sorted(batches.items()):
        mesh(city + '_district_architecture', verts, faces, mat, part, rough=.40 if mat == 2 else .25)


def basin_foothills(city, to_asset, part):
    """Two receding mountain spurs behind the last commuter districts."""
    taipei = city == 'taipei'
    for layer, (distance, relief) in enumerate(((646, 53), (757, 105))):
        verts, faces = [], []
        for i in range(109):
            x = (i - 54) * 14
            s = x + layer * 41 + (0 if taipei else 92)
            main = .52 + .17*math.cos(s/104) + .14*math.sin(s/61) + .095*math.cos(s/29)
            detail = .043*math.sin(s/13) + .024*math.cos(s/7) + .01*math.sin(s/3.6)
            h = relief*(main + detail) * (1 if taipei else .77)
            z = distance + 12*math.sin(s/88) + 5*math.cos(s/42)
            verts.extend(to_asset(x, y, d) for y,d in
                         [(-1.79,z-52), (h*.40,z-19), (h,z+3), (h*.67,z+21), (-1.79,z+48)])
        n = 5
        faces.append(tuple(range(n)))
        for j in range(108):
            a = j*n
            faces.extend((a+i,a+n+i,a+n+(i+1)%n,a+(i+1)%n) for i in range(n))
        faces.append(tuple(reversed(range(108*n,109*n))))
        mesh(city + '_outer_foothills', verts, faces, 19, part=part, smooth=True, rough=.96)


def basin_districts(city, to_asset):
    """Build receding street blocks in world units, batching closed silhouettes by district and material."""
    taiwan = city == 'taipei'
    rng = random.Random(101 if taiwan else 755)
    groups = defaultdict(lambda: ([], []))
    # Centers and heights of the business districts. Their intervening lower neighborhoods
    # leave distinctive saddles in the skyline, including a sight line to Taipei 101.
    centers = [(-83, 120, 21), (92, 158, 20), (-145, 220, 30), (165, 326, 38)] if taiwan else [
        (-77, 105, 31), (88, 134, 30), (-172, 195, 46), (110, 255, 54), (-286, 379, 65)]
    streets_ns = (-201, -143, -94, -57, -26, 0, 26, 57, 94, 143, 201)
    streets_ew = [(88, 285), (108, 340), (132, 380), (161, 420), (195, 480),
                  (235, 560), (285, 660), (346, 755), (418, 865)]
    square = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    chamfered = [(-.76, -1), (.76, -1), (1, -.76), (1, .76),
                 (.76, 1), (-.76, 1), (-1, .76), (-1, -.76)]
    sites = []
    # Streets converge in perspective. Stagger whole rows to form avenues running
    # into the basin while leaving cross streets between the individual parcels.
    for row, (distance, spacing, reach) in enumerate([
            (98, 14, 145), (117, 16, 175), (145, 19, 210), (177, 22, 255),
            (214, 23, 302), (258, 27, 370), (312, 31, 430), (378, 37, 490), (454, 42, 560)]):
        for col in range(-int(reach / spacing), int(reach / spacing) + 1):
            x = (col + .44 * (row % 2)) * spacing + rng.uniform(-.11, .11) * spacing
            depth = distance + rng.uniform(-.17, .17) * spacing
            if abs(x + .19 * depth - 8) < spacing * .39 or rng.random() < .085:
                continue
            sites.append((x, depth, spacing))
    # The oblique bank above the vertiport and the Taipei rooftop panorama both
    # see past the sides of the original town before looking toward the far hills.
    for side in (-1, 1):
        for belt, spacing in [(79, 15), (104, 17), (138, 20), (185, 24)]:
            for depth in range(-51, 87, spacing):
                sites.append((side * (belt + rng.uniform(-2, 2)), depth + rng.uniform(-3, 3), spacing))
    # Distant commuter towns are denser, with many narrow buildings per high rise.
    # They lap the foothills in adjoining valleys rather than ending at a single wall.
    outer = random.Random(797 if taiwan else 551)
    for row, (distance, spacing) in enumerate([(500, 22), (544, 19), (595, 20), (642, 24)]):
        for col in range(-30, 31):
            x = (col + .46 * (row % 2)) * spacing + outer.uniform(-3, 3)
            depth = distance + 8*math.cos(x/72) + outer.uniform(-6, 6)
            if outer.random() < .13:
                continue
            sites.append((x, depth, spacing))
    centers += [(-215, 526, 51), (46, 588, 58), (254, 561, 55)] if taiwan else [
        (-119, 565, 54), (97, 527, 62), (294, 606, 75)]
    featured = []
    for index, (x, depth, spacing) in enumerate(sites):
        density = max(peak * math.exp(-((x - cx) ** 2 + (depth - cz) ** 2) / 3500)
                      for cx, cz, peak in centers)
        height = (.55 * spacing + density) * rng.uniform(.68, 1.40)
        # Keep the silhouette of 101 legible against the lower eastern basin.
        if taiwan and abs(x - 18.5) < 27 and depth < 220:
            height *= .60
        width = spacing * rng.uniform(.44, .75)
        length = spacing * rng.uniform(.43, .68)
        nearest_x = min(streets_ns, key=lambda value: abs(value - x))
        if depth > 84 + .21 * abs(nearest_x):
            clearance = 2.8 + width * .52
            if abs(x - nearest_x) < clearance:
                x = nearest_x + math.copysign(clearance, x - nearest_x)
        nearest_z, reach = min(streets_ew, key=lambda street: abs(street[0] - depth))
        if abs(x) < reach * .5:
            clearance = 2.4 + length * .52
            if abs(depth - nearest_z) < clearance:
                depth = nearest_z + math.copysign(clearance, depth - nearest_z)
        style = rng.randrange(5)
        if 88 < depth < 250 and index % 4 == 1:
            featured.append((x, depth, width, length, height, (5,13,14)[index%3],
                             .08*math.sin(x), 3 if x<0 else 4, (index//4 + int(taiwan))%5))
            continue
        outline = chamfered if style in (1, 3) else square
        shelves = [(0, 1, 0), (.70, 1, 0), (.71, .75, .09), (1, .75, .09)]
        if style == 1:
            shelves = [(0, 1, 0), (.72, .96, 0), (1, .57, .16)]
        elif style == 2:
            shelves = [(0, 1, 0), (.85, 1, 0), (.86, .57, -.19), (1, .57, -.19)]
        elif style == 3:
            shelves = [(0, 1, 0), (.85, .83, .08), (1, .78, .12)]
        elif style == 4:
            shelves = [(0, 1, 0), (1, 1, 0)]
        angle = rng.uniform(-.18, .18)
        ca, sa = math.cos(angle), math.sin(angle)
        verts, faces = groups[(3 if x < 0 else 4, rng.choice([5, 13, 14]))]
        base, n = len(verts), len(outline)
        for h, scale, shift in shelves:
            for dx, dz in outline:
                u = width * (.5 * dx * scale + shift)
                v = length * .5 * dz * scale
                roof_cut = .14 * (dx + 1) * .5 if style == 3 and h == 1 else 0
                verts.append(to_asset(x + ca * u - sa * v, -.9 + (h - roof_cut) * height,
                                      depth + sa * u + ca * v))
        faces.append(tuple(reversed(range(base, base + n))))
        for floor in range(len(shelves) - 1):
            a = base + floor * n
            for j in range(n):
                k = (j + 1) % n
                faces.append((a + j, a + k, a + n + k, a + n + j))
        cap = base + (len(shelves) - 1) * n
        faces.append(tuple(range(cap, cap + n)))
        # Readable at neighborhood scale: roof access housings, an occasional
        # stacked lantern crown and a narrow, continuous illuminated elevation.
        if depth < 265 and style in (0, 2, 4):
            def fixture(material, along, across, w, d, z0, z1):
                fv, ff = groups[(3 if x < 0 else 4, material)]
                first = len(fv)
                for height_z in (z0, z1):
                    for ix, iz in square:
                        u, v = along + ix * w / 2, across + iz * d / 2
                        fv.append(to_asset(x + ca*u - sa*v, height_z, depth + sa*u + ca*v))
                ff.extend(tuple(first + i for i in f) for f in [
                    (3, 2, 1, 0), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5),
                    (2, 3, 7, 6), (3, 0, 4, 7)])
            roof = height - .9
            offset = shelves[-1][2] * width
            fixture(0, offset, -.07 * length, .38 * width, .32 * length, roof - .12, roof + 1.5)
            if style == 2:
                fixture(15 if taiwan else 6, offset, -.07 * length, .34 * width,
                        .28 * length, roof + 1.52, roof + 1.84)
            if style == 4 and abs(x) > 39:
                fixture(6 if taiwan else 16, -.31 * width, -.503 * length,
                        .19, .19, height * .45, height * .91)
    for (part, material), (verts, faces) in groups.items():
        mesh(city + '_basin_blocks', verts, faces, material, part=part, rough=.31)
    district_towers(city, to_asset, featured)
    surface = .065 if taiwan else -.48
    # Long boulevards lead the eye inland. Cross streets follow gaps between successive rows.
    for depth, width in streets_ew:
        avenue(city + '_cross_street', to_asset, 0, depth, surface, width, 3.1, True)
    for x in streets_ns:
        start = 84 + .21 * abs(x)
        avenue(city + '_basin_artery', to_asset, x, (start + 468) * .5,
               surface + .035, 468 - start, 3.6, False)


def shenzhen():
    """Shenzhen Bay / Houhai waterfront civic vertiport & varied urban skyline + winged 6-tilt-rotor eVTOL.
    Blender (+X right, +Y forward/away, +Z up) -> Engine (+X right, +Y up, +Z forward/away).
    Static city partitioned across part 0..5; eVTOL airframe = 100, tilt nacelles = 110..115, 3-blade rotors = 120..125.
    """
    def sz_foliage(tag, bx, by, bz, rx, ry, rz, part_id):
        box('%s_curb' % tag, (bx, by, bz - rz * 0.52), (rx * 1.48, ry * 1.42, 0.24), 2, part=part_id, bevel=.04, rough=.50)
        box('%s_hedge_lo' % tag, (bx, by, bz - rz * 0.08), (rx * 1.34, ry * 1.24, rz * 0.72), 39, part=part_id, bevel=.22, segments=2, rough=.84)
        box('%s_hedge_hi' % tag, (bx - 0.10 * rx, by + 0.08 * ry, bz + rz * 0.28), (rx * 0.90, ry * 0.86, rz * 0.54), 39, part=part_id, bevel=.18, segments=2, rough=.82, rot_z=-0.28)

    # Part 0: Shenzhen Bay Tidal Canal, Waterfront Basin & South/North Landmasses
    box('bay_water_channel', (0.0, 26.0, -1.25), (260.0, 125.0, 0.30), 7, part=0, rough=.04)
    box('south_peninsula_land', (0.0, -42.0, -1.45), (260.0, 76.0, 1.40), 2, part=0, rough=.58)
    box('north_houhai_land', (0.0, 96.0, -1.35), (260.0, 118.0, 1.50), 2, part=0, rough=.58)
    box('west_causeway_land', (-68.0, 12.0, -1.35), (76.0, 62.0, 1.50), 2, part=0, rough=.58)
    box('east_promenade_land', (68.0, 14.0, -1.35), (72.0, 66.0, 1.50), 2, part=0, rough=.58)

    # Granite waterfront seawalls & recessed warm quay grazers along both banks
    box('south_quay_wall', (-4.0, -4.2, -0.55), (96.0, 1.2, 1.30), 2, part=0, bevel=.06, rough=.42)
    box('north_quay_wall', (4.0, 38.0, -0.45), (150.0, 1.4, 1.50), 2, part=0, bevel=.06, rough=.42)
    box('north_boardwalk', (4.0, 39.8, 0.25), (146.0, 3.6, 0.16), 2, part=0, rough=.48)
    for b_i in range(14):
        bx_q = -52.0 + b_i * 8.2
        sz_foliage('north_banyan_%d' % b_i, bx_q, 41.2, 1.05, 1.35, 1.25, 0.95, 0)
        box('north_quay_lamp_%d' % b_i, (bx_q + 4.0, 38.6, 0.42), (0.35, 0.10, 0.08), 16, part=0, rough=.22)

    # Sleek tied-arch pedestrian & transit bridge spanning the Shenzhen Bay canal (east of flight corridor)
    box('canal_bridge_deck', (22.5, 16.8, 1.65), (4.6, 44.0, 0.34), 2, part=0, bevel=.05, rough=.38)
    box('canal_bridge_road', (22.5, 16.8, 1.84), (3.4, 43.6, 0.06), 1, part=0, rough=.16)
    box('canal_bridge_soffit', (22.5, 16.8, 1.44), (3.2, 42.0, 0.06), 16, part=0, rough=.22)
    for side_x in [20.4, 24.6]:
        arch_pts = [(side_x, -3.5 + 40.6 * (k / 16.0), 1.7 + 5.6 * math.sin(math.pi * k / 16.0)) for k in range(17)]
        for k in range(16):
            rod('bridge_arch_rib', arch_pts[k], arch_pts[k + 1], .16, 11, part=0, verts=10, rough=.24)
            if k > 0 and (k % 2 == 0):
                rod('bridge_hanger', arch_pts[k], (side_x, arch_pts[k][1], 1.75), .045, 0, part=0, verts=8, rough=.3)
    for py_p in [5.5, 28.0]:
        box('bridge_pier', (22.5, py_p, 0.10), (4.2, 1.6, 3.1), 2, part=0, bevel=.08, rough=.45)

    # Part 1: Connected Civic Vertiport Peninsula — Arterial Boulevard, Stepped Landscape Terraces, Terminal & Pads
    # Coastal arterial boulevard + vehicular drop-off loop + median planting
    world_to_sz = lambda x, height, depth: (x, depth, height)
    avenue('coastal_boulevard_ew', world_to_sz, -2, -17.5, -.62, 132, 7.6, True)
    box('boulevard_median', (-2.0, -17.5, -0.58), (126.0, 0.9, 0.16), 2, part=1, rough=.48)
    avenue('west_avenue_ns', world_to_sz, -25.5, -18, -.60, 56, 6.8, False)
    box('terminal_dropoff_apron', (-3.5, -11.8, -0.64), (30.0, 4.4, 0.12), 1, part=1, rough=.16)
    for s_i in range(12):
        lx = -44.0 + s_i * 7.8
        sz_foliage('blvd_tree_s_%d' % s_i, lx, -22.2, 0.35, 1.15, 1.05, 0.92, 1)
        if abs(lx + 3.5) > 14.0:
            sz_foliage('blvd_tree_n_%d' % s_i, lx, -12.8, 0.35, 1.05, 0.95, 0.85, 1)
        rod('blvd_lamp_mast_%d' % s_i, (lx + 3.6, -17.5, -0.55), (lx + 3.6, -17.5, 1.55), .05, 0, part=1, verts=8, rough=.35)
        box('blvd_lamp_head_%d' % s_i, (lx + 3.6, -17.5, 1.60), (0.48, 0.16, 0.06), 16, part=1, rough=.20)

    # Stepped civic amphitheater terraces descending from terminal concourse toward the water
    terrace_tiers = [
        (0.0, -8.2, -0.35, 38.0, 11.5, 0.55),
        (0.0, -7.2,  0.22, 33.0,  9.8, 0.58),
        (0.0, -6.4,  0.85, 28.0,  8.2, 0.62),
        (-1.5, -5.8, 1.55, 23.0,  6.8, 0.68),
    ]
    for t_i, (tx, ty, tz, tsx, tsy, tsz) in enumerate(terrace_tiers):
        box('civic_terrace_%d' % t_i, (tx, ty, tz), (tsx, tsy, tsz), 2, part=1, bevel=.06, rough=.42)
        box('terrace_riser_grazer_%d' % t_i, (tx, ty + tsy * 0.49, tz + tsz * 0.32), (tsx * 0.86, 0.06, 0.05), 16, part=1, rough=.22)
        for side in [-1, 1]:
            sz_foliage('terrace_planting_%d_%d' % (t_i, 1 if side > 0 else 0), tx + side * (tsx * 0.41), ty + 0.6, tz + tsz * 0.65, 1.25, 0.85, 0.55, 1)

    # Glazed Vertiport Passenger Terminal Lounge & Cantilevered Concourse Deck
    box('terminal_hall_core', (-3.0, -8.8, 2.25), (20.5, 6.4, 1.45), 14, part=1, bevel=.08, rough=.18)
    box('terminal_warm_lounge', (-3.0, -8.6, 2.20), (18.4, 5.2, 1.05), 15, part=1, rough=.24)
    box('terminal_roof_canopy', (-2.2, -7.8, 3.12), (24.0, 8.6, 0.30), 11, part=1, bevel=.06, rough=.26)
    # Sweeping pedestrian ramp linking West Avenue & Secondary Pad to Main Terminal Concourse
    rod('concourse_ramp_deck', (-16.5, -8.5, 2.72), (-3.0, -6.2, 3.15), .62, 2, part=1, verts=12, rough=.40)

    # Hero Cantilevered VTOL Touchdown Pad at (0.0, -2.0, 3.60) extending over the canal edge
    for sx_p in [-2.6, 2.6]:
        rod('hero_pad_vpier_f', (sx_p * 1.2, -4.8, -0.8), (sx_p, -1.6, 3.42), .26, 0, part=1, verts=10, rough=.28)
        rod('hero_pad_vpier_b', (sx_p * 1.2, -5.6, -0.2), (sx_p, -3.2, 3.42), .26, 0, part=1, verts=10, rough=.28)
    bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=5.4, depth=.32, location=(0.0, -2.0, 3.46))
    keep(bpy.context.object, 'hero_pad_substructure', 2, part=1, rough=.36)
    bpy.ops.mesh.primitive_cylinder_add(vertices=64, radius=4.95, depth=.10, location=(0.0, -2.0, 3.64))
    keep(bpy.context.object, 'hero_pad_deck', 0, part=1, rough=.28)
    torus('hero_pad_perimeter_ring', (0.0, -2.0, 3.69), 4.55, .045, 6, part=1, seg=64, rough=.20)
    for k_b in range(8):
        ang_b = k_b * TAU / 8.0
        box('hero_pad_threshold_%d' % k_b, (4.82 * math.cos(ang_b), -2.0 + 4.82 * math.sin(ang_b), 3.70), (0.36, 0.14, 0.04), 16, part=1, rough=.2, rot_z=ang_b)
    for dx_h in [-0.78, 0.78]:
        box('hero_pad_H_leg', (dx_h, -2.0, 3.70), (0.18, 2.30, 0.025), 6, part=1, rough=.2)
    box('hero_pad_H_bar', (0.0, -2.0, 3.70), (1.74, 0.18, 0.025), 6, part=1, rough=.2)

    # Secondary West Staging Pad at (-16.5, -8.5, 2.80)
    bpy.ops.mesh.primitive_cylinder_add(vertices=48, radius=3.8, depth=.26, location=(-16.5, -8.5, 2.72))
    keep(bpy.context.object, 'west_pad_deck', 0, part=1, rough=.30)
    torus('west_pad_ring', (-16.5, -8.5, 2.86), 3.35, .035, 6, part=1, seg=48, rough=.20)
    rod('west_pad_column', (-16.5, -8.5, -0.7), (-16.5, -8.5, 2.65), .55, 2, part=1, verts=16, rough=.42)

    # Part 2: North Waterfront Cultural & Retail Podiums + Terraced Campus Blocks (varied horizontal masses)
    waterfront_podiums = [
        (-32.0, 48.0, 16.0, 10.0, 5.2, 13, 0.04),
        (-13.0, 47.0, 14.5,  9.2, 4.4, 14, 0.00),
        (  6.5, 48.5, 17.0, 10.5, 5.8, 13, -0.03),
        ( 28.0, 47.5, 15.0,  9.5, 4.8, 14, 0.02),
        ( 46.5, 49.0, 14.0, 10.0, 6.2, 13, -0.05),
    ]
    for wp_i, (wx, wy, wsx, wsy, wsz, wmat, wrot) in enumerate(waterfront_podiums):
        box('wf_podium_low_%d' % wp_i, (wx, wy, wsz * 0.5 - 0.4), (wsx, wsy, wsz), wmat, part=2, bevel=.20, rough=.28, rot_z=wrot)
        box('wf_podium_terrace_%d' % wp_i, (wx + 1.2, wy + 0.8, wsz - 0.25), (wsx * 0.72, wsy * 0.68, 0.35), 2, part=2, rough=.46, rot_z=wrot)
        box('wf_galleria_glow_%d' % wp_i, (wx, wy - wsy * 0.48, 0.85), (wsx * 0.68, 0.24, 1.10), 15, part=2, rough=.22, rot_z=wrot)
        sz_foliage('wf_roof_garden_%d' % wp_i, wx - wsx * 0.22, wy, wsz + 0.15, 1.6, 1.2, 0.55, 2)

    # Part 3 (West Houhai / Nanshan Tech Campus) & Part 4 (East Houhai Supertall Financial Cluster)
    # 1) Landmark Tapered Diagrid Supertall ("Spring Bamboo"-inspired silhouette at (16.5, 69.0), h = 33.5 + crown)
    st_verts = []
    st_faces = []
    n_st = 16
    lv_st = 26
    for k in range(lv_st + 1):
        q = k / lv_st
        # Graceful entasis: swells subtly at 25% height, then tapers smoothly to a pointed lantern crown
        rad = 4.8 * (0.92 + 0.16 * math.sin(math.pi * min(1.0, q * 0.95))) * (1.0 - 0.62 * (q ** 1.45))
        zz = -0.5 + 33.5 * q
        for j in range(n_st):
            ang = j * TAU / n_st
            st_verts.append((16.5 + rad * math.cos(ang), 69.0 + rad * math.sin(ang), zz))
    for k in range(lv_st):
        for j in range(n_st):
            jn = (j + 1) % n_st
            st_faces.append((k * n_st + j, k * n_st + jn, (k + 1) * n_st + jn, (k + 1) * n_st + j))
    st_faces.extend([tuple(reversed(range(n_st))), tuple(range(lv_st * n_st, (lv_st + 1) * n_st))])
    mesh('houhai_tapered_supertall', st_verts, st_faces, 14, part=4, smooth=False, rough=.16)
    bpy.ops.mesh.primitive_cone_add(vertices=16, radius1=1.85, radius2=0.14, depth=4.4, location=(16.5, 69.0, 35.1))
    keep(bpy.context.object, 'supertall_lantern_crown', 15, part=4, rough=.20)

    # 2) Interlocking Twin Tech HQ Towers with Mid-Air Skybridges at (-18.5 & -7.5, 63.0)
    box('tech_hq_west', (-18.5, 63.0, 12.2), (7.4, 7.0, 25.4), 14, part=3, bevel=.25, rough=.20, rot_z=0.05)
    box('tech_hq_east', (-7.5,  62.0, 10.0), (6.8, 6.6, 21.0), 5,  part=3, bevel=.25, rough=.20, rot_z=0.05)
    for sb_z in [9.2, 15.8]:
        box('tech_hq_skybridge', (-13.0, 62.5, sb_z), (6.4, 3.0, 1.9), 13, part=3, bevel=.12, rough=.24, rot_z=0.05)
        box('tech_hq_bridge_soffit', (-13.0, 62.5, sb_z - 1.00), (5.8, 2.5, 0.14), 15, part=3, rough=.22, rot_z=0.05)

    # 3) Varied High-Rise, Setback & Mid-Rise Urban Blocks across West, North, and East Districts
    urban_masses = [
        # (bx, by, sx, sy, h, mat, setback_ratio, rot, part_id)
        (-48.0, 58.0,  9.0, 7.4, 19.0, 13, 0.72, -0.08, 3),
        (-34.0, 66.0,  8.2, 7.8, 26.5, 14, 0.68,  0.04, 3),
        (-28.0, 82.0,  9.2, 8.0, 22.5, 5,  0.75,  0.00, 3),
        (-11.0, 84.0,  8.6, 7.4, 29.0, 14, 0.65, -0.05, 3),
        (  3.5, 64.0,  8.0, 6.8, 18.0, 13, 0.78,  0.06, 4),
        (  5.0, 82.0,  8.8, 8.2, 26.0, 5,  0.70,  0.00, 4),
        ( 29.5, 63.0,  8.4, 7.2, 23.0, 13, 0.74, -0.06, 4),
        ( 33.0, 80.0,  9.2, 8.2, 29.5, 14, 0.66,  0.08, 4),
        ( 46.0, 68.0,  8.6, 7.6, 20.5, 5,  0.76, -0.04, 4),
        ( 58.0, 58.0, 10.0, 7.8, 16.0, 13, 0.80,  0.10, 4),
        # Flanking West & East mid-rise district street-wall campuses (grounding wide camera angles!)
        (-46.0,  8.0, 11.0, 9.0, 12.0, 13, 0.00,  0.00, 3),
        (-44.0, -10.0, 10.5, 8.5,  9.5, 14, 0.00,  0.04, 3),
        (-38.0, 26.0,  9.5, 8.2, 14.5, 14, 0.75, -0.05, 3),
        ( 44.0, 18.0, 10.8, 8.6, 13.5, 13, 0.78,  0.05, 4),
        ( 48.0, -4.0, 11.5, 9.2, 10.5, 14, 0.00, -0.04, 4),
        ( 58.0, 32.0,  9.8, 8.4, 16.0, 14, 0.72,  0.00, 4),
    ]
    # Individual waterfront parcels keep their real street frontages, but their crowns
    # break at different heights. Preserve the quay-side midrises below the flight route.
    typologies = [3, 2, 1, 4, 0, 2, 1, 4, 0, 3, 3, 1, 4, 0, 1, 2]
    waterfront = [(x, z, sx, sz, h * (1.16 if setback else 1), mat, rot, part, style)
                  for (x,z,sx,sz,h,mat,setback,rot,part), style in zip(urban_masses, typologies)]
    district_towers('shenzhen', world_to_sz, waterfront)
    avenue('houhai_station_road', world_to_sz, 6, 42.8, -.48, 163, 2.9, True)
    basin_districts('shenzhen', world_to_sz)
    box('outer_houhai_basin', (0, 448, -1.7), (1512, 702, 1.0), 2, part=5, rough=.75)
    basin_foothills('shenzhen', world_to_sz, 5)

    shenzhen_waterfront_architecture()
    shenzhen_aircraft()


def shenzhen_waterfront_architecture():
    """West-bank street wall, elevated coastal line and the split quay tower, in Blender coordinates."""
    # Batch the small architectural solids by material so the repeated train windows,
    # columns and storefronts stay inexpensive to generate and share draw calls.
    batches = defaultdict(lambda: ([], []))
    def loft(part, material, rings):
        vertices, faces = batches[part, material]
        base, n = len(vertices), len(rings[0])
        vertices.extend(point for ring in rings for point in ring)
        faces.append(tuple(base + i for i in reversed(range(n))))
        for j in range(len(rings) - 1):
            a = base + j * n
            for i in range(n):
                k = (i + 1) % n
                faces.append((a + i, a + k, a + n + k, a + n + i))
        faces.append(tuple(base + n * (len(rings) - 1) + i for i in range(n)))

    def block(part, material, center, size, angle=0.0):
        x, y, z = center
        hx, hy, hz = (v * .5 for v in size)
        ca, sa = math.cos(angle), math.sin(angle)
        footprint = [(x + u * ca - v * sa, y + u * sa + v * ca)
                     for u, v in [(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)]]
        loft(part, material, [[(px, py, z + dz) for px, py in footprint] for dz in [-hz, hz]])

    def beam(part, material, first, last, width, depth):
        a, b = Vector(first), Vector(last)
        axis = (b - a).normalized()
        across = axis.cross(Vector((0, 0, 1))).normalized() * (depth * .5)
        up = axis.cross(across).normalized() * (width * .5)
        loft(part, material, [[tuple(p + s * across + t * up) for s, t in
                              [(-1, -1), (1, -1), (1, 1), (-1, 1)]] for p in [a, b]])

    # Two guideways follow the west causeway, taking a twelve-unit radius bend
    # onto the north quay. Distance along the rail drives the bearings of the cars.
    west_x, start_y, turn_y, radius = -31.4, -77.0, 27.6, 12.0
    straight = turn_y - start_y
    turn_end = straight + radius * math.pi / 2
    end = turn_end + 144.0
    def rail_pose(s):
        if s <= straight:
            return west_x, start_y + s, math.pi / 2
        if s < turn_end:
            a = (s - straight) / radius
            return west_x + radius * (1.0 - math.cos(a)), turn_y + radius * math.sin(a), math.pi / 2 - a
        return west_x + radius + s - turn_end, turn_y + radius, 0.0

    def rail_point(s, side, z):
        x, y, a = rail_pose(s)
        return x - math.sin(a) * side, y + math.cos(a) * side, z

    stations = [0.0] + [straight + (turn_end - straight) * j / 16 for j in range(17)] + [end]
    # The profiled soffit narrows into the hammerhead piers. Four separate rail
    # bars sit on top and the parapet stops below passenger-window height.
    for mat, profile in [
        (2, [(-1.48, 3.93), (-.82, 3.57), (.82, 3.57), (1.48, 3.93), (1.48, 4.11), (-1.48, 4.11)]),
        *[(0, [(v-.05, 4.10), (v+.05, 4.10), (v+.05, 4.25), (v-.05, 4.25)]) for v in [-.98,-.36,.36,.98]],
        *[(11, [(v-.05, 4.09), (v+.05, 4.09), (v+.05, 4.34), (v-.05, 4.34)]) for v in [-1.37,1.37]],
    ]:
        loft(0, mat, [[rail_point(s, side, z) for side, z in profile] for s in stations])
    for s in range(5, int(end), 8):
        px, py, angle = rail_pose(s)
        block(0, 2, (px, py, 1.01), (.68, .65, 4.20), angle)
        for side in [-1, 1]:
            beam(0, 2, rail_point(s, side * .12, 2.42), rail_point(s, side * 1.08, 3.75), .35, .55)

    # Each carriage is a rigid shell. Facing ends meet at a narrower bellows so
    # the consist can actually follow the bend; the outer cabs taper to a nose.
    for car in range(5):
        # One rigid part per car. The renderer positions local +X along the two
        # straights and the inner rail of the bend using the film sample clock.
        car_part = 20 + car
        def carriage_block(mat, u, v, z, size):
            block(car_part, mat, (u, v, z), size)
        profile = [(-.40,4.38), (.40,4.38), (.52,4.60), (.52,5.30),
                   (.41,5.53), (.26,5.60), (-.26,5.60), (-.41,5.53), (-.52,5.30), (-.52,4.60)]
        sections = []
        for u in [-2.05, -1.57, 1.57, 2.05]:
            cab = (car == 0 and u < -2) or (car == 4 and u > 2)
            sections.append([(u, v * (.69 if cab else 1.0),
                              4.4 + (z-4.4) * (.61 if cab else 1.0)) for v,z in profile])
        loft(car_part, 11, sections)
        carriage_block(0, 0, 0, 4.34, (3.4, .67, .19))
        for side in [-1, 1]:
            carriage_block(6, 0, side * .525, 4.68, (3.18, .027, .052))
            for seat in range(6):
                carriage_block(15 if (seat + car) % 4 else 38, (seat - 2.5) * .52,
                               side * .522, 5.04, (.40, .032, .39))
        if car in (0, 4):
            direction = -1 if car == 0 else 1
            # Set the windscreen directly onto the raked nose; its longitudinal
            # sections track the same roof profile as the shell underneath.
            loft(car_part, 38, [[(direction * u, direction * v, z + dz) for v,dz in
                          [(-w, 0), (w, 0), (w, .034), (-w, .034)]]
                         for u,w,z in [(1.65, .32, 5.52), (1.96, .23, 5.19)]])
            for side in [-1, 1]:
                carriage_block(16, direction * 2.056, side * .20, 4.72, (.025, .16, .055))
        if car < 4:
            carriage_block(38, 2.125, 0.0, 4.94, (.66, .74, .91))

    # A shallow platform shares the viaduct structure and faces the existing
    # north-bank shopping arcade. Sawtooth canopies alternate with open bays.
    block(2, 2, (-9.3, 42.0, 4.10), (18.0, 2.12, .38))
    for i in range(5):
        x = -17.0 + 3.72 * i
        block(2, 0, (x, 42.7, 4.85), (.17, .18, 3.02))
        beam(2, 11, (x, 42.77, 6.14), (x, 40.98, 6.46), .15, 3.28)
        block(2, 15, (x, 41.60, 6.23), (2.72, .13, .09))
    block(2, 13, (-.9, 43.8, 2.04), (2.6, 2.4, 5.30))
    block(2, 0, (-.9, 42.72, 4.18), (2.5, 1.52, .28))

    # Recessed shops face the water. Upper apartments step away from the
    # awnings, leaving terraces and rooftop services visible from the aircraft.
    for i, (cx, cy, width, a) in enumerate([
        (-35.5,-3.7,10.3,math.pi/2), (-35.8,8.5,11.2,math.pi/2),
        (-35.7,18.0,6.1,math.pi/2), (-61.5,46.0,15.4,0), (-47.0,46.5,11.8,0),
    ]):
        ca, sa = math.cos(a), math.sin(a)
        def shop(mat, u, v, z, size):
            block(2, mat, (cx + u*ca - v*sa, cy + u*sa + v*ca, z), size, a)
        h = [4.3, 5.8, 3.5, 6.3, 4.9][i]
        shop(2, 0, 0, .60, (width, 4.5, 2.55))
        shop(13 if i % 2 else 14, -.36, .5, 1.8+h/2, (width*.82, 2.9, h))
        shop(2, -.36, .5, 1.8+h, (width*.87, 3.16, .22))
        shop(0, width*.19, .8, 2.13+h, (width*.20, 1.46, .57))
        for j in range(5):
            u = width * (j - 2) * .185
            shop(15 if (i+j)%3 else 38, u, -2.27, .87, (width*.15, .12, .95))
            shop(11, u, -2.56, 1.56 + (j%2)*.18, (width*.183, .87, .15))
            shop(2, u, -.99, 2.20 + h*.44, (width*.16, .55, .14))

    # Market lanes between the north-bank podiums and on the narrow east bank.
    # The latter address the canal; their sawtooth roofs are visible over the rail.
    for row, (ox, oy, width, count, angle) in enumerate([
        (-22.35,44.45,2.1,2,0), (-3.8,44.55,2.2,2,0),
        (17.05,44.45,2.7,2,0), (35.85,44.85,2.2,2,0),
        (34.3,1.7,3.2,4,-math.pi/2), (34.3,19.8,2.45,5,-math.pi/2),
        (36.0,33.4,2.55,4,-math.pi/2),
    ]):
        ca, sa = math.cos(angle), math.sin(angle)
        def store_point(u, v, z):
            return ox + ca*u - sa*v, oy + sa*u + ca*v, z
        def store(mat, u, v, z, dimensions):
            block(2, mat, store_point(u, v, z), dimensions, angle)
        for i in range(count):
            # Share party walls but leave one break for the service entrance.
            u = (i - (count - 1)*.5)*width
            h = 2.15 + .25*((row*2+i)%4)
            length = 3.2 + .32*((row+i)%3)
            store(2,u,length*.5,h*.5-.56, (width-.035,length,h))
            store(13,u,-.025,.32, (width*.86,.09,1.39))
            for side in (-1,1):
                store(15 if (row+i+(side==1))%4 else 38, u+side*width*.22,-.085,.46,
                      (width*.30,.075,.72))
            store(11,u,-.33,1.08, (width*.96,.72,.14))
            store(6 if (row+i)%7==0 else 15,u,-.70,1.08,(width*.72,.06,.075))
            # Clerestory glass under a shed roof, rising toward the party wall.
            x0, x1 = u-width*.47, u+width*.47
            loft(2,11, [[store_point(x,v,z) for x,z in
                         [(x0,h-.56),(x0,h-.41),(x1,h+.10),(x1,h-.56)]]
                        for v in (.08,length-.08)])
            store(15,u+width*.46,length*.52,h-.26,(.08,length*.72,.25))
            if (row+i)%3==0:
                store(38,u-width*.36,-.25,1.9,(width*.22,.20,1.37))
                for j in range(3):
                    store(6 if row%3==0 else 16,u-width*.36,-.37,1.50+j*.32,
                          (width*(.13 if j!=1 else .075),.065,.12))

    # Two skewed glazed legs form a narrow portal between the old west campuses.
    # Its middle is open to the sky; the bridges and braced edges cast real shadows.
    tower_y = 17.45
    def tower_x(side, z):
        return -45.2 + side * (4.15 - z * .068) - z * .115
    for side, height, mat in [(-1, 18.8, 5), (1, 21.2, 14)]:
        levels = [-.68, 4.2, 10.0, 15.5, height]
        def tower_ring(z):
            x = tower_x(side,z)
            hw = 1.40 - z * .012
            return [(x+dx,tower_y+dy,z) for dx,dy in
                    [(-hw,-2.08), (hw-.32,-2.08), (hw,-1.75), (hw,2.08), (-hw,2.08)]]
        loft(3, mat, [tower_ring(z) for z in levels])
        for front in [-2.13, 2.13]:
            for j in range(len(levels)-1):
                z0, z1 = levels[j:j+2]
                off = 1.13 if j%2 else -1.13
                beam(3, 11, (tower_x(side,z0)+off,tower_y+front,z0),
                     (tower_x(side,z1)-off,tower_y+front,z1), .27, .20)
        x = tower_x(side,height)
        block(3, 0, (x,tower_y,height+.18), (2.60,4.32,.36))
    for z in [8.5, 16.7]:
        left, right = tower_x(-1,z), tower_x(1,z)
        block(3,13, ((left+right)/2,tower_y,z), (right-left,2.48,1.50))
        block(3,11, ((left+right)/2,tower_y,z-.76), (right-left,2.66,.18))
        block(3,15, ((left+right)/2,tower_y-1.27,z-.60), ((right-left)*.75,.07,.085))

    # Two backlit quay numbers provide wayfinding at different scales. Individual
    # strokes leave the dark enamel backing visible in daylight and at night.
    digits = {'0': 'abcedf', '2': 'abged', '5': 'afgcd'}
    strokes = {'a':(0,1,1,.15),'g':(0,0,1,.15),'d':(0,-1,1,.15),
               'f':(-.48,.50,.15,.86),'b':(.48,.50,.15,.86),
               'e':(-.48,-.50,.15,.86),'c':(.48,-.50,.15,.86)}
    for part, x, y, z, scale, number, mat in [
        (3,tower_x(1,10.4),tower_y-2.17,10.4,1.02,'05',6), (2,-11.0,40.91,5.59,.37,'02',16)
    ]:
        block(part, 38, (x,y,z), (2.88*scale,.18,2.62*scale))
        for j, digit in enumerate(number):
            for segment in digits[digit]:
                px, pz, w, h = strokes[segment]
                block(part, mat, (x+(px+(j-.5)*1.36)*scale, y-.104, z+pz*scale),
                      (w*scale,.035,h*scale))

    for (part, mat), (vertices, faces) in sorted(batches.items()):
        mesh('waterfront_%d_%d' % (part, mat), vertices, faces, mat, part,
             rough=.38 if mat == 2 else .23)


def shenzhen_aircraft():
    # Loft a closed hull with an inset cockpit deck. The canopy, instrument
    # binnacle and seats occupy the space above that deck.
    ring_count, ring_size = 44, 48
    verts = [(0.0, -2.95, 0.045)]
    for k in range(1, ring_count):
        angle = math.pi * k / ring_count
        along = 0.5 - 0.5 * math.cos(angle)
        y = -2.95 + 6.15 * along
        width = math.sin(angle) * (.90 - .29 * along)
        height = width * .66
        center_z = .045 + .16 * along * along
        deck = min(1.0, max(0.0, (y + 2.64) / .55), max(0.0, (.97 - y) / .50))
        deck = deck * deck * (3.0 - 2.0 * deck)
        for j in range(ring_size):
            phi = TAU * j / ring_size
            s = math.sin(phi)
            z = height * (s - .90 * deck * max(s, 0.0) ** 2)
            verts.append((width * math.cos(phi), y, center_z + z))
    verts.append((0.0, 3.20, .205))
    faces = []
    for j in range(ring_size):
        nj = (j + 1) % ring_size
        faces.append((0, 1 + j, 1 + nj))
        for k in range(ring_count - 2):
            a = 1 + k * ring_size
            faces.append((a + nj, a + j, a + j + ring_size, a + nj + ring_size))
        a = 1 + (ring_count - 2) * ring_size
        faces.append((a + nj, a + j, len(verts) - 1))
    mesh('evtol_pressure_hull', verts, faces, 11, part=100, smooth=True, smooth_angle=160, rough=.12)

    def glazing(u, v):
        bulge = math.sin(math.pi * u) ** .68
        half_width = .045 + .62 * bulge
        arch = .5 * math.pi * v
        pos = (half_width * math.sin(arch), -2.47 + 3.32*u,
               .14 + .05*bulge + (.025 + .52*bulge)*math.cos(arch))
        return pos, .012

    closed_blade('panoramic_canopy', 34, 24, glazing, 18, 100, rough=.035)
    for side in (-1, 1):
        sill = [glazing(k/32, side)[0] for k in range(33)]
        closed_tube('canopy_sill', sill, [.028]*33, 37, 100, n_ring=10)
        stripe = [(x*1.012, y, z - .019) for x,y,z in sill]
        closed_tube('sill_trim', stripe, [.011]*33, 8, 100, n_ring=8)
        seat_x = side * .235
        box('seat_cushion', (seat_x, -.40, .24), (.32, .47, .16), 37, 100, bevel=.052)
        box('seat_back', (seat_x, -.15, .36), (.33, .14, .29), 37, 100, bevel=.050, rot_euler=(-.19, 0, 0))
        box('seat_headrest', (seat_x, -.10, .51), (.19, .12, .10), 37, 100, bevel=.033)
        box('instrument_binnacle', (seat_x, -1.39, .255), (.38, .29, .13), 37, 100, bevel=.036)
        box('instrument_display', (seat_x, -1.36, .326), (.22, .12, .010), 6, 100, bevel=.003)
        box('instrument_status', (seat_x, -1.48, .325), (.16, .022, .010), 16, 100, bevel=.003)
        sphere('nose_lamp_recess', (side*.22, -2.70, .072), (.105, .14, .064), 37, 100, seg=16, rings=10)
        sphere('nose_lamp', (side*.22, -2.765, .084), (.078, .10, .043), 16, 100, seg=16, rings=10)
    for u, width in ((.53, .023), (.86, .033)):
        bow = [glazing(u, k/12 - 1)[0] for k in range(25)]
        closed_tube('canopy_bow', bow, [width]*25, 38, 100, n_ring=10)

    # Cambered lifting surfaces with a broad swept root and a rising tip.
    # Use the same opposing rotors and pivot locations as the runtime rig.
    for sign_w in [-1.0, 1.0]:
        def wing(u, v):
            chord = .37 + .52*(1-u) + .40*(1-u)**4
            along = .5 + .5*v
            camber = math.sin(math.pi * along) * (1.12 - .40*along)
            pos = (sign_w*(.43 + 3.70*u), -.35 + .66*u + v*.5*chord,
                   .16 + .20*u + .055*chord*camber + .08*u**5)
            return pos, .012 + .115*chord*camber
        closed_blade('cambered_wing', 20, 14, wing, 11, 100, rough=.16)
        rod('wing_root_fairing', (sign_w*.52, -.28, .19), (sign_w*1.15, -.18, .23),
            .13, 11, part=100, verts=20, rough=.15, bevel=.04)
        rod('evtol_boom_rail', (sign_w * 3.00, -1.65, 0.36), (sign_w * 2.65, 1.95, 0.46), .075, 37, part=100, verts=16, rough=.18, preserve_uv=True)
        rod('evtol_boom_brace', (sign_w * 0.62, -0.95, 0.12), (sign_w * 3.00, -1.55, 0.34), .055, 37, part=100, verts=12, rough=.18, preserve_uv=True)
        def tail(u, v):
            chord = 1.12 - .79*u
            return (sign_w*(.22 + .84*u), 2.16 + .77*u + v*chord*.5,
                    .24 + .92*u), .014 + .105*chord*(1-v*v)
        closed_blade('swept_v_tail', 10, 8, tail, 11, 100, rough=.14)
        rod('evtol_skid_tube', (sign_w * 0.78, -1.45, -0.66), (sign_w * 0.78, 1.25, -0.66), .044, 37, part=100, verts=14, rough=.20, preserve_uv=True)
        for sy_s in [-0.88, 0.72]:
            rod('evtol_skid_strut', (sign_w * 0.48, sy_s, -0.16), (sign_w * 0.78, sy_s, -0.66), .036, 23, part=100, verts=12, rough=.20)

    # Six articulated tilt-rotor nacelles (110..115) & counter-rotating carbon-titanium scimitar rotors (120..125)
    rotor_pivots = [
        (-3.05, -1.65, 0.36),
        ( 3.05, -1.65, 0.36),
        (-4.10,  0.15, 0.40),
        ( 4.10,  0.15, 0.40),
        (-2.60,  1.95, 0.46),
        ( 2.60,  1.95, 0.46),
    ]
    for i, (bx, by, bz) in enumerate(rotor_pivots):
        sphere('tilt_nacelle_%d' % i, (bx, by, bz), (.205, .38, .21), 11, part=110 + i, seg=24, rings=14, rough=.13)
        rod('tilt_axle', (bx-.24, by, bz), (bx+.24, by, bz), .092, 23, 110+i, verts=20, bevel=.012)
        for side in (-1, 1):
            x = bx + side*.191
            rod('tilt_fork', (x, by+.20, bz-.055), (x, by, bz), .046, 37, 100, verts=12)
            sphere('axle_cap', (bx + side*.246, by, bz), (.013,.058,.058), 8, 110+i, seg=14, rings=8)
        torus('mast_collar', (bx, by, bz+.18), .089, .021, 23, 110+i, seg=22, minor_seg=8)
        rod('rotor_mast_%d' % i, (bx, by, bz), (bx, by, bz + 0.32), .058, 23, part=110 + i, verts=16, rough=.19)
        sphere('rotor_spinner_%d' % i, (bx, by, bz + 0.36), (.10, .10, .13), 37, part=120 + i, seg=18, rings=10, rough=.14)
        lamp_mat = 16 if (i < 2) else 6
        sphere('nav_strobe_%d' % i, (bx * 1.02, by, bz - 0.12), (.05, .05, .05), lamp_mat, part=110 + i, seg=10, rings=6, rough=.15)
        # 3 pitched twill-carbon aerofoil blades + brushed-titanium leading edge + safety-gold tip cap
        rot_dir = -1.0 if (i % 2 == 1) else 1.0
        for j in range(3):
            ang = j * TAU / 3.0
            for u0, u1, mat, steps in ((0., .84, 37, 9), (.84, 1., 8, 4)):
                def blade(s, v):
                    u = u0 + (u1-u0)*s
                    radius = .074 + .92*u
                    chord = .065 + .13*math.sin(math.pi*(.10 + .87*u))
                    pitch = rot_dir*(.36 - .20*u)
                    sweep = -rot_dir*(.145*u*u + .044*u**5)
                    transverse = sweep + .5*v*chord*math.cos(pitch)
                    pos = (bx + radius*math.cos(ang) - transverse*math.sin(ang),
                           by + radius*math.sin(ang) + transverse*math.cos(ang),
                           bz + .32 + .035*u*u + .5*v*chord*math.sin(pitch))
                    return pos, .007 + .11*chord*(1-v*v)
                closed_blade('scimitar_blade', steps, 5, blade, mat, 120+i, rough=.19)

def taipei():
    # Meters, retained at 508 m in the asset. Runtime places tower at world (18.5, 0.0, 60.0) with scale 0.080.
    # Spatial part IDs (0..5) partition Xinyi into disjoint BLAS bounding volumes for fast DXR traversal.
    def w2b(wx, wy, wz):
        return ((wx - 18.5) / .08, (wz - 60.0) / .08, wy / .08)
    def ws(sx, sy, sz):
        return (sx / .08, sz / .08, sy / .08)
    # Part 0: Taipei 101 Hero Tower (0..508 m true vertical scale) + Shopping Mall Podium
    notched('battered_podium', 0, 112, 38, 29, 5, part=0)
    for k in range(8):
        z = 112 + k * 35
        w = 23.0 - k * .22
        top = w + 35 * math.tan(math.radians(7))
        notched('bamboo_module_%d' % k, z, z + 33, w, top, 5, part=0, rough=.16)
        notched('ornamental_setback', z + 33, z + 35, top + 1.3, w * .98, 8, part=0, rough=.24)
        notched('module_crown_glow', z + 32.4, z + 33.0, top + .28, top + .28, 6, part=0, rough=.2)
        for tier in range(1, 8):
            q = tier / 8
            zz = z + 33 * q
            ww = w + (top - w) * q
            notched('horizontal_mullion', zz, zz + .40, ww + .18, ww + .18, 0, part=0, rough=.28)
        for face in range(4):
            a = face * TAU / 4
            o = torus('ruyi_coin', (math.cos(a) * (top + .45), math.sin(a) * (top + .45), z + 29), 2.1, .34, 8, part=0, seg=24, rough=.20)
            o.rotation_euler = (math.pi / 2, 0, a + math.pi / 2)
    for f in range(4):
        a = f * TAU / 4
        o = torus('podium_coin', (math.cos(a) * 32, math.sin(a) * 32, 70), 5.0, .68, 8, part=0, seg=32, rough=.20)
        o.rotation_euler = (math.pi / 2, 0, a + math.pi / 2)

    # The observation gallery is enclosed by low-iron glass, vertical pressure
    # plates and spandrels. Its internal structure frames the 5.5 m diameter TMD.
    notched('observatory_deck_base', 392, 396.0, 20.4, 20.4, 5, part=0, rough=.20)
    notched('observatory_upper_crown', 427.2, 430.0, 17.4, 16.2, 5, part=0, rough=.20)
    torus('observatory_gallery_ring', (0, 0, 397.2), 8.2, .45, 15, part=0, seg=36, rough=.24)
    torus('observatory_oculus_ring', (0, 0, 426.4), 5.6, .38, 15, part=0, seg=36, rough=.24)
    for x in [-17.4, 17.4]:
        for y in [-17.4, 17.4]:
            rod('observatory_column', (x, y, 392), (x, y, 430), .82, 0, part=0, rough=.24)
            rod('observatory_corner_accent', (x * .94, y * .94, 396), (x * .94, y * .94, 427), .22, 15, part=0, rough=.22)
    for z in [394.0, 427.6]:
        for y in [-17.4, 17.4]:
            rod('observatory_beam', (-17.4, y, z), (17.4, y, z), .58, 0, part=0, rough=.24)
        for x in [-17.4, 17.4]:
            rod('observatory_beam', (x, -17.4, z), (x, 17.4, z), .58, 0, part=0, rough=.24)
    for face in range(4):
        a = math.pi*.5*face
        ca,sa = math.cos(a),math.sin(a)
        def facade_piece(name, along, offset, z, width, depth, height, mat):
            box(name, (ca*along-sa*offset,sa*along+ca*offset,z),
                (width,depth,height), mat, part=0, rot_z=a, rough=.12 if mat==40 else .28)
        facade_piece('observatory_clear_glass',0,-17.36,411.6,34.72,.075,31.2,40)
        for u in (-14.2,-8.5,-2.84,2.84,8.5,14.2):
            facade_piece('observatory_window_cap',u,-17.46,411.6,.23,.25,31.2,11)
        for z in (396.6,400.8,405.0,413.6,417.9,422.2,426.5):
            facade_piece('observatory_spandrel',0,-17.46,z,34.84,.29,.50,5)

    sphere('660_tonne_TMD', (0, 0, 409.5), (2.75, 2.75, 2.75), 8, part=0, seg=64, rings=32, rough=.12)
    for j in range(1, 41):
        dz = -2.75 + 5.5 * j / 41
        radius = math.sqrt(max(.01, 2.75 ** 2 - dz ** 2))
        torus('steel_plate_ridge', (0, 0, 409.5 + dz), radius, .035, 8, part=0, seg=48, rough=.15)
    for j in range(8):
        a = j * TAU / 8
        rod('TMD_suspension', (3.1 * math.cos(a), 3.1 * math.sin(a), 426.8), (2.25 * math.cos(a), 2.25 * math.sin(a), 409.0), .055, 8, part=0, rough=.18)
        rod('hydraulic_bumper', (4.8 * math.cos(a), 4.8 * math.sin(a), 404.8), (2.5 * math.cos(a), 2.5 * math.sin(a), 406.2), .16, 0, part=0, rough=.25)
        if j % 2 == 0:
            rod('bumper_stanchion', (4.0 * math.cos(a), 4.0 * math.sin(a), 395.8), (4.0 * math.cos(a), 4.0 * math.sin(a), 405.2), .22, 0, part=0, rough=.26)
    torus('bumper_ring', (0, 0, 405.2), 4.0, .24, 8, part=0, rough=.16)

    # Surveyed roof at 448 m; the architectural spire extends a further 60 m.
    notched('upper_occupied_storeys',430,439,11.0,10.6,5,part=0,rough=.18)
    notched('mechanical_crown',439,448,6.55,6.30,11,part=0,rough=.25)
    notched('roof_collar',447.4,448,6.39,6.39,15,part=0,rough=.22)
    for z0,z1,radius in [(448,462,1.78),(462,478,1.24),(478,493,.83),(493,507.55,.55)]:
        rod('spire_mast', (0,0,z0), (0,0,z1), radius, 11, part=0, rough=.20)
    sphere('spire_beacon', (0,0,507.52), (.48,.48,.48), 16, part=0, seg=12, rings=6, rough=.15)

    # Xinyi streets and buildings are measured in this asset coordinate frame.
    taipei_city_mesh(mesh, taipei_relief_height)
    box('xinyi_urban_plain', (0,0,-2.6), (4040,4040,5.2), 2, part=6, rough=.68)
    taipei_relief_mesh(mesh)

    # Part 1: Foreground Elevated Sanctuary Rooftop Rain-Terrace (zero tall columns, clean infinity coping)
    box('sanctuary_rooftop_mass', w2b(0.0, 1.26, 3.2), ws(16.0, 2.52, 11.6), 2, part=1, bevel=.25, rough=.45)
    box('sanctuary_wet_slate_deck', w2b(0.0, 2.57, 3.2), ws(15.6, .12, 11.2), 1, part=1, bevel=.08, rough=.09)
    box('sanctuary_reflecting_pool', w2b(0.2, 2.64, 5.35), ws(11.4, .05, 5.8), 7, part=1, rough=.04)
    box('sanctuary_north_coping', w2b(0.0, 2.71, 8.55), ws(15.4, .18, .36), 0, part=1, bevel=.04, rough=.24)
    box('sanctuary_west_coping', w2b(-7.55, 2.73, 3.2), ws(.36, .22, 11.0), 0, part=1, bevel=.04, rough=.24)
    box('sanctuary_east_coping', w2b(7.55, 2.73, 3.2), ws(.36, .22, 11.0), 0, part=1, bevel=.04, rough=.24)
    box('sanctuary_coping_grazer', w2b(0.0, 2.65, 8.32), ws(10.6, .035, .04), 16, part=1, rough=.2)
    for side in [-1, 1]:
        px = side * 5.6
        box('sanctuary_planter_box', w2b(px, 2.74, 7.3), ws(2.4, .26, 1.5), 24, part=1, bevel=.45, rough=.52)
        box('sanctuary_planter_rim', w2b(px, 2.88, 7.3), ws(2.48, .035, 1.58), 31, part=1, bevel=.25, rough=.24)
        box('sanctuary_planter_soil', w2b(px, 2.85, 7.3), ws(2.22, .04, 1.32), 39, part=1, rough=.84)
        import_cc0_plant('sanctuary_fern_%d' % (1 if side > 0 else 0), 'fern_02', None,
                         w2b(px - 0.35 * side, 2.86, 7.28), scale=10.5, rot_z=0.45 * side, mat=36, part=1, rough=.38, thickness=.010)
        import_cc0_plant('sanctuary_calathea_%d' % (1 if side > 0 else 0), 'calathea_orbifolia_01', ['calathea_orbifolia_01_d'],
                         w2b(px + 0.42 * side, 2.86, 7.32), scale=11.0, rot_z=-0.85 * side, mat=29, part=1, rough=.36, thickness=.010)
    box('orchid_altar_base', w2b(0.0, 2.74, 5.86), ws(1.48, .22, 1.48), 38, part=1, bevel=.45, rough=.22)
    box('orchid_altar_plinth', w2b(0.0, 2.94, 5.86), ws(.82, .26, .82), 31, part=1, bevel=.35, rough=.20)
    torus('orchid_brass_collar', w2b(0.0, 3.08, 5.86), .24 / .08, .045 / .08, 8, part=1, seg=40, rough=.16)

def orchid():
    """Botanical Phalaenopsis crystal glass sculpture (~34K triangles, 0 nonmanifold edges).
    Blender (+X right, -Y toward camera, +Z up) -> Engine (+X right, +Y up, +Z away from camera).
    """
    # 1. Posterior Whorl: Three narrower elliptic-lanceolate keeled sepals (mat = 4, part = 10..12)
    def make_sepal(name, angle, length, half_w, part_id, falcate=0.0):
        ca, sa = math.cos(angle), math.sin(angle)
        def surf(u, v):
            # Elliptic-lanceolate taper with pointed-acute botanical apex
            env = (math.sin(math.pi * u) ** 0.78) * (1.0 - 0.22 * (u ** 1.6)) + 0.045 * (1.0 - u)
            lat = half_w * env * v
            rad = 0.08 + length * u
            curve_down = falcate * (u ** 1.8)
            x = ca * rad - sa * lat
            z = sa * rad + ca * lat - curve_down
            # Seated posteriorly (+Y) behind petals, with crisp dorsal midrib keel and gentle hooding
            keel = 0.036 * math.exp(-14.0 * v * v) * math.sin(math.pi * u)
            cup = -0.075 * (v * v) * math.sin(math.pi * u)
            arch = 0.11 + 0.08 * math.sin(math.pi * u) - 0.16 * (u ** 2.1)
            y = arch + keel + cup
            th = 0.015 + 0.042 * math.exp(-8.0 * v * v) * (1.0 - 0.55 * u) + 0.010 * (1.0 - u)
            return (x, y, z), th
        return closed_blade(name, 34, 22, surf, 4, part_id, rough=.06)

    make_sepal('dorsal_sepal', math.pi * 0.5, 1.64, 0.45, 10, falcate=0.0)
    make_sepal('left_lateral_sepal', math.pi + 0.72, 1.50, 0.37, 11, falcate=0.10)
    make_sepal('right_lateral_sepal', -0.72, 1.50, 0.37, 12, falcate=0.10)

    # 2. Anterior Whorl: Two broad clawed rhomboid-obovate butterfly-wing petals (mat = 3, part = 13..14)
    def make_wing_petal(name, angle, sign_lr, part_id):
        ca, sa = math.cos(angle), math.sin(angle)
        def surf(u, v):
            # Slender basal claw (u < 0.16) -> wide rhomboid-diamond wing -> softly cuspidate lateral tip
            claw_flare = 1.0 / (1.0 + math.exp(-(u - 0.15) * 19.0))
            rhomb = (math.sin(math.pi * (u ** 0.86)) ** 0.76) * (1.0 - 0.14 * (u ** 2.4))
            cusp = 1.0 - 0.08 * (u ** 3.0) * (1.0 - abs(v))
            env = (0.075 * (1.0 - claw_flare) + 1.04 * claw_flare * rhomb) * cusp
            asym = 1.0 + 0.13 * sign_lr * v * math.sin(math.pi * u)
            lat = 0.88 * env * asym * v
            rad = 0.06 + 1.76 * u
            x = ca * rad - sa * lat
            z = sa * rad + ca * lat + 0.05 * math.sin(math.pi * u)
            # Smooth hand-blown Liuli crystal compound saucer curvature + dorsal midrib ridge + rolled margin
            saucer = -0.15 * (v * v) * claw_flare * math.sin(math.pi * u * 0.92)
            rib_ridge = 0.024 * math.exp(-14.0 * v * v) * (1.0 - 0.45 * u)
            margin_roll = 0.022 * (abs(v) ** 3.2) * claw_flare * math.sin(math.pi * u)
            forward_arc = -0.02 - 0.22 * math.sin(math.pi * 0.72 * u) + 0.05 * (u ** 2.4)
            y = forward_arc + saucer + rib_ridge + margin_roll
            th = 0.013 + 0.046 * math.exp(-10.0 * v * v) * (1.0 - 0.60 * u) + 0.018 * (1.0 - claw_flare)
            return (x, y, z), th
        return closed_blade(name, 40, 30, surf, 3, part_id, rough=.04)

    make_wing_petal('left_wing_petal', math.pi - 0.11, 1.0, 13)
    make_wing_petal('right_wing_petal', 0.11, -1.0, 14)

    # 3. Complex Trilobed Labellum (part = 15): Hastate mid-lobe + 2 cupped erect throat lobes + twin cirri + gold callus
    def labellum_mid_surf(u, v):
        # Waisted isthmus -> flared hastate triangular-ovate blade -> cleft apex
        isthmus = 0.26 + 0.74 / (1.0 + math.exp(-(u - 0.24) * 16.0))
        taper = max(0.0, math.sin(math.pi * min(1.0, u * 0.96 + 0.02))) ** 0.68
        half_w = 0.34 * isthmus * taper
        x = half_w * v
        # Projects forward (-Y) and gracefully downward (-Z), curling slightly up at the very tip
        y = -0.14 - 0.48 * (u ** 0.92)
        z = -0.10 - 0.58 * u + 0.14 * (u ** 2.8) + 0.09 * (v * v) * math.sin(math.pi * u)
        keel = -0.028 * math.exp(-16.0 * v * v) * math.sin(math.pi * u)
        th = 0.016 + 0.030 * math.exp(-9.0 * v * v) * (1.0 - 0.4 * u)
        return (x, y + keel, z), th

    closed_blade('labellum_midlobe', 32, 22, labellum_mid_surf, 9, 15, rough=.12)

    for sign_s, tag in [(-1.0, 'left'), (1.0, 'right')]:
        # Erect, inward-cupped lateral chalice lobes framing the throat
        def side_lobe_surf(u, v, s=sign_s):
            env = (math.sin(math.pi * u) ** 0.65) * (0.85 + 0.25 * u) + 0.08 * (1.0 - u)
            w = 0.19 * env * v
            # Rises outward and upward from throat bridge, cupping inward at apex
            reach = u
            x = s * (0.07 + 0.23 * math.sin(math.pi * 0.68 * reach) - 0.05 * (reach ** 2.2)) + 0.03 * w
            y = -0.13 - 0.24 * reach + w * 0.85
            z = -0.13 + 0.30 * (reach ** 1.15) - 0.06 * (v * v)
            th = 0.015 + 0.020 * (1.0 - 0.6 * u) * (1.0 - 0.4 * v * v)
            return (x, y, z), th
        closed_blade('labellum_lateral_%s' % tag, 24, 16, side_lobe_surf, 9, 15, rough=.11)

        # Delicate upturned apical cirrus tendril curling from the cleft mid-lobe apex
        cirrus_pts = []
        cirrus_rad = []
        for k in range(13):
            t_c = k / 12.0
            cx = sign_s * (0.022 + 0.13 * (t_c ** 1.35))
            cy = -0.60 - 0.14 * math.sin(math.pi * 0.65 * t_c)
            cz = -0.54 + 0.22 * (t_c ** 1.7)
            cirrus_pts.append((cx, cy, cz))
            cirrus_rad.append(0.018 * (1.0 - 0.76 * t_c) + 0.003)
        closed_tube('labellum_cirrus_%s' % tag, cirrus_pts, cirrus_rad, 9, 15, n_ring=10, rough=.10, thick=.015)

    # Jewel-scale bilobed 24k gold callus crest nestled inside the throat (mat = 8, part = 15)
    def callus_surf(u, v):
        env = math.sin(math.pi * u) ** 0.65 + 0.12 * (1.0 - u)
        x = 0.078 * env * v
        # Central longitudinal furrow creating authentic bilobed saddle
        ridge = 0.032 * (math.sin(abs(v) * math.pi) ** 1.2) * math.sin(math.pi * u)
        y = -0.17 - 0.12 * u - ridge
        z = -0.11 - 0.05 * u + ridge * 0.85
        th = 0.022 + 0.024 * math.sin(math.pi * u)
        return (x, y, z), th

    closed_blade('gold_callus_crest', 16, 12, callus_surf, 8, 15, rough=.16)

    # 4. Compact Arched Gynostemium Column & Beaked Porcelain Anther Cap (part = 16)
    col_pts = []
    col_rad = []
    for k in range(15):
        t_g = k / 14.0
        col_pts.append((0.0, 0.05 - 0.29 * t_g, 0.11 - 0.05 * (t_g ** 1.8)))
        col_rad.append(0.072 * (0.85 + 0.22 * math.sin(math.pi * t_g)))
    closed_tube('gynostemium_column', col_pts, col_rad, 4, 16, n_ring=16, rough=.08, thick=.075)

    def anther_surf(u, v):
        env = math.sin(math.pi * u) ** 0.62
        x = 0.068 * env * v
        y = -0.21 - 0.095 * u
        z = 0.075 - 0.055 * (u ** 1.4) - 0.032 * (v * v)
        return (x, y, z), 0.024

    closed_blade('anther_cap', 14, 12, anther_surf, 11, 16, rough=.16)

    # 5. Continuous S-Arched Inflorescence Stem (Peduncle), Clasping Bracts & 3 Pointed 3-Keeled Crystal Buds (part = 0)
    def stem_pos(t_s):
        # Anchors cleanly inside Sanctuary Terrace brass collar at t_s=0 -> curves gracefully to floral receptacle at t_s=1
        sx = 0.03 * (1.0 - t_s) - 0.22 * math.sin(math.pi * (t_s ** 0.88))
        sy = 0.34 + 0.18 * math.sin(math.pi * t_s) - 0.20 * (t_s ** 2.2)
        sz = -2.68 + 2.72 * (t_s ** 0.94)
        return (sx, sy, sz)

    n_stem = 36
    s_pts = [stem_pos(i / (n_stem - 1)) for i in range(n_stem)]
    s_rad = [0.044 - 0.015 * (i / (n_stem - 1)) for i in range(n_stem)]
    closed_tube('arched_peduncle', s_pts, s_rad, 10, 0, n_ring=14, rough=.38, thick=.05)

    # Three natural alternating nodes with clasping botanical bract, curved ascending pedicel & pointed 3-keeled crystal bud
    bud_specs = [
        (0.42, -1.0, 0.26, 0.29),
        (0.58,  1.0, 0.23, 0.25),
        (0.73, -1.0, 0.19, 0.21),
    ]
    for b_idx, (t_node, side_b, ped_len, bud_len) in enumerate(bud_specs):
        nx, ny, nz = stem_pos(t_node)
        # Small clasping bract scale at node
        def bract_surf(u, v, ox=nx, oy=ny, oz=nz, sb=side_b):
            env = (1.0 - u) * 0.8 + 0.2 * math.sin(math.pi * u)
            bx = ox + sb * (0.030 + 0.042 * u) + 0.015 * v * env
            by = oy + 0.028 * v * env
            bz = oz + 0.075 * u
            return (bx, by, bz), 0.010
        closed_blade('node_bract_%d' % b_idx, 8, 6, bract_surf, 10, 0, rough=.45)

        # Gracefully curved ascending/drooping pedicel
        ped_pts = []
        ped_rad = []
        for k in range(10):
            tp = k / 9.0
            px = nx + side_b * ped_len * (tp ** 0.92)
            py = ny - 0.10 * (tp ** 1.3)
            pz = nz + 0.15 * math.sin(math.pi * 0.72 * tp) - 0.05 * (tp ** 2.2)
            ped_pts.append((px, py, pz))
            ped_rad.append(0.019 - 0.006 * tp)
        closed_tube('bud_pedicel_%d' % b_idx, ped_pts, ped_rad, 10, 0, n_ring=10, rough=.36, thick=.025)

        # Pointed 3-keeled ovoid-conical crystal bud tilted naturally along the pedicel tip
        bx0, by0, bz0 = ped_pts[-1]
        dir_v = (Vector(ped_pts[-1]) - Vector(ped_pts[-3])).normalized()
        # Gently nod bud downward & forward like a real Phalaenopsis bud
        bud_axis = (dir_v * 0.72 + Vector((0.0, -0.32, -0.45))).normalized()
        bud_pts = []
        bud_rad = []
        n_bud = 22
        for k in range(n_bud):
            tb = k / (n_bud - 1)
            bp = Vector((bx0, by0, bz0)) + bud_axis * (bud_len * tb)
            # Plump rounded base tapering to a crisp pointed sepal apex
            prof = (math.sin(math.pi * tb) ** 0.62) * (1.0 - 0.42 * (tb ** 1.5))
            bud_pts.append((bp.x, bp.y, bp.z))
            bud_rad.append(max(0.004, bud_len * 0.36 * prof))
        closed_tube('crystal_bud_%d' % b_idx, bud_pts, bud_rad, 4, 0, n_ring=24, rough=.055, thick=.055, flute_amp=0.075, flute_freq=3)

def vessel(name, center, profile, mat, part, scale_xy=(1., 1.), rough=.24):
    """Closed lathed cross-section: real inner wall, rolled lip and underside, no pole degeneracy."""
    cx, cy, cz = center
    n = 48
    verts = [(cx + r * math.cos(j * TAU / n) * scale_xy[0],
              cy + r * math.sin(j * TAU / n) * scale_xy[1], cz + h)
             for r, h in profile for j in range(n)]
    faces = []
    for k in range(len(profile)):
        nk = (k + 1) % len(profile)
        for j in range(n):
            nj = (j + 1) % n
            faces.append((k*n+j, k*n+nj, nk*n+nj, nk*n+j))
    return mesh(name, verts, faces, mat, part, smooth=True, rough=rough, thick=.08)

def enamel_cathedral():
    """Architectural mineral vault, bronze-framed obsidian optical stelae & woven neural canopy; engine parts 200..219."""
    # World coordinates after export: ground y_eng = z_bl = 2.65, orchid at (0, 5.85, 5.5).
    for k, a in enumerate((-2.28, -1.68, -1.08, -.46, .46, 1.08, 1.68, 2.28)):
        x, y = 4.45 * math.sin(a), 5.5 + 4.45 * math.cos(a)
        part = 200 + k
        # Stepped Honed Calacatta Marble & Brushed Champagne-Nickel architectural plinth
        box('stela_marble_base', (x, y, 2.73), (.88, .92, .16), 31, part, bevel=.018, rough=.22, rot_z=-a)
        box('titanium_foot', (x, y, 2.89), (.68, .72, .18), 23, part, bevel=.016, rough=.20, rot_z=-a)
        torus('foot_collar', (x, y, 2.99), .26, .024, 8, part, seg=36, rough=.18)

        pts = [(x * (1.0 - .29 * t), 5.5 + (y - 5.5) * (1.0 - .29 * t), 2.96 + 5.70 * t)
               for t in [j / 28.0 for j in range(29)]]
        closed_tube('brushed_vault_rib', pts, [.082 + .024 * math.sin(j / 28.0 * math.pi) for j in range(29)],
                    23, part, n_ring=18, rough=.20, thick=.12, flute_amp=.06, flute_freq=6)

        # Bronze-framed concave optical obsidian mirror stela (mat 12 -> thin-glass ray-traced reflections!)
        def optical(u, v, aa=a, xx=x, yy=y):
            width = .33 * (.90 + .10 * math.sin(u * math.pi))
            bend = .075 * v * v
            return (xx + width * v * math.cos(aa) - bend * math.sin(aa),
                    yy - width * v * math.sin(aa) - bend * math.cos(aa), 3.04 + 4.82 * u), .038
        closed_blade('optical_stela', 26, 14, optical, 12, part, rough=.05)

        # Precision anodized dark-bronze perimeter stiles & brass cap transoms framing the mirror stela
        for sv in (-1.04, 1.04):
            stile_pts = []
            for j in range(19):
                u = j / 18.0
                w_u = .33 * (.90 + .10 * math.sin(u * math.pi)) * sv
                b_u = .075
                stile_pts.append((x + w_u * math.cos(a) - b_u * math.sin(a),
                                  y - w_u * math.sin(a) - b_u * math.cos(a), 3.02 + 4.86 * u))
            closed_tube('stela_bronze_stile', stile_pts, [.022] * 19, 38, part, n_ring=12, rough=.24, thick=.04)
        box('stela_cap_bot', (x - .04 * math.sin(a), y - .04 * math.cos(a), 3.02), (.68, .10, .05), 8, part, bevel=.008, rough=.18, rot_z=-a)
        box('stela_cap_top', (x - .04 * math.sin(a), y - .04 * math.cos(a), 7.88), (.68, .10, .05), 38, part, bevel=.008, rough=.24, rot_z=-a)

    for k, a in enumerate((-2.12, -1.30, -.53, .53, 1.30, 2.12)):
        part = 210 + k
        px_c, py_c = 5.1 * math.sin(a), 5.5 + 5.1 * math.cos(a)
        # Smooth fluted hydroxyapatite mineral columns (n_ring=18 -> sculpted 6-lobe enamel ridges, zero sharp polygon facets!)
        for j in range(3):
            aa = a + (j - 1) * .092
            pts = []
            rad = []
            for n in range(29):
                t = n / 28.0
                r = 5.1 - .95 * math.sin(t * math.pi * .74)
                pts.append((r * math.sin(aa), 5.5 + r * math.cos(aa), 2.86 + 5.54 * t))
                rad.append(.185 * (1.0 - .56 * t) + .032 * math.sin(t * math.pi))
            closed_tube('enamel_prism', pts, rad, 20, part, n_ring=18, rough=.16, thick=.16, flute_amp=.048, flute_freq=6)
        # Stepped bevelled Honed Calacatta Marble plinth + Brushed Champagne-Nickel & Brass collars
        box('prism_marble_plinth', (px_c, py_c, 2.74), (1.18, 1.18, .18), 31, part, bevel=.022, rough=.22, rot_z=-a)
        superellipse_vessel('prism_nickel_seat', (px_c, py_c, 2.83),
                            [(.14, 0.0), (.52, 0.0), (.54, .06), (.46, .13), (.14, .13)], .95, .95, 2.2, 23, part, seg=48, rough=.20)
        torus('prism_brass_collar', (px_c, py_c, 2.96), .45, .020, 8, part, seg=40, rough=.18)

    for k in range(4):
        # Woven twisted neural lamellae with anodized-bronze edge tendons
        def ribbon(u, v, kk=k):
            return ((u - .5) * 9.2, 4.0 + kk * 1.55 + v * .32 * math.cos(u * math.pi),
                    3.0 + 5.4 * math.sin(math.pi * u) + .22 * math.sin(u * TAU + kk) + v * .34 * math.sin(u * math.pi)), .034
        closed_blade('neural_lamella', 40, 12, ribbon, 21, 216 + k, rough=.18)
        for sv in (-1.02, 1.02):
            tendon = [ribbon(j / 32.0, sv, k)[0] for j in range(33)]
            closed_tube('lamella_bronze_tendon', tendon, [.014] * 33, 38, 216 + k, n_ring=10, rough=.24, thick=.028)

def microbial_sanctuary():
    """Articulated Brushed-Brass & Champagne-Nickel Optical Aperture Iris + Scanned Botanical Celadon Sanctuary."""
    # Parts 220..223: 4 articulated optical proscenium iris blades centered around local origin (transformed to (0, 5.75, 5.50))
    for k in range(4):
        part = 220 + k
        a0 = 0.16 + k * 0.72
        arc_span = 0.66
        # Curved champagne-nickel optical shutter blade
        def iris_vane(u, v, ang0=a0, span=arc_span):
            ang = ang0 + u * span
            r_mid = 2.34 + 0.16 * math.sin(math.pi * u)
            w_rad = 0.085 * (0.45 + 0.55 * math.sin(math.pi * u))
            rr = r_mid + w_rad * v
            yy = -0.10 + 0.05 * math.sin(math.pi * u) + 0.018 * v
            return (rr * math.cos(ang), yy, rr * math.sin(ang)), 0.020
        closed_blade('optical_iris_vane', 26, 8, iris_vane, 23, part, rough=.19)

        # Turned brushed-brass outer actuator rail + anodized dark-bronze pivot knuckles + flush jade optical index pips
        rail_pts = []
        for j in range(25):
            t = j / 24.0
            ang = a0 + t * arc_span
            rr = 2.45 + 0.16 * math.sin(math.pi * t)
            yy = -0.11 + 0.05 * math.sin(math.pi * t)
            rail_pts.append((rr * math.cos(ang), yy, rr * math.sin(ang)))
        closed_tube('iris_brass_rail', rail_pts, [.022] * 25, 8, part, n_ring=16, rough=.18, thick=.04)
        for idx_k in (0, 12, 24):
            kx, ky, kz = rail_pts[idx_k]
            sphere('iris_bronze_knuckle', (kx, ky, kz), (.042, .048, .042), 38, part, seg=18, rings=10, rough=.24)
        for idx_p in (6, 18):
            px_i, py_i, pz_i = rail_pts[idx_p]
            sphere('iris_jade_index', (px_i, py_i - 0.024, pz_i), (.022, .016, .022), 6, part, seg=14, rings=8, rough=.16)

    # Parts 224..227: 4 Stepped Marble & Mossy Limestone Botanical Alcoves with Crazed Celadon Bowls & Real CC0 Scans
    alcove_coords = ((-2.85, 4.65), (2.85, 4.65), (-3.75, 7.00), (3.75, 7.00))
    for k, (x, y) in enumerate(alcove_coords):
        part = 224 + k
        # Stepped bevelled architectural sanctuary plinths (Mossy Limestone base -> Brass reveal -> Honed Calacatta top)
        box('sanctuary_stone_tier0', (x, y, 2.73), (1.76, 1.76, 0.16), 39, part, bevel=.028, rough=.72, rot_z=0.38 * k)
        box('sanctuary_brass_reveal', (x, y, 2.83), (1.42, 1.42, 0.04), 8, part, bevel=.008, rough=.18, rot_z=0.38 * k)
        box('sanctuary_marble_tier1', (x, y, 2.95), (1.36, 1.36, 0.20), 31, part, bevel=.024, rough=.22, rot_z=0.38 * k)
        superellipse_vessel('sanctuary_pedestal_neck', (x, y, 3.05),
                            [(.12, 0.0), (.48, 0.0), (.50, .08), (.42, .22), (.12, .22)], 1.0, 1.0, 2.2, 24, part, seg=48, rough=.48)

        # High-resolution Crazed Celadon Stoneware Sanctuary Bowl + Brushed-Brass Rim Collar + Moist Humus Bed
        bowl_prof = [
            (.10, 0.00), (.38, 0.00), (.62, 0.22), (.70, 0.46), (.68, 0.56),
            (.63, 0.57), (.59, 0.46), (.42, 0.18), (.10, 0.16),
        ]
        superellipse_vessel('celadon_sanctuary_bowl', (x, y, 3.26), bowl_prof, 1.0, 1.0, 2.0, 25, part, seg=64, rough=.14, thick=.024)
        torus('vessel_brass_rim', (x, y, 3.825), .655, .018, 8, part, seg=48, rough=.18)
        sphere('sanctuary_humus_bed', (x, y, 3.74), (.58, .58, .08), 39, part, seg=36, rings=14, rough=.84)

        # Watertight solidified CC0 scanned tropical flora rooted naturally inside each celadon sanctuary bowl!
        if k < 2:
            import_cc0_plant('sanctuary_calathea_%d' % k, 'calathea_orbifolia_01', ['calathea_orbifolia_01_b'],
                             (x - 0.08 * (1 if k == 0 else -1), y - 0.05, 3.76),
                             scale=1.95, rot_z=0.55 + k * 1.4, mat=29, part=part, rough=.34, thickness=.0014)
            import_cc0_plant('sanctuary_fern_front_%d' % k, 'fern_02', None,
                             (x + 0.16 * (1 if k == 0 else -1), y + 0.10, 3.75),
                             scale=1.45, rot_z=-0.75 + k * 1.9, mat=36, part=part, rough=.38, thickness=.0012)
            import_cc0_plant('sanctuary_succulent_%d' % k, 'potted_plant_04', ['potted_plant_04_plant'],
                             (x + 0.04, y - 0.22, 3.77),
                             scale=2.10, rot_z=k * 1.2, mat=30, part=part, rough=.32, thickness=.0015)
        else:
            import_cc0_plant('sanctuary_fern_rear_%d' % k, 'fern_02', None,
                             (x, y - 0.04, 3.76),
                             scale=1.85, rot_z=0.35 + k * 1.6, mat=36, part=part, rough=.38, thickness=.0014)
            import_cc0_plant('sanctuary_calathea_rear_%d' % k, 'calathea_orbifolia_01', ['calathea_orbifolia_01_d'],
                             (x - 0.14 * (1 if k == 2 else -1), y + 0.12, 3.76),
                             scale=1.80, rot_z=-1.1 + k * 0.9, mat=29, part=part, rough=.35, thickness=.0014)

        # Restrained low-lying symbiotic micro-flora spore buds nestled in the moss bed (responds subtly to Madame N bloom!)
        for b in range(4):
            ang_b = b * TAU / 4.0 + k * 0.62
            bx_s = x + 0.38 * math.cos(ang_b)
            by_s = y + 0.38 * math.sin(ang_b)
            def spore_leaf(u, v, ox=bx_s, oy=by_s, aa=ang_b):
                w_l = 0.048 * (math.sin(math.pi * u) ** 0.72)
                return (ox + math.cos(aa) * 0.16 * u - math.sin(aa) * w_l * v,
                        oy + math.sin(aa) * 0.16 * u + math.cos(aa) * w_l * v,
                        3.77 + 0.09 * math.sin(math.pi * 0.82 * u) + 0.015 * v * v), 0.010
            closed_blade('symbiotic_spore_leaf', 10, 6, spore_leaf, 22, part, rough=.24)

def folded_linen_towel(name, center, width_x, mat, part, rough=.84):
    """Continuous 3-layer S-folded spa hand towel with rounded cloth bends and true waffle-linen UVs."""
    cx, cy, cz = center
    # Cross-section centerline in (y_local, z_local): bottom layer -> front round fold -> middle layer -> back round fold -> top layer
    pts_yz = []
    # Bottom layer: back (+0.072) to front (-0.070) at z = +0.0045
    for i in range(9):
        t = i / 8.0
        pts_yz.append((0.068 - 0.136 * t, 0.0045))
    # Front U-bend around (y = -0.068, z = 0.0090), r = 0.0045
    for i in range(1, 8):
        a = -math.pi * 0.5 - math.pi * (i / 7.0)
        pts_yz.append((-0.068 + 0.0055 * math.cos(a), 0.0095 + 0.0050 * math.sin(a)))
    # Middle layer: front (-0.068) to back (+0.066) at z = +0.0145
    for i in range(1, 9):
        t = i / 8.0
        pts_yz.append((-0.068 + 0.134 * t, 0.0145 + 0.0008 * math.sin(math.pi * t)))
    # Back U-bend around (y = +0.066, z = 0.0195), r = 0.0050
    for i in range(1, 8):
        a = -math.pi * 0.5 + math.pi * (i / 7.0)
        pts_yz.append((0.066 + 0.0055 * math.cos(a), 0.0195 + 0.0050 * math.sin(a)))
    # Top layer: back (+0.066) to slightly recessed front (-0.056) at z = +0.0245
    for i in range(1, 10):
        t = i / 9.0
        pts_yz.append((0.066 - 0.122 * t, 0.0245 + 0.0012 * math.sin(math.pi * t)))

    half_th = 0.0034
    nx = 18
    ns = len(pts_yz)
    # Build closed 2-manifold ribbon extruded across width_x with gently drooped lateral edges
    prof_2d = []
    normals_2d = []
    s_acc = [0.0]
    for i in range(ns):
        y0, z0 = pts_yz[max(0, i - 1)]
        y1, z1 = pts_yz[min(ns - 1, i + 1)]
        dy, dz = y1 - y0, z1 - z0
        ln = max(math.hypot(dy, dz), 1e-6)
        # Normal (-dz, dy)
        normals_2d.append((-dz / ln, dy / ln))
        prof_2d.append(pts_yz[i])
        if i > 0:
            s_acc.append(s_acc[-1] + math.hypot(pts_yz[i][0] - pts_yz[i - 1][0], pts_yz[i][1] - pts_yz[i - 1][1]))
    # Closed cross-section loop: top side forward (0..ns-1) + bottom side backward (ns-1..0)
    loop_yz = []
    loop_v = []
    for i in range(ns):
        ny, nz = normals_2d[i]
        loop_yz.append((prof_2d[i][0] + ny * half_th, prof_2d[i][1] + nz * half_th))
        loop_v.append(s_acc[i] / 0.26)
    for i in range(ns - 1, -1, -1):
        ny, nz = normals_2d[i]
        loop_yz.append((prof_2d[i][0] - ny * half_th, prof_2d[i][1] - nz * half_th))
        loop_v.append((2.0 * s_acc[-1] - s_acc[i]) / 0.26)
    nl = len(loop_yz)

    verts = []
    uvs_loop = []
    # Left cap pole, nx+1 rings, right cap pole
    verts.append((cx - width_x * 0.5, cy, cz + 0.013))
    for ix in range(nx + 1):
        fx = ix / nx - 0.5
        xx = cx + fx * width_x
        droop = -0.0018 * (4.0 * fx * fx)
        for ly, lz in loop_yz:
            verts.append((xx, cy + ly, max(cz + 0.0005, cz + lz + droop)))
    right_pole = 1 + (nx + 1) * nl
    verts.append((cx + width_x * 0.5, cy, cz + 0.013))

    faces = []
    # Left cap
    for j in range(nl):
        jn = (j + 1) % nl
        faces.append((0, 1 + jn, 1 + j))
        uvs_loop.extend([(0.0, 0.5), (0.02, loop_v[jn]), (0.02, loop_v[j])])
    # Extruded body
    for ix in range(nx):
        b0 = 1 + ix * nl
        b1 = 1 + (ix + 1) * nl
        u0 = (ix / nx) * (width_x / 0.26)
        u1 = ((ix + 1) / nx) * (width_x / 0.26)
        for j in range(nl):
            jn = (j + 1) % nl
            faces.append((b0 + j, b0 + jn, b1 + jn, b1 + j))
            uvs_loop.extend([(u0, loop_v[j]), (u0, loop_v[jn]), (u1, loop_v[jn]), (u1, loop_v[j])])
    # Right cap
    last_b = 1 + nx * nl
    u_end = width_x / 0.26
    for j in range(nl):
        jn = (j + 1) % nl
        faces.append((last_b + j, last_b + jn, right_pole))
        uvs_loop.extend([(u_end, loop_v[j]), (u_end, loop_v[jn]), (u_end + 0.02, 0.5)])

    m = bpy.data.meshes.new(name)
    m.from_pydata(verts, [], faces)
    m.update()
    for f in m.polygons:
        f.use_smooth = True
    uv_layer = m.uv_layers.new(name='UVMap')
    for idx, uv in enumerate(uvs_loop):
        uv_layer.data[idx].uv = uv
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    return keep(o, name, mat, part=part, rough=rough, thick=.012, preserve_uv=True, smooth_angle=95.0)

def dawn_alcove():
    """1:1 Metric Taipei Penthouse Dawn Bathroom (1 unit = 1.00 m, floor Z_bl = Y_eng = 2.680 m).
    Parts 230..238: enclosing lime-plaster & vertical ceramic finger-tile walls, terrazzo floor,
    recessed oak ceiling cove, wraparound dark-bronze corner window framing misty Taipei 101,
    opposite oak-wainscot/sconce/shower wall for true flat mirror reflections, mitered white-oak
    floating vanity + honed marble slab, smooth 11k-tri superellipse porcelain vessel basin with
    undercut/drain/overflow/droplets, multi-part CNC brushed-nickel mixer faucet, watertight
    scanned CC0 Calathea orbifolia, 188 mm bristle toothbrush, amber apothecary soap pump,
    folded waffle-linen towel, and celadon crystal bud vase for the keepsake Glass Orchid.
    """
    z_flr = 2.680
    z_ceil = 5.860

    # -------------------------------------------------------------------------
    # Part 235: Enclosing Room Floor, Ceiling Cove & Serene Left Venetian Plaster Walls
    # -------------------------------------------------------------------------
    # Honed Aggregate Terrazzo Floor + Waffle-Linen Bath Mat
    box('terrazzo_floor', (-0.48, 4.38, z_flr - 0.06), (4.12, 4.60, 0.12), 32, 235, rough=.32)
    box('linen_bath_mat', (-0.18, 5.66, z_flr + 0.007), (0.86, 0.54, 0.014), 33, 235, bevel=.005, rough=.86)
    box('bath_mat_border', (-0.18, 5.66, z_flr + 0.008), (0.82, 0.50, 0.015), 33, 235, bevel=.004, rough=.84, uv_grain='horizontal')

    # Penthouse Ceiling + Recessed White-Oak Perimeter Cove + Indirect Warm Cove Glow
    box('plaster_ceiling', (-0.48, 4.38, z_ceil + 0.06), (4.12, 4.60, 0.12), 24, 235, rough=.68)
    box('left_ceiling_soffit', (-2.30, 4.38, z_ceil - 0.055), (0.42, 4.56, 0.11), 24, 235, bevel=.004, rough=.66)
    box('cove_oak_fascia', (-2.085, 4.38, z_ceil - 0.065), (0.022, 4.52, 0.09), 26, 235, bevel=.002, rough=.38, uv_grain='horizontal')
    box('cove_warm_strip', (-2.068, 4.38, z_ceil - 0.025), (0.016, 4.44, 0.018), 15, 235, rough=.24)

    # Left Return Wall (X = -2.46) & Serene Left Back Plaster Bay (X = -2.48..-0.96, Y_bl = 6.60)
    box('left_return_wall', (-2.48, 4.38, 4.27), (0.16, 4.56, 3.22), 24, 235, bevel=.004, rough=.66)
    box('back_plaster_left_bay', (-1.72, 6.61, 4.27), (1.52, 0.14, 3.22), 24, 235, bevel=.004, rough=.65)
    # Recessed vertical architectural shadow-reveal & warm linear graze on far left
    box('left_oak_reveal_stile', (-2.26, 6.535, 4.27), (0.026, 0.024, 3.16), 26, 235, bevel=.002, rough=.38, uv_grain='vertical')
    box('left_warm_graze', (-2.285, 6.538, 4.27), (0.014, 0.012, 3.10), 15, 235, rough=.28)
    # Flush White-Oak Baseboards with 6 mm dark shadow-gap reveal
    box('baseboard_back_left', (-1.71, 6.532, z_flr + 0.046), (1.50, 0.016, 0.092), 26, 235, bevel=.002, rough=.38, uv_grain='horizontal')
    box('baseboard_shadow_back', (-1.71, 6.536, z_flr + 0.096), (1.50, 0.012, 0.008), 0, 235, rough=.60)
    box('baseboard_left_return', (-2.392, 4.36, z_flr + 0.046), (0.016, 4.36, 0.092), 26, 235, bevel=.002, rough=.38, uv_grain='horizontal')

    # Left-of-vanity Wall-Mounted Anodized Dark Bronze Towel Bar + Draped Linen Towel
    for post_x in (-1.38, -1.06):
        rod('towel_bar_post', (post_x, 6.54, 3.36), (post_x, 6.465, 3.36), 0.009, 38, 235, verts=24, rough=.26, bevel=.001)
    rod('towel_bar_rail', (-1.40, 6.465, 3.36), (-1.04, 6.465, 3.36), 0.0085, 38, 235, verts=28, rough=.24, bevel=.001)
    box('hanging_bath_towel_front', (-1.22, 6.453, 3.13), (0.26, 0.012, 0.46), 33, 235, bevel=.005, rough=.85)
    box('hanging_bath_towel_back', (-1.22, 6.477, 3.15), (0.258, 0.012, 0.42), 33, 235, bevel=.005, rough=.85)
    box('hanging_bath_towel_top', (-1.22, 6.465, 3.366), (0.26, 0.034, 0.016), 33, 235, bevel=.006, rough=.85)

    # -------------------------------------------------------------------------
    # Part 236: Center Vanity Feature Wall — Handmade Vertical Ceramic Finger-Tiles & Oak Pilasters
    # -------------------------------------------------------------------------
    # uv_grain='horizontal' rotates mat_28 90 deg so the long narrow glazed bricks stand upright as vertical finger-tiles!
    box('vanity_tile_wall', (-0.23, 6.59, 4.27), (1.46, 0.10, 3.22), 28, 236, rough=.14, uv_grain='horizontal', uv_scale=0.68)
    box('vanity_pilaster_left', (-0.96, 6.535, 4.27), (0.028, 0.036, 3.18), 26, 236, bevel=.0025, rough=.36, uv_grain='vertical')
    box('vanity_pilaster_right', (0.50, 6.535, 4.27), (0.028, 0.036, 3.18), 26, 236, bevel=.0025, rough=.36, uv_grain='vertical')
    box('vanity_under_baseboard', (-0.23, 6.532, z_flr + 0.046), (1.43, 0.016, 0.092), 26, 236, bevel=.002, rough=.38, uv_grain='horizontal')

    # -------------------------------------------------------------------------
    # Part 237: Wraparound Corner Architectural Window & Penthouse Exterior Stone Balcony
    # Unobstructed upper low-iron picture glass (X = +0.54..+1.48) frames Taipei 101 cleanly across the entire 10s camera move!
    # -------------------------------------------------------------------------
    # Low Knee Wall + Honed Marble Window Sill + Ceiling Header Beam
    box('win_knee_front', (1.02, 6.60, 2.795), (1.04, 0.14, 0.23), 24, 237, bevel=.003, rough=.65)
    box('win_sill_front', (1.02, 6.59, 2.926), (1.06, 0.18, 0.032), 31, 237, bevel=.003, rough=.24)
    box('win_header_front', (1.02, 6.60, 5.795), (1.06, 0.16, 0.13), 24, 237, bevel=.003, rough=.65)

    box('win_knee_right', (1.53, 5.63, 2.795), (0.14, 1.98, 0.23), 24, 237, bevel=.003, rough=.65)
    box('win_sill_right', (1.52, 5.63, 2.926), (0.18, 1.98, 0.032), 31, 237, bevel=.003, rough=.24)
    box('win_header_right', (1.53, 5.63, 5.795), (0.16, 1.98, 0.13), 24, 237, bevel=.003, rough=.65)
    # Solid Venetian lime-plaster return wall enclosing the walk-in shower & rear zone (Y_bl = 2.16 .. 4.66)
    box('right_shower_return_wall', (1.53, 3.41, 4.27), (0.14, 2.52, 3.22), 24, 237, bevel=.004, rough=.65)
    box('baseboard_right_return', (1.452, 3.88, z_flr + 0.046), (0.016, 1.52, 0.092), 26, 237, bevel=.002, rough=.38, uv_grain='horizontal')

    # Anodized Dark Bronze Extruded Window Mullions & Tracks (mat 38 -> InstanceMask=1u casts real ray-traced shadows!)
    z_win_bot = 2.942
    z_win_top = 5.730
    z_win_mid = 4.336
    h_win = z_win_top - z_win_bot
    z_transom = 3.560  # Dado-height transom leaves 3.58..5.71 m completely clear for Taipei 101 view!

    # Front corner picture window framing (X = +0.52 .. +1.515 at Y_bl = 6.58 — single wide structural picture bay!)
    box('bz_front_bot_track', (1.02, 6.58, z_win_bot + 0.020), (1.00, 0.068, 0.040), 38, 237, bevel=.002, rough=.25)
    box('bz_front_top_track', (1.02, 6.58, z_win_top - 0.020), (1.00, 0.068, 0.040), 38, 237, bevel=.002, rough=.25)
    box('bz_front_transom', (1.02, 6.58, z_transom), (1.00, 0.058, 0.036), 38, 237, bevel=.002, rough=.25)
    box('bz_jamb_left', (0.525, 6.58, z_win_mid), (0.042, 0.072, h_win), 38, 237, bevel=.0025, rough=.25)
    box('bz_mullion_front_lo', (1.04, 6.58, 3.25), (0.032, 0.058, 0.58), 38, 237, bevel=.002, rough=.25)
    box('bz_corner_column', (1.515, 6.58, z_win_mid), (0.076, 0.076, h_win + 0.02), 38, 237, bevel=.003, rough=.24)

    # Right corner return picture bay (X = +1.515, Y_bl = 4.68 .. 6.58 — single clear structural bay: zero reflected vertical stripes!)
    box('bz_right_bot_track', (1.515, 5.63, z_win_bot + 0.020), (0.068, 1.92, 0.040), 38, 237, bevel=.002, rough=.25)
    box('bz_right_top_track', (1.515, 5.63, z_win_top - 0.020), (0.068, 1.92, 0.040), 38, 237, bevel=.002, rough=.25)
    box('bz_right_transom', (1.515, 5.63, z_transom), (0.058, 1.92, 0.036), 38, 237, bevel=.002, rough=.25)
    box('bz_right_rear_jamb', (1.515, 4.68, z_win_mid), (0.072, 0.042, h_win), 38, 237, bevel=.0025, rough=.25)
    box('bz_mullion_right_lo', (1.515, 5.63, 3.25), (0.058, 0.032, 0.58), 38, 237, bevel=.002, rough=.25)

    # Optically Flat Low-Iron Architectural Float Glass Panes (mat 40 -> zero droplet/vein distortion!)
    box('win_glass_front_main', (1.015, 6.58, 4.635), (0.93, 0.008, 2.11), 40, 237, rough=.008)
    box('win_glass_front_lo_l', (0.785, 6.58, 3.26), (0.47, 0.008, 0.56), 40, 237, rough=.008)
    box('win_glass_front_lo_r', (1.265, 6.58, 3.26), (0.43, 0.008, 0.56), 40, 237, rough=.008)
    box('win_glass_right_hi', (1.515, 5.63, 4.635), (0.008, 1.84, 2.11), 40, 237, rough=.008)
    box('win_glass_right_lo', (1.515, 5.63, 3.26), (0.008, 1.84, 0.56), 40, 237, rough=.008)

    # 1:1 Metric Penthouse Exterior Travertine Balcony, Glass Balustrade & Fern Planter Trough (outside Y_bl = 6.64..8.15 m)
    box('terrace_paver_deck', (1.42, 7.42, z_flr - 0.06), (2.36, 1.56, 0.12), 41, 237, bevel=.004, rough=.36)
    box('terrace_side_deck', (2.12, 4.50, z_flr - 0.06), (1.04, 4.32, 0.12), 41, 237, bevel=.004, rough=.36)
    box('terrace_edge_coping', (1.42, 8.16, z_flr + 0.035), (2.38, 0.12, 0.07), 31, 237, bevel=.003, rough=.28)
    box('terrace_rail_shoe', (1.42, 8.15, z_flr + 0.090), (2.34, 0.042, 0.045), 38, 237, bevel=.002, rough=.26)
    box('terrace_balustrade_glass', (1.42, 8.15, z_flr + 0.535), (2.30, 0.012, 0.85), 40, 237, rough=.010)
    box('terrace_balustrade_rail', (1.42, 8.15, z_flr + 0.970), (2.34, 0.044, 0.028), 38, 237, bevel=.003, rough=.24)
    # Architectural limestone planter trough + solidified CC0 fern_02 fronds catching golden Taipei dawn outside the window
    box('terrace_planter_trough', (0.86, 7.56, z_flr + 0.19), (0.72, 0.34, 0.38), 24, 237, bevel=.005, rough=.58)
    box('terrace_planter_rim', (0.86, 7.56, z_flr + 0.388), (0.75, 0.37, 0.024), 31, 237, bevel=.003, rough=.26)
    box('terrace_planter_soil', (0.86, 7.56, z_flr + 0.365), (0.66, 0.28, 0.03), 39, 237, rough=.84)
    import_cc0_plant('terrace_fern_a', 'fern_02', None, (0.72, 7.55, z_flr + 0.36), scale=0.82, rot_z=math.radians(18.0), mat=36, part=237, rough=.38, thickness=.00075)
    import_cc0_plant('terrace_fern_b', 'fern_02', None, (1.02, 7.57, z_flr + 0.36), scale=0.74, rot_z=math.radians(125.0), mat=36, part=237, rough=.38, thickness=.00075)

    # -------------------------------------------------------------------------
    # Part 238: Opposite Rear Wall (Y_bl = 2.20) — Directly Reflected in the Flat Bathroom Mirror!
    # Includes Fluted White-Oak Wainscot, Framed Botanical Relief, Warm Opal Sconce & Glass Rain Shower
    # -------------------------------------------------------------------------
    box('rear_enclosure_wall', (-0.48, 2.16, 4.27), (4.12, 0.12, 3.22), 24, 238, rough=.65)
    # Dark acoustic shadow backing + 28 individual bevelled quarter-sawn white-oak vertical wainscot slats!
    box('rear_wainscot_backing', (-1.02, 2.226, 3.33), (2.78, 0.012, 1.30), 0, 238, rough=.65)
    for s_i in range(28):
        sx = -2.32 + s_i * 0.096
        box('rear_oak_slat_%d' % s_i, (sx, 2.242, 3.33), (0.058, 0.022, 1.28), 26, 238, bevel=.0025, rough=.36, uv_grain='vertical')
    box('rear_oak_dado_ledge', (-1.02, 2.252, 3.982), (2.82, 0.042, 0.026), 26, 238, bevel=.0025, rough=.35, uv_grain='horizontal')

    # Framed Botanical Relief Shadow-Box Artwork reflected in mirror at (X = -0.36, Z_bl = 4.48)
    box('art_bronze_frame_top', (-0.36, 2.252, 4.78), (0.48, 0.036, 0.022), 38, 238, bevel=.002, rough=.24)
    box('art_bronze_frame_bot', (-0.36, 2.252, 4.18), (0.48, 0.036, 0.022), 38, 238, bevel=.002, rough=.24)
    box('art_bronze_frame_l', (-0.59, 2.252, 4.48), (0.022, 0.036, 0.62), 38, 238, bevel=.002, rough=.24)
    box('art_bronze_frame_r', (-0.13, 2.252, 4.48), (0.022, 0.036, 0.62), 38, 238, bevel=.002, rough=.24)
    box('art_cotton_matboard', (-0.36, 2.235, 4.48), (0.44, 0.014, 0.58), 24, 238, rough=.78)
    box('art_charcoal_mount', (-0.36, 2.243, 4.48), (0.28, 0.008, 0.40), 0, 238, bevel=.001, rough=.72)
    # Sculpted celadon porcelain botanical relief leaves & curved brushed-brass stem inside shadow box
    stem_art = [(-0.378 + 0.036 * (k / 14.0) + 0.012 * math.sin(math.pi * k / 14.0),
                 2.250, 4.31 + 0.30 * (k / 14.0)) for k in range(15)]
    closed_tube('art_brass_stem', stem_art, [0.0030 - 0.0010 * (k / 14.0) for k in range(15)], 8, 238, n_ring=14, rough=.18, thick=.006)
    relief_leaves = [
        (-0.365, 4.39, -0.58, 0.105, 0.038),
        (-0.348, 4.47,  0.52, 0.112, 0.041),
        (-0.352, 4.55, -0.18, 0.096, 0.035),
    ]
    for lf_i, (ox_l, oz_l, ang_l, len_l, hw_l) in enumerate(relief_leaves):
        ca_l, sa_l = math.cos(ang_l), math.sin(ang_l)
        def relief_leaf_surf(u, v, ox=ox_l, oz=oz_l, ca=ca_l, sa=sa_l, ll=len_l, hw=hw_l):
            env = (math.sin(math.pi * u) ** 0.72) * (1.0 - 0.18 * (u ** 1.8)) + 0.04 * (1.0 - u)
            lat = hw * env * v
            rad = ll * u
            xx = ox + sa * rad + ca * lat
            zz = oz + ca * rad - sa * lat
            yy = 2.252 + 0.004 * math.sin(math.pi * u) - 0.0025 * (v * v)
            return (xx, yy, zz), 0.0045
        closed_blade('art_porcelain_leaf_%d' % lf_i, 16, 10, relief_leaf_surf, 25, 238, rough=.14)
    box('art_gallery_glass', (-0.36, 2.262, 4.48), (0.45, 0.004, 0.59), 40, 238, rough=.010)

    # Warm Opal Cylindrical Architectural Wall Sconce at (X = +0.08, Y_bl = 2.28, Z_bl = 4.44)
    box('sconce_backplate', (0.08, 2.232, 4.44), (0.055, 0.016, 0.16), 38, 238, bevel=.002, rough=.26)
    rod('sconce_arm', (0.08, 2.235, 4.44), (0.08, 2.285, 4.44), 0.011, 38, 238, verts=20, rough=.26)
    rod('sconce_opal_tube', (0.08, 2.285, 4.30), (0.08, 2.285, 4.58), 0.028, 15, 238, verts=28, rough=.22, bevel=.002)
    rod('sconce_cap_ring', (0.08, 2.285, 4.43), (0.08, 2.285, 4.45), 0.030, 38, 238, verts=28, rough=.24)

    # Walk-in Rain Shower Zone (X = +0.42..+1.48, Y_bl = 2.22..3.32) reflected on right side of mirror
    box('shower_tile_wall', (0.95, 2.228, 4.27), (1.06, 0.018, 3.18), 28, 238, rough=.14, uv_grain='horizontal', uv_scale=0.68)
    box('shower_glass_channel_bot', (0.42, 2.82, z_flr + 0.012), (0.022, 1.18, 0.024), 38, 238, bevel=.002, rough=.25)
    box('shower_glass_channel_wall', (0.42, 2.24, 4.02), (0.022, 0.024, 2.68), 38, 238, bevel=.002, rough=.25)
    box('shower_glass_screen', (0.42, 2.82, 4.02), (0.010, 1.16, 2.66), 40, 238, rough=.008)
    rod('rainfall_drop_rod', (0.92, 2.75, 5.80), (0.92, 2.75, 5.42), 0.012, 23, 238, verts=24, rough=.24)
    rod('rainfall_shower_head', (0.92, 2.75, 5.412), (0.92, 2.75, 5.422), 0.135, 23, 238, verts=40, rough=.22, bevel=.002)

    # -------------------------------------------------------------------------
    # Part 231: Floating Quarter-Sawn White-Oak Vanity Cabinet & Honed Marble Countertop Slab
    # Ergonomic 87.0 cm counter height (Z_bl = 3.550 m above 2.680 m floor), 1.38 m wide x 58 cm deep
    # -------------------------------------------------------------------------
    # Recessed dark shadow-box carcass so 4 mm drawer reveals read as crisp architectural joinery
    box('vanity_shadow_carcass', (-0.19, 6.295, 3.312), (1.33, 0.53, 0.375), 0, 231, rough=.65)
    # Mitered 22 mm White-Oak Perimeter Panels (grain oriented strictly along panel length!)
    box('vanity_oak_side_l', (-0.869, 6.29, 3.312), (0.022, 0.56, 0.405), 26, 231, bevel=.0025, rough=.35, uv_grain='vertical')
    box('vanity_oak_side_r', (0.489, 6.29, 3.312), (0.022, 0.56, 0.405), 26, 231, bevel=.0025, rough=.35, uv_grain='vertical')
    box('vanity_oak_bottom', (-0.19, 6.29, 3.120), (1.336, 0.56, 0.022), 26, 231, bevel=.0025, rough=.35, uv_grain='horizontal')
    box('vanity_oak_top_rail', (-0.19, 6.022, 3.504), (1.336, 0.024, 0.020), 26, 231, bevel=.002, rough=.35, uv_grain='horizontal')

    # Four Asymmetric Book-Matched Horizontal-Grain White-Oak Drawer Fronts with Chamfered J-Pull Reveals
    # Left wide bank (X = -0.854 .. -0.022, width = 0.832 m) & Right narrow bank (X = -0.016 .. +0.474, width = 0.490 m)
    for dx_c, dw in [(-0.438, 0.832), (0.229, 0.490)]:
        # Upper drawer front
        box('vanity_drawer_upper', (dx_c, 6.018, 3.404), (dw, 0.022, 0.168), 26, 231, bevel=.0025, rough=.34, uv_grain='horizontal')
        # Chamfered finger-pull J-lip recess along top of lower drawer
        box('vanity_drawer_lower', (dx_c, 6.018, 3.218), (dw, 0.022, 0.172), 26, 231, bevel=.0025, rough=.34, uv_grain='horizontal')
        box('vanity_jpull_lip', (dx_c, 6.028, 3.312), (dw, 0.014, 0.012), 26, 231, bevel=.0015, rough=.38, uv_grain='horizontal')

    # Subtle Warm Under-Cabinet Architectural Toe-Kick Light Strip
    box('vanity_under_glow', (-0.19, 6.46, 3.105), (1.18, 0.024, 0.008), 15, 231, rough=.28)

    # Honed Calacatta/Carrara Warm Marble Countertop Slab (35 mm thick, 15 mm overhang) + 62 mm Rear Marble Upstand
    box('marble_counter_slab', (-0.19, 6.285, 3.5325), (1.41, 0.59, 0.035), 31, 231, bevel=.003, segments=3, rough=.22)
    box('marble_rear_upstand', (-0.19, 6.531, 3.581), (1.41, 0.018, 0.062), 31, 231, bevel=.0025, rough=.22)

    # -------------------------------------------------------------------------
    # Part 230: True Silvered Flat Stadium Mirror + Anodized Dark Bronze Bezel + Warm LED Halo
    # Mathematically flat front normal N=(0,-1,0) in Blender -> N=(0,0,-1) in Engine!
    # -------------------------------------------------------------------------
    mir_cx, mir_cz = -0.18, 4.39
    stadium_prism('flat_silvered_mirror', (mir_cx, 6.512, mir_cz), 0.365, 0.555, 6.508, 0.008, 0.007, 0.003, 34, 230, seg_arc=64, rough=.008)
    stadium_prism('mirror_bronze_bezel', (mir_cx, 6.516, mir_cz), 0.373, 0.563, 6.512, 0.022, 0.0025, 0.002, 38, 230, seg_arc=64, rough=.24)
    stadium_prism('mirror_led_halo', (mir_cx, 6.536, mir_cz), 0.352, 0.542, 6.535, 0.010, 0.002, 0.002, 15, 230, seg_arc=48, rough=.30)

    # -------------------------------------------------------------------------
    # Part 232: High-Poly Glazed Porcelain Vessel Basin (50 x 36 x 13.2 cm) + CNC Brushed-Nickel Mixer Faucet
    # -------------------------------------------------------------------------
    bx_c, by_c, bz_c = -0.18, 6.245, 3.550
    # Smooth 22-point anatomical ceramic cross-section: recessed foot undercut -> compound belly -> 11.5 mm rolled rim -> concave bowl -> drain well
    basin_prof = [
        (0.14, 0.000), (0.66, 0.000), (0.69, 0.007), (0.76, 0.014),
        (0.87, 0.036), (0.95, 0.066), (0.992, 0.098), (1.000, 0.121),
        (0.992, 0.130), (0.974, 0.1325), (0.954, 0.130), (0.942, 0.120),
        (0.912, 0.092), (0.845, 0.058), (0.720, 0.032), (0.520, 0.020),
        (0.280, 0.0165), (0.155, 0.0155), (0.140, 0.009),
    ]
    superellipse_vessel('porcelain_vessel_basin', (bx_c, by_c, bz_c), basin_prof, 0.252, 0.182, 2.38, 27, 232, seg=128, rough=.042, thick=.014)

    # Recessed Drain Channel + Precision Brushed Champagne-Nickel Pop-Up Drain Cap + Slotted Overflow Bezel
    rod('basin_drain_recess', (bx_c, by_c, bz_c + 0.013), (bx_c, by_c, bz_c + 0.0175), 0.035, 0, 232, verts=40, rough=.65)
    rod('basin_drain_flange', (bx_c, by_c, bz_c + 0.015), (bx_c, by_c, bz_c + 0.0185), 0.038, 23, 232, verts=48, rough=.19, bevel=.001)
    # Softly domed brushed-nickel pop-up stopper cap floating 2.5 mm above drain throat
    sphere('basin_popup_cap', (bx_c, by_c, bz_c + 0.0195), (0.031, 0.031, 0.0055), 23, 232, seg=48, rings=16, rough=.18)
    # Rear interior ceramic overflow slot & brushed-nickel trim ring
    box('basin_overflow_bezel', (bx_c, by_c + 0.156, bz_c + 0.094), (0.034, 0.006, 0.011), 23, 232, bevel=.0015, rough=.20)
    box('basin_overflow_slot', (bx_c, by_c + 0.154, bz_c + 0.094), (0.026, 0.005, 0.005), 0, 232, rough=.70)

    # 14 Tiny 3D Low-Iron Crystal Water Droplets beaded naturally inside the glazed porcelain bowl!
    droplet_specs = [
        (-0.065, -0.045, 0.032, 0.0034), (0.052, -0.038, 0.030, 0.0028), (-0.035, 0.058, 0.034, 0.0038),
        (0.078, 0.042, 0.036, 0.0031), (-0.115, 0.015, 0.044, 0.0036), (0.108, -0.018, 0.042, 0.0029),
        (-0.018, 0.088, 0.044, 0.0042), (0.024, 0.095, 0.046, 0.0032), (-0.148, -0.032, 0.062, 0.0030),
        (0.142, 0.028, 0.060, 0.0035), (0.005, -0.072, 0.035, 0.0027), (-0.082, 0.072, 0.048, 0.0033),
        (0.048, 0.118, 0.062, 0.0036), (-0.042, 0.168, 0.131, 0.0032),
    ]
    for d_i, (ddx, ddy, ddz, drad) in enumerate(droplet_specs):
        sphere('basin_droplet_%d' % d_i, (bx_c + ddx, by_c + ddy, bz_c + ddz), (drad, drad * 1.08, drad * 0.58), 3, 232, seg=18, rings=10, rough=.015, thick=.008)

    # Multi-Part CNC-Machined Brushed Champagne-Nickel Deck-Mount Tall Vessel Mixer Faucet
    fx_c, fy_c, fz_c = -0.18, 6.472, 3.550
    # 1. EPDM dark O-ring shadow seal + machined circular base escutcheon (54 mm diam)
    rod('faucet_oring_seal', (fx_c, fy_c, fz_c), (fx_c, fy_c, fz_c + 0.0020), 0.0265, 0, 232, verts=48, rough=.75)
    rod('faucet_escutcheon', (fx_c, fy_c, fz_c + 0.0018), (fx_c, fy_c, fz_c + 0.0085), 0.0275, 23, 232, verts=64, rough=.19, bevel=.0015)
    # 2. Lower & upper machined body cylinders separated by a 1 mm swivel-collar shadow groove!
    rod('faucet_body_lower', (fx_c, fy_c, fz_c + 0.0082), (fx_c, fy_c, fz_c + 0.168), 0.0195, 23, 232, verts=64, rough=.19, bevel=.001)
    rod('faucet_swivel_groove', (fx_c, fy_c, fz_c + 0.1675), (fx_c, fy_c, fz_c + 0.1705), 0.0182, 0, 232, verts=48, rough=.60)
    rod('faucet_body_upper', (fx_c, fy_c, fz_c + 0.1700), (fx_c, fy_c, fz_c + 0.212), 0.0195, 23, 232, verts=64, rough=.19, bevel=.0012)
    # 3. Side ceramic-cartridge mixer valve barrel + rotating cap + precision flat-bar pin lever
    rod('faucet_valve_barrel', (fx_c + 0.014, fy_c, fz_c + 0.095), (fx_c + 0.044, fy_c, fz_c + 0.095), 0.0145, 23, 232, verts=48, rough=.19, bevel=.001)
    rod('faucet_valve_seam', (fx_c + 0.0435, fy_c, fz_c + 0.095), (fx_c + 0.0455, fy_c, fz_c + 0.095), 0.0135, 0, 232, verts=36, rough=.60)
    rod('faucet_valve_cap', (fx_c + 0.0450, fy_c, fz_c + 0.095), (fx_c + 0.060, fy_c, fz_c + 0.095), 0.0145, 23, 232, verts=48, rough=.18, bevel=.0012)
    box('faucet_flat_lever', (fx_c + 0.053, fy_c - 0.026, fz_c + 0.128), (0.008, 0.068, 0.0045), 23, 232,
        bevel=.0012, rough=.18, rot_euler=(math.radians(-52.0), 0.0, math.radians(8.0)))
    # 4. Graceful High-Arc Gooseneck Spout (24 mm OD, 42 smooth rings x 36 segments) arching over the porcelain bowl
    spout_pts = []
    # Vertical rise from upper body (z = +0.205 .. +0.228) -> 180-deg semi-circular arch (r = 0.086 m) -> angled tip at (y = 6.300, z = 3.776)
    for i in range(6):
        spout_pts.append((fx_c, fy_c, fz_c + 0.204 + 0.005 * i))
    arch_r = 0.086
    arch_cy = fy_c - arch_r
    arch_cz = fz_c + 0.229
    for i in range(1, 33):
        ang = math.radians(196.0) * (i / 32.0)  # 0 (back +Y) over top (+Z) to 196 deg (forward -Y & slightly inward)
        spout_pts.append((fx_c, arch_cy + arch_r * math.cos(ang), arch_cz + arch_r * math.sin(ang)))
    closed_tube('faucet_gooseneck_spout', spout_pts, [0.0122] * len(spout_pts), 23, 232, n_ring=36, rough=.18, thick=.024)
    # 5. Machined Aerator Tip Collar + Recessed Honeycomb Aerator + Poised Crystal Droplet!
    tip_p = Vector(spout_pts[-1])
    tip_dir = (Vector(spout_pts[-1]) - Vector(spout_pts[-2])).normalized()
    rod('faucet_aerator_collar', tuple(tip_p - tip_dir * 0.004), tuple(tip_p + tip_dir * 0.008), 0.0132, 23, 232, verts=40, rough=.18, bevel=.001)
    rod('faucet_aerator_screen', tuple(tip_p + tip_dir * 0.005), tuple(tip_p + tip_dir * 0.009), 0.0108, 38, 232, verts=32, rough=.42)
    sphere('faucet_poised_droplet', tuple(tip_p + tip_dir * 0.0115), (0.0036, 0.0036, 0.0046), 3, 232, seg=20, rings=12, rough=.012, thick=.008)

    # -------------------------------------------------------------------------
    # Part 233: 1:1 Metric Everyday Care Props + Celadon Crystal Bud Vase + Folded Waffle-Linen Towel
    # -------------------------------------------------------------------------
    # 1. Hand-Thrown Crazed Celadon Stoneware Tumbler (7.8 cm diam x 10.2 cm tall) at (X=+0.155, Y=6.315)
    tumb_x, tumb_y = 0.155, 6.315
    tumb_prof = [
        (0.010, 0.000), (0.032, 0.000), (0.035, 0.005), (0.0385, 0.048),
        (0.0395, 0.098), (0.0380, 0.102), (0.0355, 0.099), (0.0340, 0.050),
        (0.0300, 0.010), (0.010, 0.009),
    ]
    superellipse_vessel('celadon_tumbler', (tumb_x, tumb_y, z_flr + 0.870), tumb_prof, 1.0, 1.0, 2.0, 25, 233, seg=64, rough=.12, thick=.008)

    # 2. Ergonomic 188 mm Toothbrush leaning naturally at 15 deg inside the Celadon Tumbler + 32 Fine Bristle Tufts!
    tb_base = Vector((tumb_x - 0.010, tumb_y + 0.008, 3.565))
    tb_dir = Vector((0.24, -0.10, 0.965)).normalized()
    tb_nrm = Vector((-0.22, -0.96, -0.06)).normalized()  # Bristles face forward-left toward viewer/mirror
    tb_lat = tb_dir.cross(tb_nrm).normalized()
    tb_pts = []
    tb_rads = []
    for i in range(22):
        u = i / 21.0
        # Subtle ergonomic S-curve along the 188 mm handle
        bow = 0.0035 * math.sin(u * math.pi * 1.35)
        p = tb_base + tb_dir * (0.188 * u) + tb_nrm * bow
        tb_pts.append((p.x, p.y, p.z))
        # Tapered ergonomic profile: rounded base -> thumb grip swell -> slender neck -> rounded head
        if u < 0.58:
            r = 0.0046 + 0.0020 * math.sin(u / 0.58 * math.pi)
        elif u < 0.84:
            r = 0.0046 - 0.0016 * math.sin((u - 0.58) / 0.26 * math.pi)
        else:
            r = 0.0052 * math.sin(min(1.0, (1.02 - u) / 0.18 * 0.5 * math.pi)) + 0.0018
        tb_rads.append(r)
    closed_tube('toothbrush_handle', tb_pts, tb_rads, 11, 233, n_ring=20, rough=.26, thick=.010)
    # Celadon tactile thumb-grip collar inlay mid-handle
    grip_pts = [tb_pts[i] for i in range(7, 13)]
    grip_rads = [tb_rads[i] + 0.00045 for i in range(7, 13)]
    closed_tube('toothbrush_grip_inlay', grip_pts, grip_rads, 25, 233, n_ring=20, rough=.32, thick=.010)
    # 32 Fine Tapered Nylon Bristle Tufts (8 rows x 4 columns, 1.3 mm diam x 9.2 mm tall!)
    for r_i in range(8):
        u_b = 0.865 + r_i * 0.0165
        row_c = tb_base + tb_dir * (0.188 * u_b) + tb_nrm * 0.0022
        cols = 2 if (r_i == 0 or r_i == 7) else 4
        for c_i in range(cols):
            off_lat = ((c_i - (cols - 1) * 0.5) * 0.0021)
            b_start = row_c + tb_lat * off_lat
            b_end = b_start + tb_nrm * (0.0092 + 0.0006 * ((r_i + c_i) % 2))
            rod('bristle_tuft_%d_%d' % (r_i, c_i), tuple(b_start), tuple(b_end), 0.00068, 11, 233, verts=8, rough=.42)

    # 3. Translucent Amber Apothecary Glass Hand-Wash Bottle + Machined Brushed-Nickel Pump Dispenser
    amb_x, amb_y = 0.285, 6.365
    amb_prof = [
        (0.010, 0.000), (0.033, 0.000), (0.037, 0.008), (0.0375, 0.096),
        (0.034, 0.114), (0.020, 0.128), (0.0145, 0.136), (0.0145, 0.146),
        (0.0115, 0.146), (0.0115, 0.126), (0.031, 0.106), (0.032, 0.012), (0.010, 0.010),
    ]
    superellipse_vessel('amber_soap_bottle', (amb_x, amb_y, 3.550), amb_prof, 1.0, 1.0, 2.0, 35, 233, seg=64, rough=.038, thick=.09)
    # Brushed-nickel ribbed pump collar, plunger stem, pump head & curved dispenser nozzle
    rod('soap_pump_collar', (amb_x, amb_y, 3.692), (amb_x, amb_y, 3.714), 0.0158, 23, 233, verts=40, rough=.20, bevel=.0012)
    rod('soap_pump_plunger', (amb_x, amb_y, 3.714), (amb_x, amb_y, 3.736), 0.0048, 23, 233, verts=24, rough=.18)
    rod('soap_pump_head', (amb_x, amb_y, 3.734), (amb_x, amb_y, 3.748), 0.0115, 23, 233, verts=36, rough=.19, bevel=.0015)
    pump_spout = [
        (amb_x, amb_y, 3.743),
        (amb_x - 0.014, amb_y - 0.012, 3.744),
        (amb_x - 0.028, amb_y - 0.024, 3.741),
        (amb_x - 0.036, amb_y - 0.031, 3.734),
    ]
    closed_tube('soap_pump_nozzle', pump_spout, [0.0042, 0.0038, 0.0033, 0.0028], 23, 233, n_ring=20, rough=.19, thick=.008)

    # 4. Blown Pale-Celadon Crystal Bud Vase at (X=+0.395, Y=6.250, Z=3.550) holding the Miniature Keepsake Glass Orchid!
    vase_x, vase_y = 0.395, 6.250
    vase_prof = [
        (0.008, 0.000), (0.026, 0.000), (0.034, 0.038), (0.033, 0.078),
        (0.021, 0.118), (0.016, 0.142), (0.020, 0.155), (0.0175, 0.156),
        (0.0135, 0.140), (0.026, 0.072), (0.022, 0.012), (0.008, 0.010),
    ]
    superellipse_vessel('celadon_orchid_vase', (vase_x, vase_y, 3.550), vase_prof, 1.0, 1.0, 2.0, 4, 233, seg=56, rough=.045, thick=.028)

    # 5. Soft 3-Fold Organic Waffle-Linen Spa Hand Towel on Front-Right Marble Slab
    folded_linen_towel('folded_counter_towel', (0.195, 6.115, 3.550), 0.205, 33, 233, rough=.84)

    # -------------------------------------------------------------------------
    # Part 234: Artisan Ribbed Celadon Stoneware Planter + Watertight Scanned CC0 Calathea orbifolia!
    # -------------------------------------------------------------------------
    pot_x, pot_y = -0.65, 6.265
    pot_prof = [
        (0.018, 0.008), (0.072, 0.008), (0.084, 0.028), (0.091, 0.068),
        (0.093, 0.112), (0.091, 0.150), (0.087, 0.156), (0.081, 0.152),
        (0.082, 0.125), (0.072, 0.022), (0.018, 0.020),
    ]
    superellipse_vessel('artisan_celadon_planter', (pot_x, pot_y, 3.550), pot_prof, 1.0, 1.0, 2.0, 25, 234, seg=64, rough=.14, thick=.012)
    # Recessed Anodized Dark Bronze Foot Ring under Planter
    rod('planter_bronze_foot', (pot_x, pot_y, 3.550), (pot_x, pot_y, 3.560), 0.074, 38, 234, verts=56, rough=.24, bevel=.0012)
    # Moist Dark Potting Humus Mound inside Planter
    sphere('planter_humus_soil', (pot_x, pot_y, 3.682), (0.081, 0.081, 0.016), 39, 234, seg=36, rings=14, rough=.82)
    # Real Scanned CC0 Calathea orbifolia (cluster b + juvenile cluster d for rich natural asymmetric fullness, 0 non-manifold edges!)
    import_cc0_plant('calathea_main', 'calathea_orbifolia_01', ['calathea_orbifolia_01_b'],
                     (pot_x, pot_y, 3.684), scale=0.98, rot_z=math.radians(24.0), mat=29, part=234, rough=.34, thickness=.00075)
    import_cc0_plant('calathea_understory', 'calathea_orbifolia_01', ['calathea_orbifolia_01_d'],
                     (pot_x + 0.015, pot_y - 0.018, 3.686), scale=0.92, rot_z=math.radians(-68.0), mat=29, part=234, rough=.36, thickness=.00075)


def assign_metric_uvs_to_mesh(me, mw, mat, grain_mode='auto', uv_scale=1.0):
    """Assign world-space metric box/planar UVs with grain orientation and float16-safe per-polygon integer centering."""
    tile = max(MAT_TILE_METERS.get(int(mat), 1.0) * max(float(uv_scale), 1e-3), 0.02)
    nmat = mw.to_3x3().inverted().transposed()
    if not me.uv_layers:
        me.uv_layers.new(name='UVMap')
    uv_data = me.uv_layers.active.data
    w_co = [mw @ v.co for v in me.vertices]

    for poly in me.polygons:
        wn = (nmat @ poly.normal).normalized()
        ax = max(range(3), key=lambda k: abs(wn[k]))
        # Blender world axes: 0 = X (Engine X right), 1 = Y (Engine Z depth), 2 = Z (Engine Y vertical up!)
        if ax == 2:
            # Horizontal floor/countertop/ceiling face: map (X, Y_depth)
            raw = [(w_co[me.loops[li].vertex_index].x / tile, w_co[me.loops[li].vertex_index].y / tile) for li in poly.loop_indices]
        elif ax == 1:
            # Front/back wall or drawer face: map (X, Z_up)
            raw = [(w_co[me.loops[li].vertex_index].x / tile, w_co[me.loops[li].vertex_index].z / tile) for li in poly.loop_indices]
        else:
            # Left/right side wall or cabinet side: map (Y_depth, Z_up)
            raw = [(w_co[me.loops[li].vertex_index].y / tile, w_co[me.loops[li].vertex_index].z / tile) for li in poly.loop_indices]

        if grain_mode == 'horizontal':
            # Rotate UV 90 deg so vertical-grain scans (oak mat 26, tile mat 28) run horizontally along X/Y!
            raw = [(rv, -ru) for (ru, rv) in raw]

        # Shift by integer floor per polygon so float16 UVs stay near [0..2] with maximum precision!
        u_off = math.floor(min(r[0] for r in raw))
        v_off = math.floor(min(r[1] for r in raw))
        for li, (ru, rv) in zip(poly.loop_indices, raw):
            uv_data[li].uv = (ru - u_off, rv - v_off)


def finalize_scene_uvs():
    """Ensure every object in `objects` has metric UVs baked onto its base mesh (for both Cycles parity & BMESH export)."""
    for obj in objects:
        if not obj.get('preserve_uv', False):
            assign_metric_uvs_to_mesh(obj.data, obj.matrix_world, int(obj['mat']), obj.get('uv_grain', 'auto'), obj.get('uv_scale', 1.0))


def export(name):
    finalize_scene_uvs()
    groups = defaultdict(lambda: ([], []))
    bad = 0
    tris = 0
    deps = bpy.context.evaluated_depsgraph_get()

    for obj in sorted(objects, key=lambda o: o.name):
        evaluated = obj.evaluated_get(deps)
        me = evaluated.to_mesh()
        mw = obj.matrix_world.copy()
        if not obj.get('preserve_uv', False):
            assign_metric_uvs_to_mesh(me, mw, int(obj['mat']), obj.get('uv_grain', 'auto'), obj.get('uv_scale', 1.0))

        bm = bmesh.new()
        bm.from_mesh(me)
        bad += sum(1 for e in bm.edges if not e.is_manifold)
        bmesh.ops.triangulate(bm, faces=list(bm.faces), quad_method='FIXED', ngon_method='EAR_CLIP')
        bm.to_mesh(me)
        bm.free()

        if not me.uv_layers:
            assign_metric_uvs_to_mesh(me, mw, int(obj['mat']), 'auto', 1.0)

        me.calc_loop_triangles()
        n_tris = len(me.loop_triangles)
        if n_tris == 0:
            evaluated.to_mesh_clear()
            continue

        n_verts = len(me.vertices)
        n_loops = len(me.loops)
        has_thick = 'thick' in me.attributes
        default_thick = float(obj['thick'])
        rough_val = float(obj['rough'])
        mat_id = int(obj['mat'])
        part_id = int(obj['part'])
        smooth_cos = math.cos(math.radians(float(obj.get('smooth_angle', 52.0))))

        # Extract local vertex coords and transform to Blender world space in float64
        v_local = np.empty(n_verts * 3, dtype=np.float64)
        me.vertices.foreach_get('co', v_local)
        v_local = v_local.reshape(n_verts, 3)
        m3 = np.array(mw.to_3x3(), dtype=np.float64)
        t3 = np.array(mw.to_translation(), dtype=np.float64)
        v_world = v_local @ m3.T + t3
        flip_winding = (np.linalg.det(m3) < 0.0)

        # Map Blender right-handed Z-up (x, y, z) -> Engine left-handed Y-up (x, z, y).
        # Because det(P) = -1, reversing loop order (0, 2, 1) restores standard Engine winding!
        v_eng = v_world[:, [0, 2, 1]]

        tri_v = np.empty(n_tris * 3, dtype=np.int32)
        tri_l = np.empty(n_tris * 3, dtype=np.int32)
        tri_p = np.empty(n_tris, dtype=np.int32)
        me.loop_triangles.foreach_get('vertices', tri_v)
        me.loop_triangles.foreach_get('loops', tri_l)
        me.loop_triangles.foreach_get('polygon_index', tri_p)
        tri_v = tri_v.reshape(n_tris, 3)
        tri_l = tri_l.reshape(n_tris, 3)
        if flip_winding:
            tri_v = tri_v[:, [0, 2, 1]]
            tri_l = tri_l[:, [0, 2, 1]]

        poly_smooth = np.empty(len(me.polygons), dtype=bool)
        me.polygons.foreach_get('use_smooth', poly_smooth)
        tri_smooth = poly_smooth[tri_p]

        uv_raw = np.empty(n_loops * 2, dtype=np.float64)
        me.uv_layers.active.data.foreach_get('uv', uv_raw)
        uv_raw = uv_raw.reshape(n_loops, 2)

        if has_thick:
            v_thicks = np.empty(n_verts, dtype=np.float64)
            me.attributes['thick'].data.foreach_get('value', v_thicks)
        else:
            v_thicks = np.full(n_verts, default_thick, dtype=np.float64)

        # Canonical triangle sort order guarantees 100% deterministic float64 reductions
        order = np.lexsort((tri_v[:, 2], tri_v[:, 1], tri_v[:, 0]))
        tri_v = tri_v[order]
        tri_l = tri_l[order]
        tri_smooth = tri_smooth[order]

        # Compute Engine-space area-weighted face normals: note P(a x_B b) = - (P a) x_E (P b)!
        p0 = v_eng[tri_v[:, 0]]
        p1 = v_eng[tri_v[:, 1]]
        p2 = v_eng[tri_v[:, 2]]
        e1 = p1 - p0
        e2 = p2 - p0
        fn_area = -np.cross(e1, e2)
        fn_len = np.linalg.norm(fn_area, axis=1)
        valid = fn_len > 1e-12
        if not np.any(valid):
            evaluated.to_mesh_clear()
            continue

        tri_v = tri_v[valid]
        tri_l = tri_l[valid]
        tri_smooth = tri_smooth[valid]
        p0, p1, p2 = p0[valid], p1[valid], p2[valid]
        e1, e2 = e1[valid], e2[valid]
        fn_area = fn_area[valid]
        fn_len = fn_len[valid]
        fn_unit = fn_area / fn_len[:, None]
        m_tris = len(tri_v)

        # Deterministic Face-Weighted Normals (FWN) with crease-angle splitting:
        # Build per-vertex incident triangle lists in sorted order for exact crease-compatible smoothing.
        vert_tris = defaultdict(list)
        for ti in range(m_tris):
            if tri_smooth[ti]:
                vert_tris[int(tri_v[ti, 0])].append(ti)
                vert_tris[int(tri_v[ti, 1])].append(ti)
                vert_tris[int(tri_v[ti, 2])].append(ti)

        corner_n = np.repeat(fn_unit[:, None, :], 3, axis=1)  # (m_tris, 3, 3)
        if smooth_cos <= -0.999:
            # Full smooth organic surface: fast vectorized area-weighted sum per vertex
            v_nsum = np.zeros((n_verts, 3), dtype=np.float64)
            for c in range(3):
                for k in range(3):
                    np.add.at(v_nsum[:, k], tri_v[:, c], fn_area[:, k])
            n_norms = np.linalg.norm(v_nsum, axis=1, keepdims=True)
            v_nunit = np.where(n_norms > 1e-18, v_nsum / np.maximum(n_norms, 1e-18), np.array([0.0, 1.0, 0.0]))
            for c in range(3):
                sm = tri_smooth
                corner_n[sm, c] = v_nunit[tri_v[sm, c]]
        else:
            for ti in range(m_tris):
                if not tri_smooth[ti]:
                    continue
                n_ref = fn_unit[ti]
                for c in range(3):
                    vi = int(tri_v[ti, c])
                    inc = vert_tris[vi]
                    if len(inc) == 1:
                        corner_n[ti, c] = n_ref
                    else:
                        sx = sy = sz = 0.0
                        for tj in inc:
                            nt = fn_unit[tj]
                            if n_ref[0] * nt[0] + n_ref[1] * nt[1] + n_ref[2] * nt[2] >= smooth_cos:
                                fa = fn_area[tj]
                                sx += fa[0]
                                sy += fa[1]
                                sz += fa[2]
                        ln = math.sqrt(sx * sx + sy * sy + sz * sz)
                        if ln > 1e-18:
                            corner_n[ti, c] = (sx / ln, sy / ln, sz / ln)

        # Compute exact MikkTSpace-equivalent UV-derivative tangents (dP/du, dP/dv_gl) in Engine space
        uv0 = uv_raw[tri_l[:, 0]]
        uv1 = uv_raw[tri_l[:, 1]]
        uv2 = uv_raw[tri_l[:, 2]]
        du1 = uv1[:, 0] - uv0[:, 0]
        dv1 = uv1[:, 1] - uv0[:, 1]
        du2 = uv2[:, 0] - uv0[:, 0]
        dv2 = uv2[:, 1] - uv0[:, 1]
        det_uv = du1 * dv2 - du2 * dv1
        good_uv = np.abs(det_uv) > 1e-11
        inv_det = np.where(good_uv, 1.0 / np.where(good_uv, det_uv, 1.0), 0.0)

        t_tri = (e1 * dv2[:, None] - e2 * dv1[:, None]) * inv_det[:, None]
        b_tri = (e2 * du1[:, None] - e1 * du2[:, None]) * inv_det[:, None]
        t_len = np.linalg.norm(t_tri, axis=1, keepdims=True)
        b_len = np.linalg.norm(b_tri, axis=1, keepdims=True)
        t_tri = np.where(t_len > 1e-14, t_tri / np.maximum(t_len, 1e-14) * fn_len[:, None], 0.0)
        b_tri = np.where(b_len > 1e-14, b_tri / np.maximum(b_len, 1e-14) * fn_len[:, None], 0.0)

        # Accumulate tangents across shared (vertex_index, quantized_uv, quantized_normal) seams
        tb_acc = defaultdict(lambda: [0.0, 0.0, 0.0, 0.0, 0.0, 0.0])
        corner_uvs = np.stack([uv0, uv1, uv2], axis=1)
        corner_keys = []
        for ti in range(m_tris):
            t_v = t_tri[ti]
            b_v = b_tri[ti]
            row_keys = []
            for c in range(3):
                vi = int(tri_v[ti, c])
                u_q = int(round(corner_uvs[ti, c, 0] * 8192.0))
                v_q = int(round(corner_uvs[ti, c, 1] * 8192.0))
                nx_q = int(round(corner_n[ti, c, 0] * 1024.0))
                ny_q = int(round(corner_n[ti, c, 1] * 1024.0))
                nz_q = int(round(corner_n[ti, c, 2] * 1024.0))
                key = (vi, u_q, v_q, nx_q, ny_q, nz_q)
                acc = tb_acc[key]
                acc[0] += t_v[0]; acc[1] += t_v[1]; acc[2] += t_v[2]
                acc[3] += b_v[0]; acc[4] += b_v[1]; acc[5] += b_v[2]
                row_keys.append(key)
            corner_keys.append(row_keys)

        vs, ix = groups[(part_id, mat_id)]

        def sn(x):
            return max(-32767, min(32767, round(x * 32767.0)))

        for ti in range(m_tris):
            tri_packed = []
            for c in range(3):
                vi = int(tri_v[ti, c])
                pos = v_eng[vi]
                n_e = corner_n[ti, c]
                n_l = math.sqrt(n_e[0] * n_e[0] + n_e[1] * n_e[1] + n_e[2] * n_e[2])
                nx, ny, nz = (n_e[0] / n_l, n_e[1] / n_l, n_e[2] / n_l) if n_l > 1e-12 else (0.0, 1.0, 0.0)

                acc = tb_acc[corner_keys[ti][c]]
                tx, ty, tz = acc[0], acc[1], acc[2]
                bx, by, bz = acc[3], acc[4], acc[5]
                # Gram-Schmidt orthogonalize T against N in float64
                ndott = nx * tx + ny * ty + nz * tz
                tx -= nx * ndott
                ty -= ny * ndott
                tz -= nz * ndott
                tl = math.sqrt(tx * tx + ty * ty + tz * tz)
                if tl > 1e-10:
                    tx, ty, tz = tx / tl, ty / tl, tz / tl
                    # cross(N, T) . B determines exact OpenGL (+u, +v_gl) handedness in Engine space!
                    cx_nt = ny * tz - nz * ty
                    cy_nt = nz * tx - nx * tz
                    cz_nt = nx * ty - ny * tx
                    tsign = 1.0 if (cx_nt * bx + cy_nt * by + cz_nt * bz) >= 0.0 else -1.0
                else:
                    # Orthonormal fallback when UV area is degenerate (e.g., pole cap)
                    if abs(ny) < 0.94:
                        # cross((0,1,0), N) = (nz, 0, -nx)
                        fl = math.hypot(nz, nx)
                        tx, ty, tz = nz / fl, 0.0, -nx / fl
                    else:
                        # cross((1,0,0), N) = (0, -nz, ny)
                        fl = math.hypot(nz, ny)
                        tx, ty, tz = 0.0, -nz / fl, ny / fl
                    tsign = 1.0

                u_gl, v_gl = corner_uvs[ti, c]
                # Convert Blender bottom-up v_gl -> Direct3D top-down v_d3d = 1.0 - v_gl
                u_d3d = float(np.clip(u_gl, -64.0, 64.0))
                v_d3d = float(np.clip(1.0 - v_gl, -64.0, 64.0))
                th_v = float(v_thicks[vi])
                packed = struct.pack(
                    '<3f4h4h4e2H',
                    float(pos[0]), float(pos[1]), float(pos[2]),
                    sn(nx), sn(ny), sn(nz), 0,
                    sn(tx), sn(ty), sn(tz), sn(tsign),
                    u_d3d, v_d3d, th_v, rough_val,
                    mat_id, part_id,
                )
                tri_packed.append(packed)

            # Reject triangles that collapse after float32 packing
            a, b, c_pt = [struct.unpack_from('<3f', q) for q in tri_packed]
            e = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            f = (c_pt[0] - a[0], c_pt[1] - a[1], c_pt[2] - a[2])
            cr = (e[1] * f[2] - e[2] * f[1], e[2] * f[0] - e[0] * f[2], e[0] * f[1] - e[1] * f[0])
            if cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2] <= 1e-24:
                continue
            # Reverse corner order (0, 2, 1) for Engine left-handed triangle winding
            base_idx = len(vs)
            vs.extend(tri_packed)
            ix.extend([base_idx, base_idx + 2, base_idx + 1])
            tris += 1

        evaluated.to_mesh_clear()

    verts = []
    indices = []
    table = []
    for (part, mat), (vs, ix) in sorted(groups.items()):
        triangles = []
        for j in range(0, len(ix), 3):
            tri = [vs[ix[j + k]] for k in range(3)]
            pivot = min(range(3), key=lambda k: tri[k])
            tri = tri[pivot:] + tri[:pivot]
            triangles.append(tuple(tri))
        vs = [v for tri in sorted(triangles) for v in tri]
        ix = list(range(len(vs)))
        table.append((len(indices), len(ix), mat, part, len(verts), len(vs)))
        indices.extend(i + len(verts) for i in ix)
        verts.extend(vs)

    coords = [struct.unpack_from('<3f', v) for v in verts]
    lo = [min(v[i] for v in coords) for i in range(3)]
    hi = [max(v[i] for v in coords) for i in range(3)]
    payload = b''.join(struct.pack('<6I', *r) for r in table) + b''.join(verts) + struct.pack('<%dI' % len(indices), *indices)
    header = struct.pack('<8s6I6f2I', b'BMESH001', 1, 64, 40, len(verts), len(indices), len(table), *lo, *hi, zlib.crc32(payload), len(payload))
    data = header + payload
    atomic(OUT / (name + '.bmesh'), data)
    print(name, len(verts), 'vertices', tris, 'triangles', len(table), 'submeshes', bad, 'nonmanifold edges')
    return dict(name=name, vertices=len(verts), triangles=tris, submeshes=len(table), nonmanifold_edges=bad, aabb=[lo, hi], sha256=hashlib.sha256(data).hexdigest())


ALL_KITS = [
    ('shanghai_trestle', shanghai),
    ('shenzhen_evtol', shenzhen),
    ('taipei_101_hero', taipei),
    ('glass_orchid_hero', orchid),
    ('enamel_cathedral', enamel_cathedral),
    ('microbial_sanctuary', microbial_sanctuary),
    ('dawn_alcove', dawn_alcove),
]

def run_all_exports(selected=()):
    global objects, serial
    known_names = {name for name, _ in ALL_KITS}
    if set(selected) - known_names:
        raise ValueError('Unknown mesh kits: ' + ', '.join(sorted(set(selected) - known_names)))
    report_path = OUT / 'generation_report.json'
    existing = {r['name']: r for r in json.loads(report_path.read_bytes())['assets']} if selected else {}
    reports = []
    for name, fn in ALL_KITS:
        if selected and name not in selected:
            continue
        bpy.ops.object.select_all(action='SELECT')
        bpy.ops.object.delete(use_global=False)
        objects = []
        serial = 0
        fn()
        reports.append(export(name))
    existing.update((r['name'], r) for r in reports)
    ordered = [existing[name] for name, _ in ALL_KITS]
    atomic(report_path, (json.dumps(dict(blender=bpy.app.version_string, assets=ordered), indent=2) + '\n').encode())
    manifest_path = ROOT / 'assets/manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    for r in reports:
        aid = 'mesh_' + r['name']
        previous = next((a for a in manifest['assets'] if a['id'] == aid), None)
        if previous is not None:
            previous['sha256'] = r['sha256']
            continue
        prov = (
            'Procedural Blender geometry + solidified PolyHaven CC0 botanical scans generated by tools/gen_trilogy_meshes.py'
            if r['name'] in ('dawn_alcove', 'microbial_sanctuary', 'taipei_101_hero')
            else 'Original procedural Blender geometry generated by tools/gen_trilogy_meshes.py'
        )
        manifest['assets'].append(dict(
            id=aid,
            path='assets/meshes/' + r['name'] + '.bmesh',
            kind='mesh',
            provenance=prov,
            license='CC0-1.0',
            attribution_required=False,
            sha256=r['sha256'],
        ))
    atomic(manifest_path, (json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8'))

if __name__ == '__main__' and '--skip-batch' not in sys.argv:
    # blender -b -P tools/gen_trilogy_meshes.py -- --only shenzhen_evtol taipei_101_hero
    if '--only' in sys.argv:
        names = sys.argv[sys.argv.index('--only') + 1:]
        if not names:
            raise ValueError('--only requires at least one mesh kit name')
        run_all_exports(names)
    else:
        run_all_exports()

