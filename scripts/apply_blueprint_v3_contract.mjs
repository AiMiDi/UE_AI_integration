import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";

const path = resolve(process.argv[2] ?? "Resources/Capabilities/blueprint.json");
const manifest = JSON.parse(readFileSync(path, "utf8"));
const previousIndex = manifest.capabilities.findIndex((item) => item.id === "blueprint.node.comment.set");
if (previousIndex >= 0) manifest.capabilities.splice(previousIndex, 1);

const commonComment = {
  domain: "blueprint",
  kind: "command",
  traits: { destructive: false, expensive: false },
  output: { kind: "json" },
  dsl: {
    admission: "editStep",
    scopeKinds: ["blueprint", "widgetBlueprint"],
    transactionDomain: "asset",
    deferCompile: true,
    risk: "safeWrite",
  },
  effects: { asset: "write", world: "none", editorSession: "none", external: "none" },
};
const commentIdentity = {
  blueprint: { type: "string", minLength: 1 },
  commentNodeId: { type: "string", pattern: "^[0-9A-Fa-f-]{32,36}$" },
};
const additions = [
  {
    ...commonComment,
    id: "blueprint.comment.title.set",
    description: "Set a Blueprint Comment title without changing its bubble visibility.",
    inputSchema: {
      type: "object",
      properties: { ...commentIdentity, title: { type: "string", maxLength: 512 } },
      required: ["blueprint", "commentNodeId", "title"],
      additionalProperties: false,
    },
    lifecycle: { status: "active", since: "0.10.0", canonicalId: "blueprint.comment.title.set" },
  },
  {
    ...commonComment,
    id: "blueprint.comment.bubble.set",
    description: "Set Blueprint Comment bubble visibility without changing its title.",
    inputSchema: {
      type: "object",
      properties: { ...commentIdentity, visible: { type: "boolean" } },
      required: ["blueprint", "commentNodeId", "visible"],
      additionalProperties: false,
    },
    lifecycle: { status: "active", since: "0.10.0", canonicalId: "blueprint.comment.bubble.set" },
  },
];
for (const capability of additions) {
  if (!manifest.capabilities.some((item) => item.id === capability.id)) manifest.capabilities.push(capability);
}

const organizer = manifest.capabilities.find((item) => item.id === "blueprint.layout.organize");
const comment = organizer.inputSchema.properties.groups.items.properties.comment;
Object.assign(comment.properties, {
  mode: { type: "string", enum: ["create", "update", "upsert"], default: "upsert" },
  commentNodeId: { type: "string", pattern: "^[0-9A-Fa-f-]{32,36}$" },
  groupKey: { type: "string", minLength: 1, maxLength: 128 },
  avoidOverlap: { type: "boolean", default: true },
});

const validation = manifest.capabilities.find((item) => item.id === "blueprint.layout.validate");
Object.assign(validation.inputSchema.properties, {
  tolerance: { type: "number", minimum: 0, maximum: 64, default: 0.5 },
  strictWarnings: { type: "boolean", default: false },
});

const capture = manifest.capabilities.find((item) => item.id === "blueprint.graph.capture");
Object.assign(capture.inputSchema.properties, {
  includeImageBase64: { type: "boolean", default: false },
  highlightRequestedNodes: { type: "boolean", default: false },
});
capture.output.kind = "json";

const state = manifest.capabilities.find((item) => item.id === "blueprint.asset.dirty.get");
state.description = "Return independent Blueprint packageDirty, compileStatus, needsCompile, and generatedClassUpToDate fields.";

manifest.tombstones = manifest.tombstones.filter((item) => item.id !== "blueprint.node.comment.set");
manifest.tombstones.push({
  id: "blueprint.node.comment.set",
  removedIn: "0.10.0",
  replacement: "blueprint.comment.title.set",
});
manifest.capabilities.sort((left, right) => left.id.localeCompare(right.id));
writeFileSync(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
