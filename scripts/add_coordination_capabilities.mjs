import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
const path = resolve(process.argv[2] ?? "Resources/Capabilities/production.json");
const manifest = JSON.parse(readFileSync(path, "utf8"));
const leaseType = { type: "string", enum: ["pie", "compile", "editorRestart", "performance"] };
const make = (id, kind, description, properties, required, write = false) => ({
  id, domain: "production", kind, description,
  inputSchema: { type: "object", properties, required, additionalProperties: false },
  traits: { destructive: false, expensive: false },
  effects: { asset: "none", world: "none", editorSession: write ? "write" : "read", external: "none" },
  lifecycle: { status: "active", since: "0.11.0", canonicalId: id },
  output: { kind: "json" },
});
const items = [
  make("production.session.roster.get", "query", "Return the bounded live MCP/CLI session roster and recent activity metadata.", {}, [], false),
  make("production.lease.status", "query", "Return active PIE, compile, editorRestart, and performance advisory leases plus bounded override audit.", {}, [], false),
  make("production.lease.acquire", "command", "Acquire or renew a 30-second session-bound advisory lease; cross-owner override requires the conflict planDigest.", { type: leaseType, sessionId: { type: "string", minLength: 1 }, override: { type: "boolean", default: false }, approvePlanDigest: { type: "string", pattern: "^sha256:[0-9a-f]{64}$" } }, ["type", "sessionId"], true),
  make("production.lease.release", "command", "Release an advisory lease owned by the supplied live session.", { type: leaseType, sessionId: { type: "string", minLength: 1 } }, ["type", "sessionId"], true),
];
for (const item of items) {
  const index = manifest.capabilities.findIndex((candidate) => candidate.id === item.id);
  if (index >= 0) manifest.capabilities[index] = item; else manifest.capabilities.push(item);
}
manifest.capabilities.sort((a, b) => a.id.localeCompare(b.id));
writeFileSync(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
