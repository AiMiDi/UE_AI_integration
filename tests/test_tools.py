#!/usr/bin/env python3
"""Representative live smoke tests for UE_AI_integration HTTP."""

from __future__ import annotations

import argparse
import sys
import uuid
from collections.abc import Callable

from mcp_client import MCPApiError, UEAIIntegrationClient


def run_test(name: str, test: Callable[[], str]) -> bool:
    try:
        detail = test()
        print(f"  [PASS] {name} -- {detail}")
        return True
    except (AssertionError, MCPApiError) as error:
        print(f"  [FAIL] {name} -- {error}")
        return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9847)
    args = parser.parse_args()
    client = UEAIIntegrationClient(args.port)

    def health() -> str:
        data = client.health()
        assert data.get("status") == "ready", data
        return f"{data.get('engineVersion')} / {data.get('capabilityCount')} capabilities"

    def catalog() -> str:
        capabilities = client.capabilities()
        assert len(capabilities) >= 212, len(capabilities)
        assert len({item["id"] for item in capabilities}) == len(capabilities)
        return f"{len(capabilities)} unique capabilities"

    def list_blueprints() -> str:
        data = client.execute(
            "blueprint.asset.list",
            {"filter": "", "parentClass": "", "type": "all"},
        )
        assert isinstance(data.get("blueprints"), list), data
        return f"{data.get('count', len(data['blueprints']))} blueprints"

    def list_actors() -> str:
        data = client.execute("scene.actor.list")
        assert isinstance(data.get("actors"), list), data
        return f"{data.get('count', len(data['actors']))} actors"

    def spawn_and_delete_actor() -> str:
        name = "MCP_IntegrationTest"
        try:
            spawned = client.execute(
                "scene.actor.spawn",
                {
                    "type": "PointLight",
                    "name": name,
                    "location": [0, 0, 200],
                },
            )
            assert spawned, spawned
        finally:
            client.execute("scene.actor.delete", {"name": name})
        return "spawned and removed PointLight"

    def capture_viewport() -> str:
        data = client.execute(
            "scene.viewport.capture", {"width": 640, "height": 480}
        )
        image = data.get("image_base64")
        assert isinstance(image, str) and len(image) > 100, data.keys()
        return f"{len(image)} base64 chars"

    def request_id_replay_and_conflict() -> str:
        request_id = str(uuid.uuid4())
        first = client.execute(
            "production.module.loaded.get", request_id=request_id
        )
        replay = client.execute(
            "production.module.loaded.get", request_id=request_id
        )
        assert first == replay, (first, replay)
        try:
            client.execute(
                "production.module.loaded.get",
                {"different": True},
                request_id=request_id,
            )
        except MCPApiError as error:
            assert error.status == 409, error
            assert error.code == "idempotency_conflict", error
        else:
            raise AssertionError("requestId conflict was accepted")
        return "identical payload replayed; changed payload rejected with 409"

    tests = [
        ("health", health),
        ("catalog", catalog),
        ("blueprint.asset.list", list_blueprints),
        ("scene.actor.list", list_actors),
        ("scene.actor.spawn/delete", spawn_and_delete_actor),
        ("scene.viewport.capture", capture_viewport),
        ("requestId replay/conflict", request_id_replay_and_conflict),
    ]

    print("UE_AI_integration integration tests")
    results = [run_test(name, test) for name, test in tests]
    print(f"Results: {sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
