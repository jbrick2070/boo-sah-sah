"""Original procedural trilogy kits. Blender 5.2: --background --python this_file.
BMESH001 v1: 64-byte header, 24-byte submesh records, 40-byte packed vertices.
Coordinates exported Y-up; closed individual solids may overlap intentionally.
No external assets, randomness, network access, or non-Blender dependencies.
"""
import bpy, bmesh, math, struct, zlib, hashlib, json, os
from pathlib import Path
from mathutils import Vector
from collections import defaultdict

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets' / 'meshes'
OUT.mkdir(parents=True, exist_ok=True)
TAU = math.tau
objects = []
serial = 0

def atomic(path, data):
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)

def keep(obj, name, mat, part=0, rough=.3, thick=.03):
    global serial
    serial += 1
    obj.name = '%06d_%s' % (serial, name)
    obj['mat'], obj['part'], obj['rough'], obj['thick'] = mat, part, rough, thick
    objects.append(obj)
    return obj

def box(name, pos, size, mat=0, part=0, bevel=0, rough=.3, rot_z=0.0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=pos)
    obj = bpy.context.object
    obj.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if rot_z:
        obj.rotation_euler.z = rot_z
    if bevel:
        mod = obj.modifiers.new('machined_edges', 'BEVEL'); mod.width=bevel; mod.segments=2
    return keep(obj, name, mat, part, rough=rough)

def sphere(name, pos, scale, mat, part=0, seg=24, rings=12, rough=.25):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=seg, ring_count=rings, radius=1, location=pos)
    obj=bpy.context.object; obj.scale=scale
    for f in obj.data.polygons: f.use_smooth=True
    return keep(obj,name,mat,part,rough=rough)

def rod(name, a, b, radius, mat=0, part=0, verts=12, rough=.3):
    a,b=Vector(a),Vector(b); d=b-a
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=radius, depth=d.length, location=(a+b)*.5)
    obj=bpy.context.object; obj.rotation_mode='QUATERNION'; obj.rotation_quaternion=d.to_track_quat('Z','Y')
    return keep(obj,name,mat,part,rough=rough)

def torus(name,pos,radius,tube,mat=0,part=0,seg=64,rough=.25):
    bpy.ops.mesh.primitive_torus_add(major_segments=seg,minor_segments=8,location=pos,major_radius=radius,minor_radius=tube)
    return keep(bpy.context.object,name,mat,part,rough=rough)

def mesh(name,verts,faces,mat,part=0,smooth=False,rough=.3,thick=.03):
    m=bpy.data.meshes.new(name); m.from_pydata(verts,[],faces); m.update()
    o=bpy.data.objects.new(name,m); bpy.context.collection.objects.link(o)
    for f in m.polygons: f.use_smooth=smooth
    return keep(o,name,mat,part,rough=rough,thick=thick)

def notched(name,z0,z1,w0,w1,mat=5,part=0,ox=0.0,oy=0.0,rough=.22):
    # Double-notched corners, rather than a plain square prism.
    outline=[(-1,-.70),(-.86,-.70),(-.86,-.86),(-.70,-.86),(-.70,-1),(.70,-1),(.70,-.86),(.86,-.86),(.86,-.70),(1,-.70),(1,.70),(.86,.70),(.86,.86),(.70,.86),(.70,1),(-.70,1),(-.70,.86),(-.86,.86),(-.86,.70),(-1,.70)]
    n=len(outline); v=[(ox+x*w,oy+y*w,z) for z,w in [(z0,w0),(z1,w1)] for x,y in outline]
    f=[tuple(reversed(range(n))),tuple(range(n,2*n))]+[(i,(i+1)%n,(i+1)%n+n,i+n) for i in range(n)]
    return mesh(name,v,f,mat,part=part,rough=rough)

def closed_blade(name, nu, nv, eval_surf, mat, part, rough=.07):
    """Watertight 2-manifold botanical shell with automatic outward normals, intrinsic UVs, and thickness."""
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
            row.append((Vector(pos), nrm, th, u, v))
        grid.append(row)
    for side in (-1.0, 1.0):
        for i in range(nu + 1):
            for j in range(nv + 1):
                pos, nrm, th, u, v = grid[i][j]
                p = pos + nrm * (side * 0.5 * th)
                verts.append((p.x, p.y, p.z))
                thicks.append(th)
                uvs_vert.append((u, v))
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
    return keep(o, name, mat, part=part, rough=rough, thick=.03)

