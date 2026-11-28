"""
build_holdout_rooms_map.py
Builds /Game/PlaytestBots/Maps/PT_HoldoutRooms: a held-out evaluation map in a different style from the
training gyms. A 30 x 20 m building of 3 x 2 rooms (10 x 10 m, 4 m walls, 2 m doorways), floating over the
void, with a roof deck over two rooms reached by stairs (parapets and outer walls taller than a jump). Never used for training.

Planted bugs (answer key in Plugins/PlaytestBots/TestMaps/PT_HoldoutRooms.ground_truth.json):
  sealed_room_clip   the only way into the east room is a doorway "closed" by a wall panel without collision
  floor_hole         a 2 x 2 m floor tile without collision in the middle room
  stuck_pit          a 1.2 m wide, 2 m deep pit in a room corner
  forbidden_crate    a 70 cm crate stack designers did not mean to be climbable (forbidden volume on top)
  deck_edge_gap      a missing parapet section on the roof deck's outer edge (fall into the void)
Control areas: stairs, doorways, a crouch-only vent, pillars, jumpable crates.

Run in the editor: Tools -> Execute Python Script... -> pick this file. Safe to re-run.
Then Build -> Build Paths, wait for the navmesh, save the map.

Location: Plugins/PlaytestBots/Scripts/build_holdout_rooms_map.py
"""

import json
import os

import unreal

MAP_DIR = "/Game/PlaytestBots/Maps"
MAP_NAME = "PT_HoldoutRooms"
MAP_PATH = MAP_DIR + "/" + MAP_NAME
TAG = "PTGenerated"
CUBE = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube.Cube")

ROOM = 1000          # room size, cm
COLS, ROWS = 3, 2    # rooms along X, Y
WALL_H = 400
WALL_T = 30
DOOR_W = 200
FLOOR_T = 20
FLOOR_TILE = 200     # floor is built from 2 m tiles so one can be the hole
DECK_Z = WALL_H      # deck slab sits on the wall tops
PARAPET_H = 300      # deck parapet; must be taller than a jump (template character jumps ~2.5 m)
OUTER_H = 700        # outer walls where there is no deck, so bots on the stairs can't jump onto them

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
ground_truth = {"map": MAP_PATH, "bugs": [], "controls": []}


def finish(actor, label, folder):
    actor.set_actor_label(label)
    actor.set_folder_path(folder)
    actor.set_editor_property("tags", [unreal.Name(TAG)])
    return actor


def spawn(actor_class, loc=(0, 0, 0), pitch=0.0, yaw=0.0):
    return actors.spawn_actor_from_class(actor_class, unreal.Vector(*loc), unreal.Rotator(pitch=pitch, yaw=yaw, roll=0.0))


def box(label, center, size, folder="PT/Geometry", collision=True):
    actor = spawn(unreal.StaticMeshActor, center)
    mesh = actor.static_mesh_component
    mesh.set_static_mesh(CUBE)
    actor.set_actor_scale3d(unreal.Vector(size[0] / 100.0, size[1] / 100.0, size[2] / 100.0))
    if not collision:
        mesh.set_collision_profile_name("NoCollision")
    return finish(actor, label, folder)


def add_bug(bug_id, expected, location, radius, description, also_accept=()):
    ground_truth["bugs"].append({"id": bug_id, "expectedIssue": expected, "acceptedIssues": [expected] + list(also_accept),
                                 "location": list(location), "radius": radius, "description": description})


def add_control(control_id, location, radius, description):
    ground_truth["controls"].append({"id": control_id, "location": list(location), "radius": radius, "description": description})


def wall_x(label, x0, x1, y, door_at=None, door_w=DOOR_W, height=WALL_H, z0=0, folder="PT/Walls"):
    """Wall along X from x0 to x1 at y; optional doorway centred at door_at."""
    segments = [(x0, x1)] if door_at is None else [(x0, door_at - door_w / 2), (door_at + door_w / 2, x1)]
    for i, (a, b) in enumerate(segments):
        if b - a > 1:
            box("%s_%d" % (label, i), ((a + b) / 2, y, z0 + height / 2), (b - a, WALL_T, height), folder=folder)


