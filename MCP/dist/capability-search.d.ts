import type { CapabilityDescriptor } from "./capability-catalog.js";
export interface CapabilitySearchMatch {
    score: number;
    matchedFields: Array<"id" | "title" | "keywords" | "aliases" | "description">;
    matchedTokens: string[];
}
export declare function compareCapabilityIds(left: string, right: string): number;
export declare function tokenizeCapabilitySearch(text: string): string[];
export declare function matchCapabilitySearch(query: string, capability: Pick<CapabilityDescriptor, "id" | "description" | "search">): CapabilitySearchMatch | undefined;
