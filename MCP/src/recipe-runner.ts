import { createHash, randomUUID } from "node:crypto";
import {
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { homedir, tmpdir } from "node:os";
import { basename, dirname, resolve } from "node:path";
import { fork } from "node:child_process";
import { fileURLToPath } from "node:url";

import {
  loadCapabilityCatalog,
  capabilityIsReadOnly,
  type CapabilityCatalog,
  type CapabilityDescriptor,
} from "./capability-catalog.js";
import {
  UEApiError,
  UEClient,
  type UEWorkflowRequest,
} from "./ue-bridge.js";

export const RECIPE_SCHEMA = "ue.recipe.v2";
export const RECIPE_PLAN_SCHEMA = "ue.recipe-plan.v2";
export const RECIPE_RUN_SCHEMA = "ue.recipe-run.v2";
const LEGACY_RECIPE_RUN_SCHEMA = "ue.recipe-run.v1";
const MAX_STEPS = 64;
const MAX_POLL_ATTEMPTS = 100;
const MAX_POLL_INTERVAL_MS = 30_000;
const MAX_LOG_CHARS = 8_192;
const WORKER_HEARTBEAT_MS = 2_000;
const WORKER_LEASE_MS = 10_000;

type JsonObject = Record<string, unknown>;
type RunStatus =
  | "running"
  | "interrupted"
  | "awaitingApproval"
  | "completed"
  | "failed"
  | "cancelled";

interface RetryPolicy {
  maxAttempts: number;
  backoffMs: number;
  transientErrors: string[];
}

export interface RecipeStep extends JsonObject {
  id: string;
  kind:
    | "capability"
    | "workflow"
    | "poll"
    | "condition"
    | "approval"
    | "sourceControlCheckout";
  capability?: string;
  params?: JsonObject;
  workflow?: JsonObject;
  approvePlanDigest?: string;
  condition?: unknown;
  poll?: {
    capability: string;
    params?: JsonObject;
    until: unknown;
    maxAttempts: number;
    intervalMs: number;
  };
  retry?: Partial<RetryPolicy>;
  compensate?: {
    kind: "capability" | "workflow";
    capability?: string;
    params?: JsonObject;
    runId?: string;
    approvePlanDigest?: string;
  };
  highRisk?: boolean;
}

export interface RecipeDefinition extends JsonObject {
  schema: typeof RECIPE_SCHEMA;
  id: string;
  version: string;
  inputs?: JsonObject;
  steps: RecipeStep[];
}

export interface RecipeValidation {
  ok: boolean;
  schema: "ue.recipe-validation.v2";
  diagnostics: Array<{ path: string; code: string; message: string }>;
  normalized?: RecipeDefinition;
  planDigest?: string;
  requiresApproval?: boolean;
}

interface StepState {
  id: string;
  status: "pending" | "dispatching" | "running" | "completed" | "failed" | "compensated";
  attempts: number;
  startedAt?: string;
  completedAt?: string;
  output?: unknown;
  error?: { code: string; message: string };
  workflowReceipt?: unknown;
  approvalDigest?: string;
}

export interface RecipeRunState extends JsonObject {
  schema: typeof RECIPE_RUN_SCHEMA;
  runId: string;
  recipeId: string;
  recipeVersion: string;
  planDigest: string;
  status: RunStatus;
  phase: string;
  createdAt: string;
  updatedAt: string;
  heartbeatAt: string;
  workerPid?: number;
  workerInstanceId?: string;
  workerHeartbeatAt?: string;
  workerLeaseExpiresAt?: string;
  lastProgressAt: string;
  lastLog: string;
  nextStep: number;
  inputs: JsonObject;
  recipe: RecipeDefinition;
  steps: StepState[];
  cancelRequested: boolean;
  cancelPending: boolean;
  awaitingApproval?: {
    stepId: string;
    reason: string;
    planDigest: string;
  };
  approvedSteps: string[];
  result?: unknown;
  error?: { code: string; message: string };
  compensations: Array<{ stepId: string; ok: boolean; output?: unknown; error?: unknown }>;
}

function isObject(value: unknown): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function canonical(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(canonical);
  if (!isObject(value)) return value;
  return Object.fromEntries(
    Object.keys(value)
      .sort()
      .map((key) => [key, canonical(value[key])]),
  );
}

function digest(value: unknown): string {
  return `sha256:${createHash("sha256")
    .update(JSON.stringify(canonical(value)), "utf8")
    .digest("hex")}`;
}

function now(): string {
  return new Date().toISOString();
}

function recipeRoot(env: NodeJS.ProcessEnv = process.env): string {
  if (env.UEAI_RECIPE_ROOT) return resolve(env.UEAI_RECIPE_ROOT);
  if (process.platform === "win32" && env.LOCALAPPDATA) {
    return resolve(env.LOCALAPPDATA, "UE-AI-CLI", "recipes");
  }
  const state = env.XDG_STATE_HOME ?? resolve(homedir(), ".local", "state");
  return resolve(state || tmpdir(), "ue-ai-cli", "recipes");
}

function runPath(runId: string, env?: NodeJS.ProcessEnv): string {
  if (!/^recipe-run-[a-f0-9-]{36}$/.test(runId)) {
    throw new UEApiError({
      code: "recipe_run_id_invalid",
      message: "runId is not a valid Recipe Runner identifier.",
    });
  }
  return resolve(recipeRoot(env), runId, "run.json");
}

function writeAtomic(path: string, value: unknown): void {
  mkdirSync(dirname(path), { recursive: true });
  const temporary = `${path}.tmp-${process.pid}-${randomUUID()}`;
  writeFileSync(temporary, `${JSON.stringify(value, null, 2)}\n`, "utf8");
  try {
    renameSync(temporary, path);
  } catch {
    rmSync(path, { force: true });
    renameSync(temporary, path);
  }
}

function readRun(runId: string, env?: NodeJS.ProcessEnv): RecipeRunState {
  const path = runPath(runId, env);
  if (!existsSync(path)) {
    throw new UEApiError({
      code: "recipe_run_not_found",
      message: `Recipe run ${runId} was not found.`,
    });
  }
  const parsed = JSON.parse(readFileSync(path, "utf8")) as unknown;
  if (
    !isObject(parsed) ||
    (parsed.schema !== RECIPE_RUN_SCHEMA && parsed.schema !== LEGACY_RECIPE_RUN_SCHEMA)
  ) {
    throw new UEApiError({
      code: "recipe_run_corrupt",
      message: `Recipe run ${runId} has an invalid journal.`,
    });
  }
  const state = parsed as unknown as RecipeRunState;
  let changed = false;
  if (parsed.schema === LEGACY_RECIPE_RUN_SCHEMA) {
    state.schema = RECIPE_RUN_SCHEMA;
    if (state.status === "running") {
      state.status = "interrupted";
      state.phase = "interrupted";
      state.lastLog = "Migrated a non-terminal v1 journal to interrupted; explicit resume is required.";
    }
    for (const step of state.steps ?? []) {
      if (step.status === "running") step.status = "dispatching";
    }
    changed = true;
  }
  state.approvedSteps ??= [];
  state.compensations ??= [];
  if (
    state.status === "running" &&
    typeof state.workerLeaseExpiresAt === "string" &&
    Date.parse(state.workerLeaseExpiresAt) <= Date.now() &&
    !processIsAlive(state.workerPid)
  ) {
    state.status = "interrupted";
    state.phase = "interrupted";
    state.updatedAt = now();
    state.lastProgressAt = state.updatedAt;
    state.lastLog = "Recipe Worker heartbeat lease expired and its process is no longer alive.";
    changed = true;
  }
  if (changed) writeAtomic(path, state);
  return state;
}

function processIsAlive(pid: number | undefined): boolean {
  if (!Number.isInteger(pid) || (pid ?? 0) <= 0) return false;
  try {
    process.kill(pid!, 0);
    return true;
  } catch {
    return false;
  }
}

function workerLeaseMs(env: NodeJS.ProcessEnv = process.env): number {
  if (env.UEAI_RECIPE_TEST_MODE === "1") {
    const requested = Number(env.UEAI_RECIPE_TEST_LEASE_MS);
    if (Number.isFinite(requested)) {
      return Math.min(WORKER_LEASE_MS, Math.max(100, requested));
    }
  }
  return WORKER_LEASE_MS;
}

function leaseExpiry(env: NodeJS.ProcessEnv = process.env): string {
  return new Date(Date.now() + workerLeaseMs(env)).toISOString();
}

function updateRun(
  state: RecipeRunState,
  message?: string,
  env: NodeJS.ProcessEnv = process.env,
): void {
  state.updatedAt = now();
  state.heartbeatAt = state.updatedAt;
  if (message !== undefined) {
    state.lastProgressAt = state.updatedAt;
    state.lastLog = message.slice(-MAX_LOG_CHARS);
  }
  writeAtomic(runPath(state.runId, env), state);
}

function descriptorReadOnly(descriptor: CapabilityDescriptor): boolean {
  return capabilityIsReadOnly(descriptor);
}

function defaultRetry(value: Partial<RetryPolicy> | undefined): RetryPolicy {
  return {
    maxAttempts: Math.min(5, Math.max(1, Number(value?.maxAttempts ?? 1))),
    backoffMs: Math.min(30_000, Math.max(0, Number(value?.backoffMs ?? 1_000))),
    transientErrors: Array.isArray(value?.transientErrors)
      ? value.transientErrors.filter((item): item is string => typeof item === "string")
      : [
          "editor_unreachable",
          "trace_worker_unavailable",
          "connection_reset",
          "operation_temporarily_unavailable",
        ],
  };
}

const NEVER_RETRY = new Set([
  "invalid_params",
  "schema_invalid",
  "approval_required",
  "approval_mismatch",
  "plan_digest_mismatch",
  "permission_denied",
  "source_control_checkout_failed",
  "source_control_checkout_preflight_failed",
]);

function validationError(
  diagnostics: RecipeValidation["diagnostics"],
  path: string,
  code: string,
  message: string,
): void {
  diagnostics.push({ path, code, message });
}

function validateCondition(
  value: unknown,
  path: string,
  diagnostics: RecipeValidation["diagnostics"],
): void {
  if (!isObject(value)) {
    validationError(diagnostics, path, "condition_invalid", "Condition must be an object.");
    return;
  }
  const keys = Object.keys(value);
  if (keys.length !== 1) {
    validationError(diagnostics, path, "condition_invalid", "Condition must contain exactly one operator.");
    return;
  }
  const operator = keys[0];
  if (operator === "all" || operator === "any") {
    const children = value[operator];
    if (!Array.isArray(children) || children.length === 0 || children.length > 16) {
      validationError(diagnostics, path, "condition_invalid", `${operator} must contain 1-16 conditions.`);
      return;
    }
    children.forEach((child, index) => validateCondition(child, `${path}/${operator}/${index}`, diagnostics));
    return;
  }
  if (operator === "not") {
    validateCondition(value.not, `${path}/not`, diagnostics);
    return;
  }
  if (!["equals", "notEquals", "exists"].includes(operator ?? "")) {
    validationError(diagnostics, path, "condition_operator_forbidden", "Only all, any, not, equals, notEquals, and exists are allowed.");
    return;
  }
  const expression = value[operator ?? ""];
  if (!isObject(expression) || typeof expression.path !== "string") {
    validationError(diagnostics, path, "condition_invalid", `${operator} requires a path.`);
    return;
  }
  if (!/^(inputs|steps)\.[A-Za-z0-9_.-]+$/.test(expression.path)) {
    validationError(diagnostics, `${path}/${operator}/path`, "condition_path_forbidden", "Condition paths may read only inputs and prior step outputs.");
  }
}

export function validateRecipe(
  value: unknown,
  catalog: CapabilityCatalog = loadCapabilityCatalog(),
): RecipeValidation {
  const diagnostics: RecipeValidation["diagnostics"] = [];
  if (!isObject(value)) {
    return { ok: false, schema: "ue.recipe-validation.v2", diagnostics: [{ path: "", code: "recipe_invalid", message: "Recipe must be an object." }] };
  }
  const allowedRoot = new Set(["schema", "id", "version", "inputs", "steps", "description"]);
  for (const field of Object.keys(value)) {
    if (!allowedRoot.has(field)) validationError(diagnostics, `/${field}`, "recipe_field_forbidden", `Unknown Recipe field ${field}.`);
  }
  if (value.schema !== RECIPE_SCHEMA) validationError(diagnostics, "/schema", "recipe_schema_invalid", `schema must be ${RECIPE_SCHEMA}.`);
  if (typeof value.id !== "string" || !/^[a-z0-9][a-z0-9.-]{0,127}$/.test(value.id)) validationError(diagnostics, "/id", "recipe_id_invalid", "id must be a stable lower-case identifier.");
  if (typeof value.version !== "string" || value.version.length === 0) validationError(diagnostics, "/version", "recipe_version_invalid", "version is required.");
  if (!Array.isArray(value.steps) || value.steps.length === 0 || value.steps.length > MAX_STEPS) {
    validationError(diagnostics, "/steps", "recipe_steps_invalid", `steps must contain 1-${MAX_STEPS} entries.`);
  }
  const ids = new Set<string>();
  let requiresApproval = false;
  if (Array.isArray(value.steps)) {
    value.steps.forEach((raw, index) => {
      const path = `/steps/${index}`;
      if (!isObject(raw)) {
        validationError(diagnostics, path, "recipe_step_invalid", "Step must be an object.");
        return;
      }
      if (typeof raw.id !== "string" || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(raw.id)) validationError(diagnostics, `${path}/id`, "recipe_step_id_invalid", "Step id is invalid.");
      else if (ids.has(raw.id)) validationError(diagnostics, `${path}/id`, "recipe_step_id_duplicate", "Step id is duplicated.");
      else ids.add(raw.id);
      if (!["capability", "workflow", "poll", "condition", "approval", "sourceControlCheckout"].includes(String(raw.kind))) {
        validationError(diagnostics, `${path}/kind`, "recipe_step_kind_forbidden", "Unsupported step kind; arbitrary scripts and loops are forbidden.");
        return;
      }
      if (["workflow", "approval"].includes(String(raw.kind)) || raw.highRisk === true) requiresApproval = true;
      const retry = defaultRetry(isObject(raw.retry) ? raw.retry : undefined);
      if (isObject(raw.retry)
        && raw.retry.maxAttempts !== undefined
        && (!Number.isInteger(raw.retry.maxAttempts) || Number(raw.retry.maxAttempts) < 1 || Number(raw.retry.maxAttempts) > 5)) {
        validationError(diagnostics, `${path}/retry/maxAttempts`, "retry_unbounded", "maxAttempts must be an integer between 1 and 5.");
      }
      if (raw.kind === "capability") {
        const descriptor = typeof raw.capability === "string" ? catalog.get(raw.capability) : undefined;
        if (!descriptor) validationError(diagnostics, `${path}/capability`, "capability_not_found", "Capability is not present in the local catalog.");
        else if (!descriptorReadOnly(descriptor)) validationError(diagnostics, `${path}/capability`, "recipe_direct_write_forbidden", "Direct Recipe capability steps must be asset/world read-only; use Workflow for writes.");
      }
      if (raw.kind === "sourceControlCheckout" && raw.capability !== undefined && raw.capability !== "production.source_control.checkout") {
        validationError(diagnostics, `${path}/capability`, "source_control_operation_forbidden", "Only production.source_control.checkout is allowed here.");
      }
      if (raw.kind === "workflow") {
        if (!isObject(raw.workflow)) validationError(diagnostics, `${path}/workflow`, "workflow_required", "Workflow steps require an inline Workflow v2 object.");
        else if (raw.workflow.dsl !== "ue.workflow" || raw.workflow.dslVersion !== "2.0") validationError(diagnostics, `${path}/workflow`, "workflow_v2_required", "Recipe writes accept only dsl=ue.workflow and dslVersion=2.0.");
      }
      if (raw.kind === "poll") {
        if (!isObject(raw.poll)) validationError(diagnostics, `${path}/poll`, "poll_invalid", "poll configuration is required.");
        else {
          const descriptor = typeof raw.poll.capability === "string" ? catalog.get(raw.poll.capability) : undefined;
          if (!descriptor || !descriptorReadOnly(descriptor)) validationError(diagnostics, `${path}/poll/capability`, "poll_capability_forbidden", "Poll requires a read-only capability.");
          const attempts = Number(raw.poll.maxAttempts);
          const interval = Number(raw.poll.intervalMs);
          if (!Number.isInteger(attempts) || attempts < 1 || attempts > MAX_POLL_ATTEMPTS) validationError(diagnostics, `${path}/poll/maxAttempts`, "poll_unbounded", `Poll maxAttempts must be 1-${MAX_POLL_ATTEMPTS}.`);
          if (!Number.isInteger(interval) || interval < 100 || interval > MAX_POLL_INTERVAL_MS) validationError(diagnostics, `${path}/poll/intervalMs`, "poll_interval_invalid", `Poll intervalMs must be 100-${MAX_POLL_INTERVAL_MS}.`);
          validateCondition(raw.poll.until, `${path}/poll/until`, diagnostics);
        }
      }
      if (raw.kind === "condition") validateCondition(raw.condition, `${path}/condition`, diagnostics);
      if (raw.compensate !== undefined) {
        if (!isObject(raw.compensate) || !["capability", "workflow"].includes(String(raw.compensate.kind))) validationError(diagnostics, `${path}/compensate`, "compensation_invalid", "Compensation must be a capability or Workflow action.");
        if (isObject(raw.compensate) && raw.compensate.kind === "capability") {
          const descriptor = typeof raw.compensate.capability === "string" ? catalog.get(raw.compensate.capability) : undefined;
          if (!descriptor || !descriptorReadOnly(descriptor)) validationError(diagnostics, `${path}/compensate/capability`, "compensation_write_forbidden", "Write compensation must use Workflow rollback, not a direct capability.");
        }
        if (
          isObject(raw.compensate) &&
          raw.compensate.kind === "workflow" &&
          (typeof raw.compensate.runId !== "string" ||
            !/^\$(inputs|steps)\.[A-Za-z0-9_.-]+$/.test(raw.compensate.runId))
        ) {
          validationError(
            diagnostics,
            `${path}/compensate/runId`,
            "workflow_compensation_run_id_required",
            "Workflow compensation must reference a persisted Workflow runId from inputs or a prior step output.",
          );
        }
      }
      for (const forbidden of ["script", "command", "shell", "python", "javascript", "loop", "while"]) {
        if (forbidden in raw) validationError(diagnostics, `${path}/${forbidden}`, "recipe_script_forbidden", "Arbitrary scripts and unbounded loops are forbidden.");
      }
    });
    const firstWorkflow = value.steps.findIndex((step) => isObject(step) && step.kind === "workflow");
    const firstCheckout = value.steps.findIndex((step) => isObject(step) && step.kind === "sourceControlCheckout");
    if (firstWorkflow >= 0 && (firstCheckout < 0 || firstCheckout > firstWorkflow)) {
      validationError(
        diagnostics,
        `/steps/${firstWorkflow}`,
        "source_control_checkout_required",
        "Every controlled Workflow write requires a preceding sourceControlCheckout step so checkout failure remains zero-write.",
      );
    }
  }
  if (diagnostics.length > 0) return { ok: false, schema: "ue.recipe-validation.v2", diagnostics };
  const normalized = canonical(value) as RecipeDefinition;
  return {
    ok: true,
    schema: "ue.recipe-validation.v2",
    diagnostics,
    normalized,
    planDigest: digest({ schema: RECIPE_PLAN_SCHEMA, recipe: normalized }),
    requiresApproval,
  };
}

function lookup(root: unknown, path: string): unknown {
  let current = root;
  for (const segment of path.split(".")) {
    if (!isObject(current) || !(segment in current)) return undefined;
    current = current[segment];
  }
  return current;
}

function evaluate(condition: unknown, context: JsonObject): boolean {
  if (!isObject(condition)) return false;
  if (Array.isArray(condition.all)) return condition.all.every((item) => evaluate(item, context));
  if (Array.isArray(condition.any)) return condition.any.some((item) => evaluate(item, context));
  if (condition.not !== undefined) return !evaluate(condition.not, context);
  for (const operator of ["equals", "notEquals", "exists"] as const) {
    const expression = condition[operator];
    if (!isObject(expression) || typeof expression.path !== "string") continue;
    const actual = lookup(context, expression.path);
    if (operator === "exists") return actual !== undefined;
    const same = JSON.stringify(canonical(actual)) === JSON.stringify(canonical(expression.value));
    return operator === "equals" ? same : !same;
  }
  return false;
}

function materialize(value: unknown, context: JsonObject): unknown {
  if (Array.isArray(value)) return value.map((item) => materialize(item, context));
  if (isObject(value)) return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, materialize(item, context)]));
  if (typeof value === "string" && /^\$(inputs|steps)\.[A-Za-z0-9_.-]+$/.test(value)) return lookup(context, value.slice(1));
  return value;
}

