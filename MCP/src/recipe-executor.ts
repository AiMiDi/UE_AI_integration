import type {
  CapabilityExecutionContext,
  CapabilityExecutor,
} from "./domain-router.js";
import { RecipeRunner } from "./recipe-runner.js";
import { UEApiError, type UEExecuteData } from "./ue-bridge.js";

export class RecipeRunnerExecutor implements CapabilityExecutor {
  constructor(private readonly runner = new RecipeRunner()) {}

  async execute(
    id: string,
    params: Record<string, unknown> = {},
    _requestId?: string,
    context?: CapabilityExecutionContext,
  ): Promise<UEExecuteData> {
    if (context?.signal?.aborted) {
      throw new UEApiError({
        code: "request_cancelled",
        message: "The MCP client cancelled the Recipe Runner request.",
      });
    }
    if (id === "production.recipe.validate") {
      return this.runner.validate(params.recipe) as unknown as UEExecuteData;
    }
    if (id === "production.recipe.plan") {
      return this.runner.plan(params.recipe);
    }
    if (id === "production.recipe.start") {
      return this.runner.start(
        params.recipe,
        (params.inputs ?? {}) as Record<string, unknown>,
        typeof params.approvePlanDigest === "string"
          ? params.approvePlanDigest
          : undefined,
      ) as unknown as UEExecuteData;
    }
    const runId = params.runId;
    if (typeof runId !== "string" || runId.length === 0) {
      throw new UEApiError({
        code: "recipe_run_id_required",
        message: `${id} requires runId.`,
      });
    }
    if (id === "production.recipe.status") {
      return this.runner.status(runId) as unknown as UEExecuteData;
    }
    if (id === "production.recipe.resume") {
      if (typeof params.approvePlanDigest !== "string") {
        throw new UEApiError({
          code: "approval_required",
          message: "Recipe resume requires approvePlanDigest.",
        });
      }
      return this.runner.resume(
        runId,
        params.approvePlanDigest,
        typeof params.approveStepDigest === "string"
          ? params.approveStepDigest
          : undefined,
      ) as unknown as UEExecuteData;
    }
    if (id === "production.recipe.cancel") {
      return this.runner.cancel(runId) as unknown as UEExecuteData;
    }
    if (id === "production.recipe.result") {
      return this.runner.result(runId) as unknown as UEExecuteData;
    }
    throw new UEApiError({
      code: "capability_not_found",
      message: `Recipe Runner does not own ${id}.`,
    });
  }
}
