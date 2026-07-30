#!/usr/bin/env python3
"""Validate the live UE_AI_integration health and capability catalog."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys

from mcp_client import MCPApiError, UEAIIntegrationClient


EXPECTED_VERSION = "0.7.0"
MANIFEST_DIRECTORY = (
    pathlib.Path(__file__).resolve().parents[1] / "Resources" / "Capabilities"
)


def expected_catalog() -> tuple[set[str], collections.Counter[str]]:
    ids: set[str] = set()
    domains: collections.Counter[str] = collections.Counter()
    for manifest_path in sorted(MANIFEST_DIRECTORY.glob("*.json")):
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        domain = manifest.get("domain")
        capabilities = manifest.get("capabilities")
        if not isinstance(domain, str) or not isinstance(capabilities, list):
            raise ValueError(f"invalid capability manifest: {manifest_path}")
        for capability in capabilities:
            capability_id = (
                capability.get("id") if isinstance(capability, dict) else None
            )
            if not isinstance(capability_id, str) or not capability_id:
                raise ValueError(f"invalid capability id in {manifest_path}")
            if capability_id in ids:
                raise ValueError(f"duplicate capability id: {capability_id}")
            ids.add(capability_id)
            domains[domain] += 1
    return ids, domains


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9847)
    args = parser.parse_args()

    client = UEAIIntegrationClient(args.port, timeout=10)
    try:
        expected_ids, expected_counts = expected_catalog()
        health = client.health()
        capabilities = client.capabilities()
    except (MCPApiError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"FAIL: {error}")
        return 1

    live_ids = {
        item.get("id") for item in capabilities if isinstance(item.get("id"), str)
    }
    counts = collections.Counter(
        item.get("domain") for item in capabilities if isinstance(item, dict)
    )

    print("UE_AI_integration")
    print(f"  state:        {health.get('state')}")
    print(f"  plugin:       {health.get('plugin')}")
    print(f"  version:      {health.get('version')}")
    print(f"  engineVersion:{health.get('engineVersion')}")
    print(f"  projectName:  {health.get('projectName')}")
    print(f"  capabilities: {len(capabilities)}")
    for domain, expected in sorted(expected_counts.items()):
        print(f"  {domain:10} {counts[domain]:3} / {expected}")

    errors: list[str] = []
    if health.get("state") != "ready":
        errors.append(f"server state is {health.get('state')!r}")
    if health.get("plugin") != "UE_AI_integration":
        errors.append(f"unexpected plugin: {health.get('plugin')!r}")
    if health.get("version") != EXPECTED_VERSION:
        errors.append(f"unexpected version: {health.get('version')!r}")
    if health.get("capabilityCount") != len(capabilities):
        errors.append(
            "health and catalog disagree: "
            f"{health.get('capabilityCount')!r} vs {len(capabilities)}"
        )
    if live_ids != expected_ids:
        missing = sorted(expected_ids - live_ids)
        unexpected = sorted(live_ids - expected_ids)
        errors.append(
            "live capability IDs differ from manifests: "
            f"missing={missing[:8]!r}, unexpected={unexpected[:8]!r}"
        )
    if counts != expected_counts:
        errors.append(
            "live domain counts differ from manifests: "
            f"{dict(counts)!r} vs {dict(expected_counts)!r}"
        )
    if health.get("domainCounts") != dict(counts):
        errors.append(
            "health domain counts differ from catalog: "
            f"{health.get('domainCounts')!r} vs {dict(counts)!r}"
        )

    if errors:
        for error in errors:
            print(f"FAIL: {error}")
        return 1

    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
