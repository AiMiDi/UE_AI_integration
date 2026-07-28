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
] as const;

export type CapabilityDomain = (typeof CAPABILITY_DOMAINS)[number];
export type CapabilityKind = "query" | "command" | "validation";
export type CapabilityOutputKind = "json" | "image";
export type CapabilityDslAdmission =
  | "editStep"
  | "finalizer"
  | "observeOnly"
  | "interactiveOnly"
  | "none";
export type CapabilityDslRisk =
  | "readOnly"
  | "safeWrite"
  | "confirmWrite"
  | "notOpen";

export interface CapabilityRequirements {
  plugins?: string[];
  modules?: string[];
  platforms?: string[];
  engine?: {
    min?: string;
    maxExclusive?: string;
  };
}

export interface CapabilityInputSchema {
  type: "object";
  properties: Record<string, unknown>;
  additionalProperties: false;
  required?: string[];
  [key: string]: unknown;
}

export interface CapabilityDslMetadata {
  admission: CapabilityDslAdmission;
  scopeKinds: string[];
  transactionDomain: string;
  deferCompile: boolean;
  risk: CapabilityDslRisk;
}

export interface CapabilityDescriptor {
  id: string;
  domain: CapabilityDomain;
  kind: CapabilityKind;
  description: string;
  inputSchema: CapabilityInputSchema;
  traits: {
    readOnly: boolean;
    destructive: boolean;
    expensive: boolean;
  };
  output: {
    kind: CapabilityOutputKind;
  };
  dsl?: CapabilityDslMetadata;
  requires?: CapabilityRequirements;
}

export interface CapabilityManifest {
  schemaVersion: 2;
  domain: CapabilityDomain;
  capabilities: CapabilityDescriptor[];
}

export interface CapabilityCatalogSummary {
  schemaVersion: 2;
  capabilityCount: number;
  domainCounts: Record<CapabilityDomain, number>;
}

export const DEFAULT_MANIFEST_DIR = fileURLToPath(
  new URL("../../Resources/Capabilities/", import.meta.url),
);

export class CapabilityManifestError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CapabilityManifestError";
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function requireRecord(
  value: unknown,
  location: string,
): Record<string, unknown> {
  if (!isRecord(value)) {
    throw new CapabilityManifestError(`${location} must be an object`);
  }
  return value;
}

function requireBoolean(
  record: Record<string, unknown>,
  key: string,
  location: string,
): boolean {
  if (typeof record[key] !== "boolean") {
    throw new CapabilityManifestError(`${location}.${key} must be a boolean`);
  }
  return record[key];
}

function requireString(
  record: Record<string, unknown>,
  key: string,
  location: string,
): string {
  const value = record[key];
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new CapabilityManifestError(
      `${location}.${key} must be a non-empty string`,
    );
  }
  return value;
}

function parseDslMetadata(
  value: unknown,
  location: string,
): CapabilityDslMetadata | undefined {
  if (value === undefined) {
    return undefined;
  }

  const dsl = requireRecord(value, location);
  const admissions: readonly CapabilityDslAdmission[] = [
    "editStep",
    "finalizer",
    "observeOnly",
    "interactiveOnly",
    "none",
  ];
  if (
    typeof dsl.admission !== "string" ||
    !admissions.includes(dsl.admission as CapabilityDslAdmission)
  ) {
    throw new CapabilityManifestError(
      `${location}.admission must be editStep, finalizer, observeOnly, interactiveOnly, or none`,
    );
  }

  if (
    !Array.isArray(dsl.scopeKinds) ||
    dsl.scopeKinds.some(
      (scopeKind) =>
        typeof scopeKind !== "string" || scopeKind.trim().length === 0,
    )
  ) {
    throw new CapabilityManifestError(
      `${location}.scopeKinds must be an array of non-empty strings`,
    );
  }

  const risks: readonly CapabilityDslRisk[] = [
    "readOnly",
    "safeWrite",
    "confirmWrite",
    "notOpen",
  ];
  if (
    typeof dsl.risk !== "string" ||
    !risks.includes(dsl.risk as CapabilityDslRisk)
  ) {
    throw new CapabilityManifestError(
      `${location}.risk must be readOnly, safeWrite, confirmWrite, or notOpen`,
    );
  }

  return {
    admission: dsl.admission as CapabilityDslAdmission,
    scopeKinds: [...dsl.scopeKinds] as string[],
    transactionDomain: requireString(
      dsl,
      "transactionDomain",
      location,
    ),
    deferCompile: requireBoolean(dsl, "deferCompile", location),
    risk: dsl.risk as CapabilityDslRisk,
  };
}

