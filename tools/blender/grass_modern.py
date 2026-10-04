"""Modern grass clump to replace AC1's clutter grass (vegetation export 2A7F3012BFA4FEBD: 12 triangles, ~1.2 x 1.2 m,
0.3 m tall, texture A25B0D3E, drawn ~1.8k times per frame through the instanced clutter path).

  blender --background --factory-startup --python tools/blender/grass_modern.py

Each blade is a curved 4-segment strip textured with one blade of Poly Haven's CC0 "Grass Medium 02" atlas (green and
dry versions side by side in assets/vegetation/grass_modern/grass_albedo.png, tinted to the AC1 grass palette; blade
boxes in blades.json). Game units: metres, Z up, origin at the clump's foot. Writes grass_modern.obj/.mtl, .blend and
a preview (a patch of new clumps next to a patch of the original cards).
"""

import json
import math
import os
import random

import bpy
from mathutils import Vector

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "assets", "vegetation", "grass_modern")
ORIGINAL_OBJ = os.path.join(ROOT, "pbr", "vegetation", "2A7F3012BFA4FEBD.obj")
ORIGINAL_TEX = os.path.join(ROOT, "pbr", "vegetation", "textures", "A25B0D3E9ED0B91F.png")

BLADES = 140
RADIUS = 0.5
DRY_FRACTION = 0.65
SEGMENTS = 4
ATLAS_W, ATLAS_H = 4096.0, 2048.0   # green half | dry half
rng = random.Random(11)
SPRITES = json.load(open(os.path.join(OUT, "blades.json")))


def image(path, data=False):
    img = bpy.data.images.load(path, check_existing=True)
    if data:
        img.colorspace_settings.name = "Non-Color"
    return img


def grass_material():
    mat = bpy.data.materials.new("grass_modern")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = image(os.path.join(OUT, "grass_albedo.png"))
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    rough = nt.nodes.new("ShaderNodeTexImage")
    rough.image = image(os.path.join(OUT, "grass_roughness.png"), True)
    nt.links.new(rough.outputs["Color"], bsdf.inputs["Roughness"])
    nor = nt.nodes.new("ShaderNodeTexImage")
    nor.image = image(os.path.join(OUT, "grass_normal.png"), True)
    nmap = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(nor.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    if "Subsurface Weight" in bsdf.inputs:
        bsdf.inputs["Subsurface Weight"].default_value = 0.0  # Remix opaque materials have no subsurface
    if hasattr(mat, "use_backface_culling"):
        mat.use_backface_culling = False
    return mat


def build_clump(name="grass_modern"):
    verts, faces, uvs = [], [], []
    for b in range(BLADES):
        # denser centre: radius from a clipped gaussian
        r = min(abs(rng.gauss(0.0, RADIUS * 0.45)), RADIUS)
        a = rng.uniform(0, 2 * math.pi)
        base = Vector((math.cos(a) * r, math.sin(a) * r, -0.02))
        sx, sy, sw, sh = rng.choice(SPRITES)
        dry = rng.random() < DRY_FRACTION
        ox = ATLAS_W / 2 if dry else 0.0
        height = rng.uniform(0.18, 0.5) * (1.0 - 0.35 * r / RADIUS)
        width = height * sw / sh * rng.uniform(0.9, 1.3)
        # blade leans outward (from the clump centre) and bends more toward the tip
        out = Vector((math.cos(a), math.sin(a), 0)) if r > 1e-3 else Vector((1, 0, 0))
        lean_dir = (out + Vector((rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), 0))).normalized()
        lean = rng.uniform(0.15, 0.7) * (0.4 + r / RADIUS)
        yaw = rng.uniform(0, 2 * math.pi)
        side = Vector((math.cos(yaw), math.sin(yaw), 0))
        i0 = len(verts)
        for s in range(SEGMENTS + 1):
            t = s / SEGMENTS
            centre = base + Vector((0, 0, height * t)) + lean_dir * (lean * height * t * t)
            w = width * (1.0 - 0.15 * t)
            for k, sgn in enumerate((-0.5, 0.5)):
                verts.append(tuple(centre + side * (w * sgn)))
                u = (ox + sx + (0 if k == 0 else sw)) / ATLAS_W
                v = 1.0 - (sy + sh * (1.0 - t)) / ATLAS_H
                uvs.append((u, v))
        for s in range(SEGMENTS):
            q = i0 + s * 2
            faces.append((q, q + 1, q + 3, q + 2))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    uv = mesh.uv_layers.new(name="UVMap")
    for poly in mesh.polygons:
        for li in poly.loop_indices:
            uv.data[li].uv = uvs[mesh.loops[li].vertex_index]
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def original_material():
    mat = bpy.data.materials.new("ac1_grass")
    mat.use_nodes = True
    nt = mat.node_tree
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = image(ORIGINAL_TEX)
    bsdf = nt.nodes["Principled BSDF"]
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    return mat


