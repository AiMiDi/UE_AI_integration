"""Execute one caller supplied Python script using the worker JSON protocol.

The worker deliberately does not inspect or rewrite the script AST.  The host
records the caller's modification level before starting this process, so the
audit trail describes the requested impact even when a script fails.
"""

import contextlib
import io
import json
import sys
import traceback


def main() -> None:
    try:
        request = json.loads(sys.stdin.buffer.read().decode("utf-8-sig"))
        if not isinstance(request, dict):
            raise ValueError("request must be a JSON object")
        script = request.get("script")
        data = request.get("data", {})
        if not isinstance(script, str) or not script:
            raise ValueError("script is required")
        if not isinstance(data, dict):
            raise ValueError("data must be a JSON object")

        # Expose both names so existing reflection scripts can be moved to the
        # full executor without changing their input contract.  A script may
        # assign `result`; its stdout/stderr are returned separately.
        scope = {"data": data, "input": data}
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            exec(compile(script, "<ue-ai-python>", "exec"), scope, scope)

        payload = {
            "ok": True,
            "result": scope.get("result"),
            "stdout": stdout.getvalue(),
            "stderr": stderr.getvalue(),
        }
        print(json.dumps(payload, ensure_ascii=False, default=str))
    except BaseException as exc:
        print(json.dumps({
            "ok": False,
            "error": str(exc),
            "exceptionType": type(exc).__name__,
            "traceback": traceback.format_exc(limit=12),
        }, ensure_ascii=False))


if __name__ == "__main__":
    main()
