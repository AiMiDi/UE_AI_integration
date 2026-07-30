"""Small dependency-free client for the UE_AI_integration HTTP API."""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True)
class MCPApiError(Exception):
    code: str
    message: str
    status: int | None = None
    details: Any = None

    def __str__(self) -> str:
        prefix = f"HTTP {self.status}: " if self.status is not None else ""
        return f"{prefix}{self.code}: {self.message}"


class UEAIIntegrationClient:
    def __init__(self, port: int = 9847, timeout: float = 30.0) -> None:
        self.base_url = f"http://127.0.0.1:{port}/api"
        self.timeout = timeout

    def health(self) -> dict[str, Any]:
        return self._request("GET", "/health")

    def capabilities(self, domain: str | None = None) -> list[dict[str, Any]]:
        capabilities: list[dict[str, Any]] = []
        offset = 0
        expected_total: int | None = None
        while True:
            params: dict[str, Any] = {
                "offset": offset,
                "limit": 100,
            }
            if domain:
                params["domain"] = domain
            data = self._request(
                "GET",
                "/capabilities?" + urllib.parse.urlencode(params),
            )
            page = data.get("capabilities")
            if not isinstance(page, list) or not all(
                isinstance(item, dict) for item in page
            ):
                raise MCPApiError(
                    "invalid_response", "Missing or invalid capabilities array"
                )
            total = data.get("total")
            if not isinstance(total, int) or total < 0:
                raise MCPApiError(
                    "invalid_response", "Missing or invalid capability total"
                )
            if expected_total is None:
                expected_total = total
            elif total != expected_total:
                raise MCPApiError(
                    "invalid_response", "Capability total changed during pagination"
                )
            capabilities.extend(page)
            has_more = data.get("hasMore")
            if not isinstance(has_more, bool):
                raise MCPApiError(
                    "invalid_response", "Missing or invalid hasMore flag"
                )
            if not has_more:
                break
            if not page:
                raise MCPApiError(
                    "invalid_response", "Capability pagination made no progress"
                )
            offset += len(page)

        if expected_total != len(capabilities):
            raise MCPApiError(
                "invalid_response",
                f"Capability pagination returned {len(capabilities)} of "
                f"{expected_total} descriptors",
            )
        return capabilities

    def execute(
        self,
        capability: str,
        params: dict[str, Any] | None = None,
        timeout: float | None = None,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        body: dict[str, Any] = {"capability": capability, "params": params or {}}
        if request_id is not None:
            body["requestId"] = request_id
        data = self._request(
            "POST",
            "/execute",
            body,
            timeout=timeout,
        )
        if not isinstance(data, dict):
            raise MCPApiError("invalid_response", "Execute data must be an object")
        return data

    def _request(
        self,
        method: str,
        endpoint: str,
        body: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        payload = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(
            f"{self.base_url}{endpoint}",
            data=payload,
            method=method,
            headers={"Content-Type": "application/json"} if payload else {},
        )

        try:
            with urllib.request.urlopen(
                request, timeout=self.timeout if timeout is None else timeout
            ) as response:
                raw = json.loads(response.read().decode("utf-8"))
                return self._unwrap(raw, response.status)
        except urllib.error.HTTPError as error:
            try:
                raw = json.loads(error.read().decode("utf-8"))
                self._unwrap(raw, error.code)
            except MCPApiError:
                raise
            except Exception as parse_error:
                raise MCPApiError(
                    "http_error", str(parse_error), status=error.code
                ) from error
            raise MCPApiError("http_error", error.reason, status=error.code) from error
        except urllib.error.URLError as error:
            raise MCPApiError("connection_failed", str(error.reason)) from error

    @staticmethod
    def _unwrap(payload: Any, status: int) -> dict[str, Any]:
        if not isinstance(payload, dict) or not isinstance(payload.get("ok"), bool):
            raise MCPApiError(
                "invalid_response", "Response is not a API envelope", status=status
            )

        if payload["ok"]:
            data = payload.get("data", {})
            if not isinstance(data, dict):
                raise MCPApiError(
                    "invalid_response", "Envelope data must be an object", status=status
                )
            return data

        error = payload.get("error")
        if not isinstance(error, dict):
            raise MCPApiError(
                "invalid_response", "Error envelope is missing error", status=status
            )
        raise MCPApiError(
            str(error.get("code", "unknown_error")),
            str(error.get("message", "Unknown error")),
            status=status,
            details=error.get("details"),
        )
