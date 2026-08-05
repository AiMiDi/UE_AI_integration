import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile


def descriptor(operation: str, properties: dict, required=None, execution=None):
    value = {
        "id": operation,
        "domain": "production",
        "kind": "query" if operation.endswith((".status", ".query", ".get")) else "command",
        "description": "Trace routing fixture.",
        "inputSchema": {
            "type": "object",
            "properties": properties,
            "additionalProperties": False,
        },
        "traits": {"destructive": False, "expensive": False},
        "effects": {
            "asset": "read",
            "world": "none",
            "editorSession": "none",
            "external": "none",
        },
        "lifecycle": {
            "status": "active",
            "since": "0.10.0",
            "canonicalId": operation,
        },
        "output": {"kind": "json"},
    }
    if required:
        value["inputSchema"]["required"] = required
    if execution:
        value["execution"] = execution
    return value


def trace_contract_digests(source_root: pathlib.Path):
    relative_files = sorted(
        [
            "Resources/Capabilities/production.json",
            "Resources/Trace/insights-actions.5.3.json",
            "Resources/Trace/launch-profiles.json",
            "Resources/Trace/worker-protocol.v1.json",
        ]
    )
    contract = hashlib.sha256()
    for relative in relative_files:
        contract.update(relative.replace("\\", "/").encode("utf-8"))
        contract.update(b"\0")
        contract.update((source_root / relative).read_bytes())
        contract.update(b"\0")
    provider = hashlib.sha256(
        (source_root / "Resources/Trace/insights-actions.5.3.json").read_bytes()
    )
    return f"sha256:{contract.hexdigest()}", f"sha256:{provider.hexdigest()}"


