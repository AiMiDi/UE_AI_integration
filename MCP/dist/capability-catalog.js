import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
export const CAPABILITY_DOMAINS = [
    "blueprint",
    "scene",
    "content",
    "animation",
    "ai",
    "production",
];
export const DEFAULT_MANIFEST_DIR = fileURLToPath(new URL("../../Resources/Capabilities/", import.meta.url));
export class CapabilityManifestError extends Error {
    constructor(message) {
        super(message);
        this.name = "CapabilityManifestError";
    }
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
function requireRecord(value, location) {
    if (!isRecord(value)) {
        throw new CapabilityManifestError(`${location} must be an object`);
    }
    return value;
}
function requireBoolean(record, key, location) {
    if (typeof record[key] !== "boolean") {
        throw new CapabilityManifestError(`${location}.${key} must be a boolean`);
    }
    return record[key];
}
function requireString(record, key, location) {
    const value = record[key];
    if (typeof value !== "string" || value.trim().length === 0) {
        throw new CapabilityManifestError(`${location}.${key} must be a non-empty string`);
    }
    return value;
}
function parseDslMetadata(value, location) {
    if (value === undefined) {
        return undefined;
    }
    const dsl = requireRecord(value, location);
    const admissions = [
        "editStep",
        "finalizer",
        "observeOnly",
        "interactiveOnly",
        "none",
    ];
    if (typeof dsl.admission !== "string" ||
        !admissions.includes(dsl.admission)) {
        throw new CapabilityManifestError(`${location}.admission must be editStep, finalizer, observeOnly, interactiveOnly, or none`);
    }
    if (!Array.isArray(dsl.scopeKinds) ||
        dsl.scopeKinds.some((scopeKind) => typeof scopeKind !== "string" || scopeKind.trim().length === 0)) {
        throw new CapabilityManifestError(`${location}.scopeKinds must be an array of non-empty strings`);
    }
    const risks = [
        "readOnly",
        "safeWrite",
        "confirmWrite",
        "notOpen",
    ];
    if (typeof dsl.risk !== "string" ||
        !risks.includes(dsl.risk)) {
        throw new CapabilityManifestError(`${location}.risk must be readOnly, safeWrite, confirmWrite, or notOpen`);
    }
    return {
        admission: dsl.admission,
        scopeKinds: [...dsl.scopeKinds],
        transactionDomain: requireString(dsl, "transactionDomain", location),
        deferCompile: requireBoolean(dsl, "deferCompile", location),
        risk: dsl.risk,
    };
}
function parseCapability(value, domain, index, manifestPath) {
    const location = `${manifestPath}.capabilities[${index}]`;
    const capability = requireRecord(value, location);
    if (typeof capability.id !== "string" ||
        !/^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+$/.test(capability.id)) {
        throw new CapabilityManifestError(`${location}.id must be a lowercase dotted capability ID`);
    }
    if (!capability.id.startsWith(`${domain}.`)) {
        throw new CapabilityManifestError(`${location}.id "${capability.id}" must belong to domain "${domain}"`);
    }
    if (capability.domain !== domain) {
        throw new CapabilityManifestError(`${location}.domain must equal manifest domain "${domain}"`);
    }
    if (capability.kind !== "query" &&
        capability.kind !== "command" &&
        capability.kind !== "validation") {
        throw new CapabilityManifestError(`${location}.kind must be query, command, or validation`);
    }
    if (typeof capability.description !== "string" ||
        capability.description.trim().length === 0) {
        throw new CapabilityManifestError(`${location}.description must be a non-empty string`);
    }
    const inputSchema = requireRecord(capability.inputSchema, `${location}.inputSchema`);
    if (inputSchema.type !== "object") {
        throw new CapabilityManifestError(`${location}.inputSchema.type must equal "object"`);
    }
    requireRecord(inputSchema.properties, `${location}.inputSchema.properties`);
    if (inputSchema.additionalProperties !== false) {
        throw new CapabilityManifestError(`${location}.inputSchema.additionalProperties must equal false`);
    }
    if (inputSchema.required !== undefined &&
        (!Array.isArray(inputSchema.required) ||
            inputSchema.required.some((item) => typeof item !== "string"))) {
        throw new CapabilityManifestError(`${location}.inputSchema.required must be an array of strings`);
    }
    const traits = requireRecord(capability.traits, `${location}.traits`);
    const output = requireRecord(capability.output, `${location}.output`);
    if (output.kind !== "json" && output.kind !== "image") {
        throw new CapabilityManifestError(`${location}.output.kind must be json or image`);
    }
    return {
        id: capability.id,
        domain,
        kind: capability.kind,
        description: capability.description,
        inputSchema: inputSchema,
        traits: {
            readOnly: requireBoolean(traits, "readOnly", `${location}.traits`),
            destructive: requireBoolean(traits, "destructive", `${location}.traits`),
            expensive: requireBoolean(traits, "expensive", `${location}.traits`),
        },
        output: {
            kind: output.kind,
        },
        ...(capability.dsl === undefined
            ? {}
            : {
                dsl: parseDslMetadata(capability.dsl, `${location}.dsl`),
            }),
    };
}
function parseManifest(value, expectedDomain, manifestPath) {
    const manifest = requireRecord(value, manifestPath);
    if (manifest.schemaVersion !== 2) {
        throw new CapabilityManifestError(`${manifestPath}.schemaVersion must equal 2`);
    }
    if (manifest.domain !== expectedDomain) {
        throw new CapabilityManifestError(`${manifestPath}.domain must equal "${expectedDomain}"`);
    }
    if (!Array.isArray(manifest.capabilities)) {
        throw new CapabilityManifestError(`${manifestPath}.capabilities must be an array`);
    }
    return {
        schemaVersion: 2,
        domain: expectedDomain,
        capabilities: manifest.capabilities.map((capability, index) => parseCapability(capability, expectedDomain, index, manifestPath)),
    };
}
export class CapabilityCatalog {
    manifests;
    capabilities;
    byId;
    byDomain;
    constructor(manifests) {
        const manifestMap = new Map();
        const idMap = new Map();
        const domainMap = new Map();
        const allCapabilities = [];
        for (const domain of CAPABILITY_DOMAINS) {
            const manifest = manifests.find((candidate) => candidate.domain === domain);
            if (!manifest) {
                throw new CapabilityManifestError(`Missing capability manifest for domain "${domain}"`);
            }
            if (manifestMap.has(domain)) {
                throw new CapabilityManifestError(`Duplicate capability manifest for domain "${domain}"`);
            }
            manifestMap.set(domain, manifest);
            domainMap.set(domain, manifest.capabilities);
            for (const capability of manifest.capabilities) {
                if (idMap.has(capability.id)) {
                    throw new CapabilityManifestError(`Duplicate capability ID "${capability.id}"`);
                }
                idMap.set(capability.id, capability);
                allCapabilities.push(capability);
            }
        }
        this.manifests = manifestMap;
        this.capabilities = allCapabilities;
        this.byId = idMap;
        this.byDomain = domainMap;
    }
    get(id) {
        return this.byId.get(id);
    }
    forDomain(domain) {
        return this.byDomain.get(domain) ?? [];
    }
    summary() {
        const domainCounts = Object.fromEntries(CAPABILITY_DOMAINS.map((domain) => [
            domain,
            this.forDomain(domain).length,
        ]));
        return {
            schemaVersion: 2,
            capabilityCount: this.capabilities.length,
            domainCounts,
        };
    }
}
export function loadCapabilityCatalog(manifestDir = DEFAULT_MANIFEST_DIR) {
    const manifests = CAPABILITY_DOMAINS.map((domain) => {
        const manifestPath = resolve(manifestDir, `${domain}.json`);
        let raw;
        try {
            raw = readFileSync(manifestPath, "utf8");
        }
        catch (error) {
            throw new CapabilityManifestError(`Missing capability manifest "${manifestPath}": ${error.message}`);
        }
        let parsed;
        try {
            parsed = JSON.parse(raw);
        }
        catch (error) {
            throw new CapabilityManifestError(`Invalid JSON in capability manifest "${manifestPath}": ${error.message}`);
        }
        return parseManifest(parsed, domain, manifestPath);
    });
    return new CapabilityCatalog(manifests);
}
//# sourceMappingURL=capability-catalog.js.map