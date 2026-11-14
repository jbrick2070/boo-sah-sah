"""Render 2560x1440 Blender 5.2 Cycles path-traced reference views of the Dawn Bathroom
hero scene at t = 342.0s and t = 355.0s using the exact 3D geometry, metric UVs,
42-slot PBR textures, lighting direction, and camera frusta as the D3D12/DXR runtime.

Usage:
  blender.exe --background --factory-startup --python tools/render_cycles_reference.py -- --skip-batch
"""
import math
import os
import sys
from pathlib import Path

import bpy
from mathutils import Euler, Matrix, Vector

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

if '--skip-batch' not in sys.argv:
    sys.argv.append('--skip-batch')

import gen_trilogy_meshes as gtm  # noqa: E402

MAPS_DIR = ROOT / 'build' / 'pbr_cycles_maps'
OUT_DIR = ROOT / 'build' / 'visual_proof'
EXR_SKY = ROOT / 'third_party' / 'polyhaven_cc0' / 'hdris' / 'qwantani_dawn_puresky_1k.exr'


def smootherstep01(x: float) -> float:
    t = max(0.0, min(1.0, x))
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def eval_dawn_camera_engine(t_sec: float):
    """Exact replica of evaluateShowCamera(FullFilm360s) in src/render/m1_scene.cpp for t >= 342s."""
    s_care = smootherstep01((t_sec - 335.5) / 6.3)
    s_finale = smootherstep01((t_sec - 342.0) / 10.0)
    hold_drift = smootherstep01((t_sec - 352.0) / 8.0) * 0.025

    care_eye = (-0.58, 3.88, 4.38)
    care_tgt = (-0.06, 3.84, 6.38)
    fin_eye = (-0.36 + hold_drift * 0.6, 4.08 + hold_drift * 0.2, 3.48 - hold_drift * 0.5)
    fin_tgt = (-0.28, 4.06, 6.55)

    # Since t_sec >= 342.0, s_care == 1.0 identically
    assert abs(s_care - 1.0) < 1e-9
    eye = tuple(care_eye[i] + (fin_eye[i] - care_eye[i]) * s_finale for i in range(3))
    tgt = tuple(care_tgt[i] + (fin_tgt[i] - care_tgt[i]) * s_finale for i in range(3))
    fov_y = 0.58 + (0.66 - 0.58) * s_finale
    return eye, tgt, fov_y


def eng_to_bl(v):
    """Map Engine left-handed Y-up (x, y, z) -> Blender right-handed Z-up (x, z, y)."""
    return Vector((v[0], v[2], v[1]))