def run(
    cli,
    root,
    worker,
    source_root,
    arguments,
    expected=0,
    engine_version="5.3",
):
    environment = os.environ.copy()
    environment["UEAI_TRACE_WORKER"] = str(worker)
    environment["UE_ENGINE_VERSION"] = engine_version
    environment["UEAI_TRACE_CONTRACT_ROOT"] = str(source_root)
    configured_root = root / "configured-trace-root"
    configured_root.mkdir(exist_ok=True)
    environment["UEAI_TRACE_ROOTS"] = str(configured_root.resolve())
    contract_digest, provider_digest = trace_contract_digests(source_root)
    environment["FAKE_TRACE_CONTRACT_DIGEST"] = contract_digest
    environment["FAKE_TRACE_PROVIDER_DIGEST"] = provider_digest
    completed = subprocess.run(
        [str(cli), *arguments, "--capability-root", str(root), "--endpoint", "http://127.0.0.1:1", "--json"],
        text=True,
        encoding="utf-8",
        capture_output=True,
        env=environment,
        timeout=10,
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"unexpected exit {completed.returncode}: stdout={completed.stdout!r} stderr={completed.stderr!r}"
        )
    return json.loads(completed.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, type=pathlib.Path)
    parser.add_argument("--worker", required=True, type=pathlib.Path)
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    both = {"backends": ["editor", "localTrace"], "preferred": "localTrace"}
    editor_first = {"backends": ["editor", "localTrace"], "preferred": "editor"}
    backend = {"type": "string", "enum": ["auto", "editor", "local"]}
    with tempfile.TemporaryDirectory(prefix="ue-trace-routing-") as temporary:
        root = pathlib.Path(temporary)
        manifest = {
            "schema": "ue.capability-manifest.v3",
            "schemaVersion": 3,
            "domain": "production",
            "capabilities": [
                descriptor(
                    "production.trace.start",
                    {"backend": backend, "target": {"type": "object"}},
                    execution=both,
                ),
                descriptor(
                    "production.trace.import",
                    {
                        "backend": backend,
                        "tracePath": {"type": "string"},
                        "copyMode": {
                            "type": "string",
                            "enum": ["copy", "reference"],
                        },
                    },
                    ["tracePath"],
                    both,
                ),
                descriptor(
                    "production.trace.timing.query",
                    {"backend": backend, "traceId": {"type": "string"}, "operation": {"type": "string"}},
                    ["traceId", "operation"],
                    both,
                ),
                descriptor(
                    "production.trace.target.list",
                    {"backend": backend},
                    execution=editor_first,
                ),
                descriptor(
                    "production.job.result.get",
                    {"jobId": {"type": "string"}},
                    ["jobId"],
                ),
            ],
        }
        (root / "production.json").write_text(json.dumps(manifest), encoding="utf-8")

        started = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            ["trace", "start", "--params", '{"backend":"auto","target":{"kind":"development"}}'],
        )
        assert started["meta"]["executionBackend"] == "localTrace"
        assert started["data"]["capability"] == "production.trace.start"
        resident_pid = started["data"]["serverPid"]

        external_root = root / "external traces"
        external_root.mkdir()
        external_trace = external_root / "cli explicit.utrace"
        external_trace.write_bytes(b"UEAI external trace fixture")
        imported = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            [
                "trace",
                "import",
                "--params",
                json.dumps({"tracePath": str(external_trace.resolve())}),
            ],
        )
        assert imported["meta"]["executionBackend"] == "localTrace"
        assert imported["data"]["capability"] == "production.trace.import"
        assert pathlib.Path(imported["data"]["params"]["tracePath"]) == external_trace.resolve()
        assert imported["data"]["serverPid"] != resident_pid
        child_roots = imported["data"]["traceRoots"].split(";")
        assert str((root / "configured-trace-root").resolve()) in child_roots
        assert str(external_root.resolve()) in child_roots

        reference_rejected = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            [
                "trace",
                "import",
                "--params",
                json.dumps(
                    {
                        "tracePath": str(external_trace.resolve()),
                        "copyMode": "reference",
                    }
                ),
            ],
            expected=5,
        )
        assert reference_rejected["error"]["code"] == "trace_path_not_allowed"

        configured_trace = root / "configured-trace-root" / "reference.utrace"
        configured_trace.write_bytes(b"UEAI configured reference fixture")
        referenced = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            [
                "trace",
                "import",
                "--params",
                json.dumps(
                    {
                        "tracePath": str(configured_trace.resolve()),
                        "copyMode": "reference",
                    }
                ),
            ],
        )
        assert referenced["data"]["serverPid"] == resident_pid
        assert referenced["data"]["traceRoots"] == str(
            (root / "configured-trace-root").resolve()
        )

        for invalid_path in (
            "relative.utrace",
            str((root / "missing.utrace").resolve()),
            "",
        ):
            rejected = run(
                args.cli,
                root,
                args.worker,
                args.source_root,
                [
                    "trace",
                    "import",
                    "--params",
                    json.dumps({"tracePath": invalid_path}),
                ],
                expected=2,
            )
            assert rejected["error"]["code"] == "trace_import_path_invalid"

        link_trace = root / "linked.utrace"
        try:
            link_trace.symlink_to(external_trace)
        except OSError:
            link_trace = None
        if link_trace is not None:
            rejected_link = run(
                args.cli,
                root,
                args.worker,
                args.source_root,
                [
                    "trace",
                    "import",
                    "--params",
                    json.dumps({"tracePath": str(link_trace.absolute())}),
                ],
                expected=2,
            )
            assert rejected_link["error"]["code"] == "trace_import_path_invalid"

        queried = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            ["trace", "query", "timing", "--params", '{"traceId":"trace-local-1","operation":"frames"}'],
        )
        assert queried["meta"]["executionBackend"] == "localTrace"
        assert queried["data"]["serverPid"] == resident_pid
        assert queried["data"]["traceRoots"] == str(
            (root / "configured-trace-root").resolve()
        )

        job = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            ["production.job.result.get", "--job-id", "trace-analysis-local-1"],
        )
        assert job["meta"]["executionBackend"] == "localTrace"

        targets = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            ["trace", "target", "list", "--backend", "auto"],
        )
        assert targets["meta"]["executionBackend"] == "localTrace"

        editor_only = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            ["trace", "target", "list", "--backend", "editor"],
            expected=4,
        )
        assert editor_only["error"]["code"] == "editor_unreachable"

        conflict = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            ["trace", "start", "--params", '{"backend":"editor","target":{"kind":"development"}}'],
            expected=2,
        )
        assert conflict["error"]["code"] == "execution_backend_conflict"

        bundle = root / "offline-bundle"
        (bundle / "CLI" / "bin").mkdir(parents=True)
        shutil_source = args.source_root.resolve()
        shutil.copy2(args.cli, bundle / "CLI" / "bin" / "ue-cli.exe")
        workflow_cli = args.cli.parent / "ue-workflow-cli.exe"
        if not workflow_cli.exists():
            raise AssertionError(f"missing sibling Workflow CLI: {workflow_cli}")
        shutil.copy2(
            workflow_cli, bundle / "CLI" / "bin" / "ue-workflow-cli.exe"
        )
        shutil.copy2(
            shutil_source / "UE_AI_integration.uplugin",
            bundle / "UE_AI_integration.uplugin",
        )
        shutil.copytree(shutil_source / "Resources", bundle / "Resources")
        shutil.copytree(shutil_source / "skills", bundle / "skills")
        shutil.copytree(shutil_source / "Recipes", bundle / "Recipes")
        (bundle / "scripts").mkdir()
        shutil.copy2(
            shutil_source / "scripts" / "mcp_stdio_smoke.mjs",
            bundle / "scripts" / "mcp_stdio_smoke.mjs",
        )
        (bundle / "MCP").mkdir()
        shutil.copy2(
            shutil_source / "MCP" / "package.json",
            bundle / "MCP" / "package.json",
        )
        shutil.copytree(
            shutil_source / "MCP" / "dist", bundle / "MCP" / "dist"
        )
        shutil.copytree(
            shutil_source / "MCP" / "node_modules",
            bundle / "MCP" / "node_modules",
        )

        diagnostic_environment = os.environ.copy()
        diagnostic_environment["UEAI_TRACE_WORKER"] = str(args.worker)
        diagnostic_environment["UEAI_TRACE_TRANSPORT"] = "stdio"
        diagnostic_environment["UEAI_TRACE_CONTRACT_ROOT"] = str(
            shutil_source
        )
        diagnostic_environment["UE_ENGINE_VERSION"] = "5.3"
        diagnostic_environment["UE_PORT"] = "1"
        contract_digest, provider_digest = trace_contract_digests(
            shutil_source
        )
        diagnostic_environment["FAKE_TRACE_CONTRACT_DIGEST"] = contract_digest
        diagnostic_environment["FAKE_TRACE_PROVIDER_DIGEST"] = provider_digest

        offline_tools = subprocess.run(
            [
                str(bundle / "CLI" / "bin" / "ue-cli.exe"),
                "test-tools",
                "--bundle",
                str(bundle),
                "--json",
            ],
            text=True,
            encoding="utf-8",
            capture_output=True,
            env=diagnostic_environment,
            timeout=30,
        )
        if offline_tools.returncode != 0:
            raise AssertionError(
                f"offline test-tools failed: stdout={offline_tools.stdout!r} stderr={offline_tools.stderr!r}"
            )
        offline_payload = json.loads(offline_tools.stdout)
        assert offline_payload["data"]["schema"] == "ue.test-tools.v2"
        assert offline_payload["data"]["status"] == "partial"
        assert offline_payload["ok"] is True
        checks = {
            check["id"]: check for check in offline_payload["data"]["checks"]
        }
        assert checks["mcp.stdio"]["status"] == "passed"
        assert checks["editor.health"]["status"] == "skipped"
        assert checks["editor.readonly"]["status"] == "skipped"

        required_editor = subprocess.run(
            [
                str(bundle / "CLI" / "bin" / "ue-cli.exe"),
                "test-tools",
                "--bundle",
                str(bundle),
                "--require-editor",
                "--endpoint",
                "http://127.0.0.1:1",
                "--json",
            ],
            text=True,
            encoding="utf-8",
            capture_output=True,
            env=diagnostic_environment,
            timeout=30,
        )
        assert required_editor.returncode != 0
        required_payload = json.loads(required_editor.stdout)
        assert required_payload["data"]["schema"] == "ue.test-tools.v2"
        assert required_payload["data"]["status"] == "failed"
        assert required_payload["ok"] is False

        mismatched_doctor = run(
            args.cli,
            root,
            args.worker,
            args.source_root,
            ["trace", "doctor"],
            expected=5,
            engine_version="5.4",
        )
        assert mismatched_doctor["ok"] is False
        assert mismatched_doctor["error"]["code"] == "trace_worker_contract_mismatch"
        assert mismatched_doctor["error"]["details"]["rejectedResponse"]["ok"] is True


if __name__ == "__main__":
    main()
