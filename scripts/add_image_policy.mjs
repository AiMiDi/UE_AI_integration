import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(process.argv[2] ?? "Resources/Capabilities");
const policy = {
  type: "object",
  properties: {
    targetTokens: { type: "integer", minimum: 128, maximum: 32768 },
    maxWidth: { type: "integer", minimum: 64, maximum: 8192 },
    maxHeight: { type: "integer", minimum: 64, maximum: 8192 },
    color: { type: "string", enum: ["color", "grayscale", "monochrome"], default: "color" },
    inline: { type: "string", enum: ["none", "thumbnail", "full"], default: "none" },
  },
  additionalProperties: false,
};
for (const name of readdirSync(root).filter((name) => name.endsWith(".json"))) {
  const path = resolve(root, name);
  const manifest = JSON.parse(readFileSync(path, "utf8"));
  for (const capability of manifest.capabilities) {
    if (capability.output?.kind === "image" || capability.id === "blueprint.graph.capture") {
      capability.inputSchema.properties.imagePolicy = policy;
    }
  }
  writeFileSync(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
}
