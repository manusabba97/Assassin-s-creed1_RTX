"""Renders one area of the vegetation plan (tools/veg_scatter_plan.py --preview): assets/plants/_work/scatter_area.txt
(first line the lowest ground height, then "obj|texture folder|3x4 matrix" per plant, centred on the area) ->
assets/plants/_review/scatter_area.png. Ground: the least-squares plane through the plants' feet.
  blender --background --factory-startup --python tools/blender/scatter_preview.py
"""

import math
import os

import bpy
import numpy as np
from mathutils import Matrix, Vector

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
LINES = open(os.path.join(ROOT, "assets", "plants", "_work", "scatter_area.txt")).read().splitlines()
Z0 = float(LINES[0])


def material(tex_dir):
    prefix = "plant_" if os.path.exists(os.path.join(tex_dir, "plant_albedo.png")) else "grass_"
    mat = bpy.data.materials.new(os.path.basename(tex_dir))
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    def tex(name, data):
        t = nt.nodes.new("ShaderNodeTexImage")
        t.image = bpy.data.images.load(os.path.join(tex_dir, prefix + name))
        if data:
            t.image.colorspace_settings.name = "Non-Color"
        return t
    a = tex("albedo.png", False)
    nt.links.new(a.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(a.outputs["Alpha"], bsdf.inputs["Alpha"])
    nt.links.new(tex("roughness.png", True).outputs["Color"], bsdf.inputs["Roughness"])
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(tex("normal.png", True).outputs["Color"], nm.inputs["Color"])
    nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    meshes, mats = {}, {}
    feet = []
    for line in LINES[1:]:
        obj, tex, m = line.split("|")
        m = [float(x) for x in m.split()]
        if obj not in meshes:
            bpy.ops.wm.obj_import(filepath=obj, forward_axis="Y", up_axis="Z")
            o = bpy.context.selected_objects[0]
            if tex not in mats:
                mats[tex] = material(tex)
            o.data.materials.clear()
            o.data.materials.append(mats[tex])
            meshes[obj] = o.data
            bpy.data.objects.remove(o)
        inst = bpy.data.objects.new("p", meshes[obj])
        scene.collection.objects.link(inst)
        inst.matrix_world = Matrix(((m[0], m[1], m[2], m[3]), (m[4], m[5], m[6], m[7]), (m[8], m[9], m[10], m[11] - Z0),
                                    (0, 0, 0, 1)))
        feet.append((m[3], m[7], m[11] - Z0))
    f = np.array(feet)
    A = np.c_[f[:, 0], f[:, 1], np.ones(len(f))]
    a, b, c = np.linalg.lstsq(A, f[:, 2], rcond=None)[0]
    verts = [(x, y, a * x + b * y + c + 0.01) for x, y in ((-9, -9), (9, -9), (9, 9), (-9, 9))]
    me = bpy.data.meshes.new("ground")
    me.from_pydata(verts, [], [(0, 1, 2, 3)])
    g = bpy.data.materials.new("ground")
    g.use_nodes = True
    next(n for n in g.node_tree.nodes if n.type == "BSDF_PRINCIPLED").inputs["Base Color"].default_value = (0.42, 0.34, 0.24, 1)
    me.materials.append(g)
    scene.collection.objects.link(bpy.data.objects.new("ground", me))

    world = bpy.data.worlds.new("sky")
    scene.world = world
    world.use_nodes = True
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs["Color"].default_value = (0.55, 0.62, 0.75, 1)
    bg.inputs["Strength"].default_value = 0.7
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 4.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(40))
    scene.collection.objects.link(sun)
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 64
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
    scene.render.resolution_x, scene.render.resolution_y = 1920, 1080
    for name, loc, look, out in (("eye", (0.0, -9.5, 1.7), (0.0, 1.0, 0.4), "scatter_area.png"),
                                 ("high", (6.0, -11.0, 7.0), (0.0, 0.0, 0.0), "scatter_area_high.png")):
        cam = bpy.data.objects.new(name, bpy.data.cameras.new(name))
        scene.collection.objects.link(cam)
        p = Vector(loc)
        p.z += a * p.x + b * p.y + c
        t = Vector(look)
        t.z += a * t.x + b * t.y + c
        cam.location = p
        cam.rotation_euler = (t - p).to_track_quat("-Z", "Y").to_euler()
        cam.data.lens = 28
        scene.camera = cam
        scene.render.filepath = os.path.join(ROOT, "assets", "plants", "_review", out)
        bpy.ops.render.render(write_still=True)


main()
