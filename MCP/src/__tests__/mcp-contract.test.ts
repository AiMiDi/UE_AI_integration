import assert from "node:assert/strict";
import { test } from "node:test";

import { loadCapabilityCatalog } from "../capability-catalog.js";
import { runDomainOperation } from "../domain-router.js";
import { createLocalShutdownHandler } from "../index.js";
import {
  createMcpServer,
  MCP_TOOL_NAMES,
  type UEConnectionClient,
} from "../mcp-server.js";
import {
  UEApiError,
  UEClient,
  type UEExecuteData,
  type UEWorkflowRequest,
} from "../ue-bridge.js";
import { safeStringify } from "../helpers.js";
import {
  handleWorkflowInput,
  parseWorkflowInput,
  runWorkflowAction,
  UE_WORKFLOW_INPUT_SCHEMA,
  UE_WORKFLOW_TOOL_SCHEMA,
} from "../workflow-router.js";

test("keeps oversized MCP JSON text valid when truncating", () => {
  const output = safeStringify(
    {
      values: Array.from({ length: 100 }, (_, index) => ({
        index,
        label: `value-${index}`,
      })),
    },
    512,
  );
  const payload = JSON.parse(output);

  assert.equal(payload.truncated, true);
  assert.ok(payload.totalCharacters > 512);
  assert.equal(typeof payload.preview, "string");
  assert.ok(output.length <= 512);
});

test("registers exactly ten MCP tools without contacting Unreal Editor", () => {
  let networkCalls = 0;
  const offlineClient: UEConnectionClient = {
    getHealth: async () => {
      networkCalls += 1;
      throw new Error("must not run during registration");
    },
    getCapabilities: async () => {
      networkCalls += 1;
      throw new Error("must not run during registration");
    },
    execute: async () => {
      networkCalls += 1;
      throw new Error("must not run during registration");
    },
    workflow: async () => {
      networkCalls += 1;
      throw new Error("must not run during registration");
    },
  };

  const runtime = createMcpServer({
    catalog: loadCapabilityCatalog(),
    client: offlineClient,
  });
  const registeredToolMap = (
    runtime.server as unknown as {
      _registeredTools: Record<
        string,
        {
          inputSchema?: {
            shape?: Record<string, unknown>;
          };
        }
      >;
    }
  )._registeredTools;
  const registeredTools = Object.keys(registeredToolMap);

  assert.deepEqual(runtime.registeredToolNames, [
    "ue_status",
    "ue_capabilities",
    "ue_context",
    "ue_blueprint",
    "ue_scene",
    "ue_content",
    "ue_animation",
    "ue_ai",
    "ue_production",
    "ue_workflow",
  ]);
  assert.deepEqual(runtime.registeredToolNames, MCP_TOOL_NAMES);
  assert.deepEqual(registeredTools, MCP_TOOL_NAMES);
  assert.equal(runtime.registeredToolNames.length, 10);
  for (const toolName of [
    "ue_blueprint",
    "ue_scene",
    "ue_content",
    "ue_animation",
    "ue_ai",
    "ue_production",
  ]) {
    assert.deepEqual(
      Object.keys(registeredToolMap[toolName]?.inputSchema?.shape ?? {}),
      ["operation", "params", "requestId"],
    );
  }
  assert.deepEqual(
    Object.keys(registeredToolMap.ue_workflow?.inputSchema?.shape ?? {}),
    [
      "action",
      "workflow",
      "approvePlanDigest",
      "runId",
      "saveOnSuccess",
      "confirmWrite",
      "details",
    ],
  );
  assert.equal(networkCalls, 0);
});

