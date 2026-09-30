import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const scriptsRoot = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(scriptsRoot, "..");
const validatorPath = path.join(scriptsRoot, "validate_capabilities.mjs");

const result = spawnSync(process.execPath, [validatorPath], {
  cwd: projectRoot,
  encoding: "utf8",
});

if (result.error) {
  console.error(`Could not run capability validator: ${result.error.message}`);
  process.exit(1);
}
if (result.status !== 0) {
  process.stderr.write(result.stderr);
  process.exit(result.status ?? 1);
}

let report;
try {
  report = JSON.parse(result.stdout);
} catch (error) {
  console.error(`Capability validator did not return JSON: ${error.message}`);
  process.exit(1);
}

const audit = report.registrationAudit;
const failures = [];
if (report.ok !== true) failures.push("validator did not report ok=true");
if (!audit || typeof audit !== "object") {
  failures.push("validator did not return registrationAudit");
} else {
  if (audit.staticRegistrations <= 0) {
    failures.push("registrationAudit.staticRegistrations must be positive");
  }
  if (
    !Number.isInteger(audit.unresolvedRegistrations) ||
    audit.unresolvedRegistrations < 0
  ) {
    failures.push(
      "registrationAudit.unresolvedRegistrations must be a non-negative integer"
    );
  }
  if (
    audit.registrationCandidates !==
    audit.staticRegistrations + audit.unresolvedRegistrations
  ) {
    failures.push(
      "registrationAudit.registrationCandidates must include resolved and unresolved registrations"
    );
  }
  if (
    !Number.isInteger(audit.staticallyDeclaredTools) ||
    audit.staticallyDeclaredTools !== audit.staticRegistrations
  ) {
    failures.push(
      "registrationAudit.staticallyDeclaredTools must equal resolved registrations"
    );
  }
  if (audit.unregisteredStaticTools !== 0) {
    failures.push(
      `unregistered static tools: ${audit.unregisteredStaticTools}`
    );
  }
  if (audit.duplicateRegistrationIds.length !== 0) {
    failures.push(
      `duplicate registration IDs: ${audit.duplicateRegistrationIds.join(", ")}`
    );
  }
}

const validatorSource = fs.readFileSync(validatorPath, "utf8");
const helperStart = validatorSource.indexOf("function maskCppComments");
const helperEnd = validatorSource.indexOf("const manifestIds");
const helperErrors = [];
const helperContext = vm.createContext({
  fail: (message) => helperErrors.push(message),
});
vm.runInContext(
  `${validatorSource.slice(helperStart, helperEnd)}
globalThis.collectStaticRegistrations = collectStaticRegistrations;
globalThis.findDuplicateRegistrationIds = findDuplicateRegistrationIds;
globalThis.findUnregisteredStaticTools = findUnregisteredStaticTools;`,
  helperContext
);

function inspectFixture(source) {
  return helperContext.collectStaticRegistrations(source, "fixture.cpp");
}

const toolDeclaration =
  'class FTool_Fixture : public FMCPToolBase { public: FString GetCapabilityId() const override { return TEXT("scene.fixture.test"); } };\n';
const duplicateFixture = inspectFixture(
  `${toolDeclaration}Registry.Register(MakeShared<FTool_Fixture>());\nRegistry.Register(MakeShared<FTool_Fixture>());`
);
const commentedFixture = inspectFixture(
  `${toolDeclaration}Registry.Register(MakeShared<FTool_Fixture>());\n// Registry.Register(MakeShared<FTool_Fixture>());`
);
const conditionalFixture = inspectFixture(
  `${toolDeclaration}#if WITH_EDITOR\nRegistry.Register(MakeShared<FTool_Fixture>());\n#else\nRegistry.Register(MakeShared<FTool_Fixture>());\n#endif`
);
const missingFixture = inspectFixture(toolDeclaration);
const negativeCases = [
  [
    "duplicate registration must fail",
    helperContext.findDuplicateRegistrationIds(duplicateFixture.registrations).length === 1,
  ],
  [
    "commented registration must not count",
    commentedFixture.registrations.length === 1 &&
      helperContext.findDuplicateRegistrationIds(commentedFixture.registrations).length === 0,
  ],
  [
    "mutually exclusive preprocessor branches must not fail",
    conditionalFixture.registrations.length === 2 &&
      helperContext.findDuplicateRegistrationIds(conditionalFixture.registrations).length === 0,
  ],
  [
    "unregistered static tool must be detected",
    helperContext.findUnregisteredStaticTools(
      missingFixture.declared,
      missingFixture.registrations
    ).length === 1,
  ],
];
for (const [name, passed] of negativeCases) {
  if (!passed) failures.push(name);
}
if (helperErrors.length > 0) {
  failures.push(`fixture scanner errors: ${helperErrors.join("; ")}`);
}

if (failures.length > 0) {
  console.error("Capability validator self-check failed:");
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}

console.log(
  JSON.stringify(
    {
      ok: true,
      capabilities: report.capabilities,
      staticRegistrations: audit.staticRegistrations,
      unresolvedRegistrations: audit.unresolvedRegistrations,
      registrationCandidates: audit.registrationCandidates,
      negativeCases: negativeCases.length,
    },
    null,
    2
  )
);
