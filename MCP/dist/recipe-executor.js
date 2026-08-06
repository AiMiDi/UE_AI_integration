import { RecipeRunner, recipeHasSessionSteps } from "./recipe-runner.js";
import { UEApiError } from "./ue-bridge.js";
export class RecipeRunnerExecutor {
    runner;
    constructor(runner = new RecipeRunner()) {
        this.runner = runner;
    }
    async execute(id, params = {}, _requestId, context) {
        if (context?.signal?.aborted) {
            throw new UEApiError({
                code: "request_cancelled",
                message: "The MCP client cancelled the Recipe Runner request.",
            });
        }
        if (id === "production.recipe.validate") {
            return this.runner.validate(params.recipe);
        }
        if (id === "production.recipe.plan") {
            const inputs = (params.inputs ?? {});
            return recipeHasSessionSteps(params.recipe)
                ? await this.runner.planOnline(params.recipe, inputs)
                : this.runner.plan(params.recipe, inputs);
        }
        if (id === "production.recipe.start") {
            const inputs = (params.inputs ?? {});
            const approval = typeof params.approvePlanDigest === "string"
                ? params.approvePlanDigest
                : undefined;
            return (recipeHasSessionSteps(params.recipe)
                ? await this.runner.startOnline(params.recipe, inputs, approval)
                : this.runner.start(params.recipe, inputs, approval));
        }
        const runId = params.runId;
        if (typeof runId !== "string" || runId.length === 0) {
            throw new UEApiError({
                code: "recipe_run_id_required",
                message: `${id} requires runId.`,
            });
        }
        if (id === "production.recipe.status") {
            return this.runner.status(runId);
        }
        if (id === "production.recipe.resume") {
            if (typeof params.approvePlanDigest !== "string") {
                throw new UEApiError({
                    code: "approval_required",
                    message: "Recipe resume requires approvePlanDigest.",
                });
            }
            return this.runner.resume(runId, params.approvePlanDigest, typeof params.approveStepDigest === "string"
                ? params.approveStepDigest
                : undefined);
        }
        if (id === "production.recipe.cancel") {
            return this.runner.cancel(runId);
        }
        if (id === "production.recipe.result") {
            return this.runner.result(runId);
        }
        throw new UEApiError({
            code: "capability_not_found",
            message: `Recipe Runner does not own ${id}.`,
        });
    }
}
//# sourceMappingURL=recipe-executor.js.map