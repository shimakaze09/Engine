# Generates assets/templates/island_hopper.json — the Island Hopper starter
# template scene in the engine's scene-serializer v2 format: island blockout
# from builtin primitives, bundled props (trees, rocks, crates, dock), eight
# coins + a bonus gem, hop platforms with a moving platform and falling-rock
# hazard, a goal flag, the rigged character player, and the controller
# script entity. Mesh references use the same FNV-1a-64 path ids as
# renderer::make_asset_id_from_path.
import json
import os
import sys

OUT_PATH = sys.argv[1] if len(sys.argv) > 1 else "assets/templates/island_hopper.json"

FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def asset_id(path):
    h = FNV_OFFSET
    for ch in path.replace("\\", "/").encode("utf-8"):
        h = ((h ^ ch) * FNV_PRIME) & MASK64
    return h or 1


IDENTITY = [0.0, 0.0, 0.0, 1.0]
entities = []
next_pid = [1]


def entity(name, pos, scale=(1.0, 1.0, 1.0), rot=None, parent=0, mesh=None,
           albedo=(1.0, 1.0, 1.0), roughness=0.85, metallic=0.0, opacity=1.0,
           collider=None, body=None, script=None, anim=None, light=None):
    pid = next_pid[0]
    next_pid[0] += 1
    components = {
        "Transform": {
            "position": list(pos),
            "rotation": list(rot) if rot else list(IDENTITY),
            "scale": list(scale),
            "parentId": parent,
        },
        "name": name,
    }
    if mesh is not None:
        components["MeshComponent"] = {
            "meshAssetId": asset_id(mesh),
            "albedo": list(albedo),
            "roughness": roughness,
            "metallic": metallic,
            "opacity": opacity,
        }
    if collider is not None:
        components["Collider"] = collider
    if body is not None:
        components["RigidBody"] = body
    if script is not None:
        components["ScriptComponent"] = script
    if anim is not None:
        components["AnimationComponent"] = anim
    if light is not None:
        components["LightComponent"] = light
    entities.append({"persistentId": pid, "components": components})
    return pid


def box_collider(half, local=(0.0, 0.0, 0.0), friction=(0.6, 0.45),
                 restitution=0.05):
    return {
        "shape": 0,
        "localPosition": list(local),
        "localRotation": list(IDENTITY),
        "halfExtents": list(half),
        "restitution": restitution,
        "staticFriction": friction[0],
        "dynamicFriction": friction[1],
        "density": 1.0,
        "collisionLayer": 1,
        "collisionMask": 4294967295,
    }


def capsule_collider(radius, half_height, local):
    collider = box_collider((radius, half_height, radius), local)
    collider["shape"] = 2
    return collider


def dynamic_body(gravity=True, inverse_mass=1.0):
    return {
        "velocity": [0.0, 0.0, 0.0],
        "acceleration": [0.0, -9.8, 0.0] if gravity else [0.0, 0.0, 0.0],
        "angularVelocity": [0.0, 0.0, 0.0],
        "inverseMass": inverse_mass,
        "inverseInertia": 0.0,
        "sleeping": False,
    }


SAND = (0.83, 0.75, 0.55)
GRASS = (0.28, 0.58, 0.24)
WATER = (0.12, 0.35, 0.55)
STONE = (0.45, 0.45, 0.47)
WOOD = (0.48, 0.33, 0.19)
TRUNK = (0.42, 0.28, 0.16)
CANOPY = (0.20, 0.52, 0.22)
GOLD = (0.95, 0.80, 0.25)

CUBE = "builtin://cube"
PROP = "assets/props/{}.mesh".format

# --- Lighting and water ---
entity("Sun", (0.0, 12.0, 0.0), light={
    "color": [1.0, 0.96, 0.90],
    "direction": [-0.35, -0.80, -0.45],
    "intensity": 1.15,
    "type": 0,
})
entity("Water", (0.0, -0.85, 0.0), scale=(60.0, 0.4, 60.0), mesh=CUBE,
       albedo=WATER, roughness=0.15, opacity=0.75)

# --- Island blockout (top of the beach slab sits at y = 0) ---
entity("Beach", (0.0, -0.55, 0.0), scale=(14.0, 1.1, 14.0), mesh=CUBE,
       albedo=SAND, roughness=0.95, collider=box_collider((0.5, 0.5, 0.5)))
entity("Meadow", (0.0, -0.40, -0.8), scale=(10.0, 1.0, 9.0), mesh=CUBE,
       albedo=GRASS, roughness=0.9, collider=box_collider((0.5, 0.5, 0.5)))
entity("Cliff", (-5.5, 0.35, -3.5), scale=(3.0, 1.5, 3.0), mesh=CUBE,
       albedo=STONE, collider=box_collider((0.5, 0.5, 0.5)))

# --- Trees (trunk root + parented canopy) ---
for i, (tx, tz) in enumerate([(-3.5, -2.0), (3.0, -3.2), (-2.0, 3.0)]):
    trunk = entity(f"Tree{i + 1}", (tx, 0.1, tz), mesh=PROP("tree_trunk"),
                   albedo=TRUNK,
                   collider=box_collider((0.14, 0.5, 0.14), (0.0, 0.5, 0.0)))
    entity(f"Tree{i + 1}Canopy", (0.0, 0.95, 0.0), parent=trunk,
           mesh=PROP("tree_canopy"), albedo=CANOPY)

# --- Decor and props ---
entity("RockLarge", (4.6, 0.1, 1.6), mesh=PROP("rock_large"), albedo=STONE,
       collider=box_collider((0.55, 0.5, 0.45), (0.0, 0.45, 0.0)))
