#!/usr/bin/env python3
"""Reversible scene-construction smoke test using the Scene domain."""

from __future__ import annotations

import argparse
import sys
import time

from mcp_client import MCPApiError, UEAIIntegrationClient


TAG = "MCP_V2_WorldGenTest"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9847)
    args = parser.parse_args()
    client = UEAIIntegrationClient(args.port)

    parts = [
        # wall
        *[
            (f"{TAG}_Wall_{index}", [index * 200, 0, 100], [1.0, 0.2, 2.0])
            for index in range(5)
        ],
        # tower
        *[
            (f"{TAG}_Tower_{index}", [1500, 0, 100 + index * 200], [1, 1, 1])
            for index in range(4)
        ],
        # house
        (f"{TAG}_House_Floor", [3000, 0, 0], [4, 4, 0.1]),
        (f"{TAG}_House_Front", [3000, -400, 200], [4, 0.1, 2]),
        (f"{TAG}_House_Back", [3000, 400, 200], [4, 0.1, 2]),
        (f"{TAG}_House_Left", [2600, 0, 200], [0.1, 4, 2]),
        (f"{TAG}_House_Right", [3400, 0, 200], [0.1, 4, 2]),
    ]

    spawned: list[str] = []
    failures: list[str] = []
    try:
        if client.health().get("status") != "ready":
            print("FAIL: server is not ready")
            return 1

        for name, location, scale in parts:
            try:
                client.execute(
                    "scene.actor.spawn",
                    {
                        "type": "StaticMeshActor",
                        "name": name,
                        "location": location,
                        "scale": scale,
                        "staticMesh": "/Engine/BasicShapes/Cube.Cube",
                    },
                )
                spawned.append(name)
            except MCPApiError as error:
                failures.append(f"spawn {name}: {error}")

        time.sleep(0.5)
        actor_data = client.execute("scene.actor.list")
        actors = actor_data.get("actors", [])
        actor_names = {
            candidate
            for actor in actors
            if isinstance(actor, dict)
            for candidate in (actor.get("name"), actor.get("label"))
            if isinstance(candidate, str)
        }
        missing = sorted(set(spawned) - actor_names)
        if missing:
            failures.append(f"spawned actors missing from list: {missing}")
    except MCPApiError as error:
        failures.append(str(error))
    finally:
        for name in spawned:
            try:
                client.execute("scene.actor.delete", {"name": name})
            except MCPApiError as error:
                failures.append(f"cleanup {name}: {error}")

    print(f"spawned={len(spawned)}/{len(parts)}")
    for failure in failures:
        print(f"FAIL: {failure}")
    return 1 if failures or len(spawned) != len(parts) else 0


if __name__ == "__main__":
    sys.exit(main())
