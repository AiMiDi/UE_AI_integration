import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const root = resolve(process.argv[2] ?? "Resources/Capabilities");

function effectsFor(capability, readOnly) {
  const access = readOnly ? "read" : "write";
  const effects = { asset: "none", world: "none", editorSession: "none", external: "none" };
  if (["blueprint", "content", "animation", "ai"].includes(capability.domain)) effects.asset = access;
  else if (capability.domain === "scene") effects.world = access;
  else effects.external = access;

  if (/\.(selection\.set|focus|open|capture)(\.|$)/.test(capability.id)) effects.editorSession = "write";
  if (capability.id === "blueprint.selection.set") {
    effects.asset = "read";
    effects.world = "none";
    effects.editorSession = "write";
  }
  if (capability.execution?.preferred && capability.execution.preferred !== "editor") {
    effects.external = access;
  }
  return effects;
}

for (const name of readdirSync(root).filter((name) => name.endsWith(".json")).sort()) {
  const path = resolve(root, name);
  const manifest = JSON.parse(readFileSync(path, "utf8"));
  manifest.schemaVersion = 3;
  manifest.tombstones ??= [];
  for (const capability of manifest.capabilities) {
    const readOnly = capability.traits?.readOnly === true;
    capability.effects = effectsFor(capability, readOnly);
    capability.traits = {
      destructive: capability.traits?.destructive === true,
      expensive: capability.traits?.expensive === true,
    };
    const isRecipe = capability.execution?.preferred === "localRecipe";
    const isSourceControl = capability.id.startsWith("production.source_control.");
    capability.lifecycle = {
      status: "active",
      since: isRecipe ? "0.10.0" : isSourceControl ? "0.9.1" : "0.9.0",
      canonicalId: capability.id,
    };
    if (capability.id === "blueprint.layout.straighten") {
      capability.lifecycle = {
        status: "deprecated",
        since: "0.9.0",
        canonicalId: "blueprint.layout.organize",
        replacement: "blueprint.layout.organize",
      };
    }
  }
  writeFileSync(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
}
