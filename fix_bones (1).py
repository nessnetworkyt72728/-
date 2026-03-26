import bpy
import json
import os
import mathutils

# ==========================================
# CONFIGURATION
# ==========================================
# Replace this path with the full path to your JSON file
JSON_FILE_PATH = r"C:\Users\peppe\OneDrive\Desktop\nini\MVS Modding\Fmodel\Output\Exports\MultiVersus\Content\Panda_Main\Characters\C035\c035_Body_SkelMesh_Skeleton.json"

def create_bone_index_armature(json_path):
    print(f"Attempting to read JSON from: {json_path}")

    if not os.path.exists(json_path) or json_path == "INSERT_YOUR_JSON_PATH_HERE":
        print("Error: File not found or path not set. Please edit the JSON_FILE_PATH variable in the script.")
        return

    try:
        with open(json_path, 'r') as f:
            data = json.load(f)
    except Exception as e:
        print(f"Error parsing JSON: {e}")
        return

    # Helper to search for the map recursively
    def find_key(obj, key):
        if isinstance(obj, dict):
            if key in obj:
                return obj[key]
            for v in obj.values():
                res = find_key(v, key)
                if res:
                    return res
        elif isinstance(obj, list):
            for v in obj:
                res = find_key(v, key)
                if res:
                    return res
        return None

    bone_map = find_key(data, "FinalNameToIndexMap")

    if not bone_map:
        print("Error: Could not find 'FinalNameToIndexMap' in the JSON file.")
        return

    # Sort bones by index
    sorted_bones = sorted(bone_map.items(), key=lambda item: item[1])

    print(f"Found {len(sorted_bones)} bones in index map.")

    # Switch to Object mode if needed
    if bpy.context.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')

    # Create new armature object
    bpy.ops.object.add(type='ARMATURE', enter_editmode=True, location=(0, 0, 0))
    obj = bpy.context.object
    obj.name = "BoneIndexArmature"
    amt = obj.data
    amt.name = "BoneIndexArmatureData"

    bpy.ops.object.mode_set(mode='EDIT')

    # UNREAL SINGLE ROOT FIX:
    # Unreal Engine requires exactly ONE root bone in the hierarchy.
    # Every bone from index 1 onward is parented to bone 0 (star hierarchy).

    root_bone = None
    bone_length = 0.001

    for i, (name, index) in enumerate(sorted_bones):
        if name in amt.edit_bones:
            bone = amt.edit_bones[name]
        else:
            bone = amt.edit_bones.new(name)

        # Keep at origin to prevent transform accumulation
        bone.head = (0, 0, 0)
        bone.tail = (0, 0, bone_length)

        if i == 0:
            root_bone = bone
        elif root_bone:
            bone.parent = root_bone

    bpy.ops.object.mode_set(mode='OBJECT')

    # ==========================================
    # ORIENTATION FIX
    # ==========================================
    # Unreal uses a different coordinate system (X forward, Z up, Y right).
    # Blender uses Z up, Y forward. This rotation corrects the
    # upside-down and mirrored result when importing from Unreal/FModel.
    # Rotate -90 degrees on X axis to fix orientation.
    obj.rotation_euler = (0, 0, 0)
    obj.scale = (1, 1, 1)
    obj.location = (0, 0, 0)

    # Apply coordinate system correction: flip from Unreal to Blender space
    # This fixes the upside-down + mirrored issue
    obj.rotation_euler[0] = 3.14159265358979  # 180 degrees on X axis

    print(f"Successfully created armature with {len(sorted_bones)} bones.")
    print(f"Root bone: {sorted_bones[0][0]}. All other bones parented to it for Unreal compatibility.")
    print("Orientation fix applied: 180-degree rotation on X axis to correct Unreal coordinate system.")

# Run the function
create_bone_index_armature(JSON_FILE_PATH)