def wall_y(label, y0, y1, x, door_at=None, door_w=DOOR_W, height=WALL_H, z0=0, folder="PT/Walls"):
    segments = [(y0, y1)] if door_at is None else [(y0, door_at - door_w / 2), (door_at + door_w / 2, y1)]
    for i, (a, b) in enumerate(segments):
        if b - a > 1:
            box("%s_%d" % (label, i), (x, (a + b) / 2, z0 + height / 2), (WALL_T, b - a, height), folder=folder)


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

W, H = COLS * ROOM, ROWS * ROOM   # building spans x 0..W, y 0..H

# Lighting, start, navmesh
sun = spawn(unreal.DirectionalLight, (0, 0, 1500), pitch=-55.0, yaw=40.0)
sun.light_component.set_editor_property("atmosphere_sun_light", True)
finish(sun, "Sun", "PT/Lighting")
sky = spawn(unreal.SkyLight, (0, 0, 1200))
sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sky.light_component.set_editor_property("real_time_capture", True)
finish(sky, "SkyLight", "PT/Lighting")
finish(spawn(unreal.SkyAtmosphere), "SkyAtmosphere", "PT/Lighting")
finish(spawn(unreal.PlayerStart, (ROOM / 2, ROOM / 2, 100)), "PlayerStart", "PT")
nav = spawn(unreal.NavMeshBoundsVolume, (W / 2, H / 2, 200))
nav.set_actor_scale3d(unreal.Vector((W / 2 + 300) / 100.0, (H / 2 + 300) / 100.0, 6.0))
finish(nav, "NavMeshBounds", "PT")

# Floor tiles (one is the hole: middle room, off-centre so the doorway path doesn't always cross it)
HOLE = (ROOM + 500, ROOM + 300)   # a tile centre (tiles are centred on odd hundreds) in room (1,1)
for ix in range(int(W / FLOOR_TILE)):
    for iy in range(int(H / FLOOR_TILE)):
        cx, cy = ix * FLOOR_TILE + FLOOR_TILE / 2, iy * FLOOR_TILE + FLOOR_TILE / 2
        if (cx, cy) == HOLE:
            box("BUG_FloorHole", (cx, cy, -FLOOR_T / 2), (FLOOR_TILE, FLOOR_TILE, FLOOR_T), folder="PT/Bugs", collision=False)
        elif abs(cx - (2 * ROOM + 100)) < 1 and abs(cy - 100) < 1:
            continue  # stuck pit tile, built below
        else:
            box("Floor_%d_%d" % (ix, iy), (cx, cy, -FLOOR_T / 2), (FLOOR_TILE, FLOOR_TILE, FLOOR_T), folder="PT/Floor")
add_bug("floor_hole", "OutOfWorld", (HOLE[0], HOLE[1], 0), 300, "Floor tile without collision.", also_accept=("Clip",))

# Outer walls (rooms are open to the sky). Under the deck (rooms (0,0) and (1,0)) they stop at the deck;
# elsewhere they are tall enough that nobody jumps onto them.
wall_x("OuterS_deck", 0, 2 * ROOM, -WALL_T / 2)
wall_x("OuterS", 2 * ROOM, W, -WALL_T / 2, height=OUTER_H)
wall_x("OuterN", 0, W, H + WALL_T / 2, height=OUTER_H)
wall_y("OuterW_deck", 0, ROOM, -WALL_T / 2)
wall_y("OuterW", ROOM, H, -WALL_T / 2, height=OUTER_H)
wall_y("OuterE", 0, H, W + WALL_T / 2, height=OUTER_H)

# Interior walls with doorways. Rooms: (col, row). The east-north room (2,1) is sealed.
for col in range(1, COLS):                       # walls between columns, per row
    x = col * ROOM
    for row in range(ROWS):
        y0, y1 = row * ROOM, (row + 1) * ROOM
        if (col, row) == (2, 1):
            wall_y("Inner_c%d_r%d" % (col, row), y0, y1, x)           # (1,1)|(2,1): solid, no door
        else:
            wall_y("Inner_c%d_r%d" % (col, row), y0, y1, x, door_at=(y0 + y1) / 2)
            add_control("door_c%d_r%d" % (col, row), (x, (y0 + y1) / 2, 0), 200, "Doorway.")