def scatter(src, centre, count, spread):
    """Linked copies of `src` around `centre`, random yaw and scale, like the game's clutter."""
    for i in range(count):
        o = src.copy()
        bpy.context.collection.objects.link(o)
        o.location = centre + Vector((rng.uniform(-spread, spread), rng.uniform(-spread, spread), 0))
        o.rotation_euler = (0, 0, rng.uniform(0, 2 * math.pi))
        s = rng.uniform(0.8, 1.2)
        o.scale = (s, s, s)


def scene_setup():
    world = bpy.data.worlds.new("sky")
    bpy.context.scene.world = world
    world.use_nodes = True
    nt = world.node_tree
    sky = nt.nodes.new("ShaderNodeTexSky")
    nt.links.new(sky.outputs["Color"], nt.nodes["Background"].inputs["Color"])
    nt.nodes["Background"].inputs["Strength"].default_value = 0.3
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 4.0
    sun.rotation_euler = (math.radians(55), 0, math.radians(-30))
    bpy.context.collection.objects.link(sun)
    bpy.ops.mesh.primitive_plane_add(size=30)
    gm = bpy.data.materials.new("ground")
    gm.use_nodes = True
    gm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.36, 0.30, 0.22, 1)
    bpy.context.active_object.data.materials.append(gm)


def render(path, cam_loc, target):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 64
    scene.cycles.transparent_max_bounces = 128  # dense alpha blades (the game alpha-tests instead)
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "OPTIX"
        prefs.get_devices()
        for d in prefs.devices:
            d.use = True
        scene.cycles.device = "GPU"
    except Exception:
        pass
    scene.render.resolution_x, scene.render.resolution_y = 1600, 900
    cam = bpy.data.objects.get("cam")
    if not cam:
        cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
        bpy.context.collection.objects.link(cam)
        scene.camera = cam
    cam.location = cam_loc
    cam.rotation_euler = (target - cam_loc).to_track_quat("-Z", "Y").to_euler()
    cam.data.lens = 35
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    clump = build_clump()
    clump.data.materials.append(grass_material())
    print(f"grass_modern: {len(clump.data.polygons) * 2} triangles")
    bpy.ops.object.select_all(action="DESELECT")
    clump.select_set(True)
    bpy.ops.wm.obj_export(filepath=os.path.join(OUT, "grass_modern.obj"), export_selected_objects=True,
                          forward_axis="Y", up_axis="Z", export_materials=True, path_mode="RELATIVE")
    # preview: new patch (right), original patch (left)
    scatter(clump, Vector((1.6, 0, 0)), 14, 1.3)
    clump.location = (1.6, 0, 0)
    bpy.ops.wm.obj_import(filepath=ORIGINAL_OBJ, forward_axis="Y", up_axis="Z")
    orig = bpy.context.selected_objects[0]
    orig.data.materials.clear()
    orig.data.materials.append(original_material())
    orig.location = (-1.6, 0, 0)
    scatter(orig, Vector((-1.6, 0, 0)), 14, 1.3)
    scene_setup()
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "grass_modern.blend"))
    render(os.path.join(OUT, "preview.png"), Vector((0, -5.5, 1.6)), Vector((0, 0, 0.2)))
    render(os.path.join(OUT, "preview_close.png"), Vector((1.9, -1.6, 0.6)), Vector((1.6, 0, 0.15)))


main()