def closed_tube(name, centers, radii, mat, part, n_ring=14, rough=.18, thick=.04, flute_amp=0.0, flute_freq=3):
    """Watertight 2-manifold swept botanical stem/pedicel/bud with end-cap poles."""
    ns = len(centers)
    verts = []
    uvs_vert = []
    thicks = []
    frames = []
    for i in range(ns):
        p_prev = Vector(centers[max(0, i - 1)])
        p_next = Vector(centers[min(ns - 1, i + 1)])
        tang = (p_next - p_prev)
        if tang.length < 1e-6:
            tang = Vector((0.0, 0.0, 1.0))
        tang.normalize()
        up_ref = Vector((1.0, 0.0, 0.0)) if abs(tang.x) < 0.85 else Vector((0.0, 1.0, 0.0))
        n1 = tang.cross(up_ref).normalized()
        n2 = tang.cross(n1).normalized()
        frames.append((Vector(centers[i]), tang, n1, n2))
    # Start pole
    c0, t0, _, _ = frames[0]
    verts.append(tuple(c0 - t0 * max(0.003, radii[0] * 0.35)))
    uvs_vert.append((0.0, 0.0))
    thicks.append(thick)
    for i in range(ns):
        u = i / max(1, ns - 1)
        ci, _, n1, n2 = frames[i]
        r_base = max(0.0025, radii[i])
        for j in range(n_ring):
            ang = j * TAU / n_ring
            flute = 1.0 + flute_amp * math.cos(flute_freq * ang) * math.sin(math.pi * u)
            r = r_base * flute
            pt = ci + (n1 * math.cos(ang) + n2 * math.sin(ang)) * r
            verts.append(tuple(pt))
            uvs_vert.append((u, j / n_ring * 2.0 - 1.0))
            thicks.append(thick * (0.7 + 0.3 * flute))
    # End pole
    cn, tn, _, _ = frames[-1]
    end_idx = 1 + ns * n_ring
    verts.append(tuple(cn + tn * max(0.003, radii[-1] * 0.45)))
    uvs_vert.append((1.0, 0.0))
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
    return keep(o, name, mat, part=part, rough=rough, thick=thick)

def shanghai():
    """Shanghai Huangpu River crossing: Waibaidu-inspired curved-chord riveted railway through-truss (y = -54..+6)
    opening onto a low-parapet plate-girder river span (y = +6..+56) with West Bund heritage blocks, river barges,
    and a grounded Pudong / Lujiazui waterfront embankment & landmark quartet.
    Partitioned across spatial part IDs 0..5 for tight DXR BLAS bounding boxes.
    """
    def sh_foliage(tag, bx, by, bz, rx, ry, rz, part_id):
        for k, (dx, dy, dz, sc) in enumerate([(-.24*rx, -.12*ry, .0, .74), (.25*rx, .14*ry, .05*rz, .70), (.0, .0, .20*rz, .78)]):
            sphere('%s_%d' % (tag, k), (bx + dx, by + dy, bz + dz), (rx * sc, ry * sc, rz * sc), 10, part=part_id, seg=10, rings=6, rough=.90)

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

