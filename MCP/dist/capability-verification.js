function boolOrNull(value) {
    return typeof value === "boolean" ? value : null;
}
function boolOrDefault(value, fallback) {
    return typeof value === "boolean" ? value : fallback;
}
function existingState(data) {
    const candidate = data.verificationState;
    const nested = candidate !== null && typeof candidate === "object" && !Array.isArray(candidate)
        ? candidate
        : {};
    // Native handlers historically emitted the proof fields at the payload
    // root.  Preserve those values when adapting an older response, while the
    // nested verificationState remains the canonical envelope for new callers.
    const root = data;
    return {
        ...nested,
        localDeclared: nested.localDeclared ?? root.localDeclared,
        handlerRegistered: nested.handlerRegistered ?? root.handlerRegistered,
        liveAvailable: nested.liveAvailable ?? root.liveAvailable,
        executed: nested.executed ?? root.executed,
        readbackVerified: nested.readbackVerified ?? root.readbackVerified,
        runtimeVerified: nested.runtimeVerified ?? root.runtimeVerified,
    };
}
/**
 * Add the common six-state proof to a capability payload while preserving
 * domain-specific readback/runtime fields supplied by the handler.
 */
export function attachCapabilityVerification(capability, data, defaults) {
    const prior = existingState(data);
    const state = {
        schema: "ue.capability-verification.v1",
        capability,
        // Do not let an untrusted handler payload smuggle non-boolean values into
        // the transport contract.  Native handlers own their proof, while the
        // adapter supplies conservative defaults for fields they did not provide.
        localDeclared: boolOrDefault(prior.localDeclared, defaults.localDeclared ?? true),
        handlerRegistered: boolOrDefault(prior.handlerRegistered, defaults.handlerRegistered ?? true),
        liveAvailable: boolOrDefault(prior.liveAvailable, defaults.liveAvailable ?? true),
        executed: boolOrDefault(prior.executed, defaults.executed ?? true),
        readbackVerified: boolOrNull(data.readbackVerified) ??
            boolOrNull(prior.readbackVerified) ??
            defaults.readbackVerified ??
            null,
        runtimeVerified: boolOrNull(data.runtimeVerified) ??
            boolOrNull(prior.runtimeVerified) ??
            defaults.runtimeVerified ??
            null,
    };
    return {
        ...data,
        localDeclared: state.localDeclared,
        handlerRegistered: state.handlerRegistered,
        liveAvailable: state.liveAvailable,
        executed: state.executed,
        readbackVerified: state.readbackVerified,
        runtimeVerified: state.runtimeVerified,
        verificationState: state,
    };
}
export function verificationErrorDetails(capability, details, defaults = {}) {
    const base = details !== null && typeof details === "object" && !Array.isArray(details)
        ? details
        : {};
    return attachCapabilityVerification(capability, base, {
        localDeclared: defaults.localDeclared ?? true,
        handlerRegistered: defaults.handlerRegistered ?? true,
        liveAvailable: defaults.liveAvailable ?? false,
        executed: defaults.executed ?? false,
        readbackVerified: defaults.readbackVerified ?? null,
        runtimeVerified: defaults.runtimeVerified ?? null,
    });
}
//# sourceMappingURL=capability-verification.js.map