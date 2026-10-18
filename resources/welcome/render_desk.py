# SPDX-License-Identifier: GPL-2.0-or-later
#
# The welcome-view plate: a low-poly machine in the shape of an Atari ST and
# its monitor, original geometry, no wordmark. The computer sits in the right
# half of a transparent frame so the doorway text has the left.
#
#   blender -b -P resources/welcome/render_desk.py -- --preview
#   blender -b -P resources/welcome/render_desk.py

import math
import os
import sys

import bmesh
import bpy
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
PREVIEW = "--preview" in sys.argv

KB_W, KB_D = 0.468, 0.228
KB_FRONT_H, KB_BACK_H = 0.036, 0.064
MON_W, MON_H, MON_D = 0.360, 0.300, 0.268


def srgb(colour):
    """Shader sockets store linear colour. Pass the sRGB you actually mean."""

    def channel(u):
        return u / 12.92 if u <= 0.04045 else ((u + 0.055) / 1.055) ** 2.4

    return tuple(channel(c) for c in colour)


def fresh_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    # Cycles, because the shadow catcher has to be real transparency. This
    # Blender build has no OIDN denoiser, so the lights stay large and the
    # sample count does the cleaning. EEVEE draws the catcher as a solid floor.
    scene.render.engine = "CYCLES"
    scene.cycles.device = "GPU"
    scene.cycles.samples = 96 if PREVIEW else 256
    scene.cycles.use_adaptive_sampling = False
    scene.cycles.use_denoising = False
    scene.render.film_transparent = True
    scene.render.resolution_x = 960 if PREVIEW else 1800
    scene.render.resolution_y = 540 if PREVIEW else 1012
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "AgX - Base Contrast"
    scene.view_settings.exposure = -0.15
    try:
        scene.cycles.device = "GPU"
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.compute_device_type = "CUDA"
        prefs.get_devices()
        for device in prefs.devices:
            device.use = device.type != "CPU"
    except Exception:
        scene.cycles.device = "CPU"
    world = bpy.data.worlds.new("dark")
    scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.015, 0.018, 0.015, 1)
    bg.inputs[1].default_value = 0.04
    return scene


def material(name, colour, rough=0.45, emission=0.0, emission_colour=None):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*srgb(colour), 1)
    bsdf.inputs["Roughness"].default_value = rough
    if emission > 0:
        bsdf.inputs["Emission Color"].default_value = (*srgb(emission_colour or colour), 1)
        bsdf.inputs["Emission Strength"].default_value = emission
    return mat


def link(obj):
    bpy.context.collection.objects.link(obj)
    return obj


def cube(name, loc, dims, mat, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=rot)
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = dims
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    obj.data.materials.append(mat)
    return obj


def bevel(obj, width, segments=2):
    mod = obj.modifiers.new("Bevel", "BEVEL")
    mod.width = width
    mod.segments = segments
    mod.limit_method = "ANGLE"
    mod.angle_limit = math.radians(40)
    return obj


def wedge(name, width, depth, front_h, back_h, mat, loc, top=None):
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    hw, hd = width / 2, depth / 2
    for co in (
        (-hw, -hd, 0),
        (hw, -hd, 0),
        (hw, hd, 0),
        (-hw, hd, 0),
        (-hw, -hd, front_h),
        (hw, -hd, front_h),
        (hw, hd, back_h),
        (-hw, hd, back_h),
    ):
        bm.verts.new(co)
    bm.verts.ensure_lookup_table()
    for face in (
        (0, 1, 2, 3),
        (4, 7, 6, 5),
        (0, 4, 5, 1),
        (3, 2, 6, 7),
        (0, 3, 7, 4),
        (1, 5, 6, 2),
    ):
        bm.faces.new([bm.verts[i] for i in face])
    bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(mesh)
    bm.free()
    obj = link(bpy.data.objects.new(name, mesh))
    obj.location = loc
    obj.data.materials.append(mat)
    if top is not None:
        obj.data.materials.append(top)
        # The second face built above is the top.
        obj.data.polygons[1].material_index = 1
    return obj


def surface_z(y, loc_y, depth, front_h, back_h):
    t = (y - (loc_y - depth / 2)) / depth
    return front_h + (back_h - front_h) * t


