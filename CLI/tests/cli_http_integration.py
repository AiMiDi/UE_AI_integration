#!/usr/bin/env python3

import argparse
import base64
import hashlib
import json
import os
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--contract-root", required=True)
    parser.add_argument("--capability-root", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()

    common = [
        args.cli,
        "--json",
        "--contract-root",
        args.contract_root,
        "--capability-root",
        args.capability_root,
    ]
    capability_query = [
        "capabilities",
        "--query",
        "widget",
        "--domain",
        "content",
        "--kind",
        "query",
        "--read-only",
        "true",
        "--output-kind",
        "json",
        "--risk",
        "readOnly",
        "--offset",
        "2",
        "--limit",
        "5",
    ]
    offline_capabilities = json.loads(
        subprocess.run(
            common + capability_query,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    )
    assert offline_capabilities["schema"] == "ue.workflow-capabilities.v1"
    assert offline_capabilities["offset"] == 2
    assert all(
        capability["available"] is None
        and capability["availabilityReasons"]
        == ["editor_connection_required"]
        and capability["risk"] == "readOnly"
        for capability in offline_capabilities["capabilities"]
    )
    overflow_query = capability_query.copy()
    overflow_query[overflow_query.index("2")] = "10000"
    offline_overflow = json.loads(
        subprocess.run(
            common + overflow_query,
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    )
    assert offline_overflow["offset"] == 10000
    assert offline_overflow["capabilities"] == []
    assert offline_overflow["hasMore"] is False
    unavailable_offline = subprocess.run(
        common + ["capabilities", "--available-only"],
        check=False,
        capture_output=True,
        text=True,
    )
    assert unavailable_offline.returncode == 2
    assert (
        json.loads(unavailable_offline.stdout)["diagnostics"][0]["code"]
        == "capability_availability_requires_connection"
    )
    planned = subprocess.run(
        common + ["plan", "--file", args.fixture],
        check=True,
        capture_output=True,
        text=True,
    )
    plan = json.loads(planned.stdout)
    plan_digest = plan["planDigest"]
    contract_digest = plan["contractSetDigest"]
    assert plan["corePlanDigest"] == plan_digest
    assert plan["executionReady"] is False
    assert plan["preconditions"]["prepared"] is False
    bound_plan_digest = (
        "sha256:"
        + hashlib.sha256(
            (plan_digest + "|editor-assets").encode("utf-8")
        ).hexdigest()
    )
    requests: list[dict] = []
    capability_requests: list[dict[str, list[str]]] = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            if parsed.path != "/api/capabilities":
                self.send_error(404)
                return
            query = parse_qs(parsed.query)
            capability_requests.append(query)
            source = (
                offline_overflow
                if query.get("offset") == ["10000"]
                else offline_capabilities
            )
            data = {
                key: value
                for key, value in source.items()
                if key not in {"schema", "ok", "diagnostics"}
            }
            body = json.dumps(
                {
                    "ok": True,
                    "data": data,
                }
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self) -> None:
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))
            requests.append(request)
            if self.path == "/api/execute":
                artifact_content = b"cli artifact\n"
                offset = int(request.get("params", {}).get("offset", 0))
                chunk = artifact_content[offset : offset + 5]
                next_offset = offset + len(chunk)
                body = json.dumps(
                    {
                        "ok": True,
                        "data": {
                            "schema": "ue.artifact.v1",
                            "artifactId": "artifact-cli-integration",
                            "kind": "file",
                            "mimeType": "application/octet-stream",
                            "sizeBytes": len(artifact_content),
                            "sha256": hashlib.sha256(
                                artifact_content
                            ).hexdigest(),
                            "offset": offset,
                            "nextOffset": next_offset,
                            "eof": next_offset >= len(artifact_content),
                            "contentBase64": base64.b64encode(chunk).decode(
                                "ascii"
                            ),
                        },
                    }
                ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            action = request["action"]
            if action == "plan":
                prepared_plan = dict(plan)
                prepared_plan.update(
                    {
                        "corePlanDigest": plan_digest,
                        "executionReady": True,
                        "planDigest": bound_plan_digest,
                        "approval": {
                            "planDigest": bound_plan_digest,
                            "confirmWriteRequired": False,
                        },
                        "preconditions": {
                            "schema": "ue.workflow-asset-preconditions.v1",
                            "prepared": True,
                            "assets": [
                                {
                                    "scopeId": "primary",
                                    "asset": "/Game/Automation/BP_CliHttp",
                                    "kind": "blueprint",
                                    "exists": True,
                                    "packageGuid": None,
                                    "packageSha256": None,
                                    "structureHash": "sha256:fixture",
                                    "dirty": False,
                                    "generatedClass": None,
                                }
                            ],
                            "digest": (
                                "sha256:"
                                + hashlib.sha256(
                                    b"editor-assets"
                                ).hexdigest()
                            ),
                        },
                    }
                )
                body = json.dumps(
                    {"ok": True, "data": prepared_plan}
                ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            statuses = {
                "execute": "pending",
                "status": "running",
                "resume": "completed",
                "rollback": "rolledBack",
            }
            receipt = {
                "schema": "ue.workflow-run.v1",
                "runId": "run-cli-integration",
                "planDigest": bound_plan_digest,
                "contractSetDigest": contract_digest,
                "status": statuses[action],
            }
            result = dict(receipt)
            result["schema"] = "ue.workflow-result.v1"
            result["receipt"] = receipt
            body = json.dumps({"ok": True, "data": result}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"

    try:
        precedence_environment = os.environ.copy()
        precedence_environment["UE_PORT"] = "65535"
        explicit_endpoint = subprocess.run(
            common
            + [
                "doctor",
                "--endpoint",
                "http://127.0.0.1:9847",
            ],
            check=True,
            capture_output=True,
            text=True,
            env=precedence_environment,
        )
        assert (
            json.loads(explicit_endpoint.stdout)["editor"]["endpoint"]
            == "http://127.0.0.1:9847"
        )
        environment_endpoint = subprocess.run(
            common + ["doctor"],
            check=True,
            capture_output=True,
            text=True,
            env=precedence_environment,
        )
        assert (
            json.loads(environment_endpoint.stdout)["editor"]["endpoint"]
            == "http://127.0.0.1:65535"
        )

        with tempfile.TemporaryDirectory(prefix="ue-workflow-cli-") as temporary:
            receipt_path = Path(temporary) / "receipt.json"

            def run(*command: str) -> dict:
                completed = subprocess.run(
                    common + list(command) + ["--endpoint", endpoint],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                return json.loads(completed.stdout)

            connected_plan = run(
                "plan",
                "--file",
                args.fixture,
                "--connect",
            )
            assert connected_plan["data"]["executionReady"] is True
            assert (
                connected_plan["data"]["corePlanDigest"]
                == plan_digest
            )
            assert (
                connected_plan["data"]["planDigest"]
                == bound_plan_digest
            )

            offline_digest_execute = subprocess.run(
                common
                + [
                    "execute",
                    "--file",
                    args.fixture,
                    "--approve-plan",
                    plan_digest,
                    "--receipt",
                    str(receipt_path),
                    "--endpoint",
                    endpoint,
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            assert offline_digest_execute.returncode == 3
            assert (
                json.loads(offline_digest_execute.stdout)[
                    "diagnostics"
                ][0]["code"]
                == "plan_changed"
            )
            assert not receipt_path.exists()

            run(
                "execute",
                "--file",
                args.fixture,
                "--approve-plan",
                bound_plan_digest,
                "--receipt",
                str(receipt_path),
            )
            written = json.loads(receipt_path.read_text(encoding="utf-8"))
            assert written["schema"] == "ue.workflow-run.v1"
            assert written["status"] == "pending"

            run(
                "status",
                "--receipt",
                str(receipt_path),
                "--detail-level",
                "standard",
                "--section",
                "diagnostics",
                "--section",
                "assetDiff",
            )
            assert json.loads(receipt_path.read_text(encoding="utf-8"))["status"] == "running"
            run("resume", "--receipt", str(receipt_path), "--details")
            assert json.loads(receipt_path.read_text(encoding="utf-8"))["status"] == "completed"
            run("rollback", "--receipt", str(receipt_path))
            final_receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            assert final_receipt["status"] == "rolledBack"

            artifact_path = Path(temporary) / "artifact.bin"
            exported = run(
                "operation",
                "run",
                "production.job.artifact.get",
                "--params",
                '{"jobId":"job-cli-integration","artifactId":"artifact-cli-integration"}',
                "--request-id",
                "request-cli-integration",
                "--output",
                str(artifact_path),
            )
            assert artifact_path.read_bytes() == b"cli artifact\n"
            assert "contentBase64" not in exported["data"]
            assert exported["data"]["artifactExport"]["bytes"] == 13
            assert exported["data"]["artifactExport"]["chunks"] == 3
            assert exported["data"]["artifactExport"][
                "verifiedAgainstReceipt"
            ]
            assert exported["data"]["artifactExport"]["sha256"] == (
                "sha256:"
                + hashlib.sha256(b"cli artifact\n").hexdigest()
            )
            capabilities = run(
                *capability_query,
                "--connect",
            )
            assert capabilities == offline_capabilities
            overflow_capabilities = run(
                *overflow_query,
                "--connect",
            )
            assert overflow_capabilities == offline_overflow
            available_capabilities = run(
                "capabilities",
                "--connect",
                "--available-only",
                "--limit",
                "1",
            )
            assert (
                available_capabilities["schema"]
                == "ue.workflow-capabilities.v1"
            )
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    workflow_requests = [
        request for request in requests if "action" in request
    ]
    operation_requests = [
        request for request in requests if "capability" in request
    ]
    assert [request["action"] for request in workflow_requests] == [
        "plan",
        "plan",
        "plan",
        "execute",
        "status",
        "resume",
        "rollback",
    ]
    assert workflow_requests[1]["detailLevel"] == "summary"
    assert workflow_requests[2]["detailLevel"] == "summary"
    assert (
        workflow_requests[3]["approvePlanDigest"]
        == bound_plan_digest
    )
    assert "approvePlanDigest" not in workflow_requests[4]
    assert "approvePlanDigest" not in workflow_requests[5]
    assert workflow_requests[4]["detailLevel"] == "standard"
    assert workflow_requests[4]["sections"] == [
        "diagnostics",
        "assetDiff",
    ]
    assert workflow_requests[5]["detailLevel"] == "full"
    assert "details" not in workflow_requests[5]
    assert (
        workflow_requests[6]["approvePlanDigest"]
        == bound_plan_digest
    )
    assert (
        operation_requests[0]["capability"]
        == "production.job.artifact.get"
    )
    assert (
        operation_requests[0]["requestId"]
        == "request-cli-integration"
    )
    assert operation_requests[1]["params"]["offset"] == 5
    assert "requestId" not in operation_requests[1]
    assert operation_requests[2]["params"]["offset"] == 10
    assert capability_requests == [
        {
            "query": ["widget"],
            "domain": ["content"],
            "kind": ["query"],
            "outputKind": ["json"],
            "risk": ["readOnly"],
            "detail": ["summary"],
            "readOnly": ["true"],
            "offset": ["2"],
            "limit": ["5"],
        },
        {
            "query": ["widget"],
            "domain": ["content"],
            "kind": ["query"],
            "outputKind": ["json"],
            "risk": ["readOnly"],
            "detail": ["summary"],
            "readOnly": ["true"],
            "offset": ["10000"],
            "limit": ["5"],
        },
        {
            "detail": ["summary"],
            "availableOnly": ["true"],
            "offset": ["0"],
            "limit": ["1"],
        },
    ]
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
