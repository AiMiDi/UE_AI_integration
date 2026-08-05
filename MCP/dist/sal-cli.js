import { readFileSync } from "node:fs";
import { salLint, salPlan, salStub } from "./sal-runner.js";
let cli = process.argv.slice(2);
if (cli[0] === "--args-file" && typeof cli[1] === "string") {
    cli = JSON.parse(readFileSync(cli[1], "utf8"));
}
function args() {
    const output = {};
    for (let i = 1; i < cli.length; i += 1) {
        const item = cli[i];
        if (!item.startsWith("--"))
            continue;
        const next = cli[i + 1];
        if (next && !next.startsWith("--")) {
            output[item.slice(2)] = next;
            i += 1;
        }
        else
            output[item.slice(2)] = true;
    }
    return output;
}
try {
    const command = cli[0];
    const options = args();
    const source = typeof options.file === "string" ? readFileSync(options.file, "utf8") : "";
    const data = command === "stub" ? salStub() : command === "lint" ? salLint(source) : command === "plan" ? salPlan(source) : (() => { throw new Error("Use sal stub|lint|plan."); })();
    process.stdout.write(`${JSON.stringify({ ok: true, data })}\n`);
}
catch (error) {
    const value = error;
    process.stdout.write(`${JSON.stringify({ ok: false, error: { code: value.code ?? "sal_failed", message: value.message ?? String(error), details: value.details } })}\n`);
    process.exitCode = 1;
}
//# sourceMappingURL=sal-cli.js.map