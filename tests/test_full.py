#!/usr/bin/env python3
"""Non-destructive live robustness sweep for every capability."""

from __future__ import annotations

import argparse
import sys

from mcp_client import MCPApiError, UEAIIntegrationClient


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=9847)
    args = parser.parse_args()
    client = UEAIIntegrationClient(args.port, timeout=120)

    try:
        capabilities = client.capabilities()
    except MCPApiError as error:
        print(f"FAIL: could not load catalog: {error}")
        return 1

    executed = 0
    clean_errors = 0
    skipped = 0
    failures: list[str] = []

    for index, capability in enumerate(sorted(capabilities, key=lambda item: item["id"])):
        capability_id = capability["id"]
        traits = capability.get("traits", {})
        if traits.get("destructive") or traits.get("expensive"):
            skipped += 1
            continue

        try:
            client.execute(capability_id, {})
            executed += 1
        except MCPApiError as error:
            if error.code in {"invalid_params", "execution_failed"}:
                clean_errors += 1
            else:
                failures.append(f"{capability_id}: {error}")
                break

        if index % 10 == 0:
            try:
                if client.health().get("status") != "ready":
                    failures.append(f"{capability_id}: server left ready state")
                    break
            except MCPApiError as error:
                failures.append(f"{capability_id}: editor connection lost: {error}")
                break

    print(
        f"catalog={len(capabilities)} executed={executed} "
        f"clean_errors={clean_errors} skipped={skipped}"
    )
    for failure in failures:
        print(f"FAIL: {failure}")

    if len(capabilities) < 212:
        print(
            "FAIL: capability catalog regressed below the 212-operation baseline, "
            f"got {len(capabilities)}"
        )
        return 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