def shenzhen():
    """Shenzhen Bay / Houhai waterfront civic vertiport & varied urban skyline + winged 6-tilt-rotor eVTOL.
    Blender (+X right, +Y forward/away, +Z up) -> Engine (+X right, +Y up, +Z forward/away).
    Static city partitioned across part 0..5; eVTOL airframe = 100, tilt nacelles = 110..115, 3-blade rotors = 120..125.
    """
    def sz_foliage(tag, bx, by, bz, rx, ry, rz, part_id):
        for k, (dx, dy, dz, sc) in enumerate([(-.25*rx, -.12*ry, .0, .74), (.26*rx, .14*ry, .05*rz, .70), (.0, .0, .20*rz, .78)]):
            sphere('%s_%d' % (tag, k), (bx + dx, by + dy, bz + dz), (rx * sc, ry * sc, rz * sc), 10, part=part_id, seg=10, rings=6, rough=.90)

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
    box('coastal_boulevard_ew', (-2.0, -17.5, -0.68), (132.0, 7.6, 0.12), 1, part=1, rough=.14)
    box('boulevard_median', (-2.0, -17.5, -0.58), (126.0, 0.9, 0.16), 2, part=1, rough=.48)
    box('west_avenue_ns', (-25.5, -18.0, -0.66), (6.8, 56.0, 0.12), 1, part=1, rough=.14)
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
    for u_i, (ux, uy, usx, usy, uh, umat, usb, urot, upt) in enumerate(urban_masses):
        box('sz_podium_%d' % u_i, (ux, uy, 1.6), (usx * 1.24, usy * 1.24, 3.8), 2 if (u_i % 2 == 0) else 13, part=upt, bevel=.22, rough=.38, rot_z=urot)
        box('sz_tower_%d' % u_i, (ux, uy, uh * 0.5 - 0.3), (usx, usy, uh), umat, part=upt, bevel=.26, rough=.20, rot_z=urot)
        if usb > 0.1:
            top_h = uh * 0.24
            box('sz_setback_%d' % u_i, (ux, uy, uh + top_h * 0.42 - 0.3), (usx * usb, usy * usb, top_h), umat, part=upt, bevel=.20, rough=.20, rot_z=urot)
            box('sz_crown_%d' % u_i, (ux, uy, uh + top_h * 0.88), (usx * usb * 0.55, usy * usb * 0.55, 0.9), 0, part=upt, rough=.36, rot_z=urot)

    # Part 5: Distant Nanshan / Tanglang Coastal Hills on Horizon
    sz_ridges = [(-58.0, 128.0, 74.0, 26.0, 13.0), (8.0, 136.0, 82.0, 30.0, 15.5), (66.0, 126.0, 72.0, 26.0, 12.0)]
    for hr_i, (hx, hy, hsx, hsy, hsz) in enumerate(sz_ridges):
        n_seg = 6
        mv = []
        for si in range(n_seg + 1):
            t_r = si / n_seg * 2.0 - 1.0
            px = hx + t_r * hsx * 0.5
            hp = (math.sin(math.pi * si / n_seg) ** 0.85) * (0.80 + 0.20 * math.cos(si * 2.1 + hr_i))
            mv.extend([(px, hy - hsy * 0.5, -2.0), (px, hy + hsy * 0.5, -2.0), (px, hy, max(1.5, hsz * hp))])
        mf = [(0, 2, 1), (3 * n_seg, 3 * n_seg + 1, 3 * n_seg + 2)]
        for si in range(n_seg):
            b0, b1 = 3 * si, 3 * (si + 1)
            mf.extend([(b0, b1, b1 + 2, b0 + 2), (b0 + 2, b1 + 2, b1 + 1, b0 + 1), (b0 + 1, b1 + 1, b1, b0)])
        mesh('nanshan_ridge_%d' % hr_i, mv, mf, 19, part=5, smooth=False, rough=.94)

    # Winged 6-Tilt-Rotor Aerospace eVTOL Hero (part = 100 airframe, 110..115 tilt nacelles, 120..125 spinning rotors)
    # Sleek flattened chine fuselage (nose at -Y, tailcone at +Y) — reads from above, side, and three-quarter angles
    fus_pts = []
    fus_rad = []
    for k in range(25):
        tf = k / 24.0
        fy = -2.95 + 6.15 * tf
        fz = -0.04 + 0.22 * (tf ** 1.7) + 0.05 * math.sin(math.pi * tf)
        prof = (math.sin(math.pi * (tf ** 0.68)) ** 0.56) * (1.0 - 0.42 * (tf ** 2.2))
        fus_pts.append((0.0, fy, fz))
        fus_rad.append(max(0.035, 0.68 * prof))
    o_fus = closed_tube('evtol_fuselage', fus_pts, fus_rad, 11, 100, n_ring=24, rough=.15, thick=.06)
    o_fus.scale = (1.12, 1.0, 0.72)
    bpy.ops.object.select_all(action='DESELECT')
    o_fus.select_set(True)
    bpy.context.view_layer.objects.active = o_fus
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

    # Dark carbon lower fuselage chine stripe + wrap-around tinted panoramic canopy + nose landing lights
    box('evtol_carbon_chine', (0.0, -0.35, -0.22), (1.28, 3.60, 0.16), 0, part=100, bevel=.06, rough=.28)
    sphere('evtol_canopy_glass', (0.0, -0.92, 0.24), (0.62, 1.58, 0.38), 18, part=100, seg=32, rings=16, rough=.04)
    box('evtol_cabin_glow', (0.0, -0.78, 0.16), (0.68, 1.30, 0.18), 15, part=100, bevel=.04, rough=.24)
    for sx_n in [-0.24, 0.24]:
        sphere('evtol_nose_lamp', (sx_n, -2.62, -0.06), (.065, .08, .045), 16, part=100, seg=10, rings=6, rough=.12)

    # High swept main wing + twin longitudinal carbon boom rails + V-tail stabilizers + landing gear skids
    for sign_w in [-1.0, 1.0]:
        rod('evtol_main_wing', (0.0, -0.15, 0.24), (sign_w * 4.10, 0.15, 0.36), .11, 11, part=100, verts=12, rough=.18)
        box('evtol_wing_chord', (sign_w * 2.05, 0.0, 0.30), (3.95, 0.56, 0.07), 11, part=100, bevel=.025, rough=.18, rot_z=sign_w * 0.07)
        rod('evtol_boom_rail', (sign_w * 3.00, -1.65, 0.36), (sign_w * 2.65, 1.95, 0.46), .075, 0, part=100, verts=12, rough=.26)
        rod('evtol_boom_brace', (sign_w * 0.62, -0.95, 0.12), (sign_w * 3.00, -1.55, 0.34), .055, 0, part=100, verts=10, rough=.26)
        # Sculpted V-tail fins (proper chorded aerodynamic surfaces, not thin sticks!)
        o_vt = box('evtol_vtail_fin', (sign_w * 0.68, 2.92, 0.62), (0.06, 0.72, 1.18), 11, part=100, bevel=.02, rough=.18)
        o_vt.rotation_euler = (-0.22, sign_w * 0.62, 0.0)
        rod('evtol_skid_tube', (sign_w * 0.78, -1.45, -0.66), (sign_w * 0.78, 1.25, -0.66), .044, 0, part=100, verts=10, rough=.30)
        for sy_s in [-0.88, 0.72]:
            rod('evtol_skid_strut', (sign_w * 0.48, sy_s, -0.16), (sign_w * 0.78, sy_s, -0.66), .036, 0, part=100, verts=8, rough=.30)

    # Six articulated tilt-rotor nacelles (110..115) & counter-rotating 5-blade scimitar + motion-blur rotors (120..125)
    rotor_pivots = [
        (-3.05, -1.65, 0.36),
        ( 3.05, -1.65, 0.36),
        (-4.10,  0.15, 0.40),
        ( 4.10,  0.15, 0.40),
        (-2.60,  1.95, 0.46),
        ( 2.60,  1.95, 0.46),
    ]
    for i, (bx, by, bz) in enumerate(rotor_pivots):
        sphere('tilt_nacelle_%d' % i, (bx, by, bz), (.18, .34, .24), 11, part=110 + i, seg=18, rings=10, rough=.18)
        rod('rotor_mast_%d' % i, (bx, by, bz), (bx, by, bz + 0.32), .058, 0, part=110 + i, verts=10, rough=.24)
        sphere('rotor_spinner_%d' % i, (bx, by, bz + 0.36), (.10, .10, .13), 0, part=120 + i, seg=14, rings=8, rough=.16)
        lamp_mat = 16 if (i < 2) else 6
        sphere('nav_strobe_%d' % i, (bx * 1.02, by, bz - 0.12), (.05, .05, .05), lamp_mat, part=110 + i, seg=8, rings=6, rough=.15)
        # 3 pitched carbon-titanium aerofoil blades + brushed-titanium leading edge + amber tip marker
        rot_dir = -1.0 if (i % 2 == 1) else 1.0
        for j in range(3):
            ang = j * TAU / 3.0
            cx = bx + 0.49 * math.cos(ang)
            cy = by + 0.49 * math.sin(ang)
            o_bl = box('scimitar_blade_%d_%d' % (i, j), (cx, cy, bz + 0.32), (.92, .110, .020), 0, part=120 + i, bevel=.006, rough=.18)
            o_bl.rotation_euler = (0.24 * rot_dir, 0.0, ang)
            # Brushed titanium leading-edge erosion strip
            le_off = 0.046 * rot_dir
            lx_b = cx - le_off * math.sin(ang)
            ly_b = cy + le_off * math.cos(ang)
            o_le = box('blade_le_strip_%d_%d' % (i, j), (lx_b, ly_b, bz + 0.328), (.86, .022, .016), 11, part=120 + i, rough=.14)
            o_le.rotation_euler = (0.24 * rot_dir, 0.0, ang)
            # Safety-gold blade tip cap
            tx_b = bx + 0.91 * math.cos(ang)
            ty_b = by + 0.91 * math.sin(ang)
            o_tp = box('blade_tip_cap_%d_%d' % (i, j), (tx_b, ty_b, bz + 0.32), (.08, .102, .021), 8, part=120 + i, rough=.20)
            o_tp.rotation_euler = (0.24 * rot_dir, 0.0, ang)

