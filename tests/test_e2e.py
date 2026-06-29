#!/usr/bin/env python3
"""
UE5UltimateMCP End-to-End Test (correct API)

Exercises real editor mutations against a running UE5 editor with the plugin:
spawn actors -> verify they appear -> worldgen -> capture viewport -> cleanup.

Envelope: POST /api/tool  body {"tool": <name>, ...params}
Response: {"success": bool, "result": {...}, "error": "..."}
"""
import json, sys, urllib.request, urllib.error

BASE = "http://localhost:9847"

def call(tool, **params):
    body = {"tool": tool}; body.update(params)
    data = json.dumps(body).encode()
    req = urllib.request.Request(BASE + "/api/tool", data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        try: return json.loads(e.read().decode())
        except Exception: return {"success": False, "error": f"HTTP {e.code}"}
    except Exception as e:
        return {"success": False, "error": str(e)}

results = []
def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))

print("=" * 60); print("UE5UltimateMCP End-to-End Test"); print("=" * 60)

# 1. baseline
r = call("get_actors_in_level")
base = r.get("result", {}).get("count", 0)
check("get_actors_in_level (baseline)", r.get("success"), f"{base} actors in level")

# 2. spawn a point light
r = call("spawn_actor", type="PointLight", location=[0, 0, 300], name="MCP_E2E_Light")
check("spawn_actor PointLight", r.get("success"), r.get("error") or "spawned")

# 3. spawn a cube static mesh
r = call("spawn_actor", type="StaticMeshActor", location=[200, 0, 100],
         name="MCP_E2E_Cube", static_mesh="/Engine/BasicShapes/Cube")
check("spawn_actor StaticMeshActor(Cube)", r.get("success"), r.get("error") or "spawned")

# 4. verify count grew by >=2
r = call("get_actors_in_level")
now = r.get("result", {}).get("count", 0)
check("actor count increased", now >= base + 2, f"{base} -> {now}")

# 5. find our actors by name
r = call("find_actors_by_name", pattern="MCP_E2E")
matches = r.get("result", {}).get("matches", r.get("result", {}).get("actors", []))
check("find_actors_by_name 'MCP_E2E'", r.get("success") and len(matches) >= 2, f"found {len(matches)}")

# 6. worldgen: build a tower
r = call("create_tower", location=[600, 0, 0], height=6, base_size=3, style="square")
tcount = r.get("result", {}).get("count", r.get("result", {}).get("blocks", "?"))
check("create_tower (worldgen)", r.get("success"), r.get("error") or f"placed {tcount} blocks")

# 7. capture viewport -> real JPEG bytes
r = call("capture_viewport", width=640, height=480)
img = r.get("result", {}).get("image_base64", "")
check("capture_viewport", r.get("success") and len(img) > 100, f"{len(img)} b64 chars")

# 8. console command
r = call("run_console_command", command="stat unit")
check("run_console_command", r.get("success"), r.get("error") or "ok")

# 9. cleanup spawned test actors
deleted = 0
for nm in ["MCP_E2E_Light", "MCP_E2E_Cube"]:
    if call("delete_actor", name=nm).get("success"): deleted += 1
check("cleanup test actors", deleted == 2, f"deleted {deleted}/2")

p = sum(results); t = len(results)
print("=" * 60); print(f"Results: {p}/{t} passed"); print("=" * 60)
sys.exit(0 if p == t else 1)
