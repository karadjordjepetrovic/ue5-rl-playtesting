"""
build_detector_test_map.py
Builds /Game/PlaytestBots/Maps/PT_DetectorTest: a 40 x 40 m greybox arena with one planted bug per
issue type, plus control areas where detectors must stay silent. Writes the answer key to
Plugins/PlaytestBots/TestMaps/PT_DetectorTest.ground_truth.json.

Run in the editor: Tools -> Execute Python Script... -> pick this file.
Safe to re-run: it reloads the map and rebuilds every actor tagged "PTGenerated".

Location: Plugins/PlaytestBots/Scripts/build_detector_test_map.py
"""

import json
import math
import os

import unreal

MAP_DIR = "/Game/PlaytestBots/Maps"
MAP_PATH = MAP_DIR + "/PT_DetectorTest"
TAG = "PTGenerated"
CUBE = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube.Cube")  # 100 cm cube, pivot at centre

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

ground_truth = {"map": MAP_PATH, "bugs": [], "controls": []}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def finish(actor, label, folder):
    actor.set_actor_label(label)
    actor.set_folder_path(folder)
    actor.set_editor_property("tags", [unreal.Name(TAG)])
    return actor


def spawn(actor_class, loc=(0, 0, 0), pitch=0.0, yaw=0.0, roll=0.0):
    return actors.spawn_actor_from_class(
        actor_class, unreal.Vector(*loc), unreal.Rotator(pitch=pitch, yaw=yaw, roll=roll))


def box(label, center, size, folder="PT/Geometry", collision=True, pitch=0.0, yaw=0.0):
    """Cube scaled to `size` (cm) centred on `center`. collision=False plants a 'missing collision' bug."""
    actor = spawn(unreal.StaticMeshActor, center, pitch=pitch, yaw=yaw)
    mesh = actor.static_mesh_component
    mesh.set_static_mesh(CUBE)
    actor.set_actor_scale3d(unreal.Vector(size[0] / 100.0, size[1] / 100.0, size[2] / 100.0))
    if not collision:
        mesh.set_collision_profile_name("NoCollision")
    return finish(actor, label, folder)


def add_bug(bug_id, expected_issue, location, radius, description, also_accept=()):
    ground_truth["bugs"].append({
        "id": bug_id,
        "expectedIssue": expected_issue,
        "acceptedIssues": [expected_issue] + list(also_accept),
        "location": list(location),
        "radius": radius,
        "description": description,
    })


def add_control(control_id, location, radius, description):
    ground_truth["controls"].append({
        "id": control_id,
        "location": list(location),
        "radius": radius,
        "description": description,
    })


# ---------------------------------------------------------------------------
# Open or create the map, remove previously generated actors
# ---------------------------------------------------------------------------
unreal.EditorAssetLibrary.make_directory(MAP_DIR)
if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
    level_editor.load_level(MAP_PATH)
    for existing in actors.get_all_level_actors():
        if existing.actor_has_tag(TAG):
            actors.destroy_actor(existing)
else:
    if not level_editor.new_level(MAP_PATH):
        raise RuntimeError("Could not create " + MAP_PATH)

# ---------------------------------------------------------------------------
# Layout constants (cm). Arena floor top is at Z = 0, arena spans -2000..2000 in X and Y.
# ---------------------------------------------------------------------------
TILE = 400
FLOOR_T = 20
HALF = 2000
FLOOR_HOLE_TILE = (-1000, 1000)
STUCK_SHAFT_TILE = (1000, 1000)

# ---------------------------------------------------------------------------
# Lighting, player start, navmesh
# ---------------------------------------------------------------------------
sun = spawn(unreal.DirectionalLight, (0, 0, 1000), pitch=-50.0, yaw=30.0)
sun.light_component.set_editor_property("atmosphere_sun_light", True)
finish(sun, "Sun", "PT/Lighting")

sky = spawn(unreal.SkyLight, (0, 0, 800))
sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sky.light_component.set_editor_property("real_time_capture", True)
finish(sky, "SkyLight", "PT/Lighting")

finish(spawn(unreal.SkyAtmosphere), "SkyAtmosphere", "PT/Lighting")

finish(spawn(unreal.PlayerStart, (0, 0, 100)), "PlayerStart", "PT")

nav = spawn(unreal.NavMeshBoundsVolume, (0, 0, 0))
nav.set_actor_scale3d(unreal.Vector(23.0, 23.0, 6.0))  # default brush is 200 cm -> ~4600 x 4600 x 1200
finish(nav, "NavMeshBounds", "PT")

