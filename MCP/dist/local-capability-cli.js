import { readFileSync, statSync, writeFileSync } from "node:fs";
import { loadCapabilityCatalog } from "./capability-catalog.js";
import { DevelopmentBridgeExecutor } from "./development-bridge.js";
import { LocalAssetExecutor, LocalProjectExecutor } from "./project-executor.js";
import { RecipeRunnerExecutor } from "./recipe-executor.js";
import { SalExecutor } from "./sal-executor.js";
import { UEApiError } from "./ue-bridge.js";
import { attachCapabilityVerification, verificationErrorDetails, } from "./capability-verification.js";
const MAX_ARGS_BYTES = 4 * 1024 * 1024;
function argument(name) {
    const index = process.argv.indexOf(name);
    if (index < 0 || index + 1 >= process.argv.length)
        throw new Error(`${name} is required.`);
    return process.argv[index + 1];
}
function executor(backend) {
    if (backend === "localProject")
        return new LocalProjectExecutor();
    if (backend === "localAsset")
        return new LocalAssetExecutor();
    if (backend === "localRecipe")
        return new RecipeRunnerExecutor();
    if (backend === "localSal")
        return new SalExecutor();
    return new DevelopmentBridgeExecutor();
}
async function main() {
    const argsPath = argument("--args-file");
    const resultPath = argument("--result-file");
    if (statSync(argsPath).size > MAX_ARGS_BYTES)
        throw new Error("Local capability args exceed 4 MiB.");
    const input = JSON.parse(readFileSync(argsPath, "utf8"));
    const allowed = new Set(["localProject", "localAsset", "localRecipe", "localSal", "developmentRuntime"]);
    if (typeof input.capability !== "string" || input.capability.length === 0)
        throw new Error("capability is required.");
    if (typeof input.backend !== "string" || !allowed.has(input.backend))
        throw new Error("backend is not a supported local capability backend.");
    if (input.params === null || typeof input.params !== "object" || Array.isArray(input.params))
        throw new Error("params must be an object.");
    const backend = input.backend;
    const descriptor = loadCapabilityCatalog().get(input.capability);
    const backendDeclared = descriptor?.execution?.backends.includes(backend) ?? false;
    const verificationDefaults = {
        localDeclared: descriptor !== undefined,
        handlerRegistered: backendDeclared,
        liveAvailable: backendDeclared,
    };
    try {
        const data = await executor(backend).execute(input.capability, input.params, typeof input.requestId === "string" ? input.requestId : undefined);
        writeFileSync(resultPath, JSON.stringify({
            ok: true,
            data: attachCapabilityVerification(input.capability, data, {
                ...verificationDefaults,
                executed: true,
            }),
            meta: {
                requestId: typeof input.requestId === "string" ? input.requestId : null,
                executionBackend: backend,
            },
        }), "utf8");
    }
    catch (error) {
        const apiError = error instanceof UEApiError ? error : undefined;
        const handlerRejected = apiError?.code === "capability_not_found"
            || apiError?.code.endsWith("_operation_unsupported")
            || apiError?.code === "recipe_run_id_required";
        writeFileSync(resultPath, JSON.stringify({
            ok: false,
            error: {
                code: apiError?.code ?? "local_backend_failed",
                message: error instanceof Error ? error.message : String(error),
                details: verificationErrorDetails(input.capability, apiError?.details, {
                    ...verificationDefaults,
                    handlerRegistered: handlerRejected
                        ? false
                        : verificationDefaults.handlerRegistered,
                    liveAvailable: false,
                    executed: false,
                }),
            },
            meta: {
                requestId: typeof input.requestId === "string" ? input.requestId : null,
                executionBackend: backend,
            },
        }), "utf8");
        process.exitCode = 5;
    }
}
main().catch((error) => {
    try {
        const resultPath = argument("--result-file");
        let capability = "unknown";
        try {
            const argsPath = argument("--args-file");
            const input = JSON.parse(readFileSync(argsPath, "utf8"));
            if (typeof input.capability === "string" && input.capability.length > 0) {
                capability = input.capability;
            }
        }
        catch {
            // Keep the stable unknown capability marker when the argument file
            // itself cannot be decoded.
        }
        writeFileSync(resultPath, JSON.stringify({
            ok: false,
            error: {
                code: "local_backend_adapter_failed",
                message: error instanceof Error ? error.message : String(error),
                details: verificationErrorDetails(capability, undefined, {
                    localDeclared: false,
                    handlerRegistered: false,
                    liveAvailable: false,
                    executed: false,
                }),
            },
        }), "utf8");
    }
    catch {
        process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    }
    process.exitCode = 5;
});
//# sourceMappingURL=local-capability-cli.js.map