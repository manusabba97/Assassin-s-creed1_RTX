"""Modern Italian cypress (Cupressus sempervirens) to replace AC1's crossed-card cypress (vegetation export
9C78D0971959165B: 2 crossed quads, 9.3 m tall, ~1 m radius, texture 45EA6FD9).

  blender --background --factory-startup --python tools/blender/cypress_modern.py

Builds, in game units (metres, Z up, origin at the foot like the original): a tapered trunk, inner upward branches and
~900 foliage cards on a columnar flame-shaped envelope, textured with Poly Haven's CC0 fir twig atlas tinted to the
AC1 cypress palette (assets/vegetation/cypress_modern/*.png, made beforehand). Writes cypress_modern.blend, .obj/.mtl,
.glb and preview renders (new tree next to the original cards) to assets/vegetation/cypress_modern/.
"""

import math
import os
import random

import bpy
from mathutils import Matrix, Vector

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "assets", "vegetation", "cypress_modern")
ORIGINAL_OBJ = os.path.join(ROOT, "pbr", "vegetation", "9C78D0971959165B.obj")
ORIGINAL_TEX = os.path.join(ROOT, "pbr", "vegetation", "textures", "45EA6FD9D6C4C66F.png")

HEIGHT = 9.3          # original card height (OBJ z 0 -> 9.28)
RADIUS = 1.05         # widest radius of the flame
CARDS = 1500
SEED = 7

# Twig sprites in the 2048 atlas (x, y, w, h; stem at the bottom), found from the alpha's connected components.
SPRITES = [(619, 812, 708, 783), (1292, 919, 676, 788), (1329, 75, 599, 687), (1003, 525, 296, 301)]
ATLAS = 2048.0

rng = random.Random(SEED)


def envelope(t):
    """Radius at relative height t (0 foot, 1 tip): narrow foot, widest at ~25 %, long taper to a soft point."""
    rise = min(1.0, (t + 0.04) / 0.26)
    return RADIUS * math.sin(rise * math.pi / 2) * (1.0 - t) ** 1.15


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def image(path):
    return bpy.data.images.load(path, check_existing=True)


def foliage_material():
    mat = bpy.data.materials.new("cypress_foliage")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = image(os.path.join(OUT, "cypress_foliage_albedo.png"))
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    rough = nt.nodes.new("ShaderNodeTexImage")
    rough.image = image(os.path.join(OUT, "cypress_foliage_roughness.png"))
    rough.image.colorspace_settings.name = "Non-Color"
    nt.links.new(rough.outputs["Color"], bsdf.inputs["Roughness"])
    nor = nt.nodes.new("ShaderNodeTexImage")
    nor.image = image(os.path.join(OUT, "cypress_foliage_normal.png"))
    nor.image.colorspace_settings.name = "Non-Color"
    nmap = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(nor.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    # thin leaves let light through
    if "Subsurface Weight" in bsdf.inputs:
        bsdf.inputs["Subsurface Weight"].default_value = 0.15
    mat.blend_method = "CLIP" if hasattr(mat, "blend_method") else None
    if hasattr(mat, "use_backface_culling"):
        mat.use_backface_culling = False
    return mat


def bark_material():
    mat = bpy.data.materials.new("cypress_bark")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    coords = nt.nodes.new("ShaderNodeTexCoord")
    mapping = nt.nodes.new("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (2.0, 6.0, 1.0)
    nt.links.new(coords.outputs["UV"], mapping.inputs["Vector"])
    for name, socket, color in (("cypress_bark_diff.png", "Base Color", True),
                                ("cypress_bark_rough.png", "Roughness", False)):
        t = nt.nodes.new("ShaderNodeTexImage")
        t.image = image(os.path.join(OUT, name))
        if not color:
            t.image.colorspace_settings.name = "Non-Color"
        nt.links.new(mapping.outputs["Vector"], t.inputs["Vector"])
        nt.links.new(t.outputs["Color"], bsdf.inputs[socket])
    t = nt.nodes.new("ShaderNodeTexImage")
    t.image = image(os.path.join(OUT, "cypress_bark_nor_gl.png"))
    t.image.colorspace_settings.name = "Non-Color"
    nt.links.new(mapping.outputs["Vector"], t.inputs["Vector"])
    nmap = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(t.outputs["Color"], nmap.inputs["Color"])
    nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    return mat


def tapered_cylinder(name, start, end, r0, r1, sides=8, rings=6):
    """Mesh object: a tapered tube from start to end with uv (u around, v along)."""
    axis = end - start
    length = axis.length
    rot = axis.to_track_quat("Z", "Y").to_matrix().to_4x4()
    verts, faces, uvs = [], [], []
    for j in range(rings + 1):
        f = j / rings
        r = r0 + (r1 - r0) * f
        for i in range(sides + 1):
            a = 2 * math.pi * i / sides
            p = rot @ Vector((math.cos(a) * r, math.sin(a) * r, f * length))
            verts.append(start + p)
            uvs.append((i / sides, f * length / 2.0))
    for j in range(rings):
        for i in range(sides):
            a = j * (sides + 1) + i
            faces.append((a, a + 1, a + sides + 2, a + sides + 1))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(v) for v in verts], [], faces)
    uv = mesh.uv_layers.new(name="UVMap")
    for poly in mesh.polygons:
        for li in poly.loop_indices:
            uv.data[li].uv = uvs[mesh.loops[li].vertex_index]
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    return obj


