# Generates assets/character.gltf + character.bin: a blocky rigged humanoid
# with 7 joints and three animations (idle, walk, jump) for the engine's
# skinned-character pipeline verification.
import json
import math
import os
import struct
import sys

OUT_GLTF = sys.argv[1] if len(sys.argv) > 1 else "assets/character.gltf"
OUT_BIN = OUT_GLTF.replace(".gltf", ".bin")
BIN_URI = OUT_BIN.replace("\\", "/").split("/")[-1]

# Joints: name, parent index, world bind position.
JOINTS = [
    ("root", -1, (0.0, 1.00, 0.0)),
    ("spine", 0, (0.0, 1.30, 0.0)),
    ("head", 1, (0.0, 1.60, 0.0)),
    ("armL", 1, (0.25, 1.45, 0.0)),
    ("armR", 1, (-0.25, 1.45, 0.0)),
    ("legL", 0, (0.12, 1.00, 0.0)),
    ("legR", 0, (-0.12, 1.00, 0.0)),
]

# Boxes: joint index, center (world), half extents.
BOXES = [
    (0, (0.0, 1.15, 0.0), (0.18, 0.15, 0.10)),
    (1, (0.0, 1.45, 0.0), (0.20, 0.15, 0.11)),
    (2, (0.0, 1.72, 0.0), (0.12, 0.12, 0.12)),
    (3, (0.31, 1.20, 0.0), (0.06, 0.25, 0.06)),
    (4, (-0.31, 1.20, 0.0), (0.06, 0.25, 0.06)),
    (5, (0.12, 0.50, 0.0), (0.07, 0.50, 0.07)),
    (6, (-0.12, 0.50, 0.0), (0.07, 0.50, 0.07)),
]

positions, normals, joints0, weights0, indices = [], [], [], [], []

FACES = [
    ((1, 0, 0), [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)]),
    ((-1, 0, 0), [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)]),
    ((0, 1, 0), [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)]),
    ((0, -1, 0), [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)]),
    ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
]

for joint, center, half in BOXES:
    for normal, corners in FACES:
        base = len(positions)
        for corner in corners:
            positions.append(tuple(center[i] + corner[i] * half[i] for i in range(3)))
            normals.append(normal)
            joints0.append((joint, 0, 0, 0))
            weights0.append((1.0, 0.0, 0.0, 0.0))
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]

vcount = len(positions)
icount = len(indices)


def axis_angle_quat(axis, degrees):
    r = math.radians(degrees) * 0.5
    s = math.sin(r)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(r))


X, Z = (1, 0, 0), (0, 0, 1)

# Animations: clip name -> node name -> ("rotation"|"translation", [(t, value)]).
def swing(deg):
    return [(0.0, axis_angle_quat(X, deg)), (0.4, axis_angle_quat(X, -deg)),
            (0.8, axis_angle_quat(X, deg))]


ROOT_POS = JOINTS[0][2]
ANIMATIONS = {
    "idle": {
        "spine": ("rotation", [(0.0, axis_angle_quat(Z, 0)),
                               (0.5, axis_angle_quat(Z, 2.5)),
                               (1.0, axis_angle_quat(Z, 0)),
                               (1.5, axis_angle_quat(Z, -2.5)),
                               (2.0, axis_angle_quat(Z, 0))]),
        "armL": ("rotation", [(0.0, axis_angle_quat(X, 0)),
                              (1.0, axis_angle_quat(X, 3)),
                              (2.0, axis_angle_quat(X, 0))]),
        "armR": ("rotation", [(0.0, axis_angle_quat(X, 3)),
                              (1.0, axis_angle_quat(X, 0)),
                              (2.0, axis_angle_quat(X, 3))]),
    },
    "walk": {
        "legL": ("rotation", swing(25)),
        "legR": ("rotation", swing(-25)),
        "armL": ("rotation", swing(-20)),
        "armR": ("rotation", swing(20)),
    },
    "jump": {
        "root": ("translation", [(0.0, ROOT_POS),
                                 (0.15, (ROOT_POS[0], ROOT_POS[1] - 0.15, ROOT_POS[2])),
                                 (0.35, (ROOT_POS[0], ROOT_POS[1] + 0.30, ROOT_POS[2])),
                                 (0.6, ROOT_POS)]),
        "legL": ("rotation", [(0.0, axis_angle_quat(X, 0)),
                              (0.35, axis_angle_quat(X, -35)),
                              (0.6, axis_angle_quat(X, 0))]),
        "legR": ("rotation", [(0.0, axis_angle_quat(X, 0)),
                              (0.35, axis_angle_quat(X, -35)),
                              (0.6, axis_angle_quat(X, 0))]),
    },
}

# --- Binary buffer assembly ---
blob = bytearray()
buffer_views = []
accessors = []


def align(n):
    while len(blob) % n:
        blob.append(0)


