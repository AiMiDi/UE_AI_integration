#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import process from "node:process";
import { pathToFileURL } from "node:url";

function fail(message) {
  process.stderr.write(`MCP smoke failed: ${message}\n`);
  process.exitCode = 1;
}

const packageRoot = path.resolve(process.argv[2] ?? ".");
const mcpRoot = path.join(packageRoot, "MCP");
const entry = path.join(mcpRoot, "dist", "index.js");
const sdkRoot = path.join(
  mcpRoot,
  "node_modules",
  "@modelcontextprotocol",
  "sdk",
  "dist",
  "esm",
);

for (const required of [
  entry,
  path.join(sdkRoot, "client", "index.js"),
  path.join(sdkRoot, "client", "stdio.js"),
]) {
  if (!fs.existsSync(required)) {
    fail(`required file is missing: ${required}`);
    process.exit();
  }
}

const [{ Client }, { StdioClientTransport }] = await Promise.all([
  import(pathToFileURL(path.join(sdkRoot, "client", "index.js"))),
  import(pathToFileURL(path.join(sdkRoot, "client", "stdio.js"))),
]);

const client = new Client({ name: "ue-release-smoke", version: "1.0.0" });
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [entry],
  cwd: packageRoot,
  env: {
    ...process.env,
    UE_CAPABILITY_ROOT: path.join(packageRoot, "Resources", "Capabilities"),
    UE_SKILL_ROOT: path.join(packageRoot, "skills"),
  },
  stderr: "pipe",
});

try {
  await client.connect(transport);
  const listed = await client.listTools();
  const names = listed.tools.map((tool) => tool.name).sort();
  if (names.length !== 12) {
    throw new Error(`expected 12 tools, received ${names.length}: ${names.join(", ")}`);
  }
  process.stdout.write(
    `${JSON.stringify({ ok: true, toolCount: names.length, tools: names })}\n`,
  );
} catch (error) {
  fail(error instanceof Error ? error.message : String(error));
} finally {
  await client.close().catch(() => undefined);
}
