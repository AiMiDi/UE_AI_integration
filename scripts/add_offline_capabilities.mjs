import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const path = resolve(process.argv[2] ?? "Resources/Capabilities/production.json");
const manifest = JSON.parse(readFileSync(path, "utf8"));
const root = {
  projectRoot: { type: "string", minLength: 3 },
  uproject: { type: "string" },
};
const descriptor = (id, description, backend, properties, required) => ({
  id,
  domain: "production",
  kind: "query",
  description,
  inputSchema: { type: "object", properties, required, additionalProperties: false },
  traits: { destructive: false, expensive: backend === "localAsset" },
  effects: { asset: "none", world: "none", editorSession: "none", external: "read" },
  lifecycle: { status: "active", since: "0.11.0", canonicalId: id },
  output: { kind: "json" },
  execution: { backends: [backend], preferred: backend },
});
const capabilities = [
  descriptor("production.project.summary.get", "Read a bounded .uproject summary without launching Unreal Editor.", "localProject", root, ["projectRoot"]),
  descriptor("production.project.config.get", "Resolve bounded project .ini configuration values with source provenance.", "localProject", { ...root, files: { type: "array", maxItems: 64, items: { type: "string" } } }, ["projectRoot"]),
  descriptor("production.project.validate", "Validate .uproject JSON, project-local plugin dependencies, text encoding, and BuildConfiguration presence offline.", "localProject", root, ["projectRoot"]),
  descriptor("production.asset.package.summary.get", "Read a UE 5.3 package header in an isolated local Asset Worker without instantiating UObject.", "localAsset", { projectRoot: root.projectRoot, assetPath: { type: "string" }, engineVersion: { type: "string", default: "5.3" } }, ["projectRoot", "assetPath"]),
  descriptor("production.asset.package.diff", "Compare normalized bounded UE 5.3 package-header summaries in an isolated local Asset Worker.", "localAsset", { projectRoot: root.projectRoot, assetPath: { type: "string" }, otherAssetPath: { type: "string" }, engineVersion: { type: "string", default: "5.3" } }, ["projectRoot", "assetPath", "otherAssetPath"]),
];
for (const capability of capabilities) {
  const index = manifest.capabilities.findIndex((item) => item.id === capability.id);
  if (index >= 0) manifest.capabilities[index] = capability;
  else manifest.capabilities.push(capability);
}
manifest.capabilities.sort((left, right) => left.id.localeCompare(right.id));
writeFileSync(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
