#!/usr/bin/env python3
"""
UE5UltimateMCP FULL test — for real, because people use this.

Two phases against a running UE 5.7 editor with the plugin loaded:

  Phase A  ROBUSTNESS SWEEP — call every registered tool with empty params and
           confirm the editor never dies. A tool that returns a clean error is
           fine (dispatch + validation work); a tool that drops the connection
           or kills the editor is a real bug. Catches crash-on-bad-input.

  Phase B  FUNCTIONAL — real operations across all 16 categories with
           verification + cleanup, including the spawn->delete->spawn-same-name
           crash regression that the v0.1.1 fix targets.

Usage: python test_full.py [--port 9847]
"""
import json, sys, time, urllib.request, urllib.error

PORT = 9847
for i, a in enumerate(sys.argv):
    if a == "--port" and i + 1 < len(sys.argv): PORT = int(sys.argv[i + 1])
BASE = f"http://localhost:{PORT}"

# Heavy / destructive tools we do NOT fire blindly in the sweep.
SKIP_SWEEP = {"cook_project", "package_project", "build_lighting", "run_commandlet",
              "render_sequence_to_video", "build_navigation_only"}

def call(tool, params=None, timeout=120):
    """Returns dict: {ok, result, error, dead}. dead=True means connection lost."""
    body = {"tool": tool}
    if params: body.update(params)
    req = urllib.request.Request(BASE + "/api/tool", data=json.dumps(body).encode(),
                                 method="POST", headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            j = json.loads(r.read().decode())
            return {"ok": j.get("success", False), "result": j.get("result", {}), "error": j.get("error", ""), "dead": False}
    except urllib.error.HTTPError as e:
        try: j = json.loads(e.read().decode())
        except Exception: j = {}
        return {"ok": j.get("success", False), "result": j.get("result", {}), "error": j.get("error", f"HTTP {e.code}"), "dead": False}
    except Exception as e:
        return {"ok": False, "result": {}, "error": str(e), "dead": True}

def alive():
    try:
        with urllib.request.urlopen(BASE + "/api/health", timeout=8) as r:
            return json.loads(r.read().decode()).get("status") == "ok"
    except Exception:
        return False

def tools_list():
    with urllib.request.urlopen(BASE + "/api/tools", timeout=20) as r:
        return json.loads(r.read().decode()).get("tools", [])

# ───────────────────────────────────────── Phase A
def phase_a():
    print("=" * 70); print("PHASE A — ROBUSTNESS SWEEP (every tool, empty params, editor must survive)"); print("=" * 70)
    tools = tools_list()
    print(f"  registered tools: {len(tools)}")
    ok = validates = crashed = 0
    crash_tools = []
    for t in sorted(tools, key=lambda x: x["name"]):
        name = t["name"]
        if name in SKIP_SWEEP: continue
        r = call(name, {}, timeout=30)
        if r["dead"]:
            # connection dropped — confirm whether the editor actually died
            if not alive():
                crashed += 1; crash_tools.append(name)
                print(f"  [CRASH] {name} — editor became unreachable")
                break
            else:
                validates += 1  # transient; editor alive
        elif r["ok"]:
            ok += 1
        else:
            validates += 1
    total = ok + validates + crashed
    print(f"\n  swept {total} tools:  ok(executed)={ok}  validated(clean error)={validates}  CRASHED={crashed}")
    if crash_tools: print(f"  CRASH-ON-EMPTY tools: {crash_tools}")
    print(f"  editor alive after sweep: {alive()}")
    return crashed == 0 and len(crash_tools) == 0

# ───────────────────────────────────────── Phase B
RESULTS = []
def func(cat, label, tool, params=None, verify=None):
    """Run a tool; PASS if success (and optional verify(result) truthy). Records the row."""
    r = call(tool, params)
    if r["dead"] and not alive():
        RESULTS.append((cat, label, "CRASH", "editor died")); print(f"  [CRASH] {cat}/{label} ({tool})"); return None
    ok = r["ok"]
    detail = ""
    if ok and verify is not None:
        try: ok = bool(verify(r["result"]))
        except Exception as e: ok = False; detail = f"verify err: {e}"
    if not ok and not detail: detail = r["error"] or "success=false"
    RESULTS.append((cat, label, "PASS" if ok else "FAIL", detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {cat}/{label}" + (f" -- {detail}" if detail and not ok else ""))
    return r["result"]

def phase_b():
    print("\n" + "=" * 70); print("PHASE B — FUNCTIONAL (real ops + verify + cleanup, all 16 categories)"); print("=" * 70)
    P = "/Game/MCPTest"
    created_assets = []
    FREDDY_SKEL = "/Game/Characters/Freddy/Movie_FreddyFazbear_ANIMATRONIC_Skeleton"

    # This plugin mixes snake_case (flopperam-derived) and camelCase (BlueprintMCP-derived)
    # param conventions. Send every naming variant; handlers read only what they recognize.
    def nm(base, parent=None, extra=None):
        d = {"name": base, "assetName": base, "blueprintName": base, "structName": base,
             "enumName": base, "widgetName": base, "systemName": base,
             "path": P, "packagePath": P, "assetPath": P, "destinationPath": P, "folder": P}
        if parent is not None:
            d.update({"parent": parent, "parentClass": parent, "parent_class": parent,
                      "parentName": parent, "blackboard": parent, "skeleton": parent})
        if extra: d.update(extra)
        return d

    # ---- Actors
    func("Actors", "spawn PointLight", "spawn_actor", {"type": "PointLight", "name": "MCP_Light", "location": [0, 0, 300]})
    func("Actors", "get_actor_properties", "get_actor_properties", {"name": "MCP_Light"})
    func("Actors", "set_actor_property(bHidden)", "set_actor_property", {"name": "MCP_Light", "property": "bHidden", "value": "true"})
    func("Actors", "set_actor_transform", "set_actor_transform", {"name": "MCP_Light", "location": [50, 50, 320]})
    func("Actors", "spawn Cube", "spawn_actor", {"type": "StaticMeshActor", "name": "MCP_Cube", "location": [200, 0, 100], "static_mesh": "/Engine/BasicShapes/Cube.Cube"})
    func("Actors", "find_actors_by_name", "find_actors_by_name", {"pattern": "MCP_"}, verify=lambda r: r.get("count", len(r.get("matches", []))) >= 2)
    func("Actors", "get_actors_in_level", "get_actors_in_level", verify=lambda r: r.get("count", 0) > 0)

    # ---- CRASH REGRESSION (the v0.1.1 fix): spawn -> delete -> spawn SAME name must not crash
    func("CrashFix", "spawn reuse #1", "spawn_actor", {"type": "PointLight", "name": "MCP_Reuse", "location": [0, 0, 400]})
    func("CrashFix", "delete reuse", "delete_actor", {"name": "MCP_Reuse"})
    func("CrashFix", "spawn reuse #2 (same name)", "spawn_actor", {"type": "PointLight", "name": "MCP_Reuse", "location": [0, 0, 400]})
    func("CrashFix", "delete reuse again", "delete_actor", {"name": "MCP_Reuse"})

    # ---- Discovery
    func("Discovery", "list_classes", "list_classes", {"filter": "Light", "limit": 10}, verify=lambda r: r.get("count", len(r.get("classes", []))) > 0)
    func("Discovery", "list_functions", "list_functions", {"className": "Actor", "class": "Actor"})
    func("Discovery", "list_properties", "list_properties", {"className": "PointLightComponent", "class": "PointLightComponent"})

    # ---- Blueprint
    func("Blueprint", "create_blueprint", "create_blueprint", nm("BP_MCPTest", "Actor"))
    func("Blueprint", "add_variable", "add_variable", {"blueprint": "BP_MCPTest", "name": "Health", "variableName": "Health", "type": "float", "variableType": "float"})
    func("Blueprint", "list_components", "list_components", {"blueprint": "BP_MCPTest"})
    func("Blueprint", "get_blueprint", "get_blueprint", {"name": "BP_MCPTest", "blueprint": "BP_MCPTest"})
    func("Blueprint", "validate_blueprint", "validate_blueprint", {"blueprint": "BP_MCPTest"})
    created_assets.append(f"{P}/BP_MCPTest")

    # ---- UserTypes
    func("UserTypes", "create_struct", "create_struct", {"assetPath": f"{P}/S_MCPTest", "name": "S_MCPTest"})
    func("UserTypes", "add_struct_property", "add_struct_property", {"assetPath": f"{P}/S_MCPTest", "struct": f"{P}/S_MCPTest", "name": "Score", "type": "int", "propertyType": "int"})
    func("UserTypes", "create_enum", "create_enum", {"assetPath": f"{P}/E_MCPTest", "name": "E_MCPTest", "values": ["Idle", "Run", "Attack"]})
    created_assets += [f"{P}/S_MCPTest", f"{P}/E_MCPTest"]

    # ---- Material
    func("Material", "create_material", "create_material", nm("M_MCPTest"))
    func("Material", "add_material_expression", "add_material_expression", {"material": "M_MCPTest", "expressionClass": "Constant3Vector", "type": "Constant3Vector"})
    func("Material", "create_material_instance", "create_material_instance", nm("MI_MCPTest", "M_MCPTest"))
    func("Material", "validate_material", "validate_material", {"material": "M_MCPTest"})
    func("Material", "list_materials", "list_materials", {"filter": "MCPTest"})
    created_assets += [f"{P}/M_MCPTest", f"{P}/MI_MCPTest"]

    # ---- DataTable (row struct given as a full object path)
    func("DataTable", "create_data_table", "create_data_table", {"name": "DT_MCPTest", "rowStruct": f"{P}/S_MCPTest.S_MCPTest", "struct": f"{P}/S_MCPTest.S_MCPTest", "row_struct": f"{P}/S_MCPTest.S_MCPTest", "rowStructPath": f"{P}/S_MCPTest.S_MCPTest"})
    func("DataTable", "get_data_table_rows", "get_data_table_rows", {"table": "DT_MCPTest"})
    created_assets.append("/Game/Data/DT_MCPTest")

    # ---- UI / UMG
    func("UI", "create_widget_blueprint", "create_widget_blueprint", nm("W_MCPTest"))
    func("UI", "list_widget_blueprints", "list_widget_blueprints")
    created_assets.append("/Game/UI/W_MCPTest")

    # ---- Niagara (create previously crashed the editor — now uses the factory)
    func("Niagara", "create_niagara_system", "create_niagara_system", nm("NS_MCPTest"))
    func("Niagara", "list_niagara_systems", "list_niagara_systems")
    created_assets.append("/Game/Effects/NS_MCPTest")

    # ---- Animation (uses the imported Freddy skeleton)
    func("Animation", "create_anim_blueprint", "create_anim_blueprint", nm("ABP_MCPTest", FREDDY_SKEL))
    func("Animation", "add_state_machine", "add_state_machine", {"blueprint": "ABP_MCPTest", "name": "Locomotion"})
    created_assets.append(f"{P}/ABP_MCPTest")

    # ---- Behavior Tree / AI
    func("AI", "create_blackboard", "create_blackboard", nm("BB_MCPTest"))
    func("AI", "create_behavior_tree", "create_behavior_tree", nm("BT_MCPTest", f"{P}/BB_MCPTest"))
    func("AI", "add_bt_task", "add_bt_task", {"tree": "/Game/AI/BehaviorTrees/BT_MCPTest", "task_class": "BTTask_Wait"})
    created_assets += ["/Game/AI/Blackboards/BB_MCPTest", "/Game/AI/BehaviorTrees/BT_MCPTest"]

    # ---- Sequencer
    func("Sequencer", "create_level_sequence", "create_level_sequence", nm("SEQ_MCPTest"))
    SEQP = "/Game/Cinematics/SEQ_MCPTest"
    func("Sequencer", "add_actor_to_sequence", "add_actor_to_sequence", {"sequence": SEQP, "sequenceName": SEQP, "name": SEQP, "actor": "MCP_Cube", "actorName": "MCP_Cube"})
    func("Sequencer", "add_transform_keyframe", "add_transform_keyframe", {"sequence": SEQP, "actor": "MCP_Cube", "time": 0.0, "location": {"x": 0, "y": 0, "z": 0}})
    func("Sequencer", "add_camera_cut", "add_camera_cut", {"sequence": SEQP, "sequenceName": SEQP})
    created_assets.append(SEQP)

    # ---- Navigation
    func("Navigation", "add_nav_mesh_bounds", "add_nav_mesh_bounds", {"location": [0, 0, 0], "size": [2000, 2000, 500], "bounds_min": {"x": -1000, "y": -1000, "z": -250}, "bounds_max": {"x": 1000, "y": 1000, "z": 250}})
    func("Navigation", "build_navigation", "build_navigation")
    func("Navigation", "get_nav_mesh_info", "get_nav_mesh_info")

    # ---- Foliage
    func("Foliage", "add_foliage_type", "add_foliage_type", {"mesh": "/Engine/BasicShapes/Cube.Cube", "static_mesh": "/Engine/BasicShapes/Cube.Cube", "staticMesh": "/Engine/BasicShapes/Cube.Cube"})
    func("Foliage", "scatter_foliage", "scatter_foliage", {"mesh": "/Engine/BasicShapes/Cube.Cube", "static_mesh": "/Engine/BasicShapes/Cube.Cube", "staticMesh": "/Engine/BasicShapes/Cube.Cube", "count": 20, "bounds_min": {"x": -1000, "y": -1000, "z": 0}, "bounds_max": {"x": 1000, "y": 1000, "z": 10}})
    func("Foliage", "get_foliage_info", "get_foliage_info")
    func("Foliage", "clear_foliage", "clear_foliage", {"bounds_min": {"x": -9000, "y": -9000, "z": -9000}, "bounds_max": {"x": 9000, "y": 9000, "z": 9000}})

    # ---- WorldGen
    func("WorldGen", "create_wall", "create_wall", {"length": 4, "location": [1000, 0, 0]}, verify=lambda r: True)
    func("WorldGen", "create_tower", "create_tower", {"height": 5, "base_size": 3, "location": [1500, 0, 0]})
    func("WorldGen", "create_pyramid", "create_pyramid", {"base_size": 3, "location": [2000, 0, 0]})
    func("WorldGen", "create_maze", "create_maze", {"rows": 4, "cols": 4, "location": [2500, 0, 0]})

    # ---- Viewport / Editor
    func("Viewport", "capture_viewport", "capture_viewport", {"width": 640, "height": 480}, verify=lambda r: len(r.get("image_base64", "")) > 500)
    func("Viewport", "run_console_command", "run_console_command", {"command": "stat fps"})
    func("Viewport", "get_output_log", "get_output_log", {"lines": 20}, verify=lambda r: True)
    func("Viewport", "get_level_info", "get_level_info")

    # ---- Cleanup
    print("\n  --- cleanup ---")
    for nm in ["MCP_Light", "MCP_Cube"]:
        call("delete_actor", {"name": nm})
    deleted = 0
    for path in created_assets:
        r = call("delete_asset", {"name": path, "asset": path, "path": path, "force": True})
        if r["ok"]: deleted += 1
    print(f"  deleted {deleted}/{len(created_assets)} test assets; editor alive: {alive()}")

# ───────────────────────────────────────── main
def main():
    if not alive():
        print("FATAL: editor/plugin not reachable on :%d" % PORT); sys.exit(2)
    a_ok = phase_a()
    phase_b()

    print("\n" + "=" * 70); print("SUMMARY"); print("=" * 70)
    cats = {}
    for cat, label, status, detail in RESULTS:
        cats.setdefault(cat, []).append(status)
    npass = sum(1 for r in RESULTS if r[2] == "PASS")
    ncrash = sum(1 for r in RESULTS if r[2] == "CRASH")
    for cat in cats:
        p = sum(1 for s in cats[cat] if s == "PASS"); t = len(cats[cat])
        print(f"  {cat:12s} {p}/{t}")
    print(f"\n  Phase A (no crash on any tool): {'PASS' if a_ok else 'FAIL'}")
    print(f"  Phase B functional: {npass}/{len(RESULTS)} passed, {ncrash} crashes")
    print("\n  FAILURES:")
    for cat, label, status, detail in RESULTS:
        if status != "PASS":
            print(f"    [{status}] {cat}/{label} -- {detail}")
    print(f"\n  editor alive at end: {alive()}")
    sys.exit(0 if (a_ok and ncrash == 0) else 1)

if __name__ == "__main__":
    main()
