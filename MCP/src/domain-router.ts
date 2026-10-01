import type {
  CapabilityCatalog,
  CapabilityDescriptor,
  CapabilityDomain,
} from "./capability-catalog.js";
import {
  formatCapabilityResponse,
  formatErrorResponse,
  type MCPResponse,
} from "./helpers.js";
import {
  attachCapabilityVerification,
  verificationErrorDetails,
} from "./capability-verification.js";
import { UEApiError, type UEExecuteData } from "./ue-bridge.js";

export const DOMAIN_TOOL_NAMES: Record<CapabilityDomain, string> = {
  blueprint: "ue_blueprint",
  scene: "ue_scene",
  content: "ue_content",
  animation: "ue_animation",
  ai: "ue_ai",
  production: "ue_production",
};

export const DOMAIN_DESCRIPTIONS: Record<CapabilityDomain, string> = {
  blueprint:
    "Execute Blueprint inspection, authoring, mutation, and validation capabilities.",
  scene:
    "Execute level, actor, component, world-building, viewport, and navigation capabilities.",
  content:
    "Execute asset, material, DataTable, Niagara, UI, and content-management capabilities.",
  animation:
    "Execute animation asset, AnimBlueprint, state-machine, and BlendSpace capabilities.",
  ai: "Execute Behavior Tree, Blackboard, and related AI authoring capabilities.",
  production:
    "Execute Sequencer, build, packaging, diagnostics, and production workflow capabilities.",
};

export interface CapabilityExecutor {
  execute(
    id: string,
    params?: Record<string, unknown>,
    requestId?: string,
    context?: CapabilityExecutionContext,
  ): Promise<UEExecuteData>;
}

interface VerificationAwareExecutor extends CapabilityExecutor {
  readonly verificationAware: true;
}

function isVerificationAwareExecutor(
  executor: CapabilityExecutor,
): executor is VerificationAwareExecutor {
  return (executor as Partial<VerificationAwareExecutor>).verificationAware === true;
}

export interface CapabilityExecutionContext {
  signal?: AbortSignal;
}

const LOCAL_TRACE_ID_PREFIXES = [
  "trace-local-",
  "trace-analysis-local-",
  "trace-launch-local-",
] as const;

export function isLocalTraceOwnedId(value: unknown): value is string {
  return (
    typeof value === "string" &&
    LOCAL_TRACE_ID_PREFIXES.some((prefix) => value.startsWith(prefix))
  );
}

function idBoundBackend(
  capability: string,
  params: Record<string, unknown>,
): "editor" | "localTrace" | undefined {
  const id = params.traceId ?? params.jobId ?? params.analysisId;
  if (typeof id !== "string" || id.length === 0) return undefined;
  if (
    capability.startsWith("production.trace.") ||
    capability.startsWith("production.job.")
  ) {
    return isLocalTraceOwnedId(id) ? "localTrace" : "editor";
  }
  return undefined;
}

function targetBoundBackend(
  capability: string,
  params: Record<string, unknown>,
): "editor" | "localTrace" | undefined {
  if (capability === "production.trace.start") {
    const target = params.target;
    if (typeof target === "object" && target !== null) {
      const kind = (target as Record<string, unknown>).kind;
      if (kind === "development") return "localTrace";
      if (kind === "editor" || kind === "pie") return "editor";
    }
    // The backward-compatible target-less start records the current Editor.
    return "editor";
  }
  if (capability === "production.trace.channel.list") {
    if (params.targetKind === "development") return "localTrace";
    if (params.targetKind === "editor" || params.targetKind === "pie") {
      return "editor";
    }
  }
  return undefined;
}

export class BackendRoutingExecutor implements CapabilityExecutor {
  /** Native/local routes return the shared capability verification state. */
  readonly verificationAware = true;

  constructor(
    private readonly catalog: CapabilityCatalog,
    private readonly editor: CapabilityExecutor,
    private readonly localTrace: CapabilityExecutor,
    private readonly localRecipe?: CapabilityExecutor,
    private readonly localProject?: CapabilityExecutor,
    private readonly localAsset?: CapabilityExecutor,
    private readonly localSal?: CapabilityExecutor,
    private readonly developmentRuntime?: CapabilityExecutor,
  ) {}