for col in range(COLS):                          # wall between rows
    x0, x1 = col * ROOM, (col + 1) * ROOM
    y = ROOM
    if col == 2:
        # (2,0)|(2,1): the doorway is "closed" by a panel without collision -> clip into the sealed room
        wall_x("Inner_row_c2", x0, x1, y, door_at=(x0 + x1) / 2)
        box("BUG_SealedRoom_Panel", ((x0 + x1) / 2, y, WALL_H / 2), (DOOR_W, WALL_T, WALL_H), folder="PT/Bugs", collision=False)
        add_bug("sealed_room_clip", "Clip", ((x0 + x1) / 2, y, 100), 300, "Doorway closed by a panel without collision.")
    elif col == 1:
        # Doorway plus a crouch-only vent (1.0 m high opening) further along
        wall_x("Inner_row_c1", x0, x1, y, door_at=x0 + 300)
        vent_x = x0 + 750
        box("Vent_Lintel", (vent_x, y, 100 + (WALL_H - 100) / 2), (160, WALL_T, WALL_H - 100), folder="PT/Walls")
        add_control("crouch_vent", (vent_x, y, 0), 200, "Crouch-only vent (1 m high).")
        add_control("door_row_c1", (x0 + 300, y, 0), 200, "Doorway.")
    else:
        wall_x("Inner_row_c0", x0, x1, y, door_at=(x0 + x1) / 2)
        add_control("door_row_c0", ((x0 + x1) / 2, y, 0), 200, "Doorway.")

# The vent: the wall segment above is built; carve the opening by splitting the row wall around it.
# (wall_x above built one segment over the vent position; replace it with two segments around the vent)
for existing in actors.get_all_level_actors():
    if existing.get_actor_label() == "Inner_row_c1_1":
        actors.destroy_actor(existing)
c1_x0, c1_x1 = ROOM, 2 * ROOM
seg_a = (c1_x0 + 300 + DOOR_W / 2, c1_x0 + 750 - 80)
seg_b = (c1_x0 + 750 + 80, c1_x1)
for i, (a, b) in enumerate((seg_a, seg_b)):
    box("Inner_row_c1_v%d" % i, ((a + b) / 2, ROOM, WALL_H / 2), (b - a, WALL_T, WALL_H), folder="PT/Walls")

# Stuck pit in the corner of room (2,0)
px, py = 2 * ROOM + 100, 100
G, D = 60, 200
side = FLOOR_TILE / 2 - G
f = "PT/Bugs/StuckPit"
box("Pit_FloorL", (px - G - side / 2, py, -FLOOR_T / 2), (side, FLOOR_TILE, FLOOR_T), folder=f)
box("Pit_FloorR", (px + G + side / 2, py, -FLOOR_T / 2), (side, FLOOR_TILE, FLOOR_T), folder=f)
box("Pit_FloorB", (px, py - G - side / 2, -FLOOR_T / 2), (2 * G, side, FLOOR_T), folder=f)
box("Pit_FloorT", (px, py + G + side / 2, -FLOOR_T / 2), (2 * G, side, FLOOR_T), folder=f)
wz = -FLOOR_T - D / 2
box("Pit_WallW", (px - G - 10, py, wz), (20, 2 * G + 40, D), folder=f)
box("Pit_WallE", (px + G + 10, py, wz), (20, 2 * G + 40, D), folder=f)
box("Pit_WallS", (px, py - G - 10, wz), (2 * G, 20, D), folder=f)
box("Pit_WallN", (px, py + G + 10, wz), (2 * G, 20, D), folder=f)
box("Pit_Bottom", (px, py, -FLOOR_T - D - 10), (2 * G + 40, 2 * G + 40, 20), folder=f)
add_bug("stuck_pit", "Stuck", (px, py, -D), 250, "Narrow 2 m deep pit in a room corner.")

# Forbidden crate stack in room (1,0) (under the deck)
cx, cy = ROOM + 300, 700
box("BUG_ForbiddenCrate", (cx, cy, 35), (240, 240, 70), folder="PT/Bugs")
trigger = spawn(unreal.TriggerBox, (cx, cy, 70 + 60))
trigger.get_component_by_class(unreal.BoxComponent).set_box_extent(unreal.Vector(120, 120, 60))
finish(trigger, "BUG_ForbiddenCrate_Volume", "PT/Bugs")
trigger.set_editor_property("tags", [unreal.Name(TAG), unreal.Name("PTForbidden")])
add_bug("forbidden_crate", "ForbiddenArea", (cx, cy, 70), 250, "Crate stack designers did not mean to be climbable.")