def taipei():
    # Meters, retained at 508 m in the asset. Runtime places tower at world (18.5, 0.0, 60.0) with scale 0.080.
    # Spatial part IDs (0..5) partition Xinyi into disjoint BLAS bounding volumes for fast DXR traversal.
    def w2b(wx, wy, wz):
        return ((wx - 18.5) / .08, (wz - 60.0) / .08, wy / .08)
    def ws(sx, sy, sz):
        return (sx / .08, sz / .08, sy / .08)
    def foliage_cluster(tag, wx, wy, wz, rx, ry, rz, part_id):
        offsets = [(-0.26 * rx, 0.0, -0.14 * rz, 0.72), (0.28 * rx, 0.06 * ry, 0.16 * rz, 0.68), (0.02 * rx, 0.18 * ry, -0.04 * rz, 0.76)]
        for k, (dx, dy, dz, sc) in enumerate(offsets):
            sphere('%s_%d' % (tag, k), w2b(wx + dx, wy + dy, wz + dz), ws(rx * sc, ry * sc, rz * sc), 10, part=part_id, seg=10, rings=6, rough=.90)

    # Part 0: Taipei 101 Hero Tower (0..508 m true vertical scale) + Shopping Mall Podium
    notched('battered_podium', 0, 112, 38, 29, 5, part=0)
    box('taipei_mall_wing', (-66, -12, 14), (72, 62, 28), 13, part=0, bevel=.6, rough=.35)
    box('taipei_mall_skylight', (-66, -12, 29.2), (52, 20, 2.4), 15, part=0, bevel=.3, rough=.22)
    box('taipei_plaza_apron', (-18, -8, 1.0), (190, 150, 2.0), 2, part=0, rough=.42)

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

    # Open 88F-91F Observatory Lantern framing the true-scale 5.5 m (r = 2.75 m) golden 41-plate TMD sphere
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

    for k in range(4):
        notched('stepped_spire', 430 + k * 12, 442 + k * 12, 15.5 - k * 2.8, 13.5 - k * 2.8, 5, part=0, rough=.18)
        notched('spire_collar_glow', 441.3 + k * 12, 441.9 + k * 12, 13.6 - k * 2.8, 13.6 - k * 2.8, 15, part=0, rough=.22)
    rod('508m_pinnacle', (0, 0, 478), (0, 0, 508), .62, 8, part=0, rough=.18)
    sphere('spire_beacon', (0, 0, 508.2), (.48, .48, .48), 16, part=0, seg=12, rings=6, rough=.15)

    # Part 5: Taipei Basin Rain-Wet Ground, Xinyi Boulevards & Distant Basin Mountain Ridges
    box('taipei_basin_ground', w2b(12.0, -.45, 62.0), ws(290.0, .90, 280.0), 2, part=5, rough=.68)
    for av_x, av_w in [(-6.0, 2.8), (6.5, 3.2), (29.5, 3.0)]:
        box('xinyi_avenue_ns', w2b(av_x, .02, 54.0), ws(av_w, .05, 96.0), 1, part=5, rough=.10)
        for s_i in range(9):
            wz = 16.0 + s_i * 8.6
            for side in [-1, 1]:
                lx = av_x + side * (av_w * .60)
                foliage_cluster('street_tree_%d_%d' % (s_i, 1 if side > 0 else 0), lx, .34, wz + 1.8, .48, .38, .48, 5)
                box('street_lamp_pole', w2b(lx, .30, wz), ws(.04, .60, .04), 0, part=5, rough=.35)
                box('street_lamp_glow', w2b(lx - side * .06, .58, wz), ws(.14, .05, .18), 16, part=5, rough=.2)
    for av_z, av_w in [(18.5, 2.8), (36.5, 3.2), (50.5, 2.6), (74.0, 3.2)]:
        box('xinyi_avenue_ew', w2b(12.0, .02, av_z), ws(110.0, .05, av_w), 1, part=5, rough=.10)

    # Sculpted dark mountain ridge silhouettes rimming the Taipei Basin far behind the skyline
    ridge_defs = [
        (-48.0, 146.0, 62.0, 10.5, 24.0, 0.08),
        (2.0,   154.0, 68.0, 13.5, 28.0, -0.05),
        (52.0,  144.0, 64.0, 11.8, 26.0, 0.06),
    ]
    for r_idx, (rx, rz, rsx, rsy, rsz, rrot) in enumerate(ridge_defs):
        # Ridged triangular-prism mountain massif (not a green UV sphere!)
        bx, by, _ = w2b(rx, 0.0, rz)
        bsx, bsy, bsz = ws(rsx, rsy, rsz)
        n_seg = 6
        mv = []
        for si in range(n_seg + 1):
            t_r = si / n_seg * 2.0 - 1.0
            px = bx + t_r * bsx * 0.5
            h_prof = math.sin(math.pi * (si / n_seg)) ** 0.85 * (0.78 + 0.22 * math.cos(si * 2.3 + r_idx))
            mv.extend([
                (px, by - bsy * 0.5, -8.0),
                (px, by + bsy * 0.5, -8.0),
                (px, by + bsy * 0.08 * math.sin(si * 1.7), max(4.0, bsz * h_prof)),
            ])
        mf = [(0, 2, 1), (3 * n_seg, 3 * n_seg + 1, 3 * n_seg + 2)]
        for si in range(n_seg):
            b0, b1 = 3 * si, 3 * (si + 1)
            mf.extend([(b0, b1, b1 + 2, b0 + 2), (b0 + 2, b1 + 2, b1 + 1, b0 + 1), (b0 + 1, b1 + 1, b1, b0)])
        o = mesh('basin_ridge_%d' % r_idx, mv, mf, 19, part=5, smooth=False, rough=.95)
        o.rotation_euler.z = rrot

    # Part 2: Mid-ground Xinyi Commercial Podiums & Covered Steel-and-Glass Pedestrian Skywalks
    podium_blocks = [
        (0.5, 25.5, 8.2, 2.4, 7.5, 13, True),
        (12.8, 26.0, 8.8, 2.1, 8.0, 14, True),
        (22.5, 27.0, 6.8, 2.6, 7.2, 13, True),
        (1.0, 43.5, 7.8, 3.1, 7.8, 14, True),
        (12.6, 43.2, 8.4, 2.5, 7.4, 13, True),
        (22.8, 43.8, 6.6, 3.0, 7.0, 14, False),
    ]
    for p_i, (wx, wz, sx, sy, sz, mat, gal) in enumerate(podium_blocks):
        box('xinyi_podium_%d' % p_i, w2b(wx, sy * .5, wz), ws(sx, sy, sz), mat, part=2, bevel=.35, rough=.32)
        box('podium_roof_terrace_%d' % p_i, w2b(wx, sy + .08, wz), ws(sx * .84, .14, sz * .84), 2, part=2, rough=.52)
        foliage_cluster('podium_greenery_%d' % p_i, wx - sx * .22, sy + .22, wz, sx * .16, .18, sz * .16, 2)
        if gal:
            box('podium_arcade_glow_%d' % p_i, w2b(wx, .34, wz - sz * .48), ws(sx * .72, .44, .18), 15, part=2, rough=.24)

    skywalks = [
        (6.6, 25.8, 4.4, .18, .82),
        (17.8, 26.5, 3.6, .18, .82),
        (6.8, 43.4, 4.2, .18, .82),
        (12.7, 35.0, .86, .18, 9.8),
        (0.8, 34.5, .82, .18, 9.5),
    ]
    for sw_i, (wx, wz, sx, sy, sz) in enumerate(skywalks):
        box('xinyi_skywalk_deck_%d' % sw_i, w2b(wx, .96, wz), ws(sx, sy, sz), 0, part=2, bevel=.04, rough=.32)
        box('xinyi_skywalk_parapet_%d' % sw_i, w2b(wx, 1.14, wz), ws(sx * .98, .22, sz * .96), 14, part=2, rough=.18)
        box('xinyi_skywalk_canopy_%d' % sw_i, w2b(wx, 1.30, wz), ws(sx, .08, sz), 0, part=2, rough=.30)
        box('xinyi_skywalk_soffit_%d' % sw_i, w2b(wx, .85, wz), ws(sx * .88, .04, sz * .82), 15, part=2, rough=.25)

    # Part 3 (Left) & Part 4 (Right/North): Varied Xinyi District Towers (never crowding 101's silhouette)
    district_towers = [
        (-26.0, 34.0, 5.4, 11.5, 5.2, 14, .12, 3),
        (-18.5, 28.0, 4.6, 8.2, 4.8, 13, .0, 3),
        (-13.5, 39.5, 5.2, 14.8, 4.8, 14, .06, 3),
        (-22.0, 52.0, 6.0, 17.5, 5.6, 13, -.08, 3),
        (-11.5, 58.0, 4.8, 12.2, 5.0, 14, .0, 3),
        (-4.2, 62.5, 4.4, 9.6, 4.4, 13, .05, 3),
        (3.8, 68.0, 4.8, 11.4, 4.6, 14, .0, 4),
        (34.5, 44.0, 5.0, 13.8, 4.8, 14, -.10, 4),
        (36.0, 63.0, 5.8, 21.5, 4.6, 14, .16, 4),
        (44.5, 54.0, 5.2, 15.2, 5.2, 13, .0, 4),
        (48.0, 72.0, 6.2, 18.4, 5.6, 14, -.06, 4),
        (-16.0, 80.0, 5.8, 15.5, 5.4, 14, .0, 3),
        (-3.0, 84.0, 5.2, 13.0, 5.0, 13, .08, 3),
        (9.5, 86.0, 5.0, 10.8, 4.8, 14, .0, 4),
        (27.5, 84.0, 5.4, 14.6, 5.2, 13, -.05, 4),
    ]
    for idx_t, (wx, wz, sx, sy, sz, mat, rot, pt) in enumerate(district_towers):
        box('district_base_%d' % idx_t, w2b(wx, 1.1, wz), ws(sx * 1.28, 2.2, sz * 1.28), 2 if idx_t % 2 else 13, part=pt, bevel=.3, rough=.38, rot_z=rot)
        box('district_shaft_%d' % idx_t, w2b(wx, sy * .5, wz), ws(sx, sy, sz), mat, part=pt, bevel=.35, rough=.22, rot_z=rot)
        if sy > 11.0:
            upper_h = sy * .26
            box('district_setback_%d' % idx_t, w2b(wx, sy + upper_h * .45, wz), ws(sx * .72, upper_h, sz * .72), mat, part=pt, bevel=.25, rough=.22, rot_z=rot)
            box('district_mech_%d' % idx_t, w2b(wx, sy + upper_h + .35, wz), ws(sx * .42, .70, sz * .42), 0, part=pt, rough=.40, rot_z=rot)
        else:
            box('district_mech_%d' % idx_t, w2b(wx, sy + .35, wz), ws(sx * .52, .70, sz * .52), 0, part=pt, rough=.40, rot_z=rot)

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
        box('sanctuary_planter_box', w2b(px, 2.74, 7.3), ws(2.4, .26, 1.5), 2, part=1, bevel=.05, rough=.45)
        foliage_cluster('sanctuary_fern_%d' % (1 if side > 0 else 0), px, 2.94, 7.3, .85, .24, .52, 1)
    box('orchid_altar_base', w2b(0.0, 2.74, 5.86), ws(1.45, .22, 1.45), 0, part=1, bevel=.06, rough=.18)
    box('orchid_altar_plinth', w2b(0.0, 2.94, 5.86), ws(.78, .26, .78), 2, part=1, bevel=.05, rough=.28)
    torus('orchid_brass_collar', w2b(0.0, 3.08, 5.86), .24 / .08, .045 / .08, 8, part=1, seg=32, rough=.18)

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
            # Compound saucer curvature + 3 broad, smooth botanical vein flutes for natural specular ribbons
            saucer = -0.15 * (v * v) * claw_flare * math.sin(math.pi * u * 0.92)
            vein_flutes = 0.018 * math.cos(v * math.pi * 2.5) * claw_flare * math.sin(math.pi * u)
            rib_ridge = 0.028 * math.exp(-16.0 * v * v) * (1.0 - 0.45 * u)
            margin_roll = 0.024 * (abs(v) ** 3.2) * claw_flare * math.sin(math.pi * u)
            forward_arc = -0.02 - 0.22 * math.sin(math.pi * 0.72 * u) + 0.05 * (u ** 2.4)
            y = forward_arc + saucer + vein_flutes + rib_ridge + margin_roll
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

