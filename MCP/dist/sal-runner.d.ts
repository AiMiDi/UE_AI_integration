type JsonObject = Record<string, unknown>;
export declare function parseSalSource(source: string): JsonObject;
export declare function salStub(): JsonObject;
export declare function salLint(source: string): JsonObject;
export declare function salPlan(source: string): JsonObject;
export declare function readSalSource(path: string): string;
export {};
