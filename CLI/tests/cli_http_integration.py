#!/usr/bin/env python3

import argparse
import json
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


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
    planned = subprocess.run(
        common + ["plan", "--file", args.fixture],
        check=True,
        capture_output=True,
        text=True,
    )
    plan = json.loads(planned.stdout)
    plan_digest = plan["planDigest"]
    contract_digest = plan["contractSetDigest"]
    requests: list[dict] = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self) -> None:
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))
            requests.append(request)
            action = request["action"]
            statuses = {
                "execute": "pending",
                "status": "running",
                "resume": "completed",
                "rollback": "rolledBack",
            }
            receipt = {
                "schema": "ue.workflow-run.v1",
                "runId": "run-cli-integration",
                "planDigest": plan_digest,
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

            run(
                "execute",
                "--file",
                args.fixture,
                "--approve-plan",
                plan_digest,
                "--receipt",
                str(receipt_path),
            )
            written = json.loads(receipt_path.read_text(encoding="utf-8"))
            assert written["schema"] == "ue.workflow-run.v1"
            assert written["status"] == "pending"

            run("status", "--receipt", str(receipt_path))
            assert json.loads(receipt_path.read_text(encoding="utf-8"))["status"] == "running"
            run("resume", "--receipt", str(receipt_path))
            assert json.loads(receipt_path.read_text(encoding="utf-8"))["status"] == "completed"
            run("rollback", "--receipt", str(receipt_path))
            final_receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            assert final_receipt["status"] == "rolledBack"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)

    assert [request["action"] for request in requests] == [
        "execute",
        "status",
        "resume",
        "rollback",
    ]
    assert "approvePlanDigest" not in requests[1]
    assert "approvePlanDigest" not in requests[2]
    assert requests[3]["approvePlanDigest"] == plan_digest
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