def build_wood(mat):
    parts = [tapered_cylinder("trunk", Vector((0, 0, -0.1)), Vector((0, 0, HEIGHT * 0.88)), 0.13, 0.015, 10, 12)]
    for k in range(70):
        t = rng.uniform(0.04, 0.82)
        z = t * HEIGHT
        a = rng.uniform(0, 2 * math.pi)
        reach = envelope(t) * rng.uniform(0.5, 0.8)
        tilt = math.radians(rng.uniform(22, 38))  # cypress branches climb steeply
        length = reach / math.sin(tilt)
        d = Vector((math.cos(a) * math.sin(tilt), math.sin(a) * math.sin(tilt), math.cos(tilt)))
        start = Vector((0, 0, z))
        parts.append(tapered_cylinder(f"branch{k}", start, start + d * length, 0.035 * (1 - t) + 0.01, 0.006, 5, 3))
    bpy.ops.object.select_all(action="DESELECT")
    for p in parts:
        p.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    wood = bpy.context.view_layer.objects.active
    wood.name = "cypress_wood"
    wood.data.materials.append(mat)
    return wood


def build_foliage(mat):
    verts, faces, uvs = [], [], []
    for c in range(CARDS):
        t = rng.random() ** 0.9 * 0.995          # a bit denser low down, where the crown is wide
        z = t * HEIGHT
        r_env = envelope(t)
        r = r_env * math.sqrt(rng.uniform(0.5, 1.0))  # dense outer shell, like the clipped-looking real crown
        a = rng.uniform(0, 2 * math.pi)
        centre = Vector((math.cos(a) * r, math.sin(a) * r, z))
        sx, sy, sw, sh = rng.choice(SPRITES)
        size = rng.uniform(0.4, 0.75) * (0.35 + 0.65 * (r_env / RADIUS))  # small sprays toward the tip
        w, h = size * sw / max(sw, sh), size * sh / max(sw, sh)
        # card faces outward, stem low and inside, tip up and out (sprays point skyward)
        out = Vector((math.cos(a), math.sin(a), 0))
        up = Vector((0, 0, 1))
        tilt = math.radians(rng.uniform(10, 35))
        yaw = math.radians(rng.uniform(-40, 40))
        right = out.cross(up).normalized()
        frame = Matrix.Rotation(yaw, 3, up) @ Matrix((right, up, -out)).transposed()
        frame = Matrix.Rotation(tilt, 3, Matrix.Rotation(yaw, 3, up) @ right) @ frame
        roll = Matrix.Rotation(math.radians(rng.uniform(-15, 15)), 3, frame.col[2])
        frame = roll @ frame
        ex, ey = frame.col[0] * (w / 2), frame.col[1]
        base = centre - ey * (h * 0.15)
        quad = [base - ex, base + ex, base + ex + ey * h, base - ex + ey * h]
        i0 = len(verts)
        verts += [tuple(v) for v in quad]
        faces.append((i0, i0 + 1, i0 + 2, i0 + 3))
        u0, u1 = sx / ATLAS, (sx + sw) / ATLAS
        v0, v1 = 1 - (sy + sh) / ATLAS, 1 - sy / ATLAS
        uvs += [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
    mesh = bpy.data.meshes.new("cypress_foliage")
    mesh.from_pydata(verts, [], faces)
    uv = mesh.uv_layers.new(name="UVMap")
    for poly in mesh.polygons:
        for li in poly.loop_indices:
            uv.data[li].uv = uvs[mesh.loops[li].vertex_index]
    mesh.update()
    obj = bpy.data.objects.new("cypress_foliage", mesh)
    bpy.context.collection.objects.link(obj)
    obj.data.materials.append(mat)
    return obj


def original_cards():
    """The AC1 cards for comparison, placed 3 m to the side."""
    bpy.ops.wm.obj_import(filepath=ORIGINAL_OBJ, forward_axis="Y", up_axis="Z")
    obj = bpy.context.selected_objects[0]
    obj.name = "AC1_original"
    obj.location.x = -3.0
    mat = bpy.data.materials.new("ac1_cypress")
    mat.use_nodes = True
    nt = mat.node_tree
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = image(ORIGINAL_TEX)
    bsdf = nt.nodes["Principled BSDF"]
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    nt.links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    return obj


def render(path, cam_loc, target):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 64
    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "OPTIX"
        prefs.get_devices()
        for d in prefs.devices:
            d.use = True
        scene.cycles.device = "GPU"
    except Exception:
        pass
    scene.render.resolution_x, scene.render.resolution_y = 1280, 1280
    scene.render.film_transparent = False
    cam = bpy.data.objects.get("cam")
    if not cam:
        cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
        bpy.context.collection.objects.link(cam)
        scene.camera = cam
    cam.location = cam_loc
    cam.rotation_euler = (target - cam_loc).to_track_quat("-Z", "Y").to_euler()
    cam.data.lens = 50
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def lighting():
    world = bpy.data.worlds.new("sky")
    bpy.context.scene.world = world
    world.use_nodes = True
    nt = world.node_tree
    sky = nt.nodes.new("ShaderNodeTexSky")
    nt.links.new(sky.outputs["Color"], nt.nodes["Background"].inputs["Color"])
    nt.nodes["Background"].inputs["Strength"].default_value = 0.35
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 4.0
    sun.rotation_euler = (math.radians(50), 0, math.radians(35))
    bpy.context.collection.objects.link(sun)
    bpy.ops.mesh.primitive_plane_add(size=40)
    ground = bpy.context.active_object
    gm = bpy.data.materials.new("ground")
    gm.use_nodes = True
    gm.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.42, 0.36, 0.27, 1)
    ground.data.materials.append(gm)


