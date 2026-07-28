#!/usr/bin/env python3
"""End-to-end smoke: query, mutate, image response, and cleanup."""

from __future__ import annotations

import argparse
import sys

from mcp_client import MCPApiError, UEAIIntegrationClient


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9847)
    args = parser.parse_args()
    client = UEAIIntegrationClient(args.port, timeout=60)

    checks: list[tuple[str, bool, str]] = []

    def check(name: str, condition: bool, detail: str = "") -> None:
        checks.append((name, condition, detail))
        print(f"  [{'PASS' if condition else 'FAIL'}] {name} -- {detail}")

    light_name = "MCP_V2_E2E_Light"
    cube_name = "MCP_V2_E2E_Cube"

    try:
        baseline = client.execute("scene.actor.list")
        baseline_count = int(baseline.get("count", 0))
        check("baseline actor query", True, f"{baseline_count} actors")

        client.execute(
            "scene.actor.spawn",
            {
                "type": "PointLight",
                "name": light_name,
                "location": [0, 0, 300],
            },
        )
        check("spawn PointLight", True, light_name)

        client.execute(
            "scene.actor.spawn",
            {
                "type": "StaticMeshActor",
                "name": cube_name,
                "location": [200, 0, 100],
                "static_mesh": "/Engine/BasicShapes/Cube.Cube",
            },
        )
        check("spawn StaticMeshActor", True, cube_name)

        after = client.execute("scene.actor.list")
        after_count = int(after.get("count", 0))
        check("actor count increased", after_count >= baseline_count + 2, f"{baseline_count} -> {after_count}")

        matches = client.execute("scene.actor.find", {"pattern": "MCP_V2_E2E"})
        actors = matches.get("matches", matches.get("actors", []))
        check("find spawned actors", isinstance(actors, list) and len(actors) >= 2, f"{len(actors)} matches")

        image = client.execute(
            "scene.viewport.capture", {"width": 640, "height": 480}
        ).get("image_base64", "")
        check("capture viewport", isinstance(image, str) and len(image) > 100, f"{len(image)} chars")

    except (AssertionError, MCPApiError) as error:
        check("end-to-end execution", False, str(error))
    finally:
        removed = 0
        for name in (light_name, cube_name):
            try:
                client.execute("scene.actor.delete", {"name": name})
                removed += 1
            except MCPApiError:
                pass
        check("cleanup", removed == 2, f"{removed}/2 actors removed")

    return 0 if all(result for _, result, _ in checks) else 1


if __name__ == "__main__":
    sys.exit(main())