  async execute(
    id: string,
    params: Record<string, unknown> = {},
    requestId?: string,
    context?: CapabilityExecutionContext,
  ): Promise<UEExecuteData> {
    const capability = this.catalog.get(id);
    if (id.startsWith("production.recipe.")) {
      if (!this.localRecipe) {
        throw new UEApiError({
          code: "recipe_runner_unavailable",
          message: "The local Recipe Runner backend is unavailable.",
        });
      }
      return this.localRecipe.execute(id, params, requestId, context);
    }
    const execution = capability?.execution;
    const localBackends = new Map<string, CapabilityExecutor | undefined>([
      ["localTrace", this.localTrace],
      ["localRecipe", this.localRecipe],
      ["localProject", this.localProject],
      ["localAsset", this.localAsset],
      ["localSal", this.localSal],
      ["developmentRuntime", this.developmentRuntime],
    ]);
    const requested = params.backend ?? "auto";
    if (
      requested !== "auto" &&
      requested !== "editor" &&
      requested !== "local"
    ) {
      throw new UEApiError({
        code: "invalid_execution_backend",
        message: "backend must be auto, editor, or local.",
        details: { capability: id, backend: requested },
      });
    }
    const forced =
      targetBoundBackend(id, params) ?? idBoundBackend(id, params);
    if (
      forced !== undefined &&
      requested !== "auto" &&
      (requested === "local" ? "localTrace" : requested) !== forced
    ) {
      throw new UEApiError({
        code: "execution_backend_conflict",
        message: `backend "${requested}" conflicts with the target or owning ID for capability "${id}".`,
        details: { capability: id, backend: requested, required: forced },
      });
    }
    const dynamicJobRoute =
      forced !== undefined && id.startsWith("production.job.");
    if (execution === undefined && !dynamicJobRoute && forced === undefined) {
      return this.editor.execute(id, params, requestId, context);
    }
    const declared = execution?.backends ?? ["editor", "localTrace"];
    const supportsEditor = declared.includes("editor");
    const declaredLocal = declared.find((backend) => backend !== "editor" && localBackends.has(backend));
    const supportsLocal = declaredLocal !== undefined;
    if (forced === "editor") {
      if (!supportsEditor) throw this.unsupported(id, "editor", declared);
      return this.editor.execute(id, params, requestId, context);
    }
    if (forced === "localTrace") {
      if (!supportsLocal) throw this.unsupported(id, "local", declared);
      const executor = localBackends.get("localTrace");
      if (!executor) throw this.unsupported(id, "local", declared);
      return executor.execute(id, params, requestId, context);
    }

    if (execution?.preferred !== undefined
      && execution.preferred !== "editor"
      && execution.preferred !== "localTrace") {
      const executor = localBackends.get(execution.preferred);
      if (!executor) throw this.unsupported(id, "local", declared);
      return executor.execute(id, params, requestId, context);
    }
    if (requested === "editor") {
      if (!supportsEditor) {
        throw this.unsupported(id, "editor", declared);
      }
      return this.editor.execute(id, params, requestId, context);
    }
    if (requested === "local") {
      if (!supportsLocal) {
        throw this.unsupported(id, "local", declared);
      }
      return localBackends.get(declaredLocal ?? "")!.execute(id, params, requestId, context);
    }

    if (execution?.preferred === "localTrace" && supportsLocal) {
      try {
        return await this.localTrace.execute(id, params, requestId, context);
      } catch (error) {
        if (
          !supportsEditor ||
          !(error instanceof UEApiError) ||
          error.code !== "trace_worker_unavailable"
        ) {
          throw error;
        }
        return this.editor.execute(id, params, requestId, context);
      }
    }
    if (execution?.preferred === "editor" && supportsEditor) {
      try {
        return await this.editor.execute(id, params, requestId, context);
      } catch (error) {
        if (
          !supportsLocal ||
          !(error instanceof UEApiError) ||
          error.code !== "editor_unreachable"
        ) {
          throw error;
        }
        return this.localTrace.execute(id, params, requestId, context);
      }
    }
    if (supportsEditor) {
      return this.editor.execute(id, params, requestId, context);
    }
    if (supportsLocal) {
      return this.localTrace.execute(id, params, requestId, context);
    }
    throw new UEApiError({
      code: "execution_backend_unavailable",
      message: `Capability "${id}" declares no usable execution backend.`,
    });
  }

  private unsupported(
    capability: string,
    requested: string,
    declared: readonly string[],
  ): UEApiError {
    return new UEApiError({
      code: "execution_backend_unsupported",
      message: `Capability "${capability}" does not support backend "${requested}".`,
      details: { capability, requested, declared },
    });
  }
}

export function validateDomainOperation(
  catalog: CapabilityCatalog,
  domain: CapabilityDomain,
  operation: string,
): CapabilityDescriptor {
  const capability = catalog.get(operation);
  if (!capability) {
    const removed = catalog.removed(operation);
    if (removed) {
      throw new UEApiError({
        code: "capability_removed",
        message: `Capability "${operation}" was removed; use "${removed.replacement}".`,
        details: {
          ...removed,
          nextAction: `Run ue-cli help ${removed.replacement} --json and retry with the canonical ID.`,
          helpCommand: `ue-cli help ${removed.replacement} --json`,
          safeToRetry: true,
        },
      });
    }
    const suggestions = suggestCapabilityIds(catalog, domain, operation);
    throw new UEApiError({
      code: "capability_not_found",
      message:
        suggestions.length === 0
          ? `Unknown capability "${operation}"`
          : `Unknown capability "${operation}". Did you mean ${suggestions
              .map((candidate) => `"${candidate}"`)
              .join(", ")}?`,
      details: {
        requestedDomain: domain,
        suggestions,
        nextAction:
          suggestions.length === 0
            ? `Run ue-cli capabilities --domain ${domain} --json to discover the current ID.`
            : `Run ue-cli help ${suggestions[0]} --json, then retry with the exact canonical ID.`,
        helpCommand:
          suggestions.length === 0
            ? `ue-cli capabilities --domain ${domain} --json`
            : `ue-cli help ${suggestions[0]} --json`,
        safeToRetry: true,
      },
    });
  }
  if (capability.domain !== domain) {
    throw new UEApiError({
      code: "cross_domain_operation",
      message: `Capability "${operation}" belongs to domain "${capability.domain}", not "${domain}"`,
      details: {
        requestedDomain: domain,
        actualDomain: capability.domain,
        nextAction: `Call ${DOMAIN_TOOL_NAMES[capability.domain]} with operation "${capability.id}".`,
        helpCommand: `ue-cli help ${capability.id} --json`,
        safeToRetry: true,
      },
    });
  }
  return capability;
}