function errorPayload(error: unknown): { code: string; message: string } {
  return error instanceof UEApiError
    ? { code: error.code, message: error.message }
    : { code: "recipe_step_failed", message: error instanceof Error ? error.message : String(error) };
}

function sleep(milliseconds: number): Promise<void> {
  return new Promise((resolvePromise) => setTimeout(resolvePromise, milliseconds));
}

function spawnWorker(
  state: RecipeRunState,
  endpoint: string,
  env: NodeJS.ProcessEnv,
): void {
  const script = fileURLToPath(import.meta.url).replace(/\.ts$/, ".js");
  const workerInstanceId = randomUUID();
  const timestamp = now();
  state.workerInstanceId = workerInstanceId;
  state.workerPid = undefined;
  state.workerHeartbeatAt = timestamp;
  state.workerLeaseExpiresAt = leaseExpiry(env);
  updateRun(state, "Recipe Worker lease is being created.", env);
  try {
    const child = fork(
      script,
      [
        "--worker",
        state.runId,
        "--worker-instance",
        workerInstanceId,
        "--endpoint",
        endpoint,
      ],
      {
        detached: true,
        stdio: "ignore",
        env,
      },
    );
    state.workerPid = child.pid;
    state.workerHeartbeatAt = now();
    state.workerLeaseExpiresAt = leaseExpiry(env);
    updateRun(state, "Recipe Worker started and acquired its heartbeat lease.", env);
    child.unref();
  } catch (error) {
    state.status = "interrupted";
    state.phase = "interrupted";
    state.workerLeaseExpiresAt = now();
    updateRun(
      state,
      `Recipe Worker could not start: ${error instanceof Error ? error.message : String(error)}`,
      env,
    );
    throw error;
  }
}