test("validates ue_workflow action-specific inputs without accepting file paths", () => {
  const workflow = {
    dsl: "ue.workflow",
    dslVersion: "1.0",
    workflowKind: "assetEdit",
    workflowId: "test-widget",
    scope: {
      kind: "widgetBlueprint",
      asset: "/Game/UI/WBP_Test",
    },
    operations: [],
  };

  assert.equal(
    UE_WORKFLOW_INPUT_SCHEMA.safeParse({
      action: "plan",
      workflow,
      details: true,
    }).success,
    true,
  );
  assert.equal(
    UE_WORKFLOW_INPUT_SCHEMA.safeParse({
      action: "execute",
      workflow,
      approvePlanDigest: "sha256:test",
      saveOnSuccess: true,
    }).success,
    true,
  );
  assert.equal(
    UE_WORKFLOW_INPUT_SCHEMA.safeParse({
      action: "status",
      runId: "run-1",
    }).success,
    true,
  );
  assert.equal(
    UE_WORKFLOW_INPUT_SCHEMA.safeParse({
      action: "rollback",
      runId: "run-1",
      approvePlanDigest: "sha256:approved",
    }).success,
    true,
  );

  for (const invalid of [
    { action: "plan" },
    { action: "execute", workflow },
    { action: "status" },
    { action: "rollback", runId: "run-1" },
    { action: "resume", runId: "run-1", workflow },
    { action: "plan", workflow, file: "workflow.json" },
    { action: "plan", workflow, runId: "run-1" },
    { action: "validate", workflow, approvePlanDigest: "not-applicable" },
  ]) {
    assert.equal(UE_WORKFLOW_INPUT_SCHEMA.safeParse(invalid).success, false);
  }

  assert.throws(
    () => parseWorkflowInput({ action: "rollback" }),
    (error: unknown) =>
      error instanceof UEApiError &&
      error.code === "invalid_workflow_request" &&
      error.details !== undefined,
  );

  assert.deepEqual(Object.keys(UE_WORKFLOW_TOOL_SCHEMA.shape), [
    "action",
    "workflow",
    "approvePlanDigest",
    "runId",
    "saveOnSuccess",
    "confirmWrite",
    "details",
  ]);
});

test("rejects action-specific ue_workflow inputs before HTTP forwarding", async () => {
  let workflowCalls = 0;
  const response = await handleWorkflowInput(
    {
      workflow: async () => {
        workflowCalls += 1;
        return {};
      },
    },
    {
      action: "execute",
      workflow: {
        dsl: "ue.workflow",
      },
    },
  );

  assert.equal(workflowCalls, 0);
  assert.equal(response.isError, true);
  assert.equal(response.content[0]?.type, "text");
  if (response.content[0]?.type !== "text") {
    assert.fail("Expected workflow validation error as MCP text content");
  }
  const payload = JSON.parse(response.content[0].text);
  assert.equal(payload.error.code, "invalid_workflow_request");
});

test("forwards ue_workflow envelopes unchanged and maps structured responses", async () => {
  const calls: UEWorkflowRequest[] = [];
  const request = parseWorkflowInput({
    action: "execute",
    workflow: {
      dsl: "ue.workflow",
      dslVersion: "1.0",
      workflowKind: "assetEdit",
      workflowId: "build-material",
      scope: {
        kind: "material",
        asset: "/Game/Materials/M_Test",
      },
      operations: [],
    },
    approvePlanDigest: "sha256:approved",
    saveOnSuccess: false,
    confirmWrite: true,
    details: true,
  });

  const response = await runWorkflowAction(
    {
      workflow: async (value) => {
        calls.push(value);
        return {
          runId: "run-1",
          status: "succeeded",
          diagnostics: [],
        };
      },
    },
    request,
  );

  assert.equal(response.isError, false);
  assert.deepEqual(calls, [request]);
  assert.equal(response.content[0]?.type, "text");
  if (response.content[0]?.type !== "text") {
    assert.fail("Expected workflow JSON result as MCP text content");
  }
  assert.deepEqual(JSON.parse(response.content[0].text), {
    runId: "run-1",
    status: "succeeded",
    diagnostics: [],
  });
});

test("maps UE workflow API errors to canonical MCP errors", async () => {
  const response = await runWorkflowAction(
    {
      workflow: async () => {
        throw new UEApiError(
          {
            code: "plan_digest_mismatch",
            message: "The approved plan no longer matches",
            details: {
              expected: "sha256:new",
              actual: "sha256:old",
            },
          },
          409,
        );
      },
    },
    parseWorkflowInput({
      action: "status",
      runId: "run-1",
    }),
  );

  assert.equal(response.isError, true);
  assert.equal(response.content[0]?.type, "text");
  if (response.content[0]?.type !== "text") {
    assert.fail("Expected workflow error as MCP text content");
  }
  assert.deepEqual(JSON.parse(response.content[0].text), {
    ok: false,
    error: {
      code: "plan_digest_mismatch",
      message: "The approved plan no longer matches",
      details: {
        expected: "sha256:new",
        actual: "sha256:old",
      },
      status: 409,
    },
  });
});

test("rejects a cross-domain dotted operation before HTTP execution", async () => {
  const catalog = loadCapabilityCatalog();
  const sceneOperation = catalog.forDomain("scene")[0]?.id;
  assert.ok(sceneOperation);

  let executeCalls = 0;
  const response = await runDomainOperation(
    catalog,
    {
      execute: async () => {
        executeCalls += 1;
        return {};
      },
    },
    "blueprint",
    sceneOperation,
    {},
    "must-not-forward",
  );

  assert.equal(response.isError, true);
  assert.equal(executeCalls, 0);
  assert.equal(response.content[0]?.type, "text");
  const payload = JSON.parse(response.content[0].text);
  assert.equal(payload.error.code, "cross_domain_operation");
});