# Controls: pillars in room (1,1), jumpable crates in room (0,1)
for i, (x, y) in enumerate(((ROOM + 300, ROOM + 300), (ROOM + 700, ROOM + 700))):
    box("Pillar_%d" % i, (x, y, WALL_H / 2), (60, 60, WALL_H), folder="PT/Controls")
    add_control("pillar_%d" % i, (x, y, 0), 200, "Pillar.")
for i, (x, y, h) in enumerate(((800, ROOM + 700, 60), (700, ROOM + 300, 75))):
    box("Crate_%d" % i, (x, y, h / 2), (150, 150, h), folder="PT/Controls")
    add_control("crate_%d" % i, (x, y, 0), 200, "Jumpable crate (allowed).")

# Roof deck over rooms (0,0) and (1,0), reached by stairs in room (0,1) along its west wall, rising towards -Y
deck_x0, deck_x1, deck_y0, deck_y1 = 0, 2 * ROOM, 0, ROOM
box("Deck", ((deck_x0 + deck_x1) / 2, (deck_y0 + deck_y1) / 2, DECK_Z + 10), (deck_x1 - deck_x0, deck_y1 - deck_y0, 20), folder="PT/Deck")
deck_top = DECK_Z + 20
steps = 21
step_h = deck_top / steps
step_d = 35
stair_x = 200                                   # flight 2 m wide: x 100..300
stair_y_bottom = ROOM + 50 + steps * step_d     # lowest step furthest from the row wall
for i in range(steps):
    h = step_h * (i + 1)
    box("Stair_%02d" % i, (stair_x, stair_y_bottom - step_d * (i + 0.5), h / 2), (200, step_d, h), folder="PT/Deck")
# landing at the top, bridging the row wall onto the deck
box("Stair_Landing", (stair_x, ROOM, deck_top - 10), (200, 200, 20), folder="PT/Deck")
add_control("stairs", (stair_x, ROOM + 50 + steps * step_d / 2, 0), 450, "Stairs up to the roof deck.")

# Deck parapets (a 2 m gap in the outer south parapet = edge-gap bug; an opening on the north side for the stairs)
PT_ = 30
pz = deck_top + PARAPET_H / 2
gap_x = 1400
box("Parapet_S_a", ((deck_x0 + gap_x - 100) / 2, deck_y0 + PT_ / 2, pz), (gap_x - 100 - deck_x0, PT_, PARAPET_H), folder="PT/Deck")
box("Parapet_S_b", ((gap_x + 100 + deck_x1) / 2, deck_y0 + PT_ / 2, pz), (deck_x1 - gap_x - 100, PT_, PARAPET_H), folder="PT/Deck")
box("Parapet_W", (deck_x0 + PT_ / 2, (deck_y0 + deck_y1) / 2, pz), (PT_, deck_y1 - deck_y0, PARAPET_H), folder="PT/Deck")
box("Parapet_E", (deck_x1 - PT_ / 2, (deck_y0 + deck_y1) / 2, pz), (PT_, deck_y1 - deck_y0, PARAPET_H), folder="PT/Deck")
open_x0, open_x1 = stair_x - 100, stair_x + 100
box("Parapet_N_a", ((deck_x0 + open_x0) / 2, deck_y1 - PT_ / 2, pz), (open_x0 - deck_x0, PT_, PARAPET_H), folder="PT/Deck")
box("Parapet_N_b", ((open_x1 + deck_x1) / 2, deck_y1 - PT_ / 2, pz), (deck_x1 - open_x1, PT_, PARAPET_H), folder="PT/Deck")
add_bug("deck_edge_gap", "OutOfWorld", (gap_x, deck_y0, deck_top), 400, "Missing parapet section on the roof deck's outer edge.")

level_editor.save_current_level()
out_dir = os.path.join(unreal.Paths.project_plugins_dir(), "PlaytestBots", "TestMaps")
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.abspath(os.path.join(out_dir, MAP_NAME + ".ground_truth.json"))
with open(out_path, "w") as handle:
    json.dump(ground_truth, handle, indent=2)
unreal.log("%s built: %d bugs, %d control areas. Ground truth: %s" % (MAP_NAME, len(ground_truth["bugs"]), len(ground_truth["controls"]), out_path))
unreal.log_warning("NEXT: Build -> Build Paths, check the navmesh reaches the deck (press P), then save the map.")