function editDistance(left: string, right: string): number {
  const previous = Array.from({ length: right.length + 1 }, (_, index) => index);
  for (let leftIndex = 1; leftIndex <= left.length; leftIndex += 1) {
    let diagonal = previous[0];
    previous[0] = leftIndex;
    for (let rightIndex = 1; rightIndex <= right.length; rightIndex += 1) {
      const above = previous[rightIndex];
      previous[rightIndex] = Math.min(
        previous[rightIndex] + 1,
        previous[rightIndex - 1] + 1,
        diagonal + (left[leftIndex - 1] === right[rightIndex - 1] ? 0 : 1),
      );
      diagonal = above;
    }
  }
  return previous[right.length];
}

function commonPrefixLength(left: string, right: string): number {
  let length = 0;
  while (
    length < left.length &&
    length < right.length &&
    left[length] === right[length]
  ) {
    length += 1;
  }
  return length;
}

function sharedCapabilitySegments(requested: string, candidate: string): number {
  const requestedSegments = requested.split(".");
  const candidateSegments = candidate.split(".");
  return requestedSegments.slice(1).filter(
    (segment, index) =>
      segment.length > 0 && segment === candidateSegments[index + 1],
  ).length;
}

function suggestCapabilityIds(
  catalog: CapabilityCatalog,
  domain: CapabilityDomain,
  operation: string,
): string[] {
  const normalized = operation.trim().toLowerCase();
  if (normalized.length === 0 || normalized.length > 128) return [];

  const scored = catalog
    .forDomain(domain)
    .map((candidate) => {
      const id = candidate.id.toLowerCase();
      const sharedSegments = sharedCapabilitySegments(normalized, id);
      const prefixLength = commonPrefixLength(normalized, id);
      const distance = editDistance(normalized, id);
      return {
        id: candidate.id,
        score:
          sharedSegments * 100 +
          prefixLength * 2 -
          distance,
        qualifies:
          id.startsWith(normalized) ||
          distance <= 3 ||
          prefixLength >= Math.max(4, normalized.length - 2),
      };
    })
    .filter(({ qualifies }) => qualifies)
    .sort((left, right) => right.score - left.score || left.id.localeCompare(right.id));

  return scored.slice(0, 3).map(({ id }) => id);
}

export async function runDomainOperation(
  catalog: CapabilityCatalog,
  executor: CapabilityExecutor,
  domain: CapabilityDomain,
  operation: string,
  params: Record<string, unknown> = {},
  requestId?: string,
  context?: CapabilityExecutionContext,
): Promise<MCPResponse> {
  let capability: CapabilityDescriptor | undefined;
  let handlerReturned = false;
  try {
    capability = validateDomainOperation(catalog, domain, operation);
    const data = await executor.execute(
      capability.id,
      params,
      requestId,
      context,
    );
    handlerReturned = true;
    const responseData = isVerificationAwareExecutor(executor)
      ? attachCapabilityVerification(capability.id, data, {
          localDeclared: true,
          handlerRegistered: true,
          liveAvailable: true,
          executed: true,
        })
      : data;
    return formatCapabilityResponse(capability, responseData);
  } catch (error) {
    const capabilityId = capability?.id ?? operation;
    if (!isVerificationAwareExecutor(executor)) {
      return formatErrorResponse(error);
    }
    if (error instanceof UEApiError) {
      return formatErrorResponse(
        new UEApiError(
          {
            code: error.code,
            message: error.message,
            details: verificationErrorDetails(
              capabilityId,
              error.details,
              {
                localDeclared: capability !== undefined,
                // A transport error does not prove that a UE handler was
                // registered. Preserve any native proof in error.details;
                // otherwise report the conservative false state.
                handlerRegistered: handlerReturned,
                liveAvailable: handlerReturned,
                executed: handlerReturned,
              },
            ),
          },
          error.status,
        ),
      );
    }
    return formatErrorResponse(
      new UEApiError({
        code: "mcp_error",
        message: error instanceof Error ? error.message : String(error),
        details: verificationErrorDetails(capabilityId, undefined, {
          localDeclared: capability !== undefined,
          handlerRegistered: handlerReturned,
          liveAvailable: handlerReturned,
          executed: handlerReturned,
        }),
      }),
    );
  }
}