def build_cycles_material(mat_id: int, is_droplet: bool = False) -> bpy.types.Material:
    key = f'boo_mat_{mat_id:02d}' + ('_droplet' if is_droplet else '')
    if key in bpy.data.materials:
        return bpy.data.materials[key]

    mat = bpy.data.materials.new(name=key)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()

    out = nt.nodes.new('ShaderNodeOutputMaterial')
    out.location = (780, 0)

    # Architectural thin float glass (mat 40): low-reflectance AR float glass with transparent shadow rays
    if mat_id == 40:
        fres = nt.nodes.new('ShaderNodeFresnel')
        fres.inputs['IOR'].default_value = 1.51
        fres.location = (0, 180)

        math_mul = nt.nodes.new('ShaderNodeMath')
        math_mul.operation = 'MULTIPLY'
        math_mul.inputs[1].default_value = 0.52
        math_mul.location = (180, 180)
        nt.links.new(fres.outputs['Fac'], math_mul.inputs[0])

        transp = nt.nodes.new('ShaderNodeBsdfTransparent')
        transp.inputs['Color'].default_value = (0.985, 0.995, 0.990, 1.0)
        transp.location = (180, 20)

        gloss = nt.nodes.new('ShaderNodeBsdfGlossy')
        gloss.inputs['Color'].default_value = (0.96, 0.99, 0.98, 1.0)
        gloss.inputs['Roughness'].default_value = 0.008
        gloss.location = (180, -140)

        mix_vis = nt.nodes.new('ShaderNodeMixShader')
        mix_vis.location = (420, 40)
        nt.links.new(math_mul.outputs['Value'], mix_vis.inputs['Fac'])
        nt.links.new(transp.outputs['BSDF'], mix_vis.inputs[1])
        nt.links.new(gloss.outputs['BSDF'], mix_vis.inputs[2])

        lp = nt.nodes.new('ShaderNodeLightPath')
        lp.location = (180, 340)
        mix_sh = nt.nodes.new('ShaderNodeMixShader')
        mix_sh.location = (600, 80)
        nt.links.new(lp.outputs['Is Shadow Ray'], mix_sh.inputs['Fac'])
        nt.links.new(mix_vis.outputs['Shader'], mix_sh.inputs[1])
        nt.links.new(transp.outputs['BSDF'], mix_sh.inputs[2])
        nt.links.new(mix_sh.outputs['Shader'], out.inputs['Surface'])
        return mat

    bsdf = nt.nodes.new('ShaderNodeBsdfPrincipled')
    bsdf.location = (420, 0)
    nt.links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])

    alb_path = MAPS_DIR / f'mat_{mat_id:02d}_albedo.png'
    nor_path = MAPS_DIR / f'mat_{mat_id:02d}_normal.png'
    orm_path = MAPS_DIR / f'mat_{mat_id:02d}_ormt.png'

    tex_alb = nt.nodes.new('ShaderNodeTexImage')
    tex_alb.image = bpy.data.images.load(str(alb_path), check_existing=True)
    tex_alb.image.colorspace_settings.name = 'sRGB'
    tex_alb.location = (-520, 240)

    tex_orm = nt.nodes.new('ShaderNodeTexImage')
    tex_orm.image = bpy.data.images.load(str(orm_path), check_existing=True)
    tex_orm.image.colorspace_settings.name = 'Non-Color'
    tex_orm.location = (-520, -40)

    tex_nor = nt.nodes.new('ShaderNodeTexImage')
    tex_nor.image = bpy.data.images.load(str(nor_path), check_existing=True)
    tex_nor.image.colorspace_settings.name = 'Non-Color'
    tex_nor.location = (-520, -320)

    sep_orm = nt.nodes.new('ShaderNodeSeparateColor')
    sep_orm.location = (-220, -40)
    nt.links.new(tex_orm.outputs['Color'], sep_orm.inputs['Color'])

    # Micro-cavity AO multiply onto Base Color
    mix_ao = nt.nodes.new('ShaderNodeMix')
    mix_ao.data_type = 'RGBA'
    mix_ao.blend_type = 'MULTIPLY'
    mix_ao.inputs['Factor'].default_value = 0.45
    mix_ao.location = (60, 220)
    nt.links.new(tex_alb.outputs['Color'], mix_ao.inputs['A'])
    nt.links.new(sep_orm.outputs['Red'], mix_ao.inputs['B'])
    nt.links.new(mix_ao.outputs['Result'], bsdf.inputs['Base Color'])

    nt.links.new(sep_orm.outputs['Green'], bsdf.inputs['Roughness'])
    nt.links.new(sep_orm.outputs['Blue'], bsdf.inputs['Metallic'])

    nmap = nt.nodes.new('ShaderNodeNormalMap')
    nmap.location = (60, -280)
    nmap.inputs['Strength'].default_value = 0.03 if mat_id == 34 else 1.0
    nt.links.new(tex_nor.outputs['Color'], nmap.inputs['Color'])
    nt.links.new(nmap.outputs['Normal'], bsdf.inputs['Normal'])

    # Cutout botanical foliage (mat 29 Calathea, mat 36 Fern)
    if mat_id in (29, 36):
        nt.links.new(tex_alb.outputs['Alpha'], bsdf.inputs['Alpha'])
        if 'Subsurface Weight' in bsdf.inputs:
            bsdf.inputs['Subsurface Weight'].default_value = 0.42
            bsdf.inputs['Subsurface Scale'].default_value = 0.008

    # Glazed ceramics, porcelain, celadon & marble clearcoat
    if mat_id in (25, 27, 28, 31) and 'Coat Weight' in bsdf.inputs:
        bsdf.inputs['Coat Weight'].default_value = 0.35 if mat_id == 31 else 0.85
        bsdf.inputs['Coat Roughness'].default_value = 0.05

    # Silvered flat bathroom mirror
    if mat_id == 34:
        bsdf.inputs['Base Color'].default_value = (0.95, 0.96, 0.95, 1.0)
        bsdf.inputs['Metallic'].default_value = 1.0
        bsdf.inputs['Roughness'].default_value = 0.005

    # Refractive dielectric crystal / amber glass / water droplets
    if mat_id in (3, 4, 35):
        if 'Transmission Weight' in bsdf.inputs:
            bsdf.inputs['Transmission Weight'].default_value = 0.96
        bsdf.inputs['Metallic'].default_value = 0.0
        bsdf.inputs['Roughness'].default_value = 0.015 if is_droplet else (0.038 if mat_id == 35 else 0.04)
        bsdf.inputs['IOR'].default_value = 1.333 if is_droplet else (1.52 if mat_id == 35 else 1.495)
        if is_droplet:
            bsdf.inputs['Base Color'].default_value = (0.98, 0.995, 1.0, 1.0)
        elif mat_id == 35:
            bsdf.inputs['Base Color'].default_value = (0.76, 0.34, 0.06, 1.0)
        elif mat_id == 4:
            bsdf.inputs['Base Color'].default_value = (0.82, 0.96, 0.90, 1.0)
        else:
            bsdf.inputs['Base Color'].default_value = (0.96, 0.98, 0.99, 1.0)

    # Warm architectural bathroom lighting strips / opal sconce / mirror halo
    if mat_id in (15, 16, 6) and 'Emission Color' in bsdf.inputs:
        if mat_id == 15:
            bsdf.inputs['Emission Color'].default_value = (1.0, 0.72, 0.42, 1.0)
            bsdf.inputs['Emission Strength'].default_value = 3.2
        elif mat_id == 16:
            bsdf.inputs['Emission Color'].default_value = (1.0, 0.58, 0.18, 1.0)
            bsdf.inputs['Emission Strength'].default_value = 2.5
        else:
            bsdf.inputs['Emission Color'].default_value = (0.12, 0.88, 0.68, 1.0)
            bsdf.inputs['Emission Strength'].default_value = 1.8

    return mat


