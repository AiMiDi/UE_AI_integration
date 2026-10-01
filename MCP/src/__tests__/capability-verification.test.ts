import assert from "node:assert/strict";
import test from "node:test";

import {
  attachCapabilityVerification,
  verificationErrorDetails,
} from "../capability-verification.js";
import { loadCapabilityCatalog } from "../capability-catalog.js";
import { runDomainOperation, type CapabilityExecutor } from "../domain-router.js";
import { UEApiError } from "../ue-bridge.js";

test("capability verification preserves handler proofs and fills unknown states", () => {
  const result = attachCapabilityVerification(
    "content.test.read",
    { readbackVerified: false, domainValue: 7 },
    {
      localDeclared: true,
      handlerRegistered: true,
      liveAvailable: true,
      executed: true,
    },
  );
  assert.equal(result.localDeclared, true);
  assert.equal(result.handlerRegistered, true);
  assert.equal(result.liveAvailable, true);
  assert.equal(result.executed, true);
  assert.equal(result.readbackVerified, false);
  assert.equal(result.runtimeVerified, null);
  assert.deepEqual(result.verificationState, {
    schema: "ue.capability-verification.v1",
    capability: "content.test.read",
    localDeclared: true,
    handlerRegistered: true,
    liveAvailable: true,
    executed: true,
    readbackVerified: false,
    runtimeVerified: null,
  });
});

test("capability verification preserves legacy top-level handler proofs", () => {
  const result = attachCapabilityVerification(
    "content.test.legacy",
    {
      localDeclared: false,
      handlerRegistered: true,
      liveAvailable: false,
      executed: true,
      readbackVerified: true,
    },
    {
      localDeclared: true,
      handlerRegistered: false,
      liveAvailable: true,
      executed: false,
    },
  );
  assert.equal(result.localDeclared, false);
  assert.equal(result.handlerRegistered, true);
  assert.equal(result.liveAvailable, false);
  assert.equal(result.executed, true);
  assert.equal(result.readbackVerified, true);
  assert.deepEqual(result.verificationState, {
    schema: "ue.capability-verification.v1",
    capability: "content.test.legacy",
    localDeclared: false,
    handlerRegistered: true,
    liveAvailable: false,
    executed: true,
    readbackVerified: true,
    runtimeVerified: null,
  });
});

test("capability verification errors remain actionable and unexecuted", () => {
  const details = verificationErrorDetails(
    "production.test.write",
    { nextAction: "read back" },
    { localDeclared: true, handlerRegistered: true },
  );
  assert.equal(details.nextAction, "read back");
  assert.equal(details.executed, false);
  assert.equal(details.liveAvailable, false);
  assert.equal(
    (details.verificationState as Record<string, unknown>).capability,
    "production.test.write",
  );
});

test("capability verification rejects malformed transport booleans", () => {
  const result = attachCapabilityVerification(
    "scene.test.read",
    {
      verificationState: {
        localDeclared: "yes",
        handlerRegistered: 1,
        liveAvailable: null,
        executed: [],
        readbackVerified: "unknown",
      },
    },
    {
      localDeclared: false,
      handlerRegistered: false,
      liveAvailable: false,
      executed: false,
    },
  );
  assert.deepEqual(result.verificationState, {
    schema: "ue.capability-verification.v1",
    capability: "scene.test.read",
    localDeclared: false,
    handlerRegistered: false,
    liveAvailable: false,
    executed: false,
    readbackVerified: null,
    runtimeVerified: null,
  });
});

test("verification-aware domain routes expose six states on success and failure", async () => {
  const catalog = loadCapabilityCatalog();
  const success = await runDomainOperation(
    catalog,
    {
      verificationAware: true,
      execute: async () => ({ loaded: true }),
    } as CapabilityExecutor,
    "production",
    "production.module.loaded.get",
  );
  assert.equal(success.isError, false);
  assert.equal(success.content[0]?.type, "text");
  if (success.content[0]?.type !== "text") assert.fail("Expected JSON success payload");
  const successPayload = JSON.parse(success.content[0].text) as Record<string, unknown>;
  assert.deepEqual(successPayload.verificationState, {
    schema: "ue.capability-verification.v1",
    capability: "production.module.loaded.get",
    localDeclared: true,
    handlerRegistered: true,
    liveAvailable: true,
    executed: true,
    readbackVerified: null,
    runtimeVerified: null,
  });

  const failure = await runDomainOperation(
    catalog,
    {
      verificationAware: true,
      execute: async () => {
        throw new UEApiError({ code: "editor_unreachable", message: "offline" });
      },
    } as CapabilityExecutor,
    "production",
    "production.module.loaded.get",
  );
  assert.equal(failure.isError, true);
  assert.equal(failure.content[0]?.type, "text");
  if (failure.content[0]?.type !== "text") assert.fail("Expected JSON error payload");
  const failurePayload = JSON.parse(failure.content[0].text) as {
    error: { details: Record<string, unknown> };
  };
  assert.deepEqual(failurePayload.error.details.verificationState, {
    schema: "ue.capability-verification.v1",
    capability: "production.module.loaded.get",
    localDeclared: true,
    handlerRegistered: false,
    liveAvailable: false,
    executed: false,
    readbackVerified: null,
    runtimeVerified: null,
  });
});
