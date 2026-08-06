#!/usr/bin/env python3

import argparse
import codecs
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
    with tempfile.TemporaryDirectory(prefix="ue-workflow-help-") as help_temp:
        missing_root = str(Path(help_temp) / "missing")
        for hierarchy in [
            ["doctor", "--help"],
            ["plan", "--help"],
            ["execute", "--help"],
            ["operation", "run", "blueprint.scan", "--help"],
        ]:
            helped = subprocess.run(
                [
                    args.cli,
                    "--json",
                    "--contract-root",
                    missing_root,
                    "--capability-root",
                    missing_root,
                    "--endpoint",
                    "http://127.0.0.1:1",
                    *hierarchy,
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            help_payload = json.loads(helped.stdout)
            assert help_payload["ok"] is True
            assert help_payload["schema"] == "ue.workflow-cli-help.v1"
        strict_positional = subprocess.run(
            [
                args.cli,
                "--json",
                "--contract-root",
                missing_root,
                "--capability-root",
                missing_root,
                "doctor",
                "unexpected",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        assert strict_positional.returncode == 1
        strict_error = json.loads(strict_positional.stdout)
        assert strict_error["diagnostics"][0]["code"] == "invalid_arguments"
        strict_version = subprocess.run(
            [args.cli, "--json", "--version", "unexpected"],
            check=False,
            capture_output=True,
            text=True,
        )
        assert strict_version.returncode == 1
        strict_version_error = json.loads(strict_version.stdout)
        assert (
            strict_version_error["diagnostics"][0]["code"]
            == "invalid_arguments"
        )

    local_doctor = json.loads(
        subprocess.run(
            common + ["doctor"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    )
    assert local_doctor["contracts"]["v1"]["dslVersion"] == "1.0"
    assert local_doctor["contracts"]["v2"]["dslVersion"] == "2.0"
    assert (
        local_doctor["contracts"]["v1"]["contractSetDigest"]
        != local_doctor["contracts"]["v2"]["contractSetDigest"]
    )
    contract_digest_v1 = local_doctor["contracts"]["v1"][
        "contractSetDigest"
    ]
    contract_digest_v2 = local_doctor["contracts"]["v2"][
        "contractSetDigest"
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
    removed_operation_run = subprocess.run(
        common + ["operation", "run", "scene.pie.status"],
        check=False,
        capture_output=True,
        text=True,
    )
    assert removed_operation_run.returncode == 1
    assert "operation run" not in removed_operation_run.stdout
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

    unicode_workflow = json.loads(
        Path(args.fixture).read_text(encoding="utf-8")
    )
    unicode_workflow["workflowId"] = "blueprint-layout-unicode"
    unicode_workflow["operations"][0]["params"]["name"] = "输入处理"
    unicode_workflow_text = json.dumps(
        unicode_workflow,
        ensure_ascii=False,
    )
    with tempfile.TemporaryDirectory(
        prefix="ue-workflow-unicode-"
    ) as unicode_temp:
        unicode_path = Path(unicode_temp) / "中文工作流.json"
        unicode_path.write_text(
            unicode_workflow_text,
            encoding="utf-16",
        )
        unicode_file_plan = subprocess.run(
            common + ["plan", "--file", str(unicode_path)],
            check=True,
            capture_output=True,
        )
        assert not unicode_file_plan.stdout.startswith(codecs.BOM_UTF8)
        unicode_file_result = json.loads(
            unicode_file_plan.stdout.decode("utf-8")
        )
        assert (
            unicode_file_result["normalizedWorkflow"]["workflowId"]
            == "blueprint-layout-unicode"
        )
        assert (
            unicode_file_result["normalizedWorkflow"]["operations"][0][
                "params"
            ]["childName"]
            == "输入处理"
        )

        unicode_stdin_plan = subprocess.run(
            common + ["plan", "--file", "-"],
            input=(
                codecs.BOM_UTF16_BE
                + unicode_workflow_text.encode("utf-16-be")
            ),
            check=True,
            capture_output=True,
        )
        assert (
            json.loads(unicode_stdin_plan.stdout.decode("utf-8"))[
                "normalizedWorkflow"
            ]["workflowId"]
            == "blueprint-layout-unicode"
        )

        invalid_encoding = subprocess.run(
            common + ["plan", "--file", "-"],
            input=b"\xc3\x28",
            check=False,
            capture_output=True,
        )
        assert invalid_encoding.returncode == 2
        invalid_encoding_result = json.loads(
            invalid_encoding.stdout.decode("utf-8")
        )
        assert (
            invalid_encoding_result["diagnostics"][0]["code"]
            == "invalid_text_encoding"
        )

    bound_plan_digest = (
        "sha256:"
        + hashlib.sha256(
            (plan_digest + "|editor-assets").encode("utf-8")
        ).hexdigest()
    )
    requests: list[dict] = []
    request_headers: list[dict[str, str]] = []
    get_request_headers: list[dict[str, str]] = []
    registration_attempts: list[dict] = []
    registrations: list[dict] = []
    unregistrations: list[str] = []
    session_protocol_enabled = True
    issued_sessions: dict[str, dict] = {}
    capability_requests: list[dict[str, list[str]]] = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def caller_headers(self) -> dict[str, str]:
            return {
                "callerType": self.headers.get("X-UEAI-Caller-Type", ""),
                "caller": self.headers.get("X-UEAI-Caller", ""),
                "callerVersion": self.headers.get(
                    "X-UEAI-Caller-Version", ""
                ),
                "invocationId": self.headers.get(
                    "X-UEAI-Invocation-Id", ""
                ),
                "processId": self.headers.get("X-UEAI-Process-Id", ""),
                "transport": self.headers.get("X-UEAI-Transport", ""),
                "instanceId": self.headers.get("X-UEAI-Instance-Id", ""),
                "command": self.headers.get("X-UEAI-Command", ""),
                "sessionId": self.headers.get("X-UEAI-Session-Id", ""),
            }

        def send_json(self, status: int, payload: dict) -> None:
            body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            if parsed.path == "/api/v1/workflow/handshake":
                get_request_headers.append(self.caller_headers())
                body = json.dumps(
                    {
                        "ok": True,
                        "data": {
                            "dslVersions": ["1.0", "2.0"],
                            "contractSetDigests": {
                                "1.0": contract_digest_v1,
                                "2.0": contract_digest_v2,
                            },
                        },
                    }
                ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if parsed.path != "/api/capabilities":
                self.send_error(404)
                return
            get_request_headers.append(self.caller_headers())
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
            request = json.loads(self.rfile.read(length) or b"{}")
            if self.path == "/api/v1/clients/register":
                registration_attempts.append(request)
                if not session_protocol_enabled:
                    self.send_json(
                        404,
                        {
                            "ok": False,
                            "error": {
                                "code": "not_found",
                                "message": "Route not found.",
                            },
                        },
                    )
                    return
                session_id = (
                    f"session-workflow-{len(registrations) + 1}"
                )
                registrations.append(request)
                issued_sessions[session_id] = request
                self.send_json(
                    200,
                    {
                        "ok": True,
                        "data": {
                            "sessionId": session_id,
                            "heartbeatIntervalMs": 5000,
                            "expiresAfterMs": 15000,
                        },
                    },
                )
                return
            if self.path == "/api/v1/clients/unregister":
                session_id = self.headers.get("X-UEAI-Session-Id", "")
                unregistrations.append(session_id)
                self.send_json(
                    200,
                    {
                        "ok": True,
                        "data": {
                            "sessionId": session_id,
                            "status": "offline",
                        },
                    },
                )
                return
            requests.append(request)
            request_headers.append(self.caller_headers())
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
            if action == "execute" and request.get("sections") == [
                "diagnostics"
            ]:
                failed_receipt = {
                    "schema": "ue.workflow-run.v1",
                    "runId": "run-cli-failed-receipt",
                    "planDigest": bound_plan_digest,
                    "contractSetDigest": contract_digest,
                    "status": "failed",
                    "failurePhase": "snapshot",
                    "causeCode": "snapshot_unavailable",
                    "snapshotId": "run-cli-failed-receipt:baseline",
                    "mutationStarted": False,
                    "journalPersisted": True,
                    "rollback": {
                        "attempted": False,
                        "status": "notNeeded",
                        "verified": True,
                    },
                }
                failed_result = dict(failed_receipt)
                failed_result["schema"] = "ue.workflow-result.v1"
                failed_result["receipt"] = failed_receipt
                self.send_json(
                    500,
                    {
                        "ok": False,
                        "error": {
                            "code": "snapshot_unavailable",
                            "message": "Synthetic admitted-run failure.",
                            "details": failed_result,
                        },
                    },
                )
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
        connected_doctor = subprocess.run(
            common + ["doctor", "--connect", "--endpoint", endpoint],
            check=True,
            capture_output=True,
            text=True,
        )
        connected_doctor_json = json.loads(connected_doctor.stdout)
        assert connected_doctor_json["editor"]["contractMatch"] is True
        assert (
            connected_doctor_json["editor"]["contracts"]["v1"]["match"]
            is True
        )
        assert (
            connected_doctor_json["editor"]["contracts"]["v2"]["match"]
            is True
        )

        with tempfile.TemporaryDirectory(prefix="ue-workflow-cli-") as temporary:
            receipt_path = Path(temporary) / "中文收据-📄.json"

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

            failed_receipt_path = Path(temporary) / "failed-receipt.json"
            admitted_failure = subprocess.run(
                common
                + [
                    "execute",
                    "--file",
                    args.fixture,
                    "--approve-plan",
                    bound_plan_digest,
                    "--receipt",
                    str(failed_receipt_path),
                    "--section",
                    "diagnostics",
                    "--endpoint",
                    endpoint,
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            assert admitted_failure.returncode == 5
            failed_receipt = json.loads(
                failed_receipt_path.read_text(encoding="utf-8")
            )
            assert failed_receipt["status"] == "failed"
            assert failed_receipt["failurePhase"] == "snapshot"
            assert failed_receipt["causeCode"] == "snapshot_unavailable"
            assert failed_receipt["journalPersisted"] is True
            assert failed_receipt["rollback"] == {
                "attempted": False,
                "status": "notNeeded",
                "verified": True,
            }

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

        assert sorted(unregistrations) == sorted(issued_sessions)
        assert registrations
        assert all(
            registration["clientKind"] == "cli"
            and registration["name"] == "ue-workflow-cli"
            and registration["version"] == "1.0.0"
            and registration["transport"] == "http"
            and isinstance(registration["pid"], int)
            and registration["pid"] > 0
            and registration["instanceId"]
            == registration["invocationId"]
            and registration["invocationId"].startswith("cli-")
            and registration["command"]
            for registration in registrations
        )

        session_protocol_enabled = False
        fallback_attempt_count = len(registration_attempts)
        fallback = subprocess.run(
            common
            + [
                "plan",
                "--file",
                args.fixture,
                "--connect",
                "--endpoint",
                endpoint,
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        assert json.loads(fallback.stdout)["data"]["executionReady"] is True
        assert len(registration_attempts) == fallback_attempt_count + 1
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    workflow_requests = [
        request for request in requests if "action" in request
    ]
    workflow_headers = [
        header
        for request, header in zip(requests, request_headers)
        if "action" in request
    ]
    assert [request["action"] for request in workflow_requests] == [
        "plan",
        "plan",
        "plan",
        "execute",
        "status",
        "resume",
        "rollback",
        "plan",
        "execute",
        "plan",
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
    assert workflow_requests[8]["sections"] == ["diagnostics"]
    session_request_headers = request_headers[:-1]
    assert all(
        header["callerType"] == "cli"
        and header["caller"] == "ue-workflow-cli"
        and header["callerVersion"] == "1.0.0"
        and header["invocationId"].startswith("cli-")
        and header["processId"].isdigit()
        and header["transport"] == "http"
        and header["instanceId"] == header["invocationId"]
        and header["sessionId"] in issued_sessions
        and issued_sessions[header["sessionId"]]["invocationId"]
        == header["invocationId"]
        and issued_sessions[header["sessionId"]]["command"]
        == header["command"]
        for header in session_request_headers
    )
    assert (
        workflow_headers[2]["invocationId"]
        == workflow_headers[3]["invocationId"]
    )
    assert (
        workflow_headers[2]["sessionId"]
        == workflow_headers[3]["sessionId"]
    )
    assert workflow_headers[2]["command"] == "execute"
    assert get_request_headers
    assert all(
        header["sessionId"] in issued_sessions
        and issued_sessions[header["sessionId"]]["invocationId"]
        == header["invocationId"]
        for header in get_request_headers
    )
    fallback_header = request_headers[-1]
    assert fallback_header["sessionId"] == ""
    assert fallback_header["callerType"] == "cli"
    assert fallback_header["caller"] == "ue-workflow-cli"
    assert fallback_header["invocationId"].startswith("cli-")
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
