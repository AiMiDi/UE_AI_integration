#!/usr/bin/env python3

import argparse
import base64
import codecs
import hashlib
import json
import os
import shutil
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
    request_headers: list[dict[str, str]] = []
    get_request_headers: list[dict[str, str]] = []
    registration_attempts: list[dict] = []
    registrations: list[dict] = []
    unregistrations: list[str] = []
    unregistration_connections: list[tuple[str, int]] = []
    session_control = {
        "supported": True,
        "transientFailures": 0,
        "expireNextBusinessStatus": 0,
        "slowUnregisterSeconds": 0.0,
    }
    issued_sessions: dict[str, dict] = {}
    expired_requests: list[dict] = []
    expired_request_headers: list[dict[str, str]] = []
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
                get_request_headers.append(self.caller_headers())
                self.send_json(
                    200,
                    {
                        "ok": True,
                        "data": {
                            "status": "healthy",
                            "apiVersion": "v1",
                            "pluginVersion": "0.8.0",
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
            get_request_headers.append(self.caller_headers())
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
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            request = json.loads(body or b"{}")
            if self.path == "/api/v1/clients/register":
                registration_attempts.append(request)
                if not session_control["supported"]:
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
                if session_control["transientFailures"] > 0:
                    session_control["transientFailures"] -= 1
                    self.send_json(
                        503,
                        {
                            "ok": False,
                            "error": {
                                "code": "temporarily_unavailable",
                                "message": "Registration is temporarily unavailable.",
                            },
                        },
                    )
                    return
                session_id = f"session-short-{len(registrations) + 1}"
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
                unregistration_connections.append(
                    (session_id, self.client_address[1])
                )
                delay = float(session_control["slowUnregisterSeconds"])
                if delay > 0.0:
                    time.sleep(delay)
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
            request_header = self.caller_headers()
            if (
                self.path == "/api/execute"
                and session_control["expireNextBusinessStatus"]
                and request_header["sessionId"]
            ):
                expired_status = int(
                    session_control["expireNextBusinessStatus"]
                )
                session_control["expireNextBusinessStatus"] = 0
                expired_requests.append(request)
                expired_request_headers.append(request_header)
                issued_sessions.pop(request_header["sessionId"], None)
                self.send_json(
                    expired_status,
                    {
                        "ok": False,
                        "error": {
                            "code": (
                                "client_session_expired"
                                if expired_status == 401
                                else "client_session_not_found"
                            ),
                            "message": "The client session expired.",
                        },
                    },
                )
                return
            if self.path != "/api/execute":
                self.send_json(
                    404,
                    {
                        "ok": False,
                        "error": {"code": "not_found", "message": "Not found."},
                    },
                )
                return
            requests.append(request)
            request_headers.append(request_header)
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
            before_help_queries = len(queries)
            before_help_requests = len(requests)
            capability_help = subprocess.run(
                [
                    args.cli,
                    "test.typed",
                    "--help",
                    "--live-schema",
                    "--endpoint",
                    "http://127.0.0.1:1",
                    "--capability-root",
                    str(temporary_path / "missing-catalog"),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            assert "Usage: ue test.typed" in capability_help.stdout
            assert "performs no catalog load or Editor request" in capability_help.stdout
            assert len(queries) == before_help_queries
            assert len(requests) == before_help_requests
            status_help = run(
                ["status", "--help", "--endpoint", "http://127.0.0.1:1"],
                check=True,
            )
            assert status_help.stdout.startswith("Usage: ue status")
            capabilities_help = run(
                ["capabilities", "--help", "--live-schema"],
                check=True,
            )
            assert capabilities_help.stdout.startswith(
                "Usage: ue capabilities"
            )
            strict_positional = run(["status", "extra", "--json"])
            assert strict_positional.returncode == 2
            assert (
                json.loads(strict_positional.stdout)["error"]["code"]
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

            inline_params = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--params",
                    json.dumps(
                        {
                            "assetPath": "/Game/Inline",
                            "enabled": False,
                            "settings": {"source": "inline"},
                        }
                    ),
                    "--json",
                ],
                check=True,
            )
            assert json.loads(inline_params.stdout)["ok"] is True
            assert requests[-1]["params"] == {
                "assetPath": "/Game/Inline",
                "enabled": False,
                "settings": {"source": "inline"},
            }

            params_file = temporary_path / "params.json"
            params_file.write_text(
                json.dumps(
                    {
                        "assetPath": "/Game/File",
                        "enabled": True,
                        "tags": ["from", "file"],
                    }
                ),
                encoding="utf-8",
            )
            file_params = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--params-file",
                    str(params_file),
                ],
                check=True,
            )
            assert file_params.stdout.startswith("OK test.typed ")
            assert requests[-1]["params"] == {
                "assetPath": "/Game/File",
                "enabled": True,
                "tags": ["from", "file"],
            }

            stdin_params = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--params-file",
                    "-",
                ],
                check=True,
                shell_input=json.dumps(
                    {
                        "assetPath": "/Game/Stdin",
                        "enabled": False,
                    }
                ),
            )
            assert stdin_params.stdout.startswith("OK test.typed ")
            assert requests[-1]["params"] == {
                "assetPath": "/Game/Stdin",
                "enabled": False,
            }

            unicode_argv = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--asset-path",
                    "/Game/蓝图/输入处理",
                    "--enabled",
                    "--json",
                ],
                check=True,
            )
            assert not unicode_argv.stdout.startswith("\ufeff")
            assert (
                json.loads(unicode_argv.stdout)["data"]["assetPath"]
                == "/Game/蓝图/输入处理"
            )
            assert (
                requests[-1]["params"]["assetPath"]
                == "/Game/蓝图/输入处理"
            )

            unicode_params_file = temporary_path / "中文参数.json"
            unicode_params_file.write_text(
                json.dumps(
                    {
                        "assetPath": "/Game/文件/中文",
                        "enabled": True,
                    },
                    ensure_ascii=False,
                ),
                encoding="utf-16",
            )
            utf16_file_params = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--params-file",
                    str(unicode_params_file),
                    "--json",
                ],
                check=True,
            )
            assert (
                json.loads(utf16_file_params.stdout)["data"]["assetPath"]
                == "/Game/文件/中文"
            )
            assert requests[-1]["params"]["assetPath"] == "/Game/文件/中文"

            def run_binary_params(payload: bytes) -> subprocess.CompletedProcess:
                return subprocess.run(
                    [
                        args.cli,
                        "test.typed",
                        "--endpoint",
                        endpoint,
                        "--params-file",
                        "-",
                        "--json",
                        "--capability-root",
                        str(capability_root),
                    ],
                    check=False,
                    capture_output=True,
                    timeout=5.0,
                    input=payload,
                )

            unicode_pipe_text = json.dumps(
                {
                    "assetPath": "/Game/管道/输入",
                    "enabled": False,
                },
                ensure_ascii=False,
            )
            utf8_pipe = run_binary_params(
                codecs.BOM_UTF8 + unicode_pipe_text.encode("utf-8")
            )
            assert utf8_pipe.returncode == 0
            assert not utf8_pipe.stdout.startswith(codecs.BOM_UTF8)
            assert (
                json.loads(utf8_pipe.stdout.decode("utf-8"))["data"][
                    "assetPath"
                ]
                == "/Game/管道/输入"
            )

            utf16be_pipe = run_binary_params(
                codecs.BOM_UTF16_BE
                + unicode_pipe_text.encode("utf-16-be")
            )
            assert utf16be_pipe.returncode == 0
            assert (
                json.loads(utf16be_pipe.stdout.decode("utf-8"))["data"][
                    "assetPath"
                ]
                == "/Game/管道/输入"
            )

            if os.name == "nt":
                def powershell_quote(value: str) -> str:
                    return "'" + value.replace("'", "''") + "'"

                def run_powershell_pipeline(
                    executable: str,
                    asset_path: str,
                ) -> None:
                    payload = json.dumps(
                        {
                            "assetPath": asset_path,
                            "enabled": True,
                        },
                        ensure_ascii=False,
                        separators=(",", ":"),
                    )
                    script = "\n".join(
                        [
                            "$utf8 = New-Object "
                            "System.Text.UTF8Encoding($false)",
                            "$OutputEncoding = $utf8",
                            "[Console]::InputEncoding = $utf8",
                            "[Console]::OutputEncoding = $utf8",
                            f"$payload = {powershell_quote(payload)}",
                            "$payload | & "
                            f"{powershell_quote(str(Path(args.cli).resolve()))} "
                            "test.typed "
                            f"--endpoint {powershell_quote(endpoint)} "
                            "--params-file - --json "
                            "--capability-root "
                            f"{powershell_quote(str(capability_root))}",
                            "if ($LASTEXITCODE -ne 0) { "
                            "exit $LASTEXITCODE }",
                        ]
                    )
                    encoded = base64.b64encode(
                        script.encode("utf-16-le")
                    ).decode("ascii")
                    result = subprocess.run(
                        [
                            executable,
                            "-NoLogo",
                            "-NoProfile",
                            "-NonInteractive",
                            "-EncodedCommand",
                            encoded,
                        ],
                        check=False,
                        capture_output=True,
                        timeout=15.0,
                    )
                    assert result.returncode == 0, (
                        executable,
                        result.stdout.decode("utf-8", errors="replace"),
                        result.stderr.decode("utf-8", errors="replace"),
                    )
                    output = result.stdout.decode("utf-8-sig").strip()
                    assert json.loads(output)["data"]["assetPath"] == asset_path
                    assert requests[-1]["params"]["assetPath"] == asset_path

                windows_powershell = shutil.which("powershell.exe")
                assert windows_powershell is not None
                run_powershell_pipeline(
                    windows_powershell,
                    "/Game/WindowsPowerShell51/中文管道",
                )
                powershell_seven = shutil.which("pwsh.exe")
                if powershell_seven is not None:
                    run_powershell_pipeline(
                        powershell_seven,
                        "/Game/PowerShell7/中文管道",
                    )

            request_count_before_invalid_encoding = len(requests)
            invalid_encoding = run_binary_params(b"\xc3\x28")
            assert invalid_encoding.returncode == 2
            assert (
                json.loads(invalid_encoding.stdout.decode("utf-8"))["error"][
                    "code"
                ]
                == "invalid_text_encoding"
            )
            assert len(requests) == request_count_before_invalid_encoding

            before_conflicts = len(requests)
            params_conflict = run(
                [
                    "test.typed",
                    "--params",
                    '{"assetPath":"/Game/A","enabled":true}',
                    "--params-file",
                    str(params_file),
                    "--json",
                ]
            )
            assert params_conflict.returncode == 2
            assert (
                json.loads(params_conflict.stdout)["error"]["code"]
                == "invalid_arguments"
            )
            field_conflict = run(
                [
                    "test.typed",
                    "--params-file",
                    str(params_file),
                    "--enabled",
                    "--json",
                ]
            )
            assert field_conflict.returncode == 2
            assert (
                json.loads(field_conflict.stdout)["error"]["code"]
                == "parameter_sources_conflict"
            )
            assert len(requests) == before_conflicts

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

            output_path = temporary_path / "蓝图截图-📷.bin"
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
            header_count = len(request_headers)
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
            shell_headers = request_headers[header_count:]
            assert len(shell_headers) == 2
            assert (
                shell_headers[0]["invocationId"]
                == shell_headers[1]["invocationId"]
            )
            assert (
                shell_headers[0]["sessionId"]
                == shell_headers[1]["sessionId"]
            )

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
            assert request_headers
            assert all(
                header["callerType"] == "cli"
                and header["caller"] == "ue"
                and header["callerVersion"] == "0.8.0"
                and header["invocationId"].startswith("cli-")
                and header["processId"].isdigit()
                and header["transport"] == "http"
                and header["instanceId"] == header["invocationId"]
                and header["sessionId"] in issued_sessions
                and issued_sessions[header["sessionId"]]["invocationId"]
                == header["invocationId"]
                and issued_sessions[header["sessionId"]]["command"]
                == header["command"]
                for header in request_headers
            )
            assert get_request_headers
            assert all(
                header["sessionId"] in issued_sessions
                and issued_sessions[header["sessionId"]]["invocationId"]
                == header["invocationId"]
                for header in get_request_headers
            )
            assert registrations
            assert all(
                registration["clientKind"] == "cli"
                and registration["name"] == "ue"
                and registration["version"] == "0.8.0"
                and registration["transport"] == "http"
                and isinstance(registration["pid"], int)
                and registration["pid"] > 0
                and registration["instanceId"]
                == registration["invocationId"]
                and registration["invocationId"].startswith("cli-")
                and registration["command"]
                for registration in registrations
            )
            assert sorted(unregistrations) == sorted(issued_sessions)

            transient_attempt_start = len(registration_attempts)
            transient_registration_start = len(registrations)
            transient_header_start = len(request_headers)
            session_control["transientFailures"] = 1
            transient_shell = run(
                ["shell", "--endpoint", endpoint],
                check=True,
                shell_input=(
                    "test.typed --asset-path /Game/TransientA --enabled\n"
                    "test.typed --asset-path /Game/TransientB --enabled\n"
                    "exit\n"
                ),
            )
            assert len(transient_shell.stdout.splitlines()) == 2
            transient_headers = request_headers[transient_header_start:]
            assert len(transient_headers) == 2
            assert transient_headers[0]["sessionId"] == ""
            assert transient_headers[1]["sessionId"] in issued_sessions
            assert (
                transient_headers[0]["invocationId"]
                == transient_headers[1]["invocationId"]
            )
            assert len(registration_attempts) == transient_attempt_start + 2
            assert len(registrations) == transient_registration_start + 1
            transient_attempts = registration_attempts[
                transient_attempt_start:
            ]
            assert (
                transient_attempts[0]["invocationId"]
                == transient_attempts[1]["invocationId"]
                == transient_headers[0]["invocationId"]
            )
            assert transient_headers[1]["sessionId"] in unregistrations

            for expiry_status in (404, 401):
                expiry_attempt_start = len(registration_attempts)
                expiry_registration_start = len(registrations)
                expiry_header_start = len(request_headers)
                expiry_rejection_start = len(expired_requests)
                session_control["expireNextBusinessStatus"] = expiry_status
                expiry_recovery = run(
                    [
                        "test.typed",
                        "--endpoint",
                        endpoint,
                        "--asset-path",
                        f"/Game/ExpiredSession{expiry_status}",
                        "--enabled",
                    ],
                    check=True,
                )
                assert expiry_recovery.stdout.startswith("OK test.typed ")
                assert len(expired_requests) == expiry_rejection_start + 1
                assert len(registration_attempts) == expiry_attempt_start + 2
                assert len(registrations) == expiry_registration_start + 2
                expiry_registrations = registrations[
                    expiry_registration_start:
                ]
                assert (
                    expiry_registrations[0]["invocationId"]
                    == expiry_registrations[1]["invocationId"]
                )
                recovered_headers = request_headers[expiry_header_start:]
                assert len(recovered_headers) == 1
                assert (
                    expired_request_headers[-1]["sessionId"]
                    != recovered_headers[0]["sessionId"]
                )
                assert (
                    expired_requests[-1]
                    == requests[expiry_header_start]
                )
                assert recovered_headers[0]["sessionId"] in unregistrations

            slow_unregister_header_start = len(request_headers)
            session_control["slowUnregisterSeconds"] = 2.5
            slow_started = time.perf_counter()
            slow_unregister = run(
                [
                    "test.typed",
                    "--endpoint",
                    endpoint,
                    "--asset-path",
                    "/Game/SlowUnregister",
                    "--enabled",
                ],
                check=True,
            )
            slow_elapsed = time.perf_counter() - slow_started
            session_control["slowUnregisterSeconds"] = 0.0
            assert slow_unregister.stdout.startswith("OK test.typed ")
            assert len(request_headers) == slow_unregister_header_start + 1
            assert slow_elapsed < 2.0
            slow_session_id = request_headers[-1]["sessionId"]
            slow_unregister_port = next(
                port
                for session_id, port in unregistration_connections
                if session_id == slow_session_id
            )
            assert slow_unregister_port != execute_ports[-1]

            session_control["supported"] = False
            fallback_attempt_count = len(registration_attempts)
            fallback_header_count = len(request_headers)
            fallback = run(
                [
                    "shell",
                    "--endpoint",
                    endpoint,
                ],
                check=True,
                shell_input=(
                    "test.typed --asset-path /Game/LegacyFallbackA --enabled\n"
                    "test.typed --asset-path /Game/LegacyFallbackB --enabled\n"
                    "exit\n"
                ),
            )
            assert len(fallback.stdout.splitlines()) == 2
            assert len(registration_attempts) == fallback_attempt_count + 1
            fallback_headers = request_headers[fallback_header_count:]
            assert len(fallback_headers) == 2
            assert all(
                header["sessionId"] == ""
                and header["callerType"] == "cli"
                and header["caller"] == "ue"
                and header["invocationId"].startswith("cli-")
                for header in fallback_headers
            )
            assert (
                fallback_headers[0]["invocationId"]
                == fallback_headers[1]["invocationId"]
            )
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