def apply_kit_transform_and_materials(objs, scale=1.0, loc_bl=(0.0, 0.0, 0.0), rot_z_bl=0.0, allowed_parts=None):
    gtm.finalize_scene_uvs()
    xf = Matrix.Translation(Vector(loc_bl)) @ Euler((0.0, 0.0, rot_z_bl), 'XYZ').to_matrix().to_4x4() @ Matrix.Scale(scale, 4)
    kept = []
    for o in list(objs):
        pt = int(o.get('part', 0))
        if allowed_parts is not None and pt not in allowed_parts:
            bpy.data.objects.remove(o, do_unlink=True)
            continue
        if scale != 1.0 or loc_bl != (0.0, 0.0, 0.0) or rot_z_bl != 0.0:
            o.matrix_world = xf @ o.matrix_world
        mat_id = int(o.get('mat', 0))
        is_drop = ('droplet' in o.name)
        cmat = build_cycles_material(mat_id, is_droplet=is_drop)
        o.data.materials.clear()
        o.data.materials.append(cmat)
        kept.append(o)
    return kept


def setup_world_and_sun(scene: bpy.types.Scene):
    world = bpy.data.worlds.new('DawnPenthouseWorld')
    scene.world = world
    world.use_nodes = True
    nt = world.node_tree
    nt.nodes.clear()

    out = nt.nodes.new('ShaderNodeOutputWorld')
    out.location = (520, 0)
    bg = nt.nodes.new('ShaderNodeBackground')
    bg.inputs['Strength'].default_value = 1.15
    bg.location = (280, 0)
    nt.links.new(bg.outputs['Background'], out.inputs['Surface'])

    tex_coord = nt.nodes.new('ShaderNodeTexCoord')
    tex_coord.location = (-440, 0)
    mapping = nt.nodes.new('ShaderNodeMapping')
    mapping.inputs['Rotation'].default_value = (0.0, 0.0, math.radians(-118.0))
    mapping.location = (-220, 0)
    env = nt.nodes.new('ShaderNodeTexEnvironment')
    env.image = bpy.data.images.load(str(EXR_SKY), check_existing=True)
    env.location = (20, 0)

    nt.links.new(tex_coord.outputs['Generated'], mapping.inputs['Vector'])
    nt.links.new(mapping.outputs['Vector'], env.inputs['Vector'])
    nt.links.new(env.outputs['Color'], bg.inputs['Color'])

    # Directional golden-peach Taipei dawn sun matching sceneSunDir() = norm(0.80, 0.34, -0.49) in Engine
    # Ray toward sun in Blender = (0.80, -0.49, 0.34) -> light shines along (-0.80, 0.49, -0.34)
    sun_to = Vector((0.80, -0.49, 0.34)).normalized()
    light_data = bpy.data.lights.new(name='TaipeiDawnSun', type='SUN')
    light_data.energy = 5.6
    light_data.color = (1.0, 0.78, 0.56)
    light_data.angle = math.radians(1.35)
    sun_obj = bpy.data.objects.new(name='TaipeiDawnSun', object_data=light_data)
    scene.collection.objects.link(sun_obj)
    sun_obj.rotation_mode = 'QUATERNION'
    sun_obj.rotation_quaternion = sun_to.to_track_quat('Z', 'Y')