function parseStringArray(
  value: unknown,
  location: string,
): string[] | undefined {
  if (value === undefined) {
    return undefined;
  }
  if (
    !Array.isArray(value) ||
    value.some(
      (item) => typeof item !== "string" || item.trim().length === 0,
    )
  ) {
    throw new CapabilityManifestError(
      `${location} must be an array of non-empty strings`,
    );
  }
  return [...new Set(value as string[])];
}

function parseRequirements(
  value: unknown,
  location: string,
): CapabilityRequirements | undefined {
  if (value === undefined) {
    return undefined;
  }

  const requirements = requireRecord(value, location);
  const allowedFields = new Set([
    "plugins",
    "modules",
    "platforms",
    "engine",
  ]);
  for (const field of Object.keys(requirements)) {
    if (!allowedFields.has(field)) {
      throw new CapabilityManifestError(
        `${location}.${field} is not a supported requirement`,
      );
    }
  }

  let engine: CapabilityRequirements["engine"];
  if (requirements.engine !== undefined) {
    const engineRecord = requireRecord(
      requirements.engine,
      `${location}.engine`,
    );
    for (const field of Object.keys(engineRecord)) {
      if (field !== "min" && field !== "maxExclusive") {
        throw new CapabilityManifestError(
          `${location}.engine.${field} is not supported`,
        );
      }
    }
    const min =
      engineRecord.min === undefined
        ? undefined
        : requireString(engineRecord, "min", `${location}.engine`);
    const maxExclusive =
      engineRecord.maxExclusive === undefined
        ? undefined
        : requireString(
            engineRecord,
            "maxExclusive",
            `${location}.engine`,
          );
    engine = {
      ...(min === undefined ? {} : { min }),
      ...(maxExclusive === undefined ? {} : { maxExclusive }),
    };
  }

  const plugins = parseStringArray(
    requirements.plugins,
    `${location}.plugins`,
  );
  const modules = parseStringArray(
    requirements.modules,
    `${location}.modules`,
  );
  const platforms = parseStringArray(
    requirements.platforms,
    `${location}.platforms`,
  );

  return {
    ...(plugins === undefined ? {} : { plugins }),
    ...(modules === undefined ? {} : { modules }),
    ...(platforms === undefined ? {} : { platforms }),
    ...(engine === undefined ? {} : { engine }),
  };
}