test("routes dedicated PIE commands through the scene domain", async () => {
  const catalog = loadCapabilityCatalog();
  const calls: Array<{ operation: string; params: Record<string, unknown> }> = [];

  for (const operation of [
    "scene.pie.start",
    "scene.pie.stop",
    "scene.pie.restart",
    "scene.pie.status",
    "scene.pie.pause",
    "scene.pie.resume",
  ]) {
    const response = await runDomainOperation(
      catalog,
      {
        execute: async (capability, params) => {
          calls.push({ operation: capability, params: params ?? {} });
          return { action: capability.split(".").at(-1), requested: true };
        },
      },
      "scene",
      operation,
      {},
    );
    assert.equal(response.isError, false);
  }

  assert.deepEqual(calls, [
    { operation: "scene.pie.start", params: {} },
    { operation: "scene.pie.stop", params: {} },
    { operation: "scene.pie.restart", params: {} },
    { operation: "scene.pie.status", params: {} },
    { operation: "scene.pie.pause", params: {} },
    { operation: "scene.pie.resume", params: {} },
  ]);
});

test("forwards an optional requestId through a domain router", async () => {
  const catalog = loadCapabilityCatalog();
  const calls: Array<{
    operation: string;
    params: Record<string, unknown>;
    requestId?: string;
  }> = [];

  const response = await runDomainOperation(
    catalog,
    {
      execute: async (operation, params, requestId) => {
        calls.push({
          operation,
          params: params ?? {},
          requestId,
        });
        return { changed: true };
      },
    },
    "content",
    "content.widget.event.unbind",
    {
      widget_bp: "/Game/UI/WBP_Test",
      widget_name: "ApplyButton",
      event_name: "OnClicked",
    },
    "request-001",
  );

  assert.equal(response.isError, false);
  assert.deepEqual(calls, [
    {
      operation: "content.widget.event.unbind",
      params: {
        widget_bp: "/Game/UI/WBP_Test",
        widget_name: "ApplyButton",
        event_name: "OnClicked",
      },
      requestId: "request-001",
    },
  ]);
});

test("maps canonical JSON capability data to MCP text content", async () => {
  const catalog = loadCapabilityCatalog();
  const operation = catalog
    .forDomain("blueprint")
    .find((capability) => capability.output.kind === "json")?.id;
  assert.ok(operation);

  const response = await runDomainOperation(
    catalog,
    {
      execute: async () => ({
        assetPath: "/Game/AI/BP_Example",
        compiled: true,
      }),
    },
    "blueprint",
    operation,
    {},
  );

  assert.equal(response.isError, false);
  const firstContent = response.content[0];
  assert.equal(firstContent?.type, "text");
  if (!firstContent || firstContent.type !== "text") {
    assert.fail("Expected JSON result as MCP text content");
  }
  assert.deepEqual(JSON.parse(firstContent.text), {
    assetPath: "/Game/AI/BP_Example",
    compiled: true,
  });
});

test("maps declared image output to native MCP image content", async () => {
  const catalog = loadCapabilityCatalog();
  const operation = "scene.viewport.capture";
  assert.equal(catalog.get(operation)?.output.kind, "image");

  const response = await runDomainOperation(
    catalog,
    {
      execute: async () => ({
        image_base64: "aW1hZ2U=",
        mime_type: "image/png",
        width: 1280,
        height: 720,
      }),
    },
    "scene",
    operation,
    {},
  );

  assert.equal(response.isError, false);
  assert.deepEqual(response.content[0], {
    type: "image",
    data: "aW1hZ2U=",
    mimeType: "image/png",
  });
  assert.equal(response.content[1]?.type, "text");
  const metadata = JSON.parse(response.content[1].text);
  assert.deepEqual(metadata, { width: 1280, height: 720 });
});

test("maps scenario artifact output to native MCP image content", async () => {
  const catalog = loadCapabilityCatalog();
  const operation = "production.scenario.artifact.get";
  assert.equal(catalog.get(operation)?.output.kind, "image");

  const response = await runDomainOperation(
    catalog,
    {
      execute: async () => ({
        image_base64: "c2NlbmFyaW8=",
        mime_type: "image/png",
        runId: "scenario-1",
        artifactId: "after-click",
      }),
    },
    "production",
    operation,
    {
      runId: "scenario-1",
      artifactId: "after-click",
    },
  );

  assert.deepEqual(response.content, [
    {
      type: "image",
      data: "c2NlbmFyaW8=",
      mimeType: "image/png",
    },
    {
      type: "text",
      text: JSON.stringify(
        {
          runId: "scenario-1",
          artifactId: "after-click",
        },
        null,
        2,
      ),
    },
  ]);
  assert.equal(response.isError, false);
});

