# Generates the bundled blocky prop meshes (assets/props/*.gltf + .bin):
# ~20 static single-tone island props (rocks, tree parts, crates, platforms,
# pickups, decorations) built from boxes/cylinders/cones with flat CCW-outward
# normals, sized ~1-2 units with their base resting at y = 0. Deterministic
# output; cook each glTF to .mesh with asset_packer.
import math
import struct
import sys

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "assets/props"


class MeshBuilder:
    """Accumulates flat-shaded triangles (positions, outward normals, indices)."""

    def __init__(self):
        self.positions = []
        self.normals = []
        self.indices = []

    def _face(self, corners):
        p0, p1, p2 = corners[0], corners[1], corners[2]
        u = tuple(p1[i] - p0[i] for i in range(3))
        v = tuple(p2[i] - p0[i] for i in range(3))
        n = (u[1] * v[2] - u[2] * v[1],
             u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0])
        length = math.sqrt(sum(c * c for c in n)) or 1.0
        n = tuple(c / length for c in n)
        base = len(self.positions)
        for corner in corners:
            self.positions.append(corner)
            self.normals.append(n)
        for i in range(1, len(corners) - 1):
            self.indices += [base, base + i, base + i + 1]

    def box(self, center, half):
        cx, cy, cz = center
        hx, hy, hz = half
        corners = {
            (sx, sy, sz): (cx + sx * hx, cy + sy * hy, cz + sz * hz)
            for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)
        }
        faces = [
            [(1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1)],
            [(-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1)],
            [(-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1)],
            [(-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1)],
            [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)],
            [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)],
        ]
        for face in faces:
            self._face([corners[s] for s in face])

    def cylinder(self, center, radius, half_height, segments=10, axis="y"):
        def orient(x, y, z):
            if axis == "y":
                return (center[0] + x, center[1] + y, center[2] + z)
            return (center[0] + x, center[1] - z, center[2] + y)

        ring = [(radius * math.cos(2 * math.pi * i / segments),
                 radius * math.sin(2 * math.pi * i / segments))
                for i in range(segments)]
        for i in range(segments):
            x0, z0 = ring[i]
            x1, z1 = ring[(i + 1) % segments]
            self._face([orient(x0, -half_height, z0), orient(x0, half_height, z0),
                        orient(x1, half_height, z1), orient(x1, -half_height, z1)])
        top = [orient(x, half_height, z) for x, z in ring]
        bottom = [orient(x, -half_height, z) for x, z in ring]
        self._face(list(reversed(top)))
        self._face(bottom)

    def cone(self, center, radius, height, segments=10):
        apex = (center[0], center[1] + height, center[2])
        ring = [(center[0] + radius * math.cos(2 * math.pi * i / segments),
                 center[1],
                 center[2] + radius * math.sin(2 * math.pi * i / segments))
                for i in range(segments)]
        for i in range(segments):
            self._face([ring[i], apex, ring[(i + 1) % segments]])
        self._face(ring)

    def octahedron(self, center, half):
        cx, cy, cz = center
        tips = {
            "px": (cx + half, cy, cz), "nx": (cx - half, cy, cz),
            "py": (cx, cy + half, cz), "ny": (cx, cy - half, cz),
            "pz": (cx, cy, cz + half), "nz": (cx, cy, cz - half),
        }
        t = tips
        for tri in [
            (t["py"], t["pz"], t["px"]), (t["py"], t["px"], t["nz"]),
            (t["py"], t["nz"], t["nx"]), (t["py"], t["nx"], t["pz"]),
            (t["ny"], t["px"], t["pz"]), (t["ny"], t["nz"], t["px"]),
            (t["ny"], t["nx"], t["nz"]), (t["ny"], t["pz"], t["nx"]),
        ]:
            self._face(list(tri))


