import { z } from "zod";
import { formatErrorResponse, formatJsonResponse, } from "./helpers.js";
import { UEApiError, UE_WORKFLOW_ACTIONS, } from "./ue-bridge.js";
const nonEmptyString = z
    .string()
    .min(1)
    .refine((value) => value.trim().length > 0, "Must not be blank");
export const UE_WORKFLOW_DETAIL_LEVELS = [
    "summary",
    "standard",
    "full",
];
export const UE_WORKFLOW_SECTIONS = [
    "operations",
    "finalizers",
    "readBack",
    "assetDiff",
    "structures",
    "rollback",
    "diagnostics",
];
export const UE_WORKFLOW_TOOL_SCHEMA = z
    .object({
    action: z.enum(UE_WORKFLOW_ACTIONS),
    workflow: z
        .record(z.unknown())
        .optional()
        .describe("Inline UE Workflow AST; local file paths are not accepted"),
    approvePlanDigest: nonEmptyString.optional(),
    runId: nonEmptyString.optional(),
    saveOnSuccess: z.boolean().optional(),
    confirmWrite: z.boolean().optional(),
    details: z.boolean().optional(),
    detailLevel: z.enum(UE_WORKFLOW_DETAIL_LEVELS).optional(),
    sections: z.array(z.enum(UE_WORKFLOW_SECTIONS)).optional(),
})
    .strict();
export const UE_WORKFLOW_INPUT_SCHEMA = UE_WORKFLOW_TOOL_SCHEMA
    .strict()
    .superRefine((request, context) => {
    if (request.details !== undefined &&
        request.detailLevel !== undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["detailLevel"],
            message: "details and detailLevel cannot be used together",
        });
    }
    if ((request.action === "validate" ||
        request.action === "plan" ||
        request.action === "execute") &&
        request.workflow === undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["workflow"],
            message: `workflow is required for action "${request.action}"`,
        });
    }
    if (request.action === "execute" &&
        request.approvePlanDigest === undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["approvePlanDigest"],
            message: 'approvePlanDigest is required for action "execute"',
        });
    }
    if (request.action === "rollback" &&
        request.approvePlanDigest === undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["approvePlanDigest"],
            message: 'approvePlanDigest is required for action "rollback"',
        });
    }
    if ((request.action === "resume" ||
        request.action === "status" ||
        request.action === "rollback") &&
        request.runId === undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["runId"],
            message: `runId is required for action "${request.action}"`,
        });
    }
    if ((request.action === "resume" ||
        request.action === "status" ||
        request.action === "rollback") &&
        request.workflow !== undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["workflow"],
            message: `workflow is not accepted for action "${request.action}"`,
        });
    }
    if (request.action !== "execute" &&
        request.action !== "rollback" &&
        request.approvePlanDigest !== undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["approvePlanDigest"],
            message: "approvePlanDigest is only accepted for execute and rollback",
        });
    }
    if (request.action !== "execute" &&
        (request.saveOnSuccess !== undefined ||
            request.confirmWrite !== undefined)) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            message: "saveOnSuccess and confirmWrite are only accepted for execute",
        });
    }
    if (request.action !== "resume" &&
        request.action !== "status" &&
        request.action !== "rollback" &&
        request.runId !== undefined) {
        context.addIssue({
            code: z.ZodIssueCode.custom,
            path: ["runId"],
            message: 'runId is only accepted for actions "resume", "status", and "rollback"',
        });
    }
});
export async function runWorkflowAction(executor, request) {
    try {
        return formatJsonResponse(await executor.workflow(request));
    }
    catch (error) {
        return formatErrorResponse(error);
    }
}
export async function handleWorkflowInput(executor, value) {
    try {
        return await runWorkflowAction(executor, parseWorkflowInput(value));
    }
    catch (error) {
        return formatErrorResponse(error);
    }
}
export function parseWorkflowInput(value) {
    const result = UE_WORKFLOW_INPUT_SCHEMA.safeParse(value);
    if (result.success) {
        return result.data;
    }
    throw new UEApiError({
        code: "invalid_workflow_request",
        message: "Invalid ue_workflow request",
        details: result.error.flatten(),
    });
}
//# sourceMappingURL=workflow-router.js.map