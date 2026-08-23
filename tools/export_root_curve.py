# Export the root-motion trajectory of a mantle/vault animation to JSON
# so F4Parkour can drive the player along the EXACT curve the animation
# was authored against (the "authored-curve" mantle rework).
#
# Run INSIDE Blender (pakour.blend): Scripting tab -> open this file ->
# adjust CONFIG below -> Run Script. Writes <OUT> next to the .blend.
#
# Contract with the plugin:
#   * Author the climb naturally against a reference ledge (root bone
#     actually travels up and onto it). This script samples the ROOT
#     bone's world position every frame and stores it relative to frame
#     one, in Blender axes, scaled to game units.
#   * The plugin is axis-agnostic: it takes the FINAL sample's horizontal
#     displacement as "authored forward", re-expresses every sample as
#     (forward, lateral, up), and maps those onto the measured approach
#     direction / right / world Z in game — scaling forward and up by
#     measured-vs-nominal so one authored climb serves nearby geometry.
#   * Time is exported both as seconds (scene FPS) and normalized 0..1.

import bpy
import json

# ---------------- CONFIG ----------------
ARMATURE_NAME = ""        # "" = the active object
ACTION_NAME = ""          # "" = the armature's currently assigned action
ROOT_BONE = "Root"        # pose bone whose world path is the trajectory
CAMERA_BONE = "Camera"    # sampled too when present ("" to skip)
SCALE = 1.0               # Blender units -> game units (FO4: 1u = 1.428cm;
                          #   if your rig is real-world meters use ~70.03)
OUT = bpy.path.abspath("//mantle_high.curve.json")
# -----------------------------------------


def get_armature():
    if ARMATURE_NAME:
        obj = bpy.data.objects.get(ARMATURE_NAME)
    else:
        obj = bpy.context.active_object
    if not obj or obj.type != 'ARMATURE':
        raise RuntimeError("Select the armature (or set ARMATURE_NAME). "
                           f"Got: {obj.name if obj else 'nothing'}")
    return obj


def bone_world_pos(arm, name):
    pb = arm.pose.bones.get(name)
    if not pb:
        return None
    m = arm.matrix_world @ pb.matrix
    return m.translation.copy()


def main():
    arm = get_armature()

    if ACTION_NAME:
        action = bpy.data.actions.get(ACTION_NAME)
        if not action:
            raise RuntimeError(f"No action named '{ACTION_NAME}'")
        if not arm.animation_data:
            arm.animation_data_create()
        arm.animation_data.action = action
    else:
        action = arm.animation_data.action if arm.animation_data else None
        if not action:
            raise RuntimeError("Armature has no active action; set ACTION_NAME")

    scene = bpy.context.scene
    fps = scene.render.fps / scene.render.fps_base
    f_start, f_end = (int(round(v)) for v in action.frame_range)
    if f_end <= f_start:
        raise RuntimeError(f"Action '{action.name}' has no length")

    saved_frame = scene.frame_current
    samples = []
    base_root = None
    base_cam = None
    try:
        for f in range(f_start, f_end + 1):
            scene.frame_set(f)
            root = bone_world_pos(arm, ROOT_BONE)
            if root is None:
                raise RuntimeError(f"No pose bone '{ROOT_BONE}' on {arm.name}")
            cam = bone_world_pos(arm, CAMERA_BONE) if CAMERA_BONE else None

            if base_root is None:
                base_root = root.copy()
                base_cam = cam.copy() if cam is not None else None

            entry = {
                "t": round((f - f_start) / fps, 5),
                "root": [round((root[i] - base_root[i]) * SCALE, 3) for i in range(3)],
            }
            if cam is not None and base_cam is not None:
                entry["camera"] = [round((cam[i] - base_cam[i]) * SCALE, 3) for i in range(3)]
            samples.append(entry)
    finally:
        scene.frame_set(saved_frame)

    duration = (f_end - f_start) / fps
    final = samples[-1]["root"]
    data = {
        "source": {"blend": bpy.data.filepath, "action": action.name,
                   "rootBone": ROOT_BONE, "fps": fps, "scale": SCALE},
        "duration": round(duration, 5),
        "nominalEnd": final,       # plugin derives forward axis + rise from this
        "samples": samples,
    }

    with open(OUT, "w") as fh:
        json.dump(data, fh, indent=1)

    print(f"Wrote {len(samples)} samples over {duration:.2f}s to {OUT}")
    print(f"Nominal end displacement (game units): {final}")


main()
