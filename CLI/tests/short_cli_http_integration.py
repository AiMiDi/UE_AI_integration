#!/usr/bin/env python3

import argparse
import base64
import hashlib
import json
import os
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()

    requests: list[dict] = []
    execute_ports: list[int] = []
    queries: list[dict[str, list[str]]] = []
    artifact = b"standalone ue artifact\n"
    artifact_hash = hashlib.sha256(artifact).hexdigest()

    typed_schema = {
        "type": "object",
        "additionalProperties": False,
        "required": ["assetPath", "enabled"],
        "properties": {
            "assetPath": {"type": "string"},
            "enabled": {"type": "boolean"},
            "count": {"type": "integer"},
            "ratio": {"type": "number"},
            "tags": {"type": "array", "items": {"type": "string"}},
            "settings": {"type": "object"},
            "confirmWrite": {"type": "boolean"},
            "requestId": {"type": "string"},
        },
    }

    def descriptor(operation: str, *, live: bool = False) -> dict:
        schema = (
            typed_schema
            if operation == "test.typed"
            else {
                "type": "object",
                "additionalProperties": False,
                "properties": (
                    {
                        "offset": {"type": "integer"},
                        "requestId": {"type": "string"},
                    }
                    if operation == "test.artifact"
                    else {}
                ),
            }
        )
        result = {
            "id": operation,
            "domain": "test",
            "kind": "query",
            "description": f"Fake descriptor for {operation}.",
            "traits": {
                "readOnly": True,
                "destructive": False,
                "expensive": False,
            },
            "risk": "readOnly",
            "inputSchema": schema,
            "output": {"kind": "json"},
        }
        if live:
            result["available"] = operation != "test.unavailable"
            result["availabilityReasons"] = (
                ["required_module_missing"]
                if operation == "test.unavailable"
                else []
            )
        return result

    local_operations = [
        "test.typed",
        "test.unavailable",
        "test.artifact",
        "test.timeout",
        "test.error.422",
        "test.error.423",
        "test.error.409",
        "test.error.500",
    ]

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def send_json(self, status: int, payload: dict) -> None:
            body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (
                BrokenPipeError,
                ConnectionAbortedError,
                ConnectionResetError,
            ):
                pass

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            if parsed.path == "/api/health":
                self.send_json(
                    200,
                    {
                        "ok": True,
                        "data": {
                            "status": "healthy",
                            "apiVersion": "v1",
                            "pluginVersion": "0.6.0",
                        },
                    },
                )
                return
            if parsed.path != "/api/capabilities":
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
            query = parse_qs(parsed.query)
            queries.append(query)
            operation = query.get("operation", [None])[0]
            if operation == "test.missing":
                self.send_json(
                    404,
                    {
                        "ok": False,
                        "error": {
                            "code": "capability_not_found",
                            "message": "Capability does not exist.",
                        },
                    },
                )
                return
            capabilities = (
                [descriptor(operation, live=True)]
                if operation
                else [
                    descriptor("test.typed", live=True),
                    descriptor("test.artifact", live=True),
                ]
            )
            self.send_json(
                200,
                {
                    "ok": True,
                    "data": {
                        "capabilities": capabilities,
                        "detail": query.get("detail", ["summary"])[0],
                        "offset": 0,
                        "limit": len(capabilities),
                        "total": len(capabilities),
                        "hasMore": False,
                    },
                },
            )

        def do_POST(self) -> None:
            if self.path != "/api/execute":
                self.send_json(
                    404,
                    {
                        "ok": False,
                        "error": {"code": "not_found", "message": "Not found."},
                    },
                )
                return
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))
            requests.append(request)
            execute_ports.append(self.client_address[1])
            operation = request["capability"]

            if operation == "test.timeout":
                time.sleep(0.2)
            if operation.startswith("test.error."):
                status = int(operation.rsplit(".", 1)[1])
                self.send_json(
                    status,
                    {
                        "ok": False,
                        "error": {
                            "code": f"http_{status}",
                            "message": f"Fake HTTP {status}.",
                            "details": {
                                "validationErrors": ["first validation error"]
                            },
                        },
                    },
                )
                return
            if operation == "test.artifact":
                offset = int(request.get("params", {}).get("offset", 0))
                chunk = artifact[offset : offset + 7]
                next_offset = offset + len(chunk)
                self.send_json(
                    200,
                    {
                        "ok": True,
                        "data": {
                            "artifactId": "artifact-short-cli",
                            "kind": "file",
                            "mimeType": "application/octet-stream",
                            "sizeBytes": len(artifact),
                            "sha256": artifact_hash,
                            "offset": offset,
                            "nextOffset": next_offset,
                            "eof": next_offset == len(artifact),
                            "contentBase64": base64.b64encode(chunk).decode(
                                "ascii"
                            ),
                        },
                    },
                )
                return
            self.send_json(
                200,
                {
                    "ok": True,
                    "data": {
                        "assetPath": request.get("params", {}).get("assetPath"),
                        "enabled": request.get("params", {}).get("enabled"),
                        "graphs": [1, 2, 3],
                        "largeNodes": list(range(200)),
                        "contentBase64": base64.b64encode(b"hidden").decode(
                            "ascii"
                        ),
                    },
                },
            )

        def log_message(self, *_args: object) -> None:
            return

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    endpoint = f"http://127.0.0.1:{server.server_port}"

    with tempfile.TemporaryDirectory() as temporary:
        temporary_path = Path(temporary)
        capability_root = temporary_path / "Capabilities"
        capability_root.mkdir()
        (capability_root / "test.json").write_text(
            json.dumps(
                {
                    "schema": "ue.capability-manifest.v1",
                    "domain": "test",
                    "capabilities": [
                        descriptor(operation) for operation in local_operations
                    ],
                }
            ),
            encoding="utf-8",
        )

        def run(
            arguments: list[str],
            *,
            check: bool = False,
            timeout: float = 5.0,
            environment: dict[str, str] | None = None,
            shell_input: str | None = None,
        ) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                [
                    args.cli,
                    *arguments,
                    "--capability-root",
                    str(capability_root),
                ],
                check=check,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=environment,
                input=shell_input,
            )

        try:
            environment = os.environ.copy()
            environment["UE_PORT"] = "1"
            status = run(
                ["status", "--endpoint", endpoint, "--json"],
                check=True,
                environment=environment,
            )
            assert json.loads(status.stdout)["data"]["status"] == "healthy"

            invalid_capability = run(["invalid", "--json"])
            assert invalid_capability.returncode == 2
            assert (
                json.loads(invalid_capability.stdout)["error"]["code"]
                == "invalid_capability"
            )
            invalid_global = run(["status", "--endpoint", "--json"])
            assert invalid_global.returncode == 2
            assert (
                json.loads(invalid_global.stdout)["error"]["code"]
                == "invalid_arguments"
            )

            query_count = len(queries)
            help_result = run(
                ["help", "test.typed", "--endpoint", endpoint],
                check=True,
            )
            assert "Schema source: local manifest" in help_result.stdout
            assert "--asset-path <string> required" in help_result.stdout
            assert len(queries) == query_count

            live_help = run(
                [
                    "help",
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--live-schema",
                ],
                check=True,
            )
            assert "Schema source: Editor" in live_help.stdout
            assert queries[-1] == {
                "operation": ["test.typed"],
                "detail": ["full"],
            }

            query_count = len(queries)
            typed = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--asset-path",
                    "/Game/UI/WBP_Login",
                    "--enabled",
                    "false",
                    "--count",
                    "4",
                    "--ratio",
                    "1.5",
                    "--tags",
                    "alpha",
                    "--tags",
                    '["beta","gamma"]',
                    "--settings",
                    '{"mode":"compact"}',
                    "--confirm-write",
                ],
                check=True,
            )
            assert len(queries) == query_count
            assert typed.stdout.count("\n") == 1
            assert len(typed.stdout.encode("utf-8")) <= 1024
            assert typed.stdout.startswith("OK test.typed ")
            assert "contentBase64" not in typed.stdout
            assert "--json for full result" in typed.stdout
            typed_request = requests[-1]
            assert typed_request["params"] == {
                "assetPath": "/Game/UI/WBP_Login",
                "enabled": False,
                "count": 4,
                "ratio": 1.5,
                "tags": ["alpha", "beta", "gamma"],
                "settings": {"mode": "compact"},
                "confirmWrite": True,
            }
            assert typed_request["requestId"].startswith("ue-")

            live_typed = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--asset-path",
                    "/Game/Live",
                    "--enabled",
                    "--live-schema",
                ],
                check=True,
            )
            assert live_typed.stdout.startswith("OK test.typed ")
            assert queries[-1] == {
                "operation": ["test.typed"],
                "detail": ["full"],
            }
            assert requests[-1]["params"]["assetPath"] == "/Game/Live"

            explicit_request = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--assetPath",
                    "/Game/UI/WBP_Login",
                    "--enabled",
                    "--request-id",
                    "retry-123",
                    "--json",
                ],
                check=True,
            )
            explicit_json = json.loads(explicit_request.stdout)
            assert explicit_json["ok"] is True
            assert requests[-1]["requestId"] == "retry-123"
            assert requests[-1]["params"]["enabled"] is True

            before_invalid = len(requests)
            invalid = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--asset-path",
                    "/Game/A",
                    "--enabled",
                    "--unknown",
                    "value",
                ]
            )
            assert invalid.returncode == 2
            assert invalid.stderr.startswith("ERROR unknown_parameter:")
            assert len(requests) == before_invalid

            unavailable = run(
                [
                    "test.unavailable",
                    "--endpoint",
                    endpoint,
                    "--live-schema",
                ]
            )
            assert unavailable.returncode == 4
            assert unavailable.stderr.startswith("ERROR capability_unavailable:")

            query_count = len(queries)
            missing = run(["test.missing", "--endpoint", endpoint])
            assert missing.returncode == 2
            assert missing.stderr.startswith("ERROR capability_not_found:")
            assert len(queries) == query_count
            live_missing = run(
                [
                    "test.missing",
                    "--endpoint",
                    endpoint,
                    "--live-schema",
                ]
            )
            assert live_missing.returncode == 2
            assert live_missing.stderr.startswith("ERROR capability_not_found:")

            for status_code in [422, 423, 409, 500]:
                failure = run(
                    [f"test.error.{status_code}", "--endpoint", endpoint]
                )
                assert failure.returncode == 5
                assert failure.stderr == (
                    f"ERROR http_{status_code}: Fake HTTP {status_code}. "
                    "(first validation error)\n"
                )

            timeout_result = run(
                [
                    "test.timeout",
                    "--endpoint",
                    endpoint,
                    "--timeout-ms",
                    "30",
                ]
            )
            assert timeout_result.returncode == 4
            assert timeout_result.stderr.startswith(
                "ERROR editor_unreachable:"
            )

            output_path = temporary_path / "artifact.bin"
            exported = run(
                [
                    "test.artifact",
                    "--endpoint",
                    endpoint,
                    "--request-id",
                    "artifact-request",
                    "--output",
                    str(output_path),
                    "--json",
                ],
                check=True,
            )
            exported_json = json.loads(exported.stdout)
            assert output_path.read_bytes() == artifact
            assert (
                exported_json["data"]["artifactExport"]["sha256"]
                == f"sha256:{artifact_hash}"
            )
            assert "contentBase64" not in exported_json["data"]
            artifact_requests = [
                item
                for item in requests
                if item["capability"] == "test.artifact"
            ]
            assert len(artifact_requests) >= 2
            assert all(
                item["requestId"] == "artifact-request"
                for item in artifact_requests
            )
            offsets = [
                item["params"].get("offset", 0) for item in artifact_requests
            ]
            assert offsets == sorted(offsets)
            assert offsets[0] == 0

            query_count = len(queries)
            capabilities = run(
                [
                    "capabilities",
                    "--endpoint",
                    endpoint,
                    "--domain",
                    "test",
                    "--json",
                ],
                check=True,
            )
            local_catalog = json.loads(capabilities.stdout)
            assert local_catalog["data"]["total"] == len(local_operations)
            assert local_catalog["data"]["source"] == "local"
            assert len(queries) == query_count

            live_capabilities = run(
                [
                    "capabilities",
                    "--endpoint",
                    endpoint,
                    "--domain",
                    "test",
                    "--live-schema",
                    "--json",
                ],
                check=True,
            )
            assert json.loads(live_capabilities.stdout)["data"]["total"] == 2
            assert queries[-1]["domain"] == ["test"]

            invalid_capabilities = run(
                [
                    "capabilities",
                    "--endpoint",
                    endpoint,
                    "--unsupported",
                    "--json",
                ]
            )
            assert invalid_capabilities.returncode == 2
            assert (
                json.loads(invalid_capabilities.stdout)["error"]["code"]
                == "invalid_arguments"
            )

            query_count = len(queries)
            port_count = len(execute_ports)
            shell = run(
                [
                    "shell",
                    "--endpoint",
                    endpoint,
                ],
                check=True,
                shell_input=(
                    "test.typed --asset-path /Game/ShellA --enabled\n"
                    "test.typed --asset-path /Game/ShellB --enabled false\n"
                    "exit\n"
                ),
            )
            shell_lines = [
                line for line in shell.stdout.splitlines() if line
            ]
            assert len(shell_lines) == 2
            assert all(line.startswith("OK test.typed ") for line in shell_lines)
            assert len(queries) == query_count
            shell_ports = execute_ports[port_count:]
            assert len(shell_ports) == 2
            assert shell_ports[0] == shell_ports[1]

            live_shell = run(
                [
                    "shell",
                    "--endpoint",
                    endpoint,
                    "--live-schema",
                ],
                check=True,
                shell_input=(
                    "test.typed --asset-path /Game/LiveShell --enabled\n"
                    "exit\n"
                ),
            )
            assert live_shell.stdout.startswith("OK test.typed ")
            assert queries[-1] == {
                "operation": ["test.typed"],
                "detail": ["full"],
            }
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        unreachable = run(
            [
                "test.typed",
                "--endpoint",
                endpoint,
                "--timeout-ms",
                "50",
                "--asset-path",
                "/Game/Offline",
                "--enabled",
            ]
        )
        assert unreachable.returncode == 4
        assert unreachable.stderr.startswith("ERROR editor_unreachable:")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