export class RecipeRunner {
  private readonly catalog: CapabilityCatalog;
  private readonly env: NodeJS.ProcessEnv;
  private readonly endpoint: string;

  constructor(options: { catalog?: CapabilityCatalog; env?: NodeJS.ProcessEnv; endpoint?: string } = {}) {
    this.catalog = options.catalog ?? loadCapabilityCatalog();
    this.env = options.env ?? process.env;
    this.endpoint = options.endpoint ?? `http://127.0.0.1:${this.env.UE_PORT ?? "9847"}`;
  }

  validate(recipe: unknown): RecipeValidation {
    return validateRecipe(recipe, this.catalog);
  }

  plan(recipe: unknown): JsonObject {
    const validation = this.validate(recipe);
    if (!validation.ok) throw new UEApiError({ code: "recipe_invalid", message: "Recipe validation failed.", details: validation.diagnostics });
    return {
      schema: RECIPE_PLAN_SCHEMA,
      recipeId: validation.normalized?.id,
      recipeVersion: validation.normalized?.version,
      planDigest: validation.planDigest,
      requiresApproval: validation.requiresApproval,
      stepCount: validation.normalized?.steps.length,
      allowedStepKinds: ["capability", "workflow", "poll", "condition", "approval", "sourceControlCheckout"],
      maximumAttempts: 5,
      arbitraryScripts: false,
    };
  }

