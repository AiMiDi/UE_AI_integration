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
function parseStringArray(value, location) {
    if (value === undefined) {
        return undefined;
    }
    if (!Array.isArray(value) ||
        value.some((item) => typeof item !== "string" || item.trim().length === 0)) {
        throw new CapabilityManifestError(`${location} must be an array of non-empty strings`);
    }
    return [...new Set(value)];
}
export function capabilityIsReadOnly(capability) {
    return capability.effects.asset !== "write"
        && capability.effects.world !== "write"
        && capability.effects.external !== "write";
}
function parseSearchMetadata(value, location) {
    if (value === undefined) {
        return undefined;
    }
    const search = requireRecord(value, location);
    const allowedFields = new Set(["title", "keywords", "aliases"]);
    for (const field of Object.keys(search)) {
        if (!allowedFields.has(field)) {
            throw new CapabilityManifestError(`${location}.${field} is not supported`);
        }
    }
    const title = search.title === undefined
        ? undefined
        : requireString(search, "title", location);
    const parseUniqueArray = (field) => {
        const values = search[field];
        if (values === undefined) {
            return undefined;
        }
        if (!Array.isArray(values) ||
            values.length === 0 ||
            values.some((item) => typeof item !== "string" || item.trim().length === 0)) {
            throw new CapabilityManifestError(`${location}.${field} must be a non-empty array of non-empty strings`);
        }
        const normalized = values.map((item) => item.trim().toLowerCase());
        if (new Set(normalized).size !== normalized.length) {
            throw new CapabilityManifestError(`${location}.${field} must not contain duplicates`);
        }
        return [...values];
    };
    const keywords = parseUniqueArray("keywords");
    const aliases = parseUniqueArray("aliases");
    if (title === undefined && keywords === undefined && aliases === undefined) {
        throw new CapabilityManifestError(`${location} must declare title, keywords, or aliases`);
    }
    return {
        ...(title === undefined ? {} : { title }),
        ...(keywords === undefined ? {} : { keywords }),
        ...(aliases === undefined ? {} : { aliases }),
    };
}
function parseRequirements(value, location) {
    if (value === undefined) {
        return undefined;
    }
    const requirements = requireRecord(value, location);
    const allowedFields = new Set([
        "features",
        "plugins",
        "modules",
        "platforms",
        "engine",
    ]);
    for (const field of Object.keys(requirements)) {
        if (!allowedFields.has(field)) {
            throw new CapabilityManifestError(`${location}.${field} is not a supported requirement`);
        }
    }
    let engine;
    if (requirements.engine !== undefined) {
        const engineRecord = requireRecord(requirements.engine, `${location}.engine`);
        for (const field of Object.keys(engineRecord)) {
            if (field !== "min" && field !== "maxExclusive") {
                throw new CapabilityManifestError(`${location}.engine.${field} is not supported`);
            }
        }
        const min = engineRecord.min === undefined
            ? undefined
            : requireString(engineRecord, "min", `${location}.engine`);
        const maxExclusive = engineRecord.maxExclusive === undefined
            ? undefined
            : requireString(engineRecord, "maxExclusive", `${location}.engine`);
        engine = {
            ...(min === undefined ? {} : { min }),
            ...(maxExclusive === undefined ? {} : { maxExclusive }),
        };
    }
    const features = parseStringArray(requirements.features, `${location}.features`);
    const plugins = parseStringArray(requirements.plugins, `${location}.plugins`);
    const modules = parseStringArray(requirements.modules, `${location}.modules`);
    const platforms = parseStringArray(requirements.platforms, `${location}.platforms`);
    return {
        ...(features === undefined ? {} : { features }),
        ...(plugins === undefined ? {} : { plugins }),
        ...(modules === undefined ? {} : { modules }),
        ...(platforms === undefined ? {} : { platforms }),
        ...(engine === undefined ? {} : { engine }),
    };
}
function parseExecutionMetadata(value, location) {
    if (value === undefined) {
        return undefined;
    }
    const execution = requireRecord(value, location);
    for (const field of Object.keys(execution)) {
        if (field !== "backends" && field !== "preferred") {
            throw new CapabilityManifestError(`${location}.${field} is not supported`);
        }
    }
    const allowed = [
        "editor",
        "localTrace",
        "localRecipe",
        "localProject",
        "localAsset",
        "localSal",
        "developmentRuntime",
    ];
    if (!Array.isArray(execution.backends) ||
        execution.backends.length === 0 ||
        execution.backends.some((backend) => typeof backend !== "string" ||
            !allowed.includes(backend))) {
        throw new CapabilityManifestError(`${location}.backends contains an unsupported execution backend`);
    }
    const backends = [
        ...new Set(execution.backends),
    ];
    if (typeof execution.preferred !== "string" ||
        !backends.includes(execution.preferred)) {
        throw new CapabilityManifestError(`${location}.preferred must be one of the declared backends`);
    }
    return {
        backends,
        preferred: execution.preferred,
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
    if ("readOnly" in traits) {
        throw new CapabilityManifestError(`${location}.traits.readOnly was removed in schema v3; declare effects instead`);
    }
    const effects = requireRecord(capability.effects, `${location}.effects`);
    const parseEffect = (key) => {
        const effect = effects[key];
        if (effect !== "none" && effect !== "read" && effect !== "write") {
            throw new CapabilityManifestError(`${location}.effects.${key} must be none, read, or write`);
        }
        return effect;
    };
    const lifecycle = requireRecord(capability.lifecycle, `${location}.lifecycle`);
    if (lifecycle.status !== "active" && lifecycle.status !== "deprecated") {
        throw new CapabilityManifestError(`${location}.lifecycle.status must be active or deprecated`);
    }
    const since = requireString(lifecycle, "since", `${location}.lifecycle`);
    const canonicalId = requireString(lifecycle, "canonicalId", `${location}.lifecycle`);
    if (!/^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+$/.test(canonicalId)) {
        throw new CapabilityManifestError(`${location}.lifecycle.canonicalId must be a dotted capability ID`);
    }
    const replacement = lifecycle.replacement === undefined
        ? undefined
        : requireString(lifecycle, "replacement", `${location}.lifecycle`);
    if (lifecycle.status === "deprecated" && replacement === undefined) {
        throw new CapabilityManifestError(`${location}.lifecycle.replacement is required for deprecated capabilities`);
    }
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
            destructive: requireBoolean(traits, "destructive", `${location}.traits`),
            expensive: requireBoolean(traits, "expensive", `${location}.traits`),
            ...(traits.sessionSafe === undefined
                ? {}
                : {
                    sessionSafe: requireBoolean(traits, "sessionSafe", `${location}.traits`),
                }),
        },
        effects: {
            asset: parseEffect("asset"),
            world: parseEffect("world"),
            editorSession: parseEffect("editorSession"),
            external: parseEffect("external"),
        },
        lifecycle: {
            status: lifecycle.status,
            since,
            canonicalId,
            ...(replacement === undefined ? {} : { replacement }),
        },
        output: {
            kind: output.kind,
        },
        ...(capability.search === undefined
            ? {}
            : {
                search: parseSearchMetadata(capability.search, `${location}.search`),
            }),
        ...(capability.dsl === undefined
            ? {}
            : {
                dsl: parseDslMetadata(capability.dsl, `${location}.dsl`),
            }),
        ...(capability.requires === undefined
            ? {}
            : {
                requires: parseRequirements(capability.requires, `${location}.requires`),
            }),
        ...(capability.execution === undefined
            ? {}
            : {
                execution: parseExecutionMetadata(capability.execution, `${location}.execution`),
            }),
    };
}
function parseManifest(value, expectedDomain, manifestPath) {
    const manifest = requireRecord(value, manifestPath);
    if (manifest.schemaVersion !== 3) {
        throw new CapabilityManifestError(`${manifestPath}.schemaVersion must equal 3`);
    }
    if (manifest.domain !== expectedDomain) {
        throw new CapabilityManifestError(`${manifestPath}.domain must equal "${expectedDomain}"`);
    }
    if (!Array.isArray(manifest.capabilities)) {
        throw new CapabilityManifestError(`${manifestPath}.capabilities must be an array`);
    }
    const tombstones = manifest.tombstones === undefined ? [] : manifest.tombstones;
    if (!Array.isArray(tombstones)) {
        throw new CapabilityManifestError(`${manifestPath}.tombstones must be an array`);
    }
    return {
        schemaVersion: 3,
        domain: expectedDomain,
        capabilities: manifest.capabilities.map((capability, index) => parseCapability(capability, expectedDomain, index, manifestPath)),
        tombstones: tombstones.map((value, index) => {
            const location = `${manifestPath}.tombstones[${index}]`;
            const entry = requireRecord(value, location);
            return {
                id: requireString(entry, "id", location),
                removedIn: requireString(entry, "removedIn", location),
                replacement: requireString(entry, "replacement", location),
            };
        }),
    };
}
export class CapabilityCatalog {
    manifests;
    capabilities;
    tombstones;
    byId;
    byDomain;
    constructor(manifests) {
        const manifestMap = new Map();
        const idMap = new Map();
        const domainMap = new Map();
        const allCapabilities = [];
        const tombstones = new Map();
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
            for (const tombstone of manifest.tombstones) {
                if (idMap.has(tombstone.id) || tombstones.has(tombstone.id)) {
                    throw new CapabilityManifestError(`Duplicate active or removed capability ID "${tombstone.id}"`);
                }
                tombstones.set(tombstone.id, tombstone);
            }
        }
        this.manifests = manifestMap;
        this.capabilities = allCapabilities;
        this.byId = idMap;
        this.byDomain = domainMap;
        this.tombstones = tombstones;
    }
    get(id) {
        return this.byId.get(id);
    }
    removed(id) {
        return this.tombstones.get(id);
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
            schemaVersion: 3,
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