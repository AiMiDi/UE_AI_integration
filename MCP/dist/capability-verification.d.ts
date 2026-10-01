export type NullableVerification = boolean | null;
export interface CapabilityVerificationState {
    schema: "ue.capability-verification.v1";
    capability: string;
    localDeclared: boolean;
    handlerRegistered: boolean;
    liveAvailable: boolean;
    executed: boolean;
    readbackVerified: NullableVerification;
    runtimeVerified: NullableVerification;
}
type JsonObject = Record<string, unknown>;
/**
 * Add the common six-state proof to a capability payload while preserving
 * domain-specific readback/runtime fields supplied by the handler.
 */
export declare function attachCapabilityVerification(capability: string, data: JsonObject, defaults: Partial<CapabilityVerificationState>): JsonObject;
export declare function verificationErrorDetails(capability: string, details: unknown, defaults?: Partial<CapabilityVerificationState>): JsonObject;
export {};
