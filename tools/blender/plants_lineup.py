"""All game meshes (v1, then v2 15 % bigger) at their level scale (assets/plants/scatter_scale.json) in a row next to a 1.75 m figure and a 0.5 m grid, for checking sizes ->
assets/plants/_review/lineup.png.
  blender --background --factory-startup --python tools/blender/plants_lineup.py
"""

import math
import os

import json

import bpy

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
P = os.path.join(ROOT, "assets", "plants")
SCALE = json.load(open(os.path.join(P, "scatter_scale.json")))
ITEMS = [(os.path.join(ROOT, "assets", "vegetation", "grass_modern", "grass_modern.obj"),
          os.path.join(ROOT, "assets", "vegetation", "grass_modern"), "grass_", SCALE["grass"])]
for name in ("9_Hordeum_murinum", "6_Peganum_harmala", "8_Chenopodium_murale", "4_FIORI", "5_FIORI",
             "12_Thymbra_capitata", "10_Sarcopoterium_spinosum", "11_Sarcopoterium_spinosum", "7_Foeniculum_vulgare",
             "15_Rosa_damascena", "13_Lycium_europaeum", "14_ALBERELLO"):
    for v, k in ((1, 1.0), (2, 1.15)):
        ITEMS.append((os.path.join(P, name, f"{name}_v{v}_game.obj"), os.path.join(P, name), "plant_", SCALE[name] * k))


def material(tex_dir, prefix):
    mat = bpy.data.materials.new(os.path.basename(tex_dir))
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = bpy.data.images.load(os.path.join(tex_dir, prefix + "albedo.png"))
    nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(t.outputs["Alpha"], bsdf.inputs["Alpha"])
    return mat


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    x = 0.0
    mats = {}
    for obj, tex, prefix, scale in ITEMS:
        bpy.ops.wm.obj_import(filepath=obj, forward_axis="Y", up_axis="Z")
        o = bpy.context.selected_objects[0]
        o.data.materials.clear()
        if tex not in mats:
            mats[tex] = material(tex, prefix)
        o.data.materials.append(mats[tex])
        o.scale = (scale, scale, scale)
        bpy.context.view_layer.update()
        w = o.dimensions.x
        x += w / 2
        o.location = (x, 0, 0)
        x += w / 2 + 0.25
    # 1.75 m figure
    bpy.ops.mesh.primitive_cylinder_add(radius=0.18, depth=1.75, location=(-0.6, 0, 0.875))
    fig = bpy.context.active_object
    fm = bpy.data.materials.new("fig")
    fm.use_nodes = True
    next(n for n in fm.node_tree.nodes if n.type == "BSDF_PRINCIPLED").inputs["Base Color"].default_value = (0.6, 0.15, 0.1, 1)
    fig.data.materials.append(fm)
    # ground with 0.5 m lines
    bpy.ops.mesh.primitive_plane_add(size=1, location=(x / 2, 0, 0))
    g = bpy.context.active_object
    g.scale = (x + 4, 6, 1)
    gm = bpy.data.materials.new("ground")
    gm.use_nodes = True
    nt = gm.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    tc = nt.nodes.new("ShaderNodeTexCoord")
    ch = nt.nodes.new("ShaderNodeTexChecker")
    ch.inputs["Scale"].default_value = 1.0
    ch.inputs["Color1"].default_value = (0.5, 0.45, 0.38, 1)
    ch.inputs["Color2"].default_value = (0.4, 0.35, 0.28, 1)
    mp = nt.nodes.new("ShaderNodeMapping")
    mp.inputs["Scale"].default_value = (2, 2, 2)
    nt.links.new(tc.outputs["Object"], mp.inputs["Vector"])
    nt.links.new(mp.outputs["Vector"], ch.inputs["Vector"])
    nt.links.new(ch.outputs["Color"], bsdf.inputs["Base Color"])
    g.data.materials.append(gm)
    for z in (0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0):  # height marks behind the row
        bpy.ops.mesh.primitive_cube_add(size=1, location=(x / 2, 1.5, z))
        b = bpy.context.active_object
        b.scale = (x + 1, 0.01, 0.01)
    world = bpy.data.worlds.new("sky")
    scene.world = world
    world.use_nodes = True
    next(n for n in world.node_tree.nodes if n.type == "BACKGROUND").inputs["Strength"].default_value = 0.8
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.5
    sun.rotation_euler = (math.radians(40), 0, math.radians(20))
    scene.collection.objects.link(sun)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    scene.collection.objects.link(cam)
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = x + 1.5
    cam.location = (x / 2 - 0.3, -40, 2.0)
    cam.rotation_euler = (math.radians(90), 0, 0)
    scene.camera = cam
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 32
    scene.cycles.transparent_max_bounces = 64
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "OPTIX"
        prefs.get_devices()
        for d in prefs.devices:
            d.use = True
        scene.cycles.device = "GPU"
    except Exception:
        pass
    scene.render.resolution_x, scene.render.resolution_y = 3840, int(3840 * 4.6 / (x + 1.5))
    scene.render.filepath = os.path.join(P, "_review", "lineup_scaled.png")
    bpy.ops.render.render(write_still=True)


main()
