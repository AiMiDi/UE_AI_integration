import assert from "node:assert/strict";
import { join, resolve } from "node:path";
import { test } from "node:test";
import { pathToFileURL } from "node:url";
import { loadCapabilityCatalog } from "../capability-catalog.js";
import { locateWorkflowCli } from "../cli-locator.js";
import { runDomainOperation } from "../domain-router.js";
import { createLocalShutdownHandler } from "../index.js";
import { createMcpServer, handleCapabilities, handleContext, MCP_TOOL_NAMES, } from "../mcp-server.js";
import { UEApiError, UEClient, } from "../ue-bridge.js";
import { safeStringify } from "../helpers.js";
import { handleWorkflowInput, parseWorkflowInput, runWorkflowAction, UE_WORKFLOW_INPUT_SCHEMA, UE_WORKFLOW_TOOL_SCHEMA, } from "../workflow-router.js";
test("keeps oversized MCP JSON text valid when truncating", () => {
    const output = safeStringify({
        values: Array.from({ length: 100 }, (_, index) => ({
            index,
            label: `value-${index}`,
        })),
    }, 512);
    const payload = JSON.parse(output);
    assert.equal(payload.truncated, true);
    assert.ok(payload.totalCharacters > 512);
    assert.equal(typeof payload.preview, "string");
    assert.ok(output.length <= 512);
});
test("registers exactly eleven MCP tools without contacting Unreal Editor", () => {
    let networkCalls = 0;
    const offlineClient = {
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
    const registeredToolMap = runtime.server._registeredTools;
    const registeredTools = Object.keys(registeredToolMap);
    assert.deepEqual(runtime.registeredToolNames, [
        "ue_status",
        "ue_capabilities",
        "ue_context",
        "ue_cli",
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
    assert.equal(runtime.registeredToolNames.length, 11);
    assert.deepEqual(Object.keys(registeredToolMap.ue_cli?.inputSchema?.shape ?? {}), []);
    for (const toolName of [
        "ue_blueprint",
        "ue_scene",
        "ue_content",
        "ue_animation",
        "ue_ai",
        "ue_production",
    ]) {
        assert.deepEqual(Object.keys(registeredToolMap[toolName]?.inputSchema?.shape ?? {}), ["operation", "params", "requestId"]);
    }
    assert.deepEqual(Object.keys(registeredToolMap.ue_workflow?.inputSchema?.shape ?? {}), [
        "action",
        "workflow",
        "approvePlanDigest",
        "runId",
        "saveOnSuccess",
        "confirmWrite",
        "details",
        "detailLevel",
        "sections",
    ]);
    assert.equal(networkCalls, 0);
});
test("locates ue-workflow offline with deterministic precedence", () => {
    const fixturePluginRoot = resolve("fixture-plugin");
    const moduleUrl = pathToFileURL(join(fixturePluginRoot, "MCP", "dist", "cli-locator.js")).href;
    const executableName = process.platform === "win32" ? "ue-workflow.exe" : "ue-workflow";
    const configured = resolve("custom-tools", executableName);
    const packaged = join(fixturePluginRoot, "CLI", "bin", executableName);
    const configuredResult = locateWorkflowCli({
        moduleUrl,
        env: { PATH: "", UE_WORKFLOW_CLI: configured },
        isFile: (path) => path === configured || path === packaged,
    });
    assert.equal(configuredResult.found, true);
    assert.equal(configuredResult.executablePath, configured);
    assert.equal(configuredResult.source, "environment");
    assert.match(configuredResult.guidance, /doctor --connect$/);
    const packagedResult = locateWorkflowCli({
        moduleUrl,
        env: { PATH: "" },
        isFile: (path) => path === packaged,
    });
    assert.equal(packagedResult.found, true);
    assert.equal(packagedResult.executablePath, packaged);
    assert.equal(packagedResult.source, "packaged");
    assert.ok(packagedResult.candidates.length <= 6);
    assert.ok(Buffer.byteLength(JSON.stringify(packagedResult)) <= 4096);
    assert.ok(packagedResult.candidates.some((candidate) => candidate.source === "packaged" &&
        candidate.path === packaged &&
        candidate.exists));
    const missingResult = locateWorkflowCli({
        moduleUrl,
        env: { PATH: "" },
        isFile: () => false,
    });
    assert.equal(missingResult.found, false);
    assert.equal(missingResult.executablePath, null);
    assert.equal(missingResult.source, "not_found");
    assert.match(missingResult.guidance, /UE_WORKFLOW_CLI/);
});
test("pages and filters capability summaries without emitting schemas by default", async () => {
    const catalog = loadCapabilityCatalog();
    const noNetworkClient = {
        getHealth: async () => {
            throw new Error("not expected");
        },
        getCapabilities: async () => {
            throw new Error("not expected");
        },
        execute: async () => {
            throw new Error("not expected");
        },
        workflow: async () => {
            throw new Error("not expected");
        },
    };
    const defaultResponse = await handleCapabilities(catalog, noNetworkClient, {});
    assert.equal(defaultResponse.content[0]?.type, "text");
    if (defaultResponse.content[0]?.type !== "text") {
        assert.fail("Expected capability catalog as MCP text content");
    }
    const defaultPayload = JSON.parse(defaultResponse.content[0].text);
    assert.equal(defaultPayload.total, 212);
    assert.equal(defaultPayload.offset, 0);
    assert.equal(defaultPayload.limit, 25);
    assert.equal(defaultPayload.hasMore, true);
    assert.equal(defaultPayload.capabilities.length, 25);
    assert.equal(defaultPayload.capabilities.some((capability) => Object.hasOwn(capability, "inputSchema")), false);
    const filteredResponse = await handleCapabilities(catalog, noNetworkClient, {
        domain: "scene",
        query: "pie.",
        kind: "command",
        offset: 1,
        limit: 2,
    });
    assert.equal(filteredResponse.content[0]?.type, "text");
    if (filteredResponse.content[0]?.type !== "text") {
        assert.fail("Expected filtered capability catalog as MCP text content");
    }
    const filteredPayload = JSON.parse(filteredResponse.content[0].text);
    assert.equal(filteredPayload.offset, 1);
    assert.equal(filteredPayload.limit, 2);
    assert.ok(filteredPayload.total >= 3);
    assert.equal(filteredPayload.capabilities.length, 2);
    assert.ok(filteredPayload.capabilities.every((capability) => capability.id.includes("pie.") &&
        capability.domain === "scene" &&
        capability.kind === "command"));
    const exactResponse = await handleCapabilities(catalog, noNetworkClient, { operation: "scene.pie.start" });
    assert.equal(exactResponse.content[0]?.type, "text");
    if (exactResponse.content[0]?.type !== "text") {
        assert.fail("Expected exact capability as MCP text content");
    }
    const exactPayload = JSON.parse(exactResponse.content[0].text);
    assert.equal(exactPayload.total, 1);
    assert.equal(exactPayload.detail, "full");
    assert.equal(exactPayload.capabilities.length, 1);
    assert.ok(exactPayload.capabilities[0].inputSchema);
});
test("keeps ue_context directory-only by default and pages full schemas by domain", () => {
    const catalog = loadCapabilityCatalog();
    const directoryResponse = handleContext(catalog, {});
    assert.equal(directoryResponse.content[0]?.type, "text");
    if (directoryResponse.content[0]?.type !== "text") {
        assert.fail("Expected context directory as MCP text content");
    }
    const directoryPayload = JSON.parse(directoryResponse.content[0].text);
    assert.equal(directoryPayload.capabilityCount, 212);
    assert.equal(Object.hasOwn(directoryPayload, "capabilities"), false);
    const domainResponse = handleContext(catalog, { domain: "blueprint" });
    assert.equal(domainResponse.content[0]?.type, "text");
    if (domainResponse.content[0]?.type !== "text") {
        assert.fail("Expected paged context as MCP text content");
    }
    const domainPayload = JSON.parse(domainResponse.content[0].text);
    assert.equal(domainPayload.total, 58);
    assert.equal(domainPayload.limit, 10);
    assert.equal(domainPayload.capabilities.length, 10);
    assert.ok(domainPayload.capabilities.every((capability) => Object.hasOwn(capability, "inputSchema")));
});
test("forwards live capability filters and pagination directly to the editor", async () => {
    const catalog = loadCapabilityCatalog();
    let receivedQuery;
    const client = {
        getHealth: async () => {
            throw new Error("not expected");
        },
        getCapabilities: async (query) => {
            receivedQuery = query;
            return {
                capabilities: [],
                total: 0,
                offset: query?.offset ?? 0,
                limit: query?.limit ?? 25,
                hasMore: false,
                detail: query?.detail ?? "summary",
            };
        },
        execute: async () => {
            throw new Error("not expected");
        },
        workflow: async () => {
            throw new Error("not expected");
        },
    };
    await handleCapabilities(catalog, client, {
        live: true,
        domain: "production",
        kind: "query",
        readOnly: true,
        expensive: false,
        outputKind: "json",
        query: "module",
        offset: 5,
        limit: 7,
    });
    assert.deepEqual(receivedQuery, {
        query: "module",
        domain: "production",
        operation: undefined,
        kind: "query",
        readOnly: true,
        destructive: undefined,
        expensive: false,
        outputKind: "json",
        offset: 5,
        limit: 7,
        detail: "summary",
    });
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
    assert.equal(UE_WORKFLOW_INPUT_SCHEMA.safeParse({
        action: "plan",
        workflow,
        detailLevel: "standard",
        sections: ["diagnostics"],
    }).success, true);
    assert.equal(UE_WORKFLOW_INPUT_SCHEMA.safeParse({
        action: "execute",
        workflow,
        approvePlanDigest: "sha256:test",
        saveOnSuccess: true,
    }).success, true);
    assert.equal(UE_WORKFLOW_INPUT_SCHEMA.safeParse({
        action: "status",
        runId: "run-1",
    }).success, true);
    assert.equal(UE_WORKFLOW_INPUT_SCHEMA.safeParse({
        action: "rollback",
        runId: "run-1",
        approvePlanDigest: "sha256:approved",
    }).success, true);
    for (const invalid of [
        { action: "plan" },
        { action: "execute", workflow },
        { action: "status" },
        { action: "rollback", runId: "run-1" },
        { action: "resume", runId: "run-1", workflow },
        { action: "plan", workflow, file: "workflow.json" },
        { action: "plan", workflow, runId: "run-1" },
        { action: "validate", workflow, approvePlanDigest: "not-applicable" },
        {
            action: "plan",
            workflow,
            details: false,
            detailLevel: "summary",
        },
    ]) {
        assert.equal(UE_WORKFLOW_INPUT_SCHEMA.safeParse(invalid).success, false);
    }
    assert.throws(() => parseWorkflowInput({ action: "rollback" }), (error) => error instanceof UEApiError &&
        error.code === "invalid_workflow_request" &&
        error.details !== undefined);
    assert.deepEqual(Object.keys(UE_WORKFLOW_TOOL_SCHEMA.shape), [
        "action",
        "workflow",
        "approvePlanDigest",
        "runId",
        "saveOnSuccess",
        "confirmWrite",
        "details",
        "detailLevel",
        "sections",
    ]);
});
test("rejects action-specific ue_workflow inputs before HTTP forwarding", async () => {
    let workflowCalls = 0;
    const response = await handleWorkflowInput({
        workflow: async () => {
            workflowCalls += 1;
            return {};
        },
    }, {
        action: "execute",
        workflow: {
            dsl: "ue.workflow",
        },
    });
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
    const calls = [];
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
    const response = await runWorkflowAction({
        workflow: async (value) => {
            calls.push(value);
            return {
                runId: "run-1",
                status: "succeeded",
                diagnostics: [],
            };
        },
    }, request);
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
    const response = await runWorkflowAction({
        workflow: async () => {
            throw new UEApiError({
                code: "plan_digest_mismatch",
                message: "The approved plan no longer matches",
                details: {
                    expected: "sha256:new",
                    actual: "sha256:old",
                },
            }, 409);
        },
    }, parseWorkflowInput({
        action: "status",
        runId: "run-1",
    }));
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
    const response = await runDomainOperation(catalog, {
        execute: async () => {
            executeCalls += 1;
            return {};
        },
    }, "blueprint", sceneOperation, {}, "must-not-forward");
    assert.equal(response.isError, true);
    assert.equal(executeCalls, 0);
    assert.equal(response.content[0]?.type, "text");
    const payload = JSON.parse(response.content[0].text);
    assert.equal(payload.error.code, "cross_domain_operation");
});
test("routes dedicated PIE commands through the scene domain", async () => {
    const catalog = loadCapabilityCatalog();
    const calls = [];
    for (const operation of [
        "scene.pie.start",
        "scene.pie.stop",
        "scene.pie.restart",
        "scene.pie.status",
        "scene.pie.pause",
        "scene.pie.resume",
    ]) {
        const response = await runDomainOperation(catalog, {
            execute: async (capability, params) => {
                calls.push({ operation: capability, params: params ?? {} });
                return { action: capability.split(".").at(-1), requested: true };
            },
        }, "scene", operation, {});
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
    const calls = [];
    const response = await runDomainOperation(catalog, {
        execute: async (operation, params, requestId) => {
            calls.push({
                operation,
                params: params ?? {},
                requestId,
            });
            return { changed: true };
        },
    }, "content", "content.widget.event.unbind", {
        widget_bp: "/Game/UI/WBP_Test",
        widget_name: "ApplyButton",
        event_name: "OnClicked",
    }, "request-001");
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
    const response = await runDomainOperation(catalog, {
        execute: async () => ({
            assetPath: "/Game/AI/BP_Example",
            compiled: true,
        }),
    }, "blueprint", operation, {});
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
    const response = await runDomainOperation(catalog, {
        execute: async () => ({
            image_base64: "aW1hZ2U=",
            mime_type: "image/png",
            width: 1280,
            height: 720,
        }),
    }, "scene", operation, {});
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
    const response = await runDomainOperation(catalog, {
        execute: async () => ({
            image_base64: "c2NlbmFyaW8=",
            mime_type: "image/png",
            runId: "scenario-1",
            artifactId: "after-click",
        }),
    }, "production", operation, {
        runId: "scenario-1",
        artifactId: "after-click",
    });
    assert.deepEqual(response.content, [
        {
            type: "image",
            data: "c2NlbmFyaW8=",
            mimeType: "image/png",
        },
        {
            type: "text",
            text: JSON.stringify({
                runId: "scenario-1",
                artifactId: "after-click",
            }, null, 2),
        },
    ]);
    assert.equal(response.isError, false);
});
test("maps canonical API errors to MCP isError responses", async () => {
    const catalog = loadCapabilityCatalog();
    const operation = catalog.forDomain("ai")[0]?.id;
    assert.ok(operation);
    const response = await runDomainOperation(catalog, {
        execute: async () => {
            throw new UEApiError({
                code: "execution_failed",
                message: "Editor operation failed",
                details: { operation },
            }, 500);
        },
    }, "ai", operation, {});
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
    const calls = [];
    const fetchImpl = async (input, init) => {
        const url = String(input);
        calls.push({
            url,
            method: init?.method ?? "GET",
            body: typeof init?.body === "string" ? init.body : undefined,
        });
        let data = {};
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
        }
        else if (url.includes("/api/capabilities")) {
            data = { capabilities: [] };
        }
        else if (url.endsWith("/api/execute")) {
            data = { actors: [] };
        }
        else if (url.endsWith("/api/v1/workflow/handshake")) {
            data = {
                serverInstanceId: "editor-1",
                contractSetDigest: "sha256:contracts",
            };
        }
        else if (url.endsWith("/api/v1/workflow")) {
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
    await client.getCapabilities({
        domain: "scene",
        query: "pie",
        readOnly: false,
        offset: 2,
        limit: 3,
        detail: "full",
    });
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
            url: "http://127.0.0.1:19847/api/capabilities" +
                "?domain=scene&query=pie&readOnly=false&offset=2&limit=3&detail=full",
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
    const events = [];
    const shutdown = createLocalShutdownHandler(async () => {
        events.push("close-mcp");
    }, (code) => {
        events.push(`exit-${code}`);
    });
    await shutdown();
    await shutdown();
    assert.deepEqual(events, ["close-mcp", "exit-0"]);
    assert.equal(Object.getOwnPropertyNames(UEClient.prototype).some((name) => /shutdown|spawn|launch/i.test(name)), false);
});
//# sourceMappingURL=mcp-contract.test.js.map