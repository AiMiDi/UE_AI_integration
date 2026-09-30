import assert from "node:assert/strict";
import { test } from "node:test";

import { loadCapabilityCatalog } from "../capability-catalog.js";
import {
  createParameterTemplate,
  preflightParameters,
} from "../capability-params.js";

const catalog = loadCapabilityCatalog();

test("parameter templates contain defaults and required metadata without inventing approval", () => {
  const capability = catalog.get("blueprint.asset.list");
  assert.ok(capability);
  const template = createParameterTemplate(capability);
  assert.equal(template.capability, capability.id);
  assert.equal(template.params.limit, 50);
  assert.equal(template.params.offset, 0);
  assert.ok(template.optional.includes("filter"));
  assert.deepEqual(template.unresolved, []);

  const writeCapability = catalog.get("content.material.graph.execute_plan");
  assert.ok(writeCapability);
  const writeTemplate = createParameterTemplate(writeCapability);
  assert.equal(writeTemplate.approval.required, true);
  assert.equal("confirmWrite" in writeTemplate.params, false);
  assert.ok(writeTemplate.required.includes("assetPath"));
  assert.ok(writeTemplate.unresolved.includes("assetPath"));
});

test("preflight rejects unknown, missing, type, range, and pattern errors locally", () => {
  const capability = catalog.get("blueprint.asset.list");
  assert.ok(capability);
  const result = preflightParameters(capability, {
    typo: true,
    limit: 0,
  });
  assert.equal(result.valid, false);
  assert.equal(result.safeToProceed, false);
  assert.ok(result.errors.some((error) => error.code === "unknown"));
  assert.ok(result.errors.some((error) => error.code === "minimum"));

  const digestCapability = catalog.get("content.asset.change.execute");
  assert.ok(digestCapability);
  const digestResult = preflightParameters(digestCapability, {
    request: { actions: [] },
    approvePlanDigest: "not-a-digest",
  });
  assert.equal(digestResult.valid, false);
  assert.ok(digestResult.errors.some((error) => error.code === "minLength"));
  assert.ok(digestResult.warnings.some((error) => error.code === "approval_required"));
});

test("preflight evaluates anyOf and oneOf without mutating the caller params", () => {
  const capability = catalog.get("blueprint.pin.default.set");
  assert.ok(capability);
  const params = {
    blueprint: "/Game/Test",
    nodeId: "node",
    pinName: "Input",
    value: "1",
  };
  const valid = preflightParameters(capability, params);
  assert.equal(valid.valid, true);
  assert.equal(valid.safeToProceed, true);
  assert.deepEqual(params, {
    blueprint: "/Game/Test",
    nodeId: "node",
    pinName: "Input",
    value: "1",
  });

  const invalid = preflightParameters(capability, {
    blueprint: "/Game/Test",
    nodeId: "node",
    pinName: "Input",
    pinId: "id",
    value: "1",
    typedValue: { type: "Vector", value: {} },
  });
  assert.equal(invalid.valid, false);
  assert.ok(invalid.errors.some((error) => error.code === "oneOf"));
});