def main():
    clear_scene()
    os.makedirs(OUT, exist_ok=True)
    wood = build_wood(bark_material())
    foliage = build_foliage(foliage_material())
    tris = sum(len(p.vertices) - 2 for o in (wood, foliage) for p in o.data.polygons)
    print(f"cypress_modern: {tris} triangles ({len(foliage.data.polygons)} foliage cards)")
    # exports: only the new tree
    bpy.ops.object.select_all(action="DESELECT")
    wood.select_set(True)
    foliage.select_set(True)
    bpy.ops.wm.obj_export(filepath=os.path.join(OUT, "cypress_modern.obj"), export_selected_objects=True,
                          forward_axis="Y", up_axis="Z", export_materials=True, path_mode="RELATIVE")
    bpy.ops.export_scene.gltf(filepath=os.path.join(OUT, "cypress_modern.glb"), use_selection=True)
    original_cards()
    lighting()
    bpy.ops.wm.save_as_mainfile(filepath=os.path.join(OUT, "cypress_modern.blend"))
    render(os.path.join(OUT, "preview_side.png"), Vector((-1.5, -17.0, 4.0)), Vector((-1.5, 0, 4.4)))
    render(os.path.join(OUT, "preview_close.png"), Vector((1.5, -4.5, 2.2)), Vector((0, 0, 2.6)))


main()