test("maps canonical API errors to MCP isError responses", async () => {
  const catalog = loadCapabilityCatalog();
  const operation = catalog.forDomain("ai")[0]?.id;
  assert.ok(operation);

  const response = await runDomainOperation(
    catalog,
    {
      execute: async (): Promise<UEExecuteData> => {
        throw new UEApiError(
          {
            code: "execution_failed",
            message: "Editor operation failed",
            details: { operation },
          },
          500,
        );
      },
    },
    "ai",
    operation,
    {},
  );

  assert.equal(response.isError, true);
  assert.equal(response.content[0]?.type, "text");
  const payload = JSON.parse(response.content[0].text);
  assert.deepEqual(payload, {
    ok: false,
    error: {
      code: "execution_failed",
      message: "Editor operation failed",
      details: { operation },
      status: 500,
    },
  });
});

test("uses only HTTP endpoints and maps operations and workflows canonically", async () => {
  const calls: Array<{
    url: string;
    method: string;
    body?: string;
  }> = [];
  const fetchImpl: typeof fetch = async (input, init) => {
    const url = String(input);
    calls.push({
      url,
      method: init?.method ?? "GET",
      body: typeof init?.body === "string" ? init.body : undefined,
    });

    let data: unknown = {};
    if (url.endsWith("/api/health")) {
      data = {
        status: "ready",
        pluginVersion: "0.3.0",
        engineVersion: "5.7",
        projectName: "Test",
        mode: "editor",
        capabilityCount: 212,
        domainCounts: {},
        validationErrors: [],
      };
    } else if (url.endsWith("/api/capabilities")) {
      data = { capabilities: [] };
    } else if (url.endsWith("/api/execute")) {
      data = { actors: [] };
    } else if (url.endsWith("/api/v1/workflow/handshake")) {
      data = {
        serverInstanceId: "editor-1",
        contractSetDigest: "sha256:contracts",
      };
    } else if (url.endsWith("/api/v1/workflow")) {
      data = {
        valid: true,
        diagnostics: [],
      };
    }
    return new Response(JSON.stringify({ ok: true, data }), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    });
  };
  const client = new UEClient({
    baseUrl: "http://127.0.0.1:19847",
    fetchImpl,
  });

  await client.getHealth();
  await client.getCapabilities();
  await client.execute("scene.actor.list", { tag: "AI" }, "request-001");
  await client.getWorkflowHandshake();
  await client.workflow({
    action: "plan",
    workflow: {
      dsl: "ue.workflow",
      dslVersion: "1.0",
      workflowKind: "assetEdit",
    },
    details: true,
  });

  assert.deepEqual(calls, [
    {
      url: "http://127.0.0.1:19847/api/health",
      method: "GET",
      body: undefined,
    },
    {
      url: "http://127.0.0.1:19847/api/capabilities",
      method: "GET",
      body: undefined,
    },
    {
      url: "http://127.0.0.1:19847/api/execute",
      method: "POST",
      body: JSON.stringify({
        capability: "scene.actor.list",
        params: { tag: "AI" },
        requestId: "request-001",
      }),
    },
    {
      url: "http://127.0.0.1:19847/api/v1/workflow/handshake",
      method: "GET",
      body: undefined,
    },
    {
      url: "http://127.0.0.1:19847/api/v1/workflow",
      method: "POST",
      body: JSON.stringify({
        action: "plan",
        workflow: {
          dsl: "ue.workflow",
          dslVersion: "1.0",
          workflowKind: "assetEdit",
        },
        details: true,
      }),
    },
  ]);
  assert.equal(calls.some((call) => call.url.includes("shutdown")), false);
});

test("process shutdown closes only the local MCP transport", async () => {
  const events: string[] = [];
  const shutdown = createLocalShutdownHandler(
    async () => {
      events.push("close-mcp");
    },
    (code) => {
      events.push(`exit-${code}`);
    },
  );

  await shutdown();
  await shutdown();

  assert.deepEqual(events, ["close-mcp", "exit-0"]);
  assert.equal(
    Object.getOwnPropertyNames(UEClient.prototype).some((name) =>
      /shutdown|spawn|launch/i.test(name),
    ),
    false,
  );
});
