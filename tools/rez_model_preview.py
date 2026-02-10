"""
Rez-style segmented humanoid for Blender 5.x.
Based on 2D vertex/edge data for 32x32 grid, extruded into 3D
with depth for each quad slice to create the Rez look.
"""

import bpy
import math
from mathutils import Vector

# ---- Clean scene ----
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
for m in list(bpy.data.meshes):
    bpy.data.meshes.remove(m)
for m in list(bpy.data.materials):
    bpy.data.materials.remove(m)

# ---- Material ----
def make_emit(name, color, strength=5.0):
    mat = bpy.data.materials.new(name=name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    for n in list(nodes):
        nodes.remove(n)
    out = nodes.new('ShaderNodeOutputMaterial')
    em = nodes.new('ShaderNodeEmission')
    em.inputs['Color'].default_value = (*color, 1.0)
    em.inputs['Strength'].default_value = strength
    links.new(em.outputs['Emission'], out.inputs['Surface'])
    mat.diffuse_color = (*color, 1.0)
    return mat

mat_body = make_emit("Rez_Body", (0.12, 0.55, 0.85), 3.0)
mat_head = make_emit("Rez_Head", (0.25, 0.7, 0.95), 5.0)
mat_visor = make_emit("Rez_Visor", (0.85, 0.92, 1.0), 10.0)

parent = bpy.data.objects.new("RezHumanoid", None)
bpy.context.collection.objects.link(parent)

# ---- 2D vertices (32x32 grid space, y-down) ----
# From the LLM-generated model
verts_2d = [
    # Head (2 slices)
    (15, 2), (17, 2), (17, 3), (15, 3),      # 1-4: top
    (14, 4), (18, 4), (18, 5), (14, 5),      # 5-8: bottom

    # Torso (4 slices)
    (12, 6), (20, 6), (20, 7), (12, 7),      # 9-12: shoulders
    (13, 8), (19, 8), (19, 9), (13, 9),      # 13-16: chest
    (14, 10), (18, 10), (18, 11), (14, 11),  # 17-20: waist
    (14, 12), (18, 12), (18, 13), (14, 13),  # 21-24: hips

    # Left arm (2 slices)
    (10, 7), (12, 7), (9, 8), (11, 8),       # 25-28: upper
    (7, 9), (10, 9), (8, 10), (11, 10),      # 29-32: forearm

    # Right arm (2 slices)
    (20, 7), (22, 7), (21, 8), (23, 8),      # 33-36: upper
    (22, 9), (25, 9), (23, 10), (26, 10),    # 37-40: forearm

    # Left leg (4 slices)
    (12, 14), (14, 14), (11, 15), (13, 15),  # 41-44: thigh top
    (10, 17), (12, 17), (9, 19), (11, 19),   # 45-48: thigh mid
    (9, 21), (11, 21), (8, 24), (10, 24),    # 49-52: shin
    (8, 26), (10, 26), (7, 29), (9, 29),     # 53-56: foot

    # Right leg (4 slices)
    (18, 14), (20, 14), (19, 15), (21, 15),  # 57-60: thigh top
    (20, 17), (22, 17), (21, 19), (23, 19),  # 61-64: thigh mid
    (21, 21), (23, 21), (22, 24), (24, 24),  # 65-68: shin
    (22, 26), (24, 26), (23, 29), (25, 29),  # 69-72: foot
]

# Quads: groups of 4 vertex indices (0-based), each forming a rectangular slice
quads = [
    # Head
    (0,1,2,3), (4,5,6,7),
    # Torso
    (8,9,10,11), (12,13,14,15), (16,17,18,19), (20,21,22,23),
    # Left arm
    (24,25,26,27), (28,29,30,31),
    # Right arm
    (32,33,34,35), (36,37,38,39),
    # Left leg
    (40,41,42,43), (44,45,46,47), (48,49,50,51), (52,53,54,55),
    # Right leg
    (56,57,58,59), (60,61,62,63), (64,65,66,67), (68,69,70,71),
]

# Body part labels for coloring
quad_parts = [
    "head", "head",
    "torso", "torso", "torso", "torso",
    "arm_l", "arm_l",
    "arm_r", "arm_r",
    "leg_l", "leg_l", "leg_l", "leg_l",
    "leg_r", "leg_r", "leg_r", "leg_r",
]

# Convert 2D grid coords to 3D Blender coords
# Grid: x=0..31, y=0..31 (y-down)
# Blender: X=right, Y=forward, Z=up
# Map: grid_x -> X (centered), grid_y -> -Z (flip so y-down becomes z-up)
# Add Y depth for each quad to make it 3D

SCALE = 0.1  # scale factor: 32 pixels -> 3.2 blender units

def grid_to_3d(gx, gy, depth=0):
    """Convert grid coords to Blender 3D coords."""
    x = (gx - 16) * SCALE
    z = (16 - gy) * SCALE   # flip Y to Z-up
    y = depth * SCALE
    return (x, y, z)

def make_frame_quad(v0, v1, v2, v3, depth, bar_thickness, material):
    """Create a hollow rectangular frame (4 bars) from 4 corner positions,
    extruded along Y by depth."""
    corners_front = [Vector(v0), Vector(v1), Vector(v2), Vector(v3)]
    corners_back = [Vector((v[0], v[1] + depth, v[2])) for v in [v0, v1, v2, v3]]

    # For each edge of the quad, create a thin box (bar)
    edge_pairs = [(0,1), (1,2), (2,3), (3,0)]

    for a, b in edge_pairs:
        # Front edge
        fa, fb = corners_front[a], corners_front[b]
        ba, bb = corners_back[a], corners_back[b]

        # Direction along edge
        edge_dir = (fb - fa).normalized()
        # Normal (perpendicular in XZ plane)
        normal = Vector((0, 0, 0))
        if abs(edge_dir.x) > abs(edge_dir.z):
            normal = Vector((0, 0, bar_thickness/2))
        else:
            normal = Vector((bar_thickness/2, 0, 0))

        verts = [
            (fa + normal)[:], (fb + normal)[:],
            (fb - normal)[:], (fa - normal)[:],
            (ba + normal)[:], (bb + normal)[:],
            (bb - normal)[:], (ba - normal)[:],
        ]

        mesh = bpy.data.meshes.new("bar")
        mesh.from_pydata(
            verts, [],
            [(0,1,2,3),(7,6,5,4),(0,4,5,1),(2,6,7,3),(0,3,7,4),(1,5,6,2)]
        )
        mesh.update()
        obj = bpy.data.objects.new("bar", mesh)
        bpy.context.collection.objects.link(obj)
        obj.data.materials.append(material)
        obj.parent = parent

# ---- Build the model ----
DEPTH = 3.0       # depth in grid units (how deep each quad extends in Y)
BAR = 0.03        # bar thickness in blender units

for qi, quad in enumerate(quads):
    i0, i1, i2, i3 = quad
    part = quad_parts[qi]

    mat = mat_body
    if part == "head":
        mat = mat_head

    # Convert 4 corners to 3D
    v0 = grid_to_3d(verts_2d[i0][0], verts_2d[i0][1])
    v1 = grid_to_3d(verts_2d[i1][0], verts_2d[i1][1])
    v2 = grid_to_3d(verts_2d[i2][0], verts_2d[i2][1])
    v3 = grid_to_3d(verts_2d[i3][0], verts_2d[i3][1])

    make_frame_quad(v0, v1, v2, v3, DEPTH * SCALE, BAR, mat)

# ---- Scale ----
parent.scale = (3, 3, 3)

# ---- World ----
world = bpy.data.worlds.get("World")
if not world:
    world = bpy.data.worlds.new("World")
    bpy.context.scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes.get("Background")
if bg:
    bg.inputs["Color"].default_value = (0.005, 0.005, 0.018, 1.0)
    bg.inputs["Strength"].default_value = 1.0

# ---- Camera ----
bpy.ops.object.camera_add(location=(2.5, -5.0, 0.5))
cam = bpy.context.active_object
cam.name = "RezCam"
con = cam.constraints.new(type='TRACK_TO')
con.target = parent
con.track_axis = 'TRACK_NEGATIVE_Z'
con.up_axis = 'UP_Y'
bpy.context.scene.camera = cam

# ---- Turntable ----
bpy.context.view_layer.objects.active = parent
parent.select_set(True)
parent.rotation_euler = (0, 0, 0)
parent.keyframe_insert(data_path="rotation_euler", frame=1)
parent.rotation_euler = (0, 0, 2 * math.pi)
parent.keyframe_insert(data_path="rotation_euler", frame=250)

if parent.animation_data:
    action = parent.animation_data.action
    if action:
        if hasattr(action, 'layers'):
            for layer in action.layers:
                for strip in layer.strips:
                    if hasattr(strip, 'channelbags'):
                        for bag in strip.channelbags:
                            for fc in bag.fcurves:
                                for kfp in fc.keyframe_points:
                                    kfp.interpolation = 'LINEAR'
        elif hasattr(action, 'fcurves'):
            for fc in action.fcurves:
                for kfp in fc.keyframe_points:
                    kfp.interpolation = 'LINEAR'

bpy.context.scene.frame_end = 249

# ---- Viewport ----
for area in bpy.context.screen.areas:
    if area.type == 'VIEW_3D':
        for space in area.spaces:
            if space.type == 'VIEW_3D':
                space.shading.type = 'MATERIAL'
                space.shading.background_type = 'WORLD'

print("Rez humanoid created from 32x32 grid data!")
print(f"  Vertices: {len(verts_2d)}")
print(f"  Quads: {len(quads)}")