  start(recipe: unknown, inputs: JsonObject = {}, approvePlanDigest?: string): RecipeRunState {
    const validation = this.validate(recipe);
    if (!validation.ok || !validation.normalized || !validation.planDigest) throw new UEApiError({ code: "recipe_invalid", message: "Recipe validation failed.", details: validation.diagnostics });
    const approvalMissing = validation.requiresApproval && approvePlanDigest !== validation.planDigest;
    const timestamp = now();
    const runId = `recipe-run-${randomUUID()}`;
    const state: RecipeRunState = {
      schema: RECIPE_RUN_SCHEMA,
      runId,
      recipeId: validation.normalized.id,
      recipeVersion: validation.normalized.version,
      planDigest: validation.planDigest,
      status: approvalMissing ? "awaitingApproval" : "running",
      phase: approvalMissing ? "approval" : "starting",
      createdAt: timestamp,
      updatedAt: timestamp,
      heartbeatAt: timestamp,
      lastProgressAt: timestamp,
      lastLog: approvalMissing ? "Recipe plan is awaiting explicit approval." : "Recipe Runner process is starting.",
      nextStep: 0,
      inputs,
      recipe: validation.normalized,
      steps: validation.normalized.steps.map((step) => ({ id: step.id, status: "pending", attempts: 0 })),
      cancelRequested: false,
      cancelPending: false,
      approvedSteps: [],
      compensations: [],
      ...(approvalMissing ? { awaitingApproval: { stepId: "$plan", reason: "recipePlan", planDigest: validation.planDigest } } : {}),
    };
    updateRun(state, undefined, this.env);
    if (!approvalMissing) spawnWorker(state, this.endpoint, this.env);
    return state;
  }