function parseCapability(
  value: unknown,
  domain: CapabilityDomain,
  index: number,
  manifestPath: string,
): CapabilityDescriptor {
  const location = `${manifestPath}.capabilities[${index}]`;
  const capability = requireRecord(value, location);

  if (
    typeof capability.id !== "string" ||
    !/^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+$/.test(capability.id)
  ) {
    throw new CapabilityManifestError(
      `${location}.id must be a lowercase dotted capability ID`,
    );
  }
  if (!capability.id.startsWith(`${domain}.`)) {
    throw new CapabilityManifestError(
      `${location}.id "${capability.id}" must belong to domain "${domain}"`,
    );
  }
  if (capability.domain !== domain) {
    throw new CapabilityManifestError(
      `${location}.domain must equal manifest domain "${domain}"`,
    );
  }
  if (
    capability.kind !== "query" &&
    capability.kind !== "command" &&
    capability.kind !== "validation"
  ) {
    throw new CapabilityManifestError(
      `${location}.kind must be query, command, or validation`,
    );
  }
  if (
    typeof capability.description !== "string" ||
    capability.description.trim().length === 0
  ) {
    throw new CapabilityManifestError(
      `${location}.description must be a non-empty string`,
    );
  }

  const inputSchema = requireRecord(
    capability.inputSchema,
    `${location}.inputSchema`,
  );
  if (inputSchema.type !== "object") {
    throw new CapabilityManifestError(
      `${location}.inputSchema.type must equal "object"`,
    );
  }
  requireRecord(inputSchema.properties, `${location}.inputSchema.properties`);
  if (inputSchema.additionalProperties !== false) {
    throw new CapabilityManifestError(
      `${location}.inputSchema.additionalProperties must equal false`,
    );
  }
  if (
    inputSchema.required !== undefined &&
    (!Array.isArray(inputSchema.required) ||
      inputSchema.required.some((item) => typeof item !== "string"))
  ) {
    throw new CapabilityManifestError(
      `${location}.inputSchema.required must be an array of strings`,
    );
  }

  const traits = requireRecord(capability.traits, `${location}.traits`);
  const output = requireRecord(capability.output, `${location}.output`);
  if (output.kind !== "json" && output.kind !== "image") {
    throw new CapabilityManifestError(
      `${location}.output.kind must be json or image`,
    );
  }

  return {
    id: capability.id,
    domain,
    kind: capability.kind,
    description: capability.description,
    inputSchema: inputSchema as CapabilityInputSchema,
    traits: {
      readOnly: requireBoolean(traits, "readOnly", `${location}.traits`),
      destructive: requireBoolean(
        traits,
        "destructive",
        `${location}.traits`,
      ),
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
    ...(capability.requires === undefined
      ? {}
      : {
          requires: parseRequirements(
            capability.requires,
            `${location}.requires`,
          ),
        }),
  };
}

function parseManifest(
  value: unknown,
  expectedDomain: CapabilityDomain,
  manifestPath: string,
): CapabilityManifest {
  const manifest = requireRecord(value, manifestPath);
  if (manifest.schemaVersion !== 2) {
    throw new CapabilityManifestError(
      `${manifestPath}.schemaVersion must equal 2`,
    );
  }
  if (manifest.domain !== expectedDomain) {
    throw new CapabilityManifestError(
      `${manifestPath}.domain must equal "${expectedDomain}"`,
    );
  }
  if (!Array.isArray(manifest.capabilities)) {
    throw new CapabilityManifestError(
      `${manifestPath}.capabilities must be an array`,
    );
  }

  return {
    schemaVersion: 2,
    domain: expectedDomain,
    capabilities: manifest.capabilities.map((capability, index) =>
      parseCapability(capability, expectedDomain, index, manifestPath),
    ),
  };
}

export class CapabilityCatalog {
  readonly manifests: ReadonlyMap<CapabilityDomain, CapabilityManifest>;
  readonly capabilities: readonly CapabilityDescriptor[];

  private readonly byId: ReadonlyMap<string, CapabilityDescriptor>;
  private readonly byDomain: ReadonlyMap<
    CapabilityDomain,
    readonly CapabilityDescriptor[]
  >;

  constructor(manifests: CapabilityManifest[]) {
    const manifestMap = new Map<CapabilityDomain, CapabilityManifest>();
    const idMap = new Map<string, CapabilityDescriptor>();
    const domainMap = new Map<
      CapabilityDomain,
      readonly CapabilityDescriptor[]
    >();
    const allCapabilities: CapabilityDescriptor[] = [];

    for (const domain of CAPABILITY_DOMAINS) {
      const manifest = manifests.find((candidate) => candidate.domain === domain);
      if (!manifest) {
        throw new CapabilityManifestError(
          `Missing capability manifest for domain "${domain}"`,
        );
      }
      if (manifestMap.has(domain)) {
        throw new CapabilityManifestError(
          `Duplicate capability manifest for domain "${domain}"`,
        );
      }
      manifestMap.set(domain, manifest);
      domainMap.set(domain, manifest.capabilities);

      for (const capability of manifest.capabilities) {
        if (idMap.has(capability.id)) {
          throw new CapabilityManifestError(
            `Duplicate capability ID "${capability.id}"`,
          );
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

  get(id: string): CapabilityDescriptor | undefined {
    return this.byId.get(id);
  }

  forDomain(domain: CapabilityDomain): readonly CapabilityDescriptor[] {
    return this.byDomain.get(domain) ?? [];
  }

  summary(): CapabilityCatalogSummary {
    const domainCounts = Object.fromEntries(
      CAPABILITY_DOMAINS.map((domain) => [
        domain,
        this.forDomain(domain).length,
      ]),
    ) as Record<CapabilityDomain, number>;

    return {
      schemaVersion: 2,
      capabilityCount: this.capabilities.length,
      domainCounts,
    };
  }
}

export function loadCapabilityCatalog(
  manifestDir = DEFAULT_MANIFEST_DIR,
): CapabilityCatalog {
  const manifests = CAPABILITY_DOMAINS.map((domain) => {
    const manifestPath = resolve(manifestDir, `${domain}.json`);
    let raw: string;
    try {
      raw = readFileSync(manifestPath, "utf8");
    } catch (error) {
      throw new CapabilityManifestError(
        `Missing capability manifest "${manifestPath}": ${(error as Error).message}`,
      );
    }

    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch (error) {
      throw new CapabilityManifestError(
        `Invalid JSON in capability manifest "${manifestPath}": ${(error as Error).message}`,
      );
    }
    return parseManifest(parsed, domain, manifestPath);
  });

  return new CapabilityCatalog(manifests);
}