def configure_cycles_gpu(scene: bpy.types.Scene):
    scene.render.engine = 'CYCLES'
    prefs = bpy.context.preferences.addons['cycles'].preferences
    selected_backend = 'CPU'
    for backend in ('OPTIX', 'CUDA'):
        try:
            prefs.compute_device_type = backend
            prefs.get_devices()
            gpu_devs = [d for d in prefs.devices if d.type != 'CPU']
            if gpu_devs:
                for d in prefs.devices:
                    d.use = (d.type != 'CPU')
                scene.cycles.device = 'GPU'
                selected_backend = f'{backend} ({gpu_devs[0].name})'
                break
        except Exception:
            continue

    print(f'[Cycles] Compute backend: {selected_backend}')
    scene.cycles.samples = 256
    scene.cycles.use_adaptive_sampling = True
    scene.cycles.adaptive_threshold = 0.008
    scene.cycles.use_denoising = True
    if hasattr(scene.cycles, 'denoiser'):
        for dn in ('OPENIMAGEDENOISE', 'OPTIX'):
            try:
                scene.cycles.denoiser = dn
                break
            except Exception:
                pass
    scene.cycles.max_bounces = 10
    scene.cycles.diffuse_bounces = 4
    scene.cycles.glossy_bounces = 6
    scene.cycles.transmission_bounces = 8
    scene.cycles.transparent_max_bounces = 8
    scene.cycles.sample_clamp_indirect = 8.0

    scene.render.resolution_x = 2560
    scene.render.resolution_y = 1440
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGB'
    scene.render.image_settings.color_depth = '8'
    scene.view_settings.view_transform = 'AgX' if 'AgX' in ('AgX',) else 'Filmic'
    try:
        scene.view_settings.view_transform = 'AgX'
        scene.view_settings.look = 'AgX - Medium High Contrast'
    except Exception:
        scene.view_settings.view_transform = 'Filmic'
    scene.view_settings.exposure = 0.15


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)

    scene = bpy.context.scene

    # 1. Build Dawn Bathroom (identity 1:1 metric transform)
    gtm.objects = []
    gtm.serial = 0
    gtm.dawn_alcove()
    apply_kit_transform_and_materials(gtm.objects)

    # 2. Build Miniature Keepsake Glass Orchid in Celadon Bud Vase
    gtm.objects = []
    gtm.orchid()
    apply_kit_transform_and_materials(
        gtm.objects,
        scale=1.05 * (1.0 - 0.962),
        loc_bl=(0.402, 5.5 + 0.735, 5.85 - 2.113),
        rot_z_bl=0.18,
    )

    # 3. Build Distant Taipei 101, Xinyi Skyline & Taipei Basin Ridges (parts 0, 3, 4, 6)
    gtm.objects = []
    gtm.taipei()
    apply_kit_transform_and_materials(
        gtm.objects,
        scale=0.064,
        loc_bl=(33.0, 66.0, -9.6),
        rot_z_bl=0.0,
        allowed_parts={0, 3, 4, 6},
    )

    setup_world_and_sun(scene)
    configure_cycles_gpu(scene)

    cam_data = bpy.data.cameras.new('DawnHeroCamera')
    cam_data.type = 'PERSP'
    cam_data.sensor_fit = 'VERTICAL'
    cam_data.clip_start = 0.05
    cam_data.clip_end = 1000.0
    cam_obj = bpy.data.objects.new('DawnHeroCamera', cam_data)
    scene.collection.objects.link(cam_obj)
    scene.camera = cam_obj

    for label, t_sec in [('342s', 342.0), ('355s', 355.0)]:
        eye_e, tgt_e, fov_y = eval_dawn_camera_engine(t_sec)
        eye_bl = eng_to_bl(eye_e)
        tgt_bl = eng_to_bl(tgt_e)
        fwd_bl = (tgt_bl - eye_bl).normalized()
        # Exact frustum match: P(up_e x_E fwd_e) = fwd_bl x_B up_bl = +X_cam -> standard '-Z', 'Y' track!
        cam_obj.location = eye_bl
        cam_obj.rotation_mode = 'QUATERNION'
        cam_obj.rotation_quaternion = fwd_bl.to_track_quat('-Z', 'Y')
        cam_data.angle_y = fov_y

        out_path = OUT_DIR / f'dawn_cycles_{label}.png'
        scene.render.filepath = str(out_path)
        print(f'[Cycles] Rendering {out_path.name} (t={t_sec:.1f}s, eye={tuple(round(v, 3) for v in eye_e)}, fovY={fov_y:.3f})...')
        bpy.ops.render.render(write_still=True)
        print(f'[Cycles] Saved {out_path}')


if __name__ == '__main__':
    main()