def export(name):
    groups=defaultdict(lambda: ([],[])); bad=0; tris=0
    deps=bpy.context.evaluated_depsgraph_get()
    for obj in sorted(objects,key=lambda o:o.name):
        evaluated=obj.evaluated_get(deps); me=evaluated.to_mesh()
        bm=bmesh.new();bm.from_mesh(me);bad+=sum(1 for e in bm.edges if not e.is_manifold);bmesh.ops.triangulate(bm,faces=list(bm.faces),quad_method='FIXED',ngon_method='EAR_CLIP');bm.to_mesh(me);bm.free()
        if not me.uv_layers:
            me.uv_layers.new(name='UVMap')
            for poly in me.polygons:
                axis=max(range(3),key=lambda i:abs(poly.normal[i]));axes=[i for i in range(3) if i!=axis]
                for li in poly.loop_indices:
                    v=me.vertices[me.loops[li].vertex_index].co
                    me.uv_layers.active.data[li].uv=(v[axes[0]],v[axes[1]])
        me.calc_loop_triangles()
        has_thick = 'thick' in me.attributes
        # Accumulate normals in a fixed triangle order using Python float64.
        # Blender's parallel normal/tangent reductions can differ by one snorm LSB.
        sums=[[0.,0.,0.] for _ in me.vertices]; face_normals={}
        for tri in sorted(me.loop_triangles,key=lambda t:tuple(t.vertices)):
            a,b,c=[tuple(me.vertices[i].co) for i in tri.vertices]
            e=[b[i]-a[i] for i in range(3)];f=[c[i]-a[i] for i in range(3)]
            cross=(e[1]*f[2]-e[2]*f[1],e[2]*f[0]-e[0]*f[2],e[0]*f[1]-e[1]*f[0])
            face_normals[tri.index]=cross
            for vi in tri.vertices:
                for k in range(3):sums[vi][k]+=cross[k]
        normalmat=obj.matrix_world.to_3x3().inverted().transposed()
        vs,ix=groups[(int(obj['part']),int(obj['mat']))]
        for tri in me.loop_triangles:
            if sum(x*x for x in face_normals[tri.index]) < 1e-20: continue
            for li in tri.loops:
                loop=me.loops[li];v=obj.matrix_world@me.vertices[loop.vertex_index].co
                local_n=Vector(sums[loop.vertex_index] if me.polygons[tri.polygon_index].use_smooth else face_normals[tri.index]).normalized()
                n=(normalmat@local_n).normalized()
                axis=max(range(3),key=lambda k:abs(me.polygons[tri.polygon_index].normal[k]));axes=[k for k in range(3) if k!=axis]
                local_u=Vector(tuple(1. if k==axes[0] else 0. for k in range(3)))
                local_v=Vector(tuple(1. if k==axes[1] else 0. for k in range(3)))
                local_t=(local_u-local_n*local_n.dot(local_u)).normalized()
                tangent_sign=1. if local_n.cross(local_t).dot(local_v)>=0 else -1.
                t=(obj.matrix_world.to_3x3()@local_t).normalized()
                uv=me.uv_layers.active.data[li].uv
                v_thick = me.attributes['thick'].data[loop.vertex_index].value if has_thick else float(obj['thick'])
                # Blender Z-up -> engine Y-up. Reflection of basis reverses winding/sign.
                def sn(x):return max(-32767,min(32767,round(x*32767)))
                packed=struct.pack('<3f4h4h4e2H',v.x,v.z,v.y,sn(n.x),sn(n.z),sn(n.y),0,sn(t.x),sn(t.z),sn(t.y),sn(-tangent_sign),uv.x,uv.y,v_thick,obj['rough'],int(obj['mat']),int(obj['part']))
                ix.append(len(vs));vs.append(packed)
            # Reject triangles that collapse after world transform / float32 packing.
            a,b,c=[struct.unpack_from('<3f',q) for q in vs[-3:]]
            e=[b[k]-a[k] for k in range(3)];f=[c[k]-a[k] for k in range(3)]
            cross=(e[1]*f[2]-e[2]*f[1],e[2]*f[0]-e[0]*f[2],e[0]*f[1]-e[1]*f[0])
            if sum(q*q for q in cross)<=1e-24:
                del ix[-3:];del vs[-3:];continue
            ix[-3:]=[ix[-3],ix[-1],ix[-2]];tris+=1
        evaluated.to_mesh_clear()
    verts=[];indices=[];table=[]
    for (part,mat),(vs,ix) in sorted(groups.items()):
        # Canonical triangle order removes allocator-dependent BMesh face ordering.
        triangles=[]
        for j in range(0,len(ix),3):
            tri=[vs[ix[j+k]] for k in range(3)]
            pivot=min(range(3),key=lambda k:tri[k]);tri=tri[pivot:]+tri[:pivot]
            triangles.append(tuple(tri))
        vs=[v for tri in sorted(triangles) for v in tri];ix=list(range(len(vs)))
        table.append((len(indices),len(ix),mat,part,len(verts),len(vs)))
        indices.extend(i+len(verts) for i in ix);verts.extend(vs)
    coords=[struct.unpack_from('<3f',v) for v in verts]
    lo=[min(v[i] for v in coords) for i in range(3)];hi=[max(v[i] for v in coords) for i in range(3)]
    payload=b''.join(struct.pack('<6I',*r) for r in table)+b''.join(verts)+struct.pack('<%dI'%len(indices),*indices)
    header=struct.pack('<8s6I6f2I',b'BMESH001',1,64,40,len(verts),len(indices),len(table),*lo,*hi,zlib.crc32(payload),len(payload))
    data=header+payload;atomic(OUT/(name+'.bmesh'),data)
    print(name,len(verts),'vertices',tris,'triangles',len(table),'submeshes',bad,'nonmanifold edges')
    return dict(name=name,vertices=len(verts),triangles=tris,submeshes=len(table),nonmanifold_edges=bad,aabb=[lo,hi],sha256=hashlib.sha256(data).hexdigest())

reports=[]
for name,fn in [('shanghai_trestle',shanghai),('shenzhen_evtol',shenzhen),('taipei_101_hero',taipei),('glass_orchid_hero',orchid)]:
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False);objects=[];serial=0
    fn();reports.append(export(name))
atomic(OUT/'generation_report.json',(json.dumps(dict(blender=bpy.app.version_string,assets=reports),indent=2)+'\n').encode())
manifest_path=ROOT/'assets/manifest.json';manifest=json.loads(manifest_path.read_text(encoding='utf-8'))
for r in reports:
    aid='mesh_'+r['name'];manifest['assets']=[a for a in manifest['assets'] if a['id']!=aid]
    manifest['assets'].append(dict(id=aid,path='assets/meshes/'+r['name']+'.bmesh',kind='mesh',provenance='Original procedural Blender geometry generated by tools/gen_trilogy_meshes.py',license='CC0-1.0',attribution_required=False,sha256=r['sha256']))
atomic(manifest_path,(json.dumps(manifest,indent=2,ensure_ascii=False)+'\n').encode('utf-8'))