# ---------------------------------------------------------------------------
# Floor: 10 x 10 tiles of 4 m. Two tiles are special (floor hole, stuck shaft).
# ---------------------------------------------------------------------------
for ix in range(10):
    for iy in range(10):
        cx = -HALF + TILE // 2 + ix * TILE
        cy = -HALF + TILE // 2 + iy * TILE
        if (cx, cy) == STUCK_SHAFT_TILE:
            continue  # built below
        is_hole = (cx, cy) == FLOOR_HOLE_TILE
        box(("BUG_FloorHole" if is_hole else "Floor_%d_%d" % (ix, iy)),
            (cx, cy, -FLOOR_T / 2), (TILE, TILE, FLOOR_T),
            folder=("PT/Bugs" if is_hole else "PT/Floor"),
            collision=not is_hole)

add_bug("floor_hole", "OutOfWorld", (FLOOR_HOLE_TILE[0], FLOOR_HOLE_TILE[1], 0), 400,
        "Floor tile is visible but has NoCollision; bots fall through into the void.",
        also_accept=("Clip",))  # the clip detector catches the bot passing through the tile

# ---------------------------------------------------------------------------
# Perimeter walls (3 m high) with one 4 m section missing on the +X side (edge gap).
# ---------------------------------------------------------------------------
WALL_T, WALL_H = 50, 300
EDGE_GAP_Y = 200
for i in range(10):
    c = -HALF + TILE // 2 + i * TILE
    box("Wall_S_%d" % i, (c, -HALF - WALL_T / 2, WALL_H / 2), (TILE, WALL_T, WALL_H), folder="PT/Walls")
    box("Wall_N_%d" % i, (c, HALF + WALL_T / 2, WALL_H / 2), (TILE, WALL_T, WALL_H), folder="PT/Walls")
    box("Wall_W_%d" % i, (-HALF - WALL_T / 2, c, WALL_H / 2), (WALL_T, TILE, WALL_H), folder="PT/Walls")
    if c != EDGE_GAP_Y:
        box("Wall_E_%d" % i, (HALF + WALL_T / 2, c, WALL_H / 2), (WALL_T, TILE, WALL_H), folder="PT/Walls")

add_bug("edge_gap", "OutOfWorld", (HALF, EDGE_GAP_Y, 0), 700,
        "Missing perimeter wall section; bots walk off the edge of the arena.")

# ---------------------------------------------------------------------------
# Stuck shaft: 1.2 x 1.2 m hole in the floor, 2 m deep. Too deep to jump out of, too narrow to move in.
# ---------------------------------------------------------------------------
sx, sy = STUCK_SHAFT_TILE
G = 60  # half gap
SHAFT_D = 200
f = "PT/Bugs/StuckShaft"
box("BUG_Shaft_FloorL", (sx - 130, sy, -FLOOR_T / 2), (140, TILE, FLOOR_T), folder=f)
box("BUG_Shaft_FloorR", (sx + 130, sy, -FLOOR_T / 2), (140, TILE, FLOOR_T), folder=f)
box("BUG_Shaft_FloorB", (sx, sy - 130, -FLOOR_T / 2), (2 * G, 140, FLOOR_T), folder=f)
box("BUG_Shaft_FloorT", (sx, sy + 130, -FLOOR_T / 2), (2 * G, 140, FLOOR_T), folder=f)
wall_z = -FLOOR_T - SHAFT_D / 2
box("BUG_Shaft_WallW", (sx - G - 10, sy, wall_z), (20, 2 * G + 40, SHAFT_D), folder=f)
box("BUG_Shaft_WallE", (sx + G + 10, sy, wall_z), (20, 2 * G + 40, SHAFT_D), folder=f)
box("BUG_Shaft_WallS", (sx, sy - G - 10, wall_z), (2 * G, 20, SHAFT_D), folder=f)
box("BUG_Shaft_WallN", (sx, sy + G + 10, wall_z), (2 * G, 20, SHAFT_D), folder=f)
box("BUG_Shaft_Bottom", (sx, sy, -FLOOR_T - SHAFT_D - 10), (2 * G + 40, 2 * G + 40, 20), folder=f)

add_bug("stuck_shaft", "Stuck", (sx, sy, -SHAFT_D), 250,
        "Narrow 2 m deep shaft; a bot that falls in cannot get out.")

# ---------------------------------------------------------------------------
# Clip room: closed 4 x 4 m room; its east wall is visible but has NoCollision.
# ---------------------------------------------------------------------------
rx, ry = -1400, -1400
ROOM_H = 250
f = "PT/Bugs/ClipRoom"
box("ClipRoom_WallW", (rx - 210, ry, ROOM_H / 2), (20, 440, ROOM_H), folder=f)
box("BUG_ClipRoom_WallE_NoCollision", (rx + 210, ry, ROOM_H / 2), (20, 440, ROOM_H), folder=f, collision=False)
box("ClipRoom_WallS", (rx, ry - 210, ROOM_H / 2), (400, 20, ROOM_H), folder=f)
box("ClipRoom_WallN", (rx, ry + 210, ROOM_H / 2), (400, 20, ROOM_H), folder=f)

