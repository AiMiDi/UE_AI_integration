import { createHash } from "node:crypto";
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
function schemaProperties(schema) {
    return isRecord(schema.properties) ? schema.properties : {};
}
function schemaDigest(schema) {
    const canonical = (value) => {
        if (Array.isArray(value))
            return value.map(canonical);
        if (!isRecord(value))
            return value;
        return Object.fromEntries(Object.keys(value).sort().map((key) => [key, canonical(value[key])]));
    };
    return `sha256:${createHash("sha256").update(JSON.stringify(canonical(schema))).digest("hex")}`;
}
function requiredFields(schema) {
    return Array.isArray(schema.required)
        ? schema.required.filter((field) => typeof field === "string")
        : [];
}
function schemaRisk(capability) {
    const dsl = capability.dsl;
    return typeof dsl?.risk === "string" ? dsl.risk : undefined;
}
function schemaSample(schema, path, unresolved, includeOptional) {
    if ("default" in schema)
        return schema.default;
    if ("const" in schema)
        return schema.const;
    const type = typeof schema.type === "string" ? schema.type : undefined;
    if (type === "object" || isRecord(schema.properties)) {
        const result = {};
        const properties = schemaProperties(schema);
        const required = new Set(requiredFields(schema));
        for (const [key, child] of Object.entries(properties)) {
            if (!isRecord(child))
                continue;
            if (!includeOptional && !required.has(key) && !("default" in child) && !("const" in child))
                continue;
            const childPath = path ? `${path}.${key}` : key;
            const value = schemaSample(child, childPath, unresolved, includeOptional);
            if (value !== undefined)
                result[key] = value;
            else if (required.has(key))
                unresolved.push(childPath);
        }
        return result;
    }
    if (type === "array") {
        const minimum = typeof schema.minItems === "number" ? schema.minItems : 0;
        const itemSchema = isRecord(schema.items) ? schema.items : {};
        const result = [];
        for (let index = 0; index < minimum; index += 1) {
            const value = schemaSample(itemSchema, `${path}[${index}]`, unresolved, includeOptional);
            if (value === undefined)
                return undefined;
            result.push(value);
        }
        return result;
    }
    return undefined;
}
function effectsWrite(capability) {
    return Object.values(capability.effects).some((effect) => effect === "write");
}
function metadata(capability) {
    const schema = capability.inputSchema;
    const properties = schemaProperties(schema);
    const required = requiredFields(schema).filter((field) => field !== "requestId");
    const optional = Object.keys(properties).filter((field) => field !== "requestId" && !required.includes(field));
    const approvalFields = ["confirmWrite", "approvePlanDigest"].filter((field) => field in properties || required.includes(field));
    const approvalRequired = schemaRisk(capability) === "confirmWrite" ||
        approvalFields.some((field) => required.includes(field));
    const persistenceFields = [
        "save",
        "saveOnSuccess",
        "onlyIfDirty",
        "saveAsset",
        "savePackage",
    ].filter((field) => field in properties);
    const request = {
        acceptsRequestId: "requestId" in properties,
        generatedByCli: "requestId" in properties,
    };
    const approval = {
        required: approvalRequired,
        fields: approvalFields,
        guidance: approvalRequired
            ? "Review the plan and set the declared approval fields explicitly; this local check never grants approval."
            : "No manifest-declared approval field is required.",
    };
    const persistence = {
        fields: persistenceFields,
        guidance: persistenceFields.length > 0
            ? "Choose the save policy explicitly and verify the resulting package/readback after execution."
            : "The manifest does not declare a save policy; use the capability-specific receipt/readback contract.",
    };
    const retry = {
        safeToRetry: !effectsWrite(capability),
        guidance: effectsWrite(capability)
            ? "Do not retry an unknown write outcome; recover or read back the request and asset first."
            : "The declared effects are read-only; retry is safe after correcting transport failures.",
    };
    return { schema, properties, required, optional, approval, request, persistence, retry };
}
export function createParameterTemplate(capability) {
    const info = metadata(capability);
    const unresolved = [];
    const params = schemaSample(info.schema, "", unresolved, false);
    return {
        capability: capability.id,
        schemaSource: "local-manifest",
        schemaDigest: schemaDigest(info.schema),
        params,
        required: info.required,
        optional: info.optional,
        unresolved,
        approval: info.approval,
        request: info.request,
        persistence: info.persistence,
        retry: info.retry,
    };
}
function actualType(value) {
    if (value === null)
        return "null";
    if (Array.isArray(value))
        return "array";
    if (typeof value === "number" && Number.isInteger(value))
        return "integer";
    return typeof value;
}
function validateSchema(schema, value, path, errors, collect = true) {
    const start = errors.length;
    const issue = (code, message, expected) => {
        errors.push({ path, code, message, ...(expected === undefined ? {} : { expected }), actual: value });
    };
    if ("const" in schema && value !== schema.const) {
        issue("const", "Value must equal the schema const.", schema.const);
    }
    if (Array.isArray(schema.enum) && !schema.enum.some((candidate) => Object.is(candidate, value))) {
        issue("enum", "Value is not one of the declared enum values.", schema.enum);
    }
    const type = typeof schema.type === "string" ? schema.type : undefined;
    if (type === "object" || isRecord(schema.properties) || Array.isArray(schema.required)) {
        if (!isRecord(value))
            issue("type", "Value must be an object.", "object");
        else {
            const properties = schemaProperties(schema);
            for (const field of requiredFields(schema)) {
                if (!(field in value)) {
                    errors.push({ path: `${path}.${field}`.replace(/^\./, ""), code: "required", message: `Missing required parameter "${field}".` });
                }
            }
            if (schema.additionalProperties === false) {
                for (const field of Object.keys(value)) {
                    if (!(field in properties)) {
                        errors.push({ path: path ? `${path}.${field}` : field, code: "unknown", message: `Unknown parameter "${field}".` });
                    }
                }
            }
            for (const [field, child] of Object.entries(properties)) {
                if (field in value && isRecord(child))
                    validateSchema(child, value[field], path ? `${path}.${field}` : field, errors);
            }
        }
    }
    else if (type === "array") {
        if (!Array.isArray(value))
            issue("type", "Value must be an array.", "array");
        else {
            if (typeof schema.minItems === "number" && value.length < schema.minItems)
                issue("minItems", `Array must contain at least ${schema.minItems} item(s).`, schema.minItems);
            if (typeof schema.maxItems === "number" && value.length > schema.maxItems)
                issue("maxItems", `Array must contain at most ${schema.maxItems} item(s).`, schema.maxItems);
            if (schema.uniqueItems === true && new Set(value.map((item) => JSON.stringify(item))).size !== value.length)
                issue("uniqueItems", "Array items must be unique.");
            if (isRecord(schema.items))
                value.forEach((item, index) => validateSchema(schema.items, item, `${path}[${index}]`, errors));
        }
    }
    else if (type === "string") {
        if (typeof value !== "string")
            issue("type", "Value must be a string.", "string");
        else {
            if (typeof schema.minLength === "number" && value.length < schema.minLength)
                issue("minLength", `String must contain at least ${schema.minLength} character(s).`, schema.minLength);
            if (typeof schema.maxLength === "number" && value.length > schema.maxLength)
                issue("maxLength", `String must contain at most ${schema.maxLength} character(s).`, schema.maxLength);
            if (typeof schema.pattern === "string" && !(new RegExp(schema.pattern).test(value)))
                issue("pattern", "String does not match the declared pattern.", schema.pattern);
        }
    }
    else if (type === "integer") {
        if (typeof value !== "number" || !Number.isInteger(value))
            issue("type", "Value must be an integer.", "integer");
        else {
            if (typeof schema.minimum === "number" && value < schema.minimum)
                issue("minimum", "Value is below the declared minimum.", schema.minimum);
            if (typeof schema.maximum === "number" && value > schema.maximum)
                issue("maximum", "Value exceeds the declared maximum.", schema.maximum);
        }
    }
    else if (type === "number") {
        if (typeof value !== "number" || !Number.isFinite(value))
            issue("type", "Value must be a number.", "number");
        else {
            if (typeof schema.minimum === "number" && value < schema.minimum)
                issue("minimum", "Value is below the declared minimum.", schema.minimum);
            if (typeof schema.maximum === "number" && value > schema.maximum)
                issue("maximum", "Value exceeds the declared maximum.", schema.maximum);
        }
    }
    for (const [keyword, expectedCount] of [["anyOf", 1], ["oneOf", 1]]) {
        const branches = schema[keyword];
        if (!Array.isArray(branches))
            continue;
        const matches = branches.filter((branch) => {
            if (!isRecord(branch))
                return false;
            const branchErrors = [];
            validateSchema(branch, value, path, branchErrors, false);
            return branchErrors.length === 0;
        }).length;
        if ((keyword === "anyOf" && matches < expectedCount) || (keyword === "oneOf" && matches !== expectedCount)) {
            issue(keyword, `Value must satisfy exactly the declared ${keyword} alternative(s).`, expectedCount);
        }
    }
    if (Array.isArray(schema.allOf)) {
        for (const branch of schema.allOf) {
            if (isRecord(branch))
                validateSchema(branch, value, path, errors);
        }
    }
    if (isRecord(schema.not)) {
        const branchErrors = [];
        validateSchema(schema.not, value, path, branchErrors, false);
        if (branchErrors.length === 0)
            issue("not", "Value matches a prohibited schema.");
    }
    return errors.length === start || !collect;
}
export function preflightParameters(capability, params) {
    const info = metadata(capability);
    const errors = [];
    if (!isRecord(params)) {
        errors.push({ path: "", code: "type", message: "Params must be one JSON object.", expected: "object", actual: params });
    }
    else {
        validateSchema(info.schema, params, "", errors);
    }
    const warnings = [];
    if (info.approval.required && isRecord(params)) {
        const missingApproval = info.approval.fields.length === 0
            ? ["approval"]
            : info.approval.fields.filter((field) => field === "confirmWrite"
                ? params[field] !== true
                : typeof params[field] !== "string" || String(params[field]).length === 0);
        if (missingApproval.length > 0) {
            warnings.push({ path: missingApproval.join(","), code: "approval_required", message: "Explicit approval is required before execution.", expected: true, actual: params });
        }
    }
    return {
        capability: capability.id,
        schemaSource: "local-manifest",
        schemaDigest: schemaDigest(info.schema),
        valid: errors.length === 0,
        safeToProceed: errors.length === 0 && warnings.length === 0,
        errors,
        warnings,
        approval: info.approval,
        request: info.request,
        persistence: info.persistence,
        retry: info.retry,
    };
}
//# sourceMappingURL=capability-params.js.map