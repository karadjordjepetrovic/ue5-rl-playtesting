"""
build_gym_test_map.py
Builds /Game/PlaytestBots/Maps/PT_GymTest: an empty map with one APTGymLayout (procedural training gym),
lighting, a PlayerStart in the gym's spawn area and a NavMeshBoundsVolume. The layout itself is generated at
runtime (BeginPlay), so the map looks empty in the editor; press Play to see it.

Needs: the plugin built with PTGymLayout, and Project Settings -> Navigation Mesh -> Runtime Generation = Dynamic.
Run in the editor: Tools -> Execute Python Script... -> pick this file. Safe to re-run.

Location: Plugins/PlaytestBots/Scripts/build_gym_test_map.py
"""

import unreal

MAP_DIR = "/Game/PlaytestBots/Maps"
MAP_PATH = MAP_DIR + "/PT_GymTest"
TAG = "PTGenerated"
GRID_SIZE = 8      # tiles per side (must match the gym's GridSize)
TILE = 400         # cm

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)

if not hasattr(unreal, "PTGymLayout"):
    raise RuntimeError("PTGymLayout class not found: close the editor, build the project in Visual Studio, reopen.")


def finish(actor, label, folder):
    actor.set_actor_label(label)
    actor.set_folder_path(folder)
    actor.set_editor_property("tags", [unreal.Name(TAG)])
    return actor


def spawn(actor_class, loc=(0, 0, 0), pitch=0.0, yaw=0.0):
    return actors.spawn_actor_from_class(actor_class, unreal.Vector(*loc), unreal.Rotator(pitch=pitch, yaw=yaw, roll=0.0))


unreal.EditorAssetLibrary.make_directory(MAP_DIR)
if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
    level_editor.load_level(MAP_PATH)
    for existing in actors.get_all_level_actors():
        if existing.actor_has_tag(TAG):
            actors.destroy_actor(existing)
else:
    if not level_editor.new_level(MAP_PATH):
        raise RuntimeError("Could not create " + MAP_PATH)

# Lighting
sun = spawn(unreal.DirectionalLight, (0, 0, 1000), pitch=-50.0, yaw=30.0)
sun.light_component.set_editor_property("atmosphere_sun_light", True)
finish(sun, "Sun", "PT/Lighting")
sky = spawn(unreal.SkyLight, (0, 0, 800))
sky.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sky.light_component.set_editor_property("real_time_capture", True)
finish(sky, "SkyLight", "PT/Lighting")
finish(spawn(unreal.SkyAtmosphere), "SkyAtmosphere", "PT/Lighting")

# The gym (layout is built at runtime around its location; floor top at Z = 0)
gym = spawn(unreal.PTGymLayout, (0, 0, 0))
finish(gym, "PTGym", "PT")

# Spawn area: the centre tiles always stay open floor.
finish(spawn(unreal.PlayerStart, (0, 0, 100)), "PlayerStart", "PT")

# Navmesh bounds: the gym plus a margin, from below the shafts to above the walls.
half = GRID_SIZE * TILE / 2 + 300
nav = spawn(unreal.NavMeshBoundsVolume, (0, 0, 100))
nav.set_actor_scale3d(unreal.Vector(half / 100.0, half / 100.0, 5.0))  # default brush is 200 cm
finish(nav, "NavMeshBounds", "PT")

level_editor.save_current_level()
unreal.log("PT_GymTest built: gym %d x %d tiles of %d cm." % (GRID_SIZE, GRID_SIZE, TILE))
unreal.log_warning("NEXT: Build -> Build Paths once (creates the RecastNavMesh actor), save the map, then press Play. "
                   "With Runtime Generation = Dynamic the navmesh is rebuilt around the generated layout.")
