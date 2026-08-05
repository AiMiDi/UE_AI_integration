#!/usr/bin/env node
import { readFileSync } from "node:fs";
import process from "node:process";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { RecipeRunner } from "./recipe-runner.js";
import { UEApiError } from "./ue-bridge.js";
function parseRaw(arguments_) {
    const result = { json: false };
    for (let index = 0; index < arguments_.length; index += 1) {
        const argument = arguments_[index];
        if (!argument.startsWith("--")) {
            if (result.command === undefined)
                result.command = argument;
            else
                throw new Error(`Unexpected positional argument ${argument}.`);
            continue;
        }
        const [name, inline] = argument.slice(2).split("=", 2);
        if (name === "json") {
            result.json = true;
            continue;
        }
        const value = inline ?? arguments_[++index];
        if (!value || value.startsWith("--"))
            throw new Error(`--${name} requires a value.`);
        if (name === "file")
            result.file = value;
        else if (name === "run-id")
            result.runId = value;
        else if (name === "endpoint")
            result.endpoint = value;
        else if (name === "approve-plan")
            result.approvePlan = value;
        else if (name === "inputs")
            result.inputs = JSON.parse(value);
        else if (name === "inputs-file")
            result.inputs = JSON.parse(readFileSync(resolve(value), "utf8"));
        else
            throw new Error(`Unknown option --${name}.`);
    }
    return result;
}
function parseArguments() {
    const argsFileIndex = process.argv.indexOf("--args-file");
    if (argsFileIndex >= 0) {
        const path = process.argv[argsFileIndex + 1];
        if (!path)
            throw new Error("--args-file requires a path.");
        const raw = JSON.parse(readFileSync(path, "utf8"));
        if (!Array.isArray(raw) || !raw.every((item) => typeof item === "string"))
            throw new Error("Recipe CLI args file must contain a string array.");
        return parseRaw(raw);
    }
    return parseRaw(process.argv.slice(2));
}
function loadRecipe(path) {
    if (!path)
        throw new UEApiError({ code: "recipe_file_required", message: "--file is required." });
    return JSON.parse(readFileSync(resolve(path), "utf8"));
}
function envelope(data) {
    return { ok: true, data, meta: { runner: "independent", persistence: "checkpointed" } };
}
function print(value, pretty) {
    process.stdout.write(`${JSON.stringify(value, null, pretty ? 2 : 0)}\n`);
}
export async function main() {
    try {
        const args = parseArguments();
        if (!args.command || !["validate", "plan", "start", "status", "resume", "cancel", "result"].includes(args.command)) {
            throw new UEApiError({ code: "recipe_command_invalid", message: "Use recipe validate|plan|start|status|resume|cancel|result." });
        }
        const runner = new RecipeRunner({ endpoint: args.endpoint });
        let data;
        if (args.command === "validate")
            data = runner.validate(loadRecipe(args.file));
        else if (args.command === "plan")
            data = runner.plan(loadRecipe(args.file));
        else if (args.command === "start")
            data = runner.start(loadRecipe(args.file), args.inputs ?? {}, args.approvePlan);
        else {
            if (!args.runId)
                throw new UEApiError({ code: "recipe_run_id_required", message: "--run-id is required." });
            if (args.command === "status")
                data = runner.status(args.runId);
            else if (args.command === "resume") {
                if (!args.approvePlan)
                    throw new UEApiError({ code: "approval_required", message: "resume requires --approve-plan." });
                data = runner.resume(args.runId, args.approvePlan);
            }
            else if (args.command === "cancel")
                data = runner.cancel(args.runId);
            else
                data = runner.result(args.runId);
        }
        print(envelope(data), !args.json);
        return 0;
    }
    catch (error) {
        const payload = error instanceof UEApiError
            ? { code: error.code, message: error.message, details: error.details }
            : { code: "recipe_cli_failed", message: error instanceof Error ? error.message : String(error) };
        print({ ok: false, error: payload }, false);
        return ["recipe_invalid", "recipe_command_invalid", "recipe_file_required"].includes(payload.code) ? 2 : payload.code.includes("approval") || payload.code.includes("digest") ? 3 : 5;
    }
}
const isEntrypoint = process.argv[1] !== undefined && import.meta.url === pathToFileURL(resolve(process.argv[1])).href;
if (isEntrypoint) {
    void main().then((code) => { process.exitCode = code; });
}
//# sourceMappingURL=recipe-cli.js.map