  status(runId: string): RecipeRunState {
    return readRun(runId, this.env);
  }

  resume(
    runId: string,
    approvePlanDigest: string,
    approveStepDigest?: string,
  ): RecipeRunState {
    const state = readRun(runId, this.env);
    if (state.status === "running") {
      throw new UEApiError({
        code: "recipe_run_active",
        message: "Recipe run still has an active Worker lease.",
        details: {
          workerPid: state.workerPid,
          workerInstanceId: state.workerInstanceId,
          workerLeaseExpiresAt: state.workerLeaseExpiresAt,
        },
      });
    }
    if (state.status !== "awaitingApproval" && state.status !== "interrupted") {
      throw new UEApiError({ code: "recipe_not_resumable", message: "Recipe run is neither interrupted nor awaiting approval." });
    }
    if (approvePlanDigest !== state.planDigest) {
      throw new UEApiError({ code: "plan_digest_mismatch", message: "approvePlanDigest does not match the persisted Recipe plan." });
    }
    const approvedStepId = state.awaitingApproval?.stepId;
    if (approvedStepId && approvedStepId !== "$plan" && !state.approvedSteps.includes(approvedStepId)) {
      if (approveStepDigest !== state.awaitingApproval?.planDigest) {
        throw new UEApiError({
          code: "approval_mismatch",
          message: "approveStepDigest does not match the pending Workflow or high-risk step digest.",
        });
      }
      state.approvedSteps.push(approvedStepId);
    }
    state.status = "running";
    state.phase = "resuming";
    state.awaitingApproval = undefined;
    updateRun(state, "Recipe approval accepted; resuming from the persisted checkpoint.", this.env);
    spawnWorker(state, this.endpoint, this.env);
    return state;
  }

  cancel(runId: string): RecipeRunState {
    const state = readRun(runId, this.env);
    if (["completed", "failed", "cancelled"].includes(state.status)) return state;
    state.cancelRequested = true;
    state.cancelPending = state.status === "running";
    updateRun(state, "Cancellation requested; the Runner will stop at the next safe step boundary.", this.env);
    return state;
  }

  result(runId: string): RecipeRunState {
    const state = readRun(runId, this.env);
    if (!["completed", "failed", "cancelled"].includes(state.status)) throw new UEApiError({ code: "recipe_result_not_ready", message: "Recipe result is available only after a terminal state.", details: { status: state.status, phase: state.phase } });
    return state;
  }

  list(limit = 50): RecipeRunState[] {
    const root = recipeRoot(this.env);
    if (!existsSync(root)) return [];
    return readdirSync(root, { withFileTypes: true })
      .filter((entry) => entry.isDirectory() && entry.name.startsWith("recipe-run-"))
      .slice(0, Math.min(100, Math.max(1, limit)))
      .flatMap((entry) => {
        try { return [readRun(entry.name, this.env)]; } catch { return []; }
      });
  }
}

async function invokeStep(
  client: UEClient,
  step: RecipeStep,
  context: JsonObject,
  state: RecipeRunState,
): Promise<unknown> {
  if (step.kind === "condition") {
    if (!evaluate(step.condition, context)) throw new UEApiError({ code: "recipe_condition_failed", message: `Condition step ${step.id} evaluated to false.` });
    return { matched: true };
  }
  if (step.kind === "workflow" && !state.approvedSteps.includes(step.id)) {
    const planned = await client.workflow({
      action: "plan",
      workflow: materialize(step.workflow, context) as JsonObject,
      details: true,
    });
    const workflowPlanDigest = planned.planDigest;
    if (typeof workflowPlanDigest !== "string" || !/^sha256:[a-f0-9]{64}$/.test(workflowPlanDigest)) {
      throw new UEApiError({ code: "workflow_plan_digest_missing", message: `Workflow plan for ${step.id} did not return a valid planDigest.` });
    }
    state.status = "awaitingApproval";
    state.phase = "approval";
    state.awaitingApproval = { stepId: step.id, reason: "controlledWriteStep", planDigest: workflowPlanDigest };
    state.steps[state.nextStep]!.approvalDigest = workflowPlanDigest;
    state.steps[state.nextStep]!.workflowReceipt = planned;
    updateRun(state, `Workflow step ${step.id} is awaiting approval of its state-bound planDigest.`);
    return { awaitingApproval: true, planDigest: workflowPlanDigest };
  }
  if ((step.kind === "approval" || step.highRisk === true) && !state.approvedSteps.includes(step.id)) {
    const approvalDigest = digest({ runId: state.runId, stepId: step.id, planDigest: state.planDigest, kind: step.kind });
    state.status = "awaitingApproval";
    state.phase = "approval";
    state.awaitingApproval = {
      stepId: step.id,
      reason: step.kind === "approval" ? "explicitApprovalStep" : step.kind === "workflow" ? "controlledWriteStep" : "highRiskStep",
      planDigest: approvalDigest,
    };
    state.steps[state.nextStep]!.approvalDigest = approvalDigest;
    updateRun(state, `Step ${step.id} is awaiting explicit approval.`);
    return { awaitingApproval: true, planDigest: approvalDigest };
  }
  if (step.kind === "approval") {
    return { approved: true, approvalDigest: state.steps[state.nextStep]?.approvalDigest };
  }
  if (step.kind === "workflow") {
    const approvedDigest = state.steps[state.nextStep]?.approvalDigest;
    if (typeof approvedDigest !== "string") throw new UEApiError({ code: "approval_required", message: `Workflow step ${step.id} has no approved planDigest.` });
    const request: UEWorkflowRequest = {
      action: "execute",
      workflow: materialize(step.workflow, context) as JsonObject,
      approvePlanDigest: approvedDigest,
      confirmWrite: true,
      requestId: `${state.runId}-${step.id}`,
    };
    const result = await client.workflow(request);
    if (
      process.env.UEAI_RECIPE_TEST_MODE === "1" &&
      process.env.UEAI_RECIPE_TEST_EXIT_AFTER_WORKFLOW_ACK === step.id
    ) {
      process.exit(87);
    }
    if (
      result.idempotentReplay === true &&
      typeof result.runId === "string" &&
      result.status === "running"
    ) {
      return client.workflow({ action: "resume", runId: result.runId });
    }
    if (
      result.idempotentReplay === true &&
      typeof result.status === "string" &&
      ["failed", "cancelled", "rollbackFailed"].includes(result.status)
    ) {
      throw new UEApiError({
        code: "workflow_terminal_failure",
        message: `Workflow request ${request.requestId} already reached terminal state ${result.status}; it will not be replayed.`,
        details: result,
      });
    }
    return result;
  }
  if (step.kind === "sourceControlCheckout") {
    return client.execute(
      "production.source_control.checkout",
      { ...(materialize(step.params ?? {}, context) as JsonObject), confirmWrite: true },
      `${state.runId}-${step.id}`,
    );
  }
  if (step.kind === "capability") {
    return client.execute(step.capability!, materialize(step.params ?? {}, context) as JsonObject, `${state.runId}-${step.id}`);
  }
  const poll = step.poll!;
  let output: unknown;
  for (let attempt = 1; attempt <= poll.maxAttempts; attempt += 1) {
    const latest = readRun(state.runId);
    if (latest.cancelRequested) throw new UEApiError({ code: "recipe_cancelled", message: "Recipe cancellation reached a poll boundary." });
    output = await client.execute(poll.capability, materialize(poll.params ?? {}, context) as JsonObject, `${state.runId}-${step.id}-poll-${attempt}`);
    const pollContext = { ...context, poll: { output } };
    if (evaluate(poll.until, pollContext)) return { attempts: attempt, output };
    if (attempt < poll.maxAttempts) await sleep(poll.intervalMs);
  }
  throw new UEApiError({ code: "recipe_poll_timeout", message: `Poll step ${step.id} exhausted its bounded attempts.`, details: { output } });
}

