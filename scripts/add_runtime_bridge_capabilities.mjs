import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const path = resolve(process.argv[2] ?? "Resources/Capabilities/production.json");
const manifest = JSON.parse(readFileSync(path, "utf8"));
const targetFields = {
  projectRoot: { type: "string", minLength: 3 },
  pid: { type: "integer", minimum: 1 },
  expectedBuildId: { type: "string" },
  expectedProjectDigest: { type: "string", pattern: "^sha256:[a-f0-9]{64}$" },
};
const make = (id, kind, description, properties, required, external) => ({
  id,
  domain: "production",
  kind,
  description,
  inputSchema: { type: "object", properties, required, additionalProperties: false },
  traits: { destructive: false, expensive: false },
  effects: { asset: "none", world: "none", editorSession: "none", external },
  lifecycle: { status: "active", since: "1.0.0", canonicalId: id },
  output: { kind: "json" },
  execution: { backends: ["developmentRuntime"], preferred: "developmentRuntime" },
});
const items = [
  make("production.development.target.list", "query", "Discover live explicitly opted-in Development/DebugGame Runtime Bridge targets under an explicit project root.", { projectRoot: targetFields.projectRoot }, ["projectRoot"], "read"),
  make("production.development.attach.plan", "query", "Build a PID/start-time/Build/project/scope-bound Development Bridge attach plan without attaching.", { ...targetFields, scope: { type: "string", enum: ["observe", "control"], default: "observe" } }, ["projectRoot", "pid"], "read"),
  make("production.development.attach", "command", "Attach to an opted-in Development/DebugGame target using its single-use pairing token and an explicitly approved plan digest.", { ...targetFields, scope: { type: "string", enum: ["observe", "control"], default: "observe" }, approvePlanDigest: { type: "string", pattern: "^sha256:[a-f0-9]{64}$" }, confirmAttach: { type: "boolean", const: true } }, ["projectRoot", "pid", "approvePlanDigest", "confirmAttach"], "write"),
  make("production.development.session.status", "query", "Read bounded status from an attached Development Bridge session without loading or editing assets.", { ...targetFields, sessionToken: { type: "string", minLength: 16 } }, ["projectRoot", "pid", "sessionToken"], "read"),
  make("production.development.session.detach", "command", "Detach a Development Bridge session without terminating the user process.", { ...targetFields, sessionToken: { type: "string", minLength: 16 } }, ["projectRoot", "pid", "sessionToken"], "write"),
];
for (const item of items) {
  const index = manifest.capabilities.findIndex((candidate) => candidate.id === item.id);
  if (index >= 0) manifest.capabilities[index] = item;
  else manifest.capabilities.push(item);
}
manifest.capabilities.sort((left, right) => left.id.localeCompare(right.id));
writeFileSync(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