def build_props():
    props = {}

    def prop(name):
        builder = MeshBuilder()
        props[name] = builder
        return builder

    b = prop("rock_small")
    b.box((0.0, 0.18, 0.0), (0.30, 0.18, 0.24))
    b.box((0.14, 0.34, -0.06), (0.16, 0.10, 0.13))

    b = prop("rock_large")
    b.box((0.0, 0.35, 0.0), (0.55, 0.35, 0.45))
    b.box((-0.25, 0.75, 0.10), (0.28, 0.16, 0.24))
    b.box((0.30, 0.62, -0.15), (0.20, 0.12, 0.16))

    b = prop("tree_trunk")
    b.cylinder((0.0, 0.5, 0.0), 0.12, 0.5, segments=8)

    b = prop("tree_canopy")
    b.cone((0.0, 0.0, 0.0), 0.55, 0.9, segments=10)

    b = prop("bush")
    b.box((0.0, 0.22, 0.0), (0.35, 0.22, 0.30))
    b.box((0.22, 0.30, 0.12), (0.18, 0.16, 0.16))
    b.box((-0.20, 0.32, -0.08), (0.16, 0.14, 0.15))

    b = prop("crate")
    b.box((0.0, 0.40, 0.0), (0.40, 0.40, 0.40))
    b.box((0.0, 0.82, 0.0), (0.42, 0.02, 0.42))

    b = prop("barrel")
    b.cylinder((0.0, 0.45, 0.0), 0.30, 0.45, segments=10)
    b.cylinder((0.0, 0.12, 0.0), 0.32, 0.03, segments=10)
    b.cylinder((0.0, 0.78, 0.0), 0.32, 0.03, segments=10)

    b = prop("fence_post")
    b.box((0.0, 0.45, 0.0), (0.06, 0.45, 0.06))

    b = prop("fence_rail")
    b.box((0.0, 0.06, 0.0), (0.90, 0.06, 0.04))

    b = prop("plank")
    b.box((0.0, 0.04, 0.0), (0.80, 0.04, 0.20))

    b = prop("platform_square")
    b.box((0.0, 0.10, 0.0), (0.80, 0.10, 0.80))

    b = prop("platform_round")
    b.cylinder((0.0, 0.10, 0.0), 0.90, 0.10, segments=14)

    b = prop("coin")
    b.cylinder((0.0, 0.25, 0.0), 0.25, 0.03, segments=12, axis="z")

    b = prop("gem")
    b.octahedron((0.0, 0.30, 0.0), 0.30)

    b = prop("flag_pole")
    b.cylinder((0.0, 0.90, 0.0), 0.04, 0.90, segments=8)

    b = prop("flag_banner")
    b.box((0.28, 0.0, 0.0), (0.25, 0.15, 0.015))

    b = prop("signpost")
    b.box((0.0, 0.45, 0.0), (0.05, 0.45, 0.05))
    b.box((0.0, 0.98, 0.0), (0.40, 0.14, 0.03))

    b = prop("mushroom")
    b.cylinder((0.0, 0.16, 0.0), 0.08, 0.16, segments=8)
    b.cone((0.0, 0.30, 0.0), 0.30, 0.28, segments=10)

    b = prop("pillar")
    b.box((0.0, 0.06, 0.0), (0.34, 0.06, 0.34))
    b.cylinder((0.0, 0.76, 0.0), 0.25, 0.64, segments=10)
    b.box((0.0, 1.46, 0.0), (0.34, 0.06, 0.34))

    b = prop("stairs")
    for step in range(4):
        depth = 0.20
        b.box((0.0, 0.10 + step * 0.20, -0.30 + step * depth),
              (0.50, 0.10, depth * 0.5))

    b = prop("log")
    b.cylinder((0.0, 0.22, 0.0), 0.22, 0.70, segments=9, axis="z")

    return props


def write_gltf(name, builder):
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

    positions = builder.positions
    normals = builder.normals
    indices = builder.indices
    pos_data = b"".join(struct.pack("<3f", *p) for p in positions)
    pos_min = [min(p[i] for p in positions) for i in range(3)]
    pos_max = [max(p[i] for p in positions) for i in range(3)]
    a_pos = add_accessor(add_view(pos_data, 34962), 5126, len(positions),
                         "VEC3", pos_min, pos_max)
    a_nrm = add_accessor(add_view(
        b"".join(struct.pack("<3f", *n) for n in normals), 34962), 5126,
        len(positions), "VEC3")
    a_idx = add_accessor(add_view(
        b"".join(struct.pack("<H", i) for i in indices), 34963), 5123,
        len(indices), "SCALAR")

    bin_name = f"{name}.bin"
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_props.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm},
            "indices": a_idx,
        }]}],
        "buffers": [{"uri": bin_name, "byteLength": len(blob)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }

    import json
    with open(f"{OUT_DIR}/{name}.gltf", "w", newline="\n") as f:
        json.dump(gltf, f, indent=1, sort_keys=True)
        f.write("\n")
    with open(f"{OUT_DIR}/{bin_name}", "wb") as f:
        f.write(bytes(blob))
    print(f"wrote {OUT_DIR}/{name}.gltf ({len(positions)} verts, "
          f"{len(indices) // 3} tris)")


props = build_props()
for prop_name in sorted(props):
    write_gltf(prop_name, props[prop_name])
print(f"generated {len(props)} props")
