import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { loadCapabilityCatalog } from "./capability-catalog.js";
import { validateRecipe } from "./recipe-runner.js";
import { UEApiError } from "./ue-bridge.js";
const MAX_SOURCE = 256 * 1024;
const FORBIDDEN = /\b(import|require|open|fetch|XMLHttpRequest|WebSocket|process|globalThis|eval|Function|exec|spawn|fork|unreal|child_process|fs|net|http|https)\b/;
function canonical(value) {
    if (Array.isArray(value))
        return value.map(canonical);
    if (typeof value !== "object" || value === null)
        return value;
    return Object.fromEntries(Object.keys(value).sort().map((key) => [key, canonical(value[key])]));
}
function digest(value) {
    return `sha256:${createHash("sha256").update(JSON.stringify(canonical(value)), "utf8").digest("hex")}`;
}
export function parseSalSource(source) {
    if (Buffer.byteLength(source, "utf8") > MAX_SOURCE)
        throw new UEApiError({ code: "sal_resource_limit", message: "SAL source exceeds 256 KiB." });
    if (FORBIDDEN.test(source))
        throw new UEApiError({ code: "sal_ast_forbidden", message: "SAL source contains a forbidden import, I/O, process, network, Unreal, or dynamic-code construct." });
    const match = /^\s*plan\s*\(\s*([\s\S]*?)\s*\)\s*;?\s*$/.exec(source);
    if (!match)
        throw new UEApiError({ code: "sal_ast_invalid", message: "SAL accepts exactly one plan(<strict JSON>) expression." });
    let value;
    try {
        value = JSON.parse(match[1]);
    }
    catch (error) {
        throw new UEApiError({ code: "sal_json_invalid", message: `SAL plan payload must be strict JSON: ${error.message}` });
    }
    if (typeof value !== "object" || value === null || Array.isArray(value))
        throw new UEApiError({ code: "sal_plan_invalid", message: "SAL plan payload must be an object." });
    const object = value;
    if (object.kind !== "recipe" && object.kind !== "workflow")
        throw new UEApiError({ code: "sal_plan_kind_invalid", message: "SAL may construct only recipe or workflow plans." });
    if (!("value" in object) || typeof object.value !== "object" || object.value === null)
        throw new UEApiError({ code: "sal_plan_value_required", message: "SAL plan requires an immutable recipe/workflow value object." });
    return object;
}
export function salStub() {
    const catalog = loadCapabilityCatalog();
    const capabilities = catalog.capabilities
        .filter((capability) => capability.lifecycle.canonicalId === capability.id)
        .map((capability) => ({ id: capability.id, params: capability.inputSchema, effects: capability.effects, lifecycle: capability.lifecycle }));
    return {
        schema: "ue.sal-stub.v1",
        language: "restricted-plan-json",
        declaration: "declare function plan(value: { kind: 'recipe' | 'workflow'; value: Readonly<unknown> }): never;",
        capabilities,
        forbidden: ["imports", "filesystem", "network", "processes", "unreal Python", "direct capability execution"],
    };
}
export function salLint(source) {
    const plan = parseSalSource(source);
    const diagnostics = [];
    if (plan.kind === "recipe") {
        const validation = validateRecipe(plan.value);
        diagnostics.push(...validation.diagnostics);
    }
    else {
        const workflow = plan.value;
        if (workflow.dsl !== "ue.workflow" || typeof workflow.dslVersion !== "string")
            diagnostics.push({ code: "workflow_schema_invalid", message: "Workflow SAL plans require dsl=ue.workflow and dslVersion." });
    }
    return { schema: "ue.sal-lint.v1", ok: diagnostics.length === 0, diagnostics, resourceLimits: { sourceBytes: MAX_SOURCE }, directExecution: false };
}
export function salPlan(source) {
    const lint = salLint(source);
    if (lint.ok !== true)
        throw new UEApiError({ code: "sal_lint_failed", message: "SAL plan failed validation.", details: lint });
    const parsed = parseSalSource(source);
    return { schema: "ue.sal-plan.v1", kind: parsed.kind, value: parsed.value, planDigest: digest({ schema: "ue.sal-plan.v1", kind: parsed.kind, value: parsed.value }), executable: false, next: parsed.kind === "recipe" ? "Recipe Runner approval" : "Workflow plan and approval" };
}
export function readSalSource(path) {
    return readFileSync(path, "utf8");
}
//# sourceMappingURL=sal-runner.js.map