import argparse
import json
import os
import pathlib
import subprocess
import tempfile


def invoke(cli, source_root, capability, params, expected=0):
    environment = os.environ.copy()
    environment["UE_LOCAL_CAPABILITY_CLI"] = str(
        source_root / "MCP" / "dist" / "local-capability-cli.js"
    )
    completed = subprocess.run(
        [
            str(cli),
            capability,
            "--params",
            json.dumps(params, ensure_ascii=False),
            "--capability-root",
            str(source_root / "Resources" / "Capabilities"),
            "--endpoint",
            "http://127.0.0.1:1",
            "--json",
        ],
        text=True,
        encoding="utf-8",
        capture_output=True,
        env=environment,
        timeout=15,
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"unexpected exit {completed.returncode}: stdout={completed.stdout!r} stderr={completed.stderr!r}"
        )
    return json.loads(completed.stdout)


def require_backend(envelope, backend):
    actual = envelope.get("meta", {}).get("executionBackend")
    if actual != backend:
        raise AssertionError(f"expected backend {backend!r}, got {actual!r}: {envelope!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=pathlib.Path)
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ue-local-backends-") as temporary:
        root = pathlib.Path(temporary)
        (root / "Config").mkdir()
        (root / "Fixture.uproject").write_text(
            json.dumps({"EngineAssociation": "5.3", "Plugins": []}),
            encoding="utf-8",
        )
        secret = "cli-must-not-leak-this-token"
        (root / "Config" / "DefaultEngine.ini").write_text(
            f"[Online]\nApiToken={secret}\nEndpoint=https://example.invalid\n",
            encoding="utf-8",
        )

        config = invoke(
            args.cli,
            args.source_root,
            "production.project.config.get",
            {"projectRoot": str(root)},
        )
        require_backend(config, "localProject")
        if secret in json.dumps(config):
            raise AssertionError("localProject CLI response leaked a sensitive ini value")
        if config["data"]["merged"]["Online"]["ApiToken"]["value"] != "<redacted>":
            raise AssertionError(f"sensitive value was not redacted: {config!r}")

        sal = invoke(args.cli, args.source_root, "production.sal.stub", {})
        require_backend(sal, "localSal")

        recipe = invoke(
            args.cli,
            args.source_root,
            "production.recipe.validate",
            {
                "recipe": {
                    "schema": "ue.recipe.v2",
                    "id": "cli-local-route",
                    "version": "1.0.0",
                    "steps": [],
                }
            },
        )
        require_backend(recipe, "localRecipe")

        targets = invoke(
            args.cli,
            args.source_root,
            "production.development.target.list",
            {"projectRoot": str(root)},
        )
        require_backend(targets, "developmentRuntime")

        (root / "Invalid.uasset").write_bytes(b"invalid")
        asset = invoke(
            args.cli,
            args.source_root,
            "production.asset.package.summary.get",
            {"projectRoot": str(root), "assetPath": "Invalid.uasset"},
            expected=5,
        )
        require_backend(asset, "localAsset")
        if asset.get("error", {}).get("code") in {
            "execution_backend_unavailable",
            "execution_backend_unsupported",
        }:
            raise AssertionError(f"localAsset did not reach its executor: {asset!r}")


if __name__ == "__main__":
    main()
