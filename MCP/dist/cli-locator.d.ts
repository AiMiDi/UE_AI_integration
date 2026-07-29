export type CliLocationSource = "environment" | "packaged" | "path" | "development" | "not_found";
export interface CliLocationCandidate {
    source: Exclude<CliLocationSource, "not_found">;
    path: string;
    exists: boolean;
}
export interface CliLocationResult {
    found: boolean;
    executablePath: string | null;
    source: CliLocationSource;
    command: string;
    configuredPath: string | null;
    pluginRoot: string;
    candidates: CliLocationCandidate[];
    pathEntriesSearched: number;
    guidance: string;
}
export interface CliLocatorOptions {
    env?: NodeJS.ProcessEnv;
    platform?: NodeJS.Platform;
    moduleUrl?: string;
    isFile?: (path: string) => boolean;
}
export declare function locateWorkflowCli(options?: CliLocatorOptions): CliLocationResult;
export declare function locateShortCli(options?: CliLocatorOptions): CliLocationResult;
