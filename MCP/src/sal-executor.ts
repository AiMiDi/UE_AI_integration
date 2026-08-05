import type { CapabilityExecutor } from "./domain-router.js";
import { salLint, salPlan, salStub } from "./sal-runner.js";
import { UEApiError, type UEExecuteData } from "./ue-bridge.js";

export class SalExecutor implements CapabilityExecutor {
  async execute(id: string, params: Record<string, unknown> = {}): Promise<UEExecuteData> {
    if (id === "production.sal.stub") return salStub();
    if (typeof params.source !== "string") throw new UEApiError({ code: "sal_source_required", message: "source is required." });
    if (id === "production.sal.lint") return salLint(params.source);
    if (id === "production.sal.plan") return salPlan(params.source);
    throw new UEApiError({ code: "sal_operation_unsupported", message: `Unsupported SAL capability ${id}.` });
  }
}
