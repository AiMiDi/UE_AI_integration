import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const path = resolve(process.argv[2] ?? "Resources/Capabilities/production.json");
const manifest = JSON.parse(readFileSync(path, "utf8"));
const make = (id, description, properties, required = []) => ({
  id,
  domain: "production",
  kind: "query",
  description,
  inputSchema: { type: "object", properties, required, additionalProperties: false },
  traits: { destructive: false, expensive: false },
  effects: { asset: "none", world: "none", editorSession: "none", external: "none" },
  lifecycle: { status: "active", since: "1.0.0", canonicalId: id },
  output: { kind: "json" },
  execution: { backends: ["localSal"], preferred: "localSal" },
});
const items = [
  make("production.sal.stub", "Generate bounded typed capability schema v3 stubs for restricted SAL plan authoring.", {}),
  make("production.sal.lint", "AST-check restricted SAL source and reject imports, I/O, network, processes, Unreal, dynamic code, and direct execution.", { source: { type: "string", maxLength: 262144 } }, ["source"]),
  make("production.sal.plan", "Validate restricted SAL source and emit only a digest-bound Recipe or Workflow plan; this capability never executes it.", { source: { type: "string", maxLength: 262144 } }, ["source"]),
];
for (const item of items) {
  const index = manifest.capabilities.findIndex((candidate) => candidate.id === item.id);
  if (index >= 0) manifest.capabilities[index] = item;
  else manifest.capabilities.push(item);
}
manifest.capabilities.sort((left, right) => left.id.localeCompare(right.id));
writeFileSync(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
