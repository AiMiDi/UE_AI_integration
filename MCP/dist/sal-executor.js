import { salLint, salPlan, salStub } from "./sal-runner.js";
import { UEApiError } from "./ue-bridge.js";
export class SalExecutor {
    async execute(id, params = {}) {
        if (id === "production.sal.stub")
            return salStub();
        if (typeof params.source !== "string")
            throw new UEApiError({ code: "sal_source_required", message: "source is required." });
        if (id === "production.sal.lint")
            return salLint(params.source);
        if (id === "production.sal.plan")
            return salPlan(params.source);
        throw new UEApiError({ code: "sal_operation_unsupported", message: `Unsupported SAL capability ${id}.` });
    }
}
//# sourceMappingURL=sal-executor.js.map