entity("RockSmall", (-4.2, 0.1, 2.4), mesh=PROP("rock_small"), albedo=STONE)
entity("Crate", (2.2, 0.1, 2.3), mesh=PROP("crate"), albedo=WOOD,
       collider=box_collider((0.40, 0.42, 0.40), (0.0, 0.42, 0.0)))
entity("Barrel", (3.0, 0.1, 1.5), mesh=PROP("barrel"), albedo=WOOD,
       collider=box_collider((0.30, 0.42, 0.30), (0.0, 0.45, 0.0)))
entity("Bush1", (-1.5, 0.1, -4.0), mesh=PROP("bush"), albedo=CANOPY)
entity("Bush2", (1.8, 0.1, -4.2), mesh=PROP("bush"), albedo=CANOPY)
entity("Mushroom1", (-3.0, 0.1, 0.5), mesh=PROP("mushroom"),
       albedo=(0.85, 0.30, 0.25))
entity("Mushroom2", (-3.6, 0.1, 1.1), mesh=PROP("mushroom"),
       albedo=(0.90, 0.55, 0.30))
entity("Signpost", (1.4, 0.1, 4.6), mesh=PROP("signpost"), albedo=WOOD)

# --- Dock out over the water (collider extents stay in the plank's local
# frame: the physics collider is oriented by the entity rotation) ---
for i in range(3):
    entity(f"DockPlank{i + 1}", (0.0, 0.02, 7.2 + i * 1.6),
           mesh=PROP("plank"), albedo=WOOD,
           rot=[0.0, 0.7071068, 0.0, 0.7071068],
           collider=box_collider((0.80, 0.15, 0.20), (0.0, -0.07, 0.0)))

# --- Coins (script picks these up by name) and the bonus gem ---
COIN_SPOTS = [
    (0.0, 0.5, 3.0), (2.6, 0.5, -1.8), (-2.6, 0.5, -1.2), (4.2, 0.6, 3.4),
    (-4.6, 1.6, -3.4), (0.0, 0.6, 11.6), (6.5, 1.6, -1.0), (8.2, 2.4, -2.5),
]
for i, spot in enumerate(COIN_SPOTS):
    entity(f"Coin{i + 1}", spot, mesh=PROP("coin"), albedo=GOLD,
           roughness=0.3, metallic=1.0)
entity("Gem", (13.5, 3.6, -5.5), mesh=PROP("gem"), albedo=(0.35, 0.85, 0.90),
       roughness=0.2)

# --- The hop route: static platforms, moving platform, falling rock ---
entity("HopPlatform1", (6.5, 0.8, -1.0), mesh=PROP("platform_square"),
       albedo=STONE, collider=box_collider((0.8, 0.1, 0.8), (0.0, 0.1, 0.0)))
entity("HopPlatform2", (8.2, 1.6, -2.5), mesh=PROP("platform_square"),
       albedo=STONE, collider=box_collider((0.8, 0.1, 0.8), (0.0, 0.1, 0.0)))
entity("MovingPlatform", (9.0, 2.2, -4.0), mesh=PROP("platform_round"),
       albedo=(0.55, 0.50, 0.60),
       collider=box_collider((0.9, 0.1, 0.9), (0.0, 0.1, 0.0),
                             friction=(0.95, 0.85)),
       body=dynamic_body(gravity=False, inverse_mass=0.001),
       script="assets/scripts/moving_platform.lua")
entity("FallingRock", (8.2, 6.0, -2.5), mesh=PROP("rock_large"),
       albedo=(0.35, 0.33, 0.35),
       collider=box_collider((0.55, 0.5, 0.45), (0.0, 0.45, 0.0)),
       body=dynamic_body(gravity=False),
       script="assets/scripts/falling_rock.lua")

# --- Goal islet with the flag ---
entity("GoalIslet", (13.5, 2.4, -5.5), scale=(3.0, 0.8, 3.0), mesh=CUBE,
       albedo=GRASS, collider=box_collider((0.5, 0.5, 0.5)))
goal = entity("Goal", (13.5, 2.8, -5.5), mesh=PROP("flag_pole"), albedo=WOOD)
entity("GoalBanner", (0.0, 1.55, 0.0), parent=goal,
       mesh=PROP("flag_banner"), albedo=(0.9, 0.25, 0.2))

# --- Player and controller ---
entity("Player", (0.0, 0.15, 5.0), mesh="assets/character.mesh",
       albedo=(0.30, 0.65, 0.85), roughness=0.6,
       collider=capsule_collider(0.30, 0.60, (0.0, 0.95, 0.0)),
       body=dynamic_body(),
       script="assets/scripts/island_player.lua",
       anim="assets/character.animctrl.json")
entity("IslandController", (0.0, 0.0, 0.0),
       script="assets/scripts/island_hopper.lua")

# Staged write + atomic replace so an interrupted run never truncates the
# installed template (audit M-27).
scene = {"version": 2, "entities": entities}
out_dir = os.path.dirname(OUT_PATH)
if out_dir:
    os.makedirs(out_dir, exist_ok=True)
with open(OUT_PATH + ".tmp", "w", newline="\n") as f:
    json.dump(scene, f, indent=1, sort_keys=True)
    f.write("\n")
    f.flush()
    os.fsync(f.fileno())
os.replace(OUT_PATH + ".tmp", OUT_PATH)
print(f"wrote {OUT_PATH} ({len(entities)} entities)")