add_bug("clip_wall", "Clip", (rx + 210, ry, ROOM_H / 2), 300,
        "Wall segment with NoCollision; bots walk through it into a closed room.")

# ---------------------------------------------------------------------------
# Forbidden ledge: 70 cm high block, too high to step onto (character steps 45 cm, navmesh 35 cm), reachable only by jumping.
# Marked with a TriggerBox tagged "PTForbidden" (the forbidden-area detector will look for this tag).
# ---------------------------------------------------------------------------
lx, ly = 1400, -1400
LEDGE_H = 70
box("BUG_ForbiddenLedge", (lx, ly, LEDGE_H / 2), (300, 300, LEDGE_H), folder="PT/Bugs")
trigger = spawn(unreal.TriggerBox, (lx, ly, LEDGE_H + 60))
trigger.get_component_by_class(unreal.BoxComponent).set_box_extent(unreal.Vector(150, 150, 60))
finish(trigger, "BUG_ForbiddenLedge_Volume", "PT/Bugs")
trigger.set_editor_property("tags", [unreal.Name(TAG), unreal.Name("PTForbidden")])

add_bug("forbidden_ledge", "ForbiddenArea", (lx, ly, LEDGE_H), 250,
        "Ledge designers did not intend to be reachable; reachable only with a jump.")

# ---------------------------------------------------------------------------
# Perf spot: 40 movable, shadow-casting point lights packed into one corner area.
# ---------------------------------------------------------------------------
px, py = 0, 1600
for i in range(8):
    for j in range(5):
        light = spawn(unreal.PointLight, (px - 210 + i * 60, py - 120 + j * 60, 150))
        comp = light.get_component_by_class(unreal.PointLightComponent)
        comp.set_mobility(unreal.ComponentMobility.MOVABLE)
        comp.set_cast_shadows(True)
        comp.set_intensity(5000.0)
        comp.set_attenuation_radius(800.0)
        finish(light, "BUG_PerfLight_%d_%d" % (i, j), "PT/Bugs/PerfLights")

add_bug("perf_lights", "PerfSpike", (px, py, 0), 800,
        "40 overlapping movable shadow-casting point lights; GPU cost spikes when looking at this area.")

# ---------------------------------------------------------------------------
# Control areas: normal traversal, detectors must stay silent here.
# ---------------------------------------------------------------------------
# Stairs (5 x 20 cm steps) up to a 1 m platform.
box("Ctrl_StairsPlatform", (0, -1500, 50), (400, 200, 100), folder="PT/Controls")
for i in range(1, 6):
    box("Ctrl_Step_%d" % i, (0, -1400 + 40 * (5 - i) + 20, 10 * i), (200, 40, 20 * i), folder="PT/Controls")
add_control("stairs_platform", (0, -1450, 100), 300, "Stairs up to a platform.")

# Ramp (~14 degrees) up to a 1 m platform.
box("Ctrl_RampPlatform", (-1600, 0, 50), (200, 400, 100), folder="PT/Controls")
ramp_len = math.hypot(400, 100)
ramp_pitch = -math.degrees(math.atan2(100, 400))
box("Ctrl_Ramp", (-1300, 0, 40), (ramp_len, 300, 20), folder="PT/Controls", pitch=ramp_pitch)
add_control("ramp_platform", (-1400, 0, 50), 400, "Ramp up to a platform.")

# Plain wall in the open: walking into it must not count as stuck.
box("Ctrl_FreeWall", (600, 400, 125), (400, 30, 250), folder="PT/Controls")
add_control("free_wall", (600, 400, 0), 300, "Single wall; pushing against it is not a trap.")

# ---------------------------------------------------------------------------
# Save map and answer key
# ---------------------------------------------------------------------------
level_editor.save_current_level()

out_dir = os.path.join(unreal.Paths.project_plugins_dir(), "PlaytestBots", "TestMaps")
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.abspath(os.path.join(out_dir, "PT_DetectorTest.ground_truth.json"))
with open(out_path, "w") as handle:
    json.dump(ground_truth, handle, indent=2)

nav_origin, nav_extent = nav.get_actor_bounds(False)
unreal.log("PT_DetectorTest built: %d bugs, %d control areas." % (len(ground_truth["bugs"]), len(ground_truth["controls"])))
unreal.log("Ground truth written to " + out_path)
unreal.log("NavMeshBounds extent: %s (should be roughly 2300 x 2300 x 600; if tiny, scale it by hand)" % nav_extent)