async function compensate(
  client: UEClient,
  state: RecipeRunState,
  completed: RecipeStep[],
  context: JsonObject,
): Promise<void> {
  for (const step of completed.reverse()) {
    if (!step.compensate) continue;
    try {
      const output = step.compensate.kind === "workflow"
        ? await client.workflow({
            action: "rollback",
            runId: String(materialize(step.compensate.runId, context)),
            approvePlanDigest: step.compensate.approvePlanDigest,
          })
        : await client.execute(step.compensate.capability!, materialize(step.compensate.params ?? {}, context) as JsonObject, `${state.runId}-${step.id}-compensate`);
      state.compensations.push({ stepId: step.id, ok: true, output });
      const found = state.steps.find((item) => item.id === step.id);
      if (found) found.status = "compensated";
    } catch (error) {
      state.compensations.push({ stepId: step.id, ok: false, error: errorPayload(error) });
    }
    updateRun(state, `Compensation processed for ${step.id}.`);
  }
}

export async function executeRecipeRun(
  runId: string,
  endpoint?: string,
  workerInstanceId?: string,
): Promise<void> {
  const state = readRun(runId);
  if (state.status !== "running") return;
  if (
    typeof workerInstanceId !== "string" ||
    workerInstanceId.length === 0 ||
    state.workerInstanceId !== workerInstanceId
  ) {
    throw new UEApiError({
      code: "recipe_worker_lease_mismatch",
      message: "Recipe Worker does not own the persisted Worker lease.",
    });
  }
  state.workerPid = process.pid;
  state.workerHeartbeatAt = now();
  state.workerLeaseExpiresAt = leaseExpiry();
  updateRun(state);
  const heartbeatTimer = setInterval(() => {
    try {
      const latest = readRun(runId);
      if (
        latest.status !== "running" ||
        latest.workerInstanceId !== workerInstanceId
      ) {
        return;
      }
      latest.workerPid = process.pid;
      latest.workerHeartbeatAt = now();
      latest.workerLeaseExpiresAt = leaseExpiry();
      updateRun(latest);
    } catch {
      // A corrupt or removed journal cannot be safely renewed.
    }
  }, WORKER_HEARTBEAT_MS);
  heartbeatTimer.unref();
  const client = new UEClient({ baseUrl: endpoint });
  await client.startSession({ name: "ue-recipe-runner", version: "1.0.0" });
  const completed = state.recipe.steps.slice(0, state.nextStep).filter((_, index) => state.steps[index]?.status === "completed");
  const stepOutputs = Object.fromEntries(state.steps.filter((step) => step.output !== undefined).map((step) => [step.id, { output: step.output }]));
  const context: JsonObject = { inputs: state.inputs, steps: stepOutputs };
  try {
    for (; state.nextStep < state.recipe.steps.length;) {
      const latest = readRun(runId);
      if (latest.cancelRequested) {
        state.status = "cancelled";
        state.phase = "cancelled";
        state.cancelRequested = true;
        state.cancelPending = false;
        updateRun(state, "Recipe cancelled at a safe step boundary.");
        return;
      }
      const step = state.recipe.steps[state.nextStep]!;
      const stepState = state.steps[state.nextStep]!;
      stepState.status = "dispatching";
      stepState.startedAt = now();
      state.phase = `step:${step.id}`;
      updateRun(state, `Starting ${step.kind} step ${step.id}.`);
      const retry = defaultRetry(step.retry);
      let output: unknown;
      let succeeded = false;
      for (let attempt = 1; attempt <= retry.maxAttempts; attempt += 1) {
        stepState.attempts = attempt;
        updateRun(state, `Executing ${step.id}, attempt ${attempt}/${retry.maxAttempts}.`);
        try {
          output = await invokeStep(client, step, context, state);
          if ((state.status as RunStatus) === "awaitingApproval") return;
          succeeded = true;
          break;
        } catch (error) {
          const payload = errorPayload(error);
          stepState.error = payload;
          const retryable = !NEVER_RETRY.has(payload.code) && retry.transientErrors.includes(payload.code);
          if (!retryable || attempt >= retry.maxAttempts) throw error;
          await sleep(retry.backoffMs * 2 ** (attempt - 1));
        }
      }
      if (!succeeded) throw new UEApiError({ code: "recipe_step_failed", message: `Step ${step.id} failed.` });
      stepState.status = "completed";
      stepState.output = output;
      stepState.completedAt = now();
      if (step.kind === "workflow") stepState.workflowReceipt = output;
      (context.steps as JsonObject)[step.id] = { output };
      completed.push(step);
      state.nextStep += 1;
      updateRun(state, `Completed step ${step.id}.`);
      if (
        process.env.UEAI_RECIPE_TEST_MODE === "1" &&
        process.env.UEAI_RECIPE_TEST_EXIT_AFTER_STEP === step.id
      ) {
        process.exit(86);
      }
    }
    state.status = "completed";
    state.phase = "completed";
    state.result = { completedSteps: state.steps.length, outputs: context.steps, compensations: state.compensations };
    updateRun(state, "Recipe completed and persisted its final checkpoint.");
  } catch (error) {
    const payload = errorPayload(error);
    const current = state.steps[state.nextStep];
    if (current) { current.status = "failed"; current.error = payload; current.completedAt = now(); }
    state.status = payload.code === "recipe_cancelled" ? "cancelled" : "failed";
    state.phase = state.status;
    state.error = payload;
    updateRun(state, payload.message);
    if (state.status === "failed") await compensate(client, state, completed, context);
  } finally {
    clearInterval(heartbeatTimer);
    try {
      const latest = readRun(runId);
      if (latest.workerInstanceId === workerInstanceId) {
        latest.workerPid = undefined;
        latest.workerLeaseExpiresAt = now();
        updateRun(latest);
      }
    } catch {
      // Terminal cleanup must not replace the primary Runner result.
    }
    await client.stopSession();
  }
}

if (process.argv[2] === "--worker") {
  const runId = process.argv[3];
  const workerInstanceIndex = process.argv.indexOf("--worker-instance");
  const workerInstanceId = workerInstanceIndex >= 0 ? process.argv[workerInstanceIndex + 1] : undefined;
  const endpointIndex = process.argv.indexOf("--endpoint");
  const endpoint = endpointIndex >= 0 ? process.argv[endpointIndex + 1] : undefined;
  if (runId) {
    void executeRecipeRun(runId, endpoint, workerInstanceId).catch((error) => {
      try {
        const state = readRun(runId);
        if (state.workerInstanceId === workerInstanceId) {
          state.status = "interrupted";
          state.phase = "interrupted";
          state.error = errorPayload(error);
          state.workerPid = undefined;
          state.workerLeaseExpiresAt = now();
          updateRun(state, state.error.message);
        }
      } catch {
        // A missing/corrupt journal is already terminal and cannot be repaired.
      }
      process.exitCode = 1;
    });
  }
}
