#!/usr/bin/env python3
"""Validate the live UE_AI_integration health and capability catalog."""

from __future__ import annotations

import argparse
import collections
import sys

from mcp_client import MCPApiError, UEAIIntegrationClient


BASELINE_DOMAIN_COUNTS = {
    "blueprint": 58,
    "scene": 54,
    "content": 59,
    "animation": 10,
    "ai": 9,
    "production": 22,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9847)
    args = parser.parse_args()

    client = UEAIIntegrationClient(args.port, timeout=10)
    try:
        health = client.health()
        capabilities = client.capabilities()
    except MCPApiError as error:
        print(f"FAIL: {error}")
        return 1

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
    for domain, expected in BASELINE_DOMAIN_COUNTS.items():
        print(f"  {domain:10} {counts[domain]:3} / {expected}")

    errors: list[str] = []
    if health.get("state") != "ready":
        errors.append(f"server state is {health.get('state')!r}")
    if health.get("plugin") != "UE_AI_integration":
        errors.append(f"unexpected plugin: {health.get('plugin')!r}")
    if health.get("version") != "0.3.0":
        errors.append(f"unexpected version: {health.get('version')!r}")
    if health.get("capabilityCount") != len(capabilities):
        errors.append(
            "health and catalog disagree: "
            f"{health.get('capabilityCount')!r} vs {len(capabilities)}"
        )
    if len(capabilities) < sum(BASELINE_DOMAIN_COUNTS.values()):
        errors.append(
            "capability catalog regressed below the shipped 212-operation baseline"
        )
    for domain, baseline in BASELINE_DOMAIN_COUNTS.items():
        if counts[domain] < baseline:
            errors.append(
                f"{domain} regressed below baseline {baseline}: {counts[domain]}"
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