def aim(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def build():
    beige = material("case", (0.78, 0.74, 0.66), 0.48)
    well = material("well", (0.11, 0.12, 0.11), 0.7)
    keycap = material("key", (0.34, 0.35, 0.33), 0.42)
    bezel = material("bezel", (0.12, 0.13, 0.12), 0.38)
    screen = material(
        "screen",
        (0.01, 0.05, 0.02),
        0.22,
        emission=2.4,
        emission_colour=(0.16, 0.55, 0.24),
    )
    led = material("led", (0.30, 0.85, 0.40), 0.3, emission=4.0, emission_colour=(0.40, 0.95, 0.50))

    # Right of the camera's axis, so the left of the frame stays empty.
    kb = Vector((0.46, -0.02, 0.0))
    bevel(wedge("keyboard", KB_W, KB_D, KB_FRONT_H, KB_BACK_H, beige, kb, top=well), 0.0032, 2)

    tilt = math.atan2(KB_BACK_H - KB_FRONT_H, KB_D)
    pitch_x, pitch_y = 0.0272, 0.0340
    key_w, key_d, key_h = 0.0240, 0.0280, 0.009
    cols = 15
    front = kb.y - KB_D / 2
    row_y = [front + 0.022 + i * pitch_y for i in range(5)]
    row_y[4] += 0.006
    for row, y in enumerate(row_y):
        count = cols if row < 4 else 10
        span = (count - 1) * pitch_x
        x0 = kb.x - span / 2
        z = surface_z(y, kb.y, KB_D, KB_FRONT_H, KB_BACK_H) + (key_h / 2) * math.cos(tilt) + 0.0015
        for col in range(count):
            cube(
                f"key_{row}_{col}",
                (x0 + col * pitch_x, y, z),
                (key_w, key_d, key_h),
                keycap,
                rot=(tilt, 0, 0),
            )

    # Drive slot on the right cheek, and a power lamp on the front lip.
    cube(
        "drive_slot",
        (kb.x + KB_W / 2 - 0.005, kb.y + 0.01, 0.036),
        (0.012, 0.096, 0.007),
        well,
    )
    bpy.ops.mesh.primitive_uv_sphere_add(
        segments=12,
        ring_count=6,
        radius=0.0042,
        location=(kb.x + 0.15, kb.y - KB_D / 2 + 0.014, KB_FRONT_H * 0.62),
    )
    lamp = bpy.context.active_object
    lamp.name = "power"
    lamp.data.materials.append(led)

    mon = Vector((0.52, 0.28, 0.0))
    bevel(cube("monitor", (mon.x, mon.y, MON_H / 2 + 0.010), (MON_W, MON_D, MON_H), beige), 0.007, 3)
    face_y = mon.y - MON_D / 2 - 0.001
    # Chin under the tube: the dark recess sits in the upper part of the face.
    cube("bezel_plate", (mon.x, face_y, MON_H * 0.58 + 0.010), (MON_W * 0.84, 0.010, MON_H * 0.70), bezel)
    screen_z = MON_H * 0.60 + 0.010
    cube("crt", (mon.x, face_y - 0.004, screen_z), (MON_W * 0.70, 0.008, MON_H * 0.52), screen)
    cube("stand", (mon.x, mon.y + 0.03, 0.005), (0.14, 0.11, 0.010), bezel)

    bpy.ops.object.light_add(type="AREA", location=(mon.x, face_y - 0.08, screen_z))
    spill = bpy.context.active_object
    spill.data.energy = 6
    spill.data.color = (0.55, 1.0, 0.68)
    spill.data.size = 0.20
    aim(spill, (kb.x, kb.y, 0.05))

    bpy.ops.object.light_add(type="AREA", location=(-0.55, -0.35, 1.05))
    key = bpy.context.active_object
    key.data.energy = 35
    key.data.size = 1.1
    key.data.color = (1.0, 0.95, 0.88)
    aim(key, (0.48, 0.06, 0.10))

    bpy.ops.object.light_add(type="AREA", location=(1.35, 0.55, 0.48))
    rim = bpy.context.active_object
    rim.data.energy = 8
    rim.data.size = 0.45
    rim.data.color = (0.80, 0.88, 1.0)
    aim(rim, (0.52, 0.16, 0.16))

    bpy.ops.mesh.primitive_plane_add(size=8, location=(0.6, 0.1, 0))
    floor = bpy.context.active_object
    floor.name = "floor"
    floor.is_shadow_catcher = True

    bpy.ops.object.camera_add(location=(0.02, -1.38, 0.52))
    cam = bpy.context.active_object
    aim(cam, (0.36, 0.08, 0.12))
    cam.data.lens = 48
    cam.data.sensor_width = 36
    bpy.context.scene.camera = cam


def main():
    fresh_scene()
    build()
    name = "desk-preview.png" if PREVIEW else "desk.png"
    out = os.path.join(HERE, name)
    bpy.context.scene.render.filepath = out
    bpy.context.scene.render.image_settings.file_format = "PNG"
    bpy.context.scene.render.image_settings.color_mode = "RGBA"
    bpy.context.scene.render.image_settings.compression = 15
    bpy.ops.render.render(write_still=True)
    print("wrote", out)


if __name__ == "__main__":
    main()