def add_view(data, target=None):
    align(4)
    offset = len(blob)
    blob.extend(data)
    view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
    if target:
        view["target"] = target
    buffer_views.append(view)
    return len(buffer_views) - 1


def add_accessor(view, ctype, count, atype, vmin=None, vmax=None):
    acc = {"bufferView": view, "componentType": ctype, "count": count,
           "type": atype}
    if vmin is not None:
        acc["min"] = vmin
        acc["max"] = vmax
    accessors.append(acc)
    return len(accessors) - 1


pos_data = b"".join(struct.pack("<3f", *p) for p in positions)
pos_min = [min(p[i] for p in positions) for i in range(3)]
pos_max = [max(p[i] for p in positions) for i in range(3)]
a_pos = add_accessor(add_view(pos_data, 34962), 5126, vcount, "VEC3",
                     pos_min, pos_max)
a_nrm = add_accessor(add_view(
    b"".join(struct.pack("<3f", *n) for n in normals), 34962), 5126, vcount,
    "VEC3")
a_jnt = add_accessor(add_view(
    b"".join(struct.pack("<4H", *j) for j in joints0), 34962), 5123, vcount,
    "VEC4")
a_wgt = add_accessor(add_view(
    b"".join(struct.pack("<4f", *w) for w in weights0), 34962), 5126, vcount,
    "VEC4")
a_idx = add_accessor(add_view(
    b"".join(struct.pack("<H", i) for i in indices), 34963), 5123, icount,
    "SCALAR")

# Inverse bind matrices (column major translate(-world)).
ibm = bytearray()
for _, _, world in JOINTS:
    m = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
         -world[0], -world[1], -world[2], 1]
    ibm += struct.pack("<16f", *m)
a_ibm = add_accessor(add_view(bytes(ibm)), 5126, len(JOINTS), "MAT4")

# Nodes: joints with local translations.
name_to_node = {name: i for i, (name, _, _) in enumerate(JOINTS)}
nodes = []
for i, (name, parent, world) in enumerate(JOINTS):
    local = world if parent < 0 else tuple(
        world[k] - JOINTS[parent][2][k] for k in range(3))
    node = {"name": name, "translation": list(local)}
    children = [j for j, (_, p, _) in enumerate(JOINTS) if p == i]
    if children:
        node["children"] = children
    nodes.append(node)
mesh_node = len(nodes)
nodes.append({"name": "characterMesh", "mesh": 0, "skin": 0})

gltf_animations = []
for clip_name, tracks in ANIMATIONS.items():
    samplers, channels = [], []
    for node_name, (path, keys) in tracks.items():
        times = [k[0] for k in keys]
        values = [k[1] for k in keys]
        a_time = add_accessor(add_view(
            b"".join(struct.pack("<f", t) for t in times)), 5126, len(times),
            "SCALAR", [times[0]], [times[-1]])
        fmt = "<4f" if path == "rotation" else "<3f"
        atype = "VEC4" if path == "rotation" else "VEC3"
        a_val = add_accessor(add_view(
            b"".join(struct.pack(fmt, *v) for v in values)), 5126,
            len(values), atype)
        samplers.append({"input": a_time, "output": a_val,
                         "interpolation": "LINEAR"})
        channels.append({"sampler": len(samplers) - 1,
                         "target": {"node": name_to_node[node_name],
                                    "path": path}})
    gltf_animations.append({"name": clip_name, "samplers": samplers,
                            "channels": channels})

gltf = {
    "asset": {"version": "2.0", "generator": "engine character generator"},
    "buffers": [{"uri": BIN_URI, "byteLength": len(blob)}],
    "bufferViews": buffer_views,
    "accessors": accessors,
    "nodes": nodes,
    "scenes": [{"nodes": [0, mesh_node]}],
    "scene": 0,
    "skins": [{"skeleton": 0, "joints": list(range(len(JOINTS))),
               "inverseBindMatrices": a_ibm}],
    "meshes": [{"name": "character", "primitives": [{
        "attributes": {"POSITION": a_pos, "NORMAL": a_nrm,
                       "JOINTS_0": a_jnt, "WEIGHTS_0": a_wgt},
        "indices": a_idx}]}],
    "animations": gltf_animations,
}

# Stage both outputs, then commit atomically so an interrupted run can
# never leave a mixed-generation .gltf/.bin pair (audit M-27).
with open(OUT_BIN + ".tmp", "wb") as f:
    f.write(bytes(blob))
    f.flush()
    os.fsync(f.fileno())
with open(OUT_GLTF + ".tmp", "w", newline="\n") as f:
    json.dump(gltf, f, separators=(",", ":"))
    f.flush()
    os.fsync(f.fileno())
os.replace(OUT_BIN + ".tmp", OUT_BIN)
os.replace(OUT_GLTF + ".tmp", OUT_GLTF)
print(f"wrote {OUT_GLTF} ({vcount} verts, {icount} indices, "
      f"{len(JOINTS)} joints, {len(gltf_animations)} clips, "
      f"bin {len(blob)} bytes)")
