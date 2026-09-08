import { createHash } from "node:crypto";
import { closeSync, fstatSync, openSync, readSync, realpathSync, statSync } from "node:fs";
import { extname, isAbsolute, relative, resolve, sep } from "node:path";
import { UEApiError, type UEExecuteData } from "./ue-bridge.js";

type Json = Record<string, unknown>;
type Signal = { kind: string; line: number; byteOffset: number; excerpt: string; details: Json };

function fail(code: string, message: string): never {
  throw new UEApiError({ code, message });
}

function inside(root: string, path: string): boolean {
  const rel = relative(root, path);
  return rel !== ".." && !rel.startsWith(`..${sep}`) && !isAbsolute(rel);
}

function integer(value: unknown, fallback: number, min: number, max: number): number {
  if (value === undefined) return fallback;
  if (typeof value !== "number" || !Number.isInteger(value) || value < min || value > max) {
    return fail("invalid_parameters", `Expected an integer between ${min} and ${max}.`);
  }
  return value;
}

// Diagnostic excerpts are deliberately bounded; credentials and launch arguments
// have no role in resource/queue diagnosis and must not be copied into reports.
export function redactDiagnostic(text: string): string {
  return text
    .replace(/(?:command\s*line|commandline)\s*[:=].*/gi, "CommandLine: <redacted>")
    .replace(/\b(?:authorization|cookie|set-cookie)\s*[:=].*/gi, "<redacted-header>")
    .replace(/\b(?:bearer|basic)\s+[A-Za-z0-9+/_=.-]+/gi, "<redacted-auth>")
    .replace(/((?:[\w.-]*(?:password|passwd|token|secret|credential|api[_-]?key)[\w.-]*)\s*[=:]\s*)(?:"[^"]*"|'[^']*'|[^\s,;]+)/gi, "$1<redacted>")
    .replace(/https?:\/\/[^\s<>"']+/gi, "<redacted-url>")
    .slice(0, 512);
}

function digest(bytes: Uint8Array | string): string {
  return createHash("sha256").update(bytes).digest("hex");
}

/** Parse only observed diagnostic signals. Nearby resources and active GPU
 * breadcrumbs are evidence of location/progress, never a root-cause verdict. */
export function parseRenderLog(text: string, limit = 64, startByte = 0): {
  signals: Signal[]; matchedSignalCount: number; truncated: boolean; shortenedLineCount: number;
} {
  const signals: Signal[] = [];
  let matchedSignalCount = 0;
  let allocation: "active" | "recentlyFreed" | undefined;
  let byteOffset = startByte;
  let queue: string | undefined;
  let commandList: string | undefined;
  let shortenedLineCount = 0;
  const add = (kind: string, line: number, excerpt: string, details: Json = {}) => {
    matchedSignalCount++;
    if (signals.length < limit) signals.push({ kind, line, byteOffset, excerpt: redactDiagnostic(excerpt), details });
  };
  const lines = text.split("\n");
  for (let index = 0; index < lines.length; index++) {
    const raw = lines[index]!;
    const line = raw.replace(/\r$/, "");
    // Avoid applying expressions to an unbounded single line from an unrelated log.
    const value = line.slice(0, 4096);
    if (value.length < line.length) shortenedLineCount++;
    const renderCategory = /Log(?:D3D12RHI|D3D11RHI|RHI|Renderer|RenderCore|RenderGraph|Windows):/i.test(value);
    const crashCategory = renderCategory || /LogOutputDevice:/i.test(value);
    if (renderCategory && /DRED:\s*(?:Active objects|Recent freed objects)/i.test(value)) {
      allocation = /Recent freed/i.test(value) ? "recentlyFreed" : "active";
    } else if (!renderCategory || !/\bName:.*\(Type:/i.test(value)) {
      allocation = undefined;
    }
    const removed = /\bDXGI_ERROR_(DEVICE_REMOVED|DEVICE_HUNG|DEVICE_RESET|DRIVER_INTERNAL_ERROR)\b/.exec(value);
    if (removed && renderCategory) add("deviceRemoved", index + 1, value, { code: removed[0] });
    if (/GPU (?:Crashed|crash detected)|GPU Crash dump Triggered/i.test(value) && renderCategory) {
      add("gpuCrash", index + 1, value);
    }
    const fault = /(?:DRED|PageFault):\s*PageFault at VA GPUAddress\s*"?(0x[0-9a-f]+)/i.exec(value);
    if (fault && renderCategory) add("pageFault", index + 1, value, { gpuAddress: fault[1] });
    const frame = /Last completed frame ID:\s*(\d+)\s*\(cached:\s*(\d+)\)\s*-\s*Current frame ID:\s*(\d+)/i.exec(value);
    if (frame && renderCategory) add("gpuFrameProgress", index + 1, value, { lastCompletedFrame: frame[1], cachedCompletedFrame: frame[2], currentFrame: frame[3] });
    const name = /\bName:\s*(.*?)\s*\(Type:\s*([^)]*)\)/i.exec(value);
    if (allocation && name && renderCategory) {
      add("dredAllocation", index + 1, value, { lifetime: allocation, name: redactDiagnostic(name[1]!), allocationType: redactDiagnostic(name[2]!), relation: "reportedMatchingFaultAddress" });
    }
    const resource = /GPU Address:\s*\[(0x[0-9a-f]+)\s*\.\.\s*(0x[0-9a-f]+)\].*?Distance to page fault:\s*(-?\d+) bytes.*?Transient:\s*(\d+)\s*-\s*Name:\s*(.*?)\s*-\s*Desc:/i.exec(value);
    if (resource && renderCategory) add("trackedResource", index + 1, value, { addressBegin: resource[1], addressEnd: resource[2], distanceBytes: resource[3], transient: resource[4] === "1", name: redactDiagnostic(resource[5]!), relation: "nearFaultAddress" });
    const released = /GPU Address:\s*\[(0x[0-9a-f]+)\s*\.\.\s*(0x[0-9a-f]+)\].*?FrameID:\s*(\d+)\s*-\s*DefragFree:\s*(\d+)\s*-\s*Transient:\s*(\d+)\s*-\s*Heap:\s*(\d+)\s*-\s*Name:\s*(.*?)\s*-\s*Desc:/i.exec(value);
    if (released && renderCategory) add("releasedResource", index + 1, value, { addressBegin: released[1], addressEnd: released[2], frameId: released[3], defragFree: released[4] === "1", transient: released[5] === "1", heap: released[6] === "1", name: redactDiagnostic(released[7]!), relation: "reportedReleasedRangeContainingFaultAddress" });
    const dredList = /DRED:\s*Commandlist\s*"([^"]*)"\s*on CommandQueue\s*"([^"]*)",\s*(\d+) completed of (\d+)/i.exec(value);
    const dredOp = /\bOp:\s*(\d+),\s*(.*)/.exec(value);
    const queueMatch = /(?:Queue|Command Queue)\s*(?:[=:]|\[)\s*([^\]\r\n]+)/i.exec(value);
    if (renderCategory && dredList) {
      commandList = redactDiagnostic(dredList[1]!);
      queue = redactDiagnostic(dredList[2]!);
      add("dredCommandList", index + 1, value, { queue, commandList, completedOperations: dredList[3], totalOperations: dredList[4] });
    } else if (renderCategory && dredOp && commandList) {
      add("dredOperation", index + 1, value, { queue, commandList, operationIndex: dredOp[1], operation: redactDiagnostic(dredOp[2]!), lastCompleted: / - LAST COMPLETED\s*$/.test(value), rootCauseProven: false });
    } else {
      commandList = undefined;
      queue = renderCategory && /Breadcrumb|DRED/i.test(value) && queueMatch ? redactDiagnostic(queueMatch[1]!) : undefined;
    }
    if (renderCategory && /Breadcrumb|DRED/i.test(value) && /(?:No breadcrumb|No command list|all finished|not enabled|not available|could not find DRED|No PageFault)/i.test(value)) {
      add("diagnosticUnavailable", index + 1, value);
    } else if (renderCategory && /(?:\[Active\]|\[Not Started\]|\[Finished\]|Breadcrumb|DRED:.*(?:Queue|CommandList))/i.test(value)) {
      const state = /\[(Active|Not Started|Finished)\]/i.exec(value)?.[1];
      add("gpuBreadcrumb", index + 1, value, { ...(queue === undefined ? {} : { queue }), ...(state === undefined ? {} : { state }), rootCauseProven: false });
    }
    if (renderCategory && /(?:Out of video memory|Video memory has been exhausted|E_OUTOFMEMORY)/i.test(value)) add("videoMemoryFailure", index + 1, value);
    if (renderCategory && /(?:resource state|resource transition|barrier|aliasing)/i.test(value) && /(?:error|assert|ensure|invalid|mismatch)/i.test(value)) add("resourceStateFailure", index + 1, value);
    if (crashCategory && /Ensure condition failed/i.test(value)) add("ensure", index + 1, value, { fatality: "notEstablishedByEnsure" });
    const exception = /Unhandled Exception:\s*(EXCEPTION_[A-Z_]+)(?:\s+(reading|writing|executing) address\s+(0x[0-9a-f]+))?/i.exec(value);
    if (crashCategory && exception) add("cpuException", index + 1, value, { code: exception[1], ...(exception[2] ? { access: exception[2], address: exception[3] } : {}) });
    const stack = /\b(UnrealEditor-(?:Renderer|RenderCore|RenderGraph|RHI|D3D12RHI|D3D11RHI|Niagara|NiagaraShader)\.dll)!(.*)/i.exec(value);
    if (crashCategory && stack) add("renderCallstack", index + 1, value, { module: stack[1], frame: redactDiagnostic(stack[2]!), symbolVerification: "notPerformed" });
    const moduleFailure = /Failed to (?:preload|load) '[^']*\b(UnrealEditor-(?:Niagara|NiagaraShader|NiagaraVertexFactories|Renderer|RenderCore|D3D12RHI|RHI)\.dll)'\s*\(GetLastError=(\d+)\)/i.exec(value);
    if (crashCategory && moduleFailure) add("renderModuleLoadFailure", index + 1, value, { module: moduleFailure[1], systemError: moduleFailure[2], importVerification: "notPerformed" });
    byteOffset += Buffer.byteLength(raw, "utf8") + (index + 1 < lines.length ? 1 : 0);
  }
  return { signals, matchedSignalCount, shortenedLineCount, truncated: matchedSignalCount > signals.length || shortenedLineCount > 0 };
}

function decodeXmlValue(value: string): string {
  return value.replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&quot;/g, '"').replace(/&apos;/g, "'").replace(/&amp;/g, "&");
}

export function parseCrashContext(text: string): Json {
  // No general XML parser: DTDs/entities are forbidden and never resolved.
  if (/<!DOCTYPE|<!ENTITY/i.test(text)) fail("crash_context_unsupported_xml", "CrashContext must not contain DTD or entity declarations.");
  // This is a whitelist extractor for UE CrashContext, not general XML validation.
  text = text.replace(/<!--[\s\S]*?-->/g, "");
  if (!/<FGenericCrashContext(?:\s[^>]*)?>[\s\S]*<\/FGenericCrashContext>\s*$/i.test(text)) {
    fail("crash_context_unsupported_xml", "Expected a complete UE FGenericCrashContext document.");
  }
  const result: Json = {};
  for (const field of ["CrashGUID", "CrashType", "ProcessId", "EngineVersion", "BuildVersion", "ErrorMessage", "RHI.RHIName", "RHI.AdapterName", "RHI.DREDHasBreadcrumbData", "RHI.DREDHasPageFaultData"]) {
    const escaped = field.replace(/\./g, "\\.");
    const match = new RegExp(`<${escaped}>([\\s\\S]*?)<\\/${escaped}>`, "i").exec(text);
    if (match && !match[1]!.includes("<")) result[field] = redactDiagnostic(decodeXmlValue(match[1]!));
  }
  return result;
}

/** An offline localProject capability, also shared by the native CLI adapter. */
export function analyzeRenderFailure(root: string, params: Json): UEExecuteData {
  const paths = params.files;
  if (!Array.isArray(paths) || paths.length < 1 || paths.length > 8 || paths.some((p) => typeof p !== "string" || p.length === 0 || isAbsolute(p))) {
    fail("invalid_parameters", "files must contain 1-8 explicit project-relative diagnostic paths.");
  }
  const maxBytes = integer(params.maxBytesPerFile, 2 * 1024 * 1024, 4096, 4 * 1024 * 1024);
  const limit = integer(params.limit, 64, 1, 256);
  const sources: Json[] = [];
  let retained = 0;
  const kinds = new Set<string>();
  for (const item of paths as string[]) {
    const candidate = resolve(root, item);
    if (!inside(root, candidate)) fail("path_outside_allowed_root", "Diagnostic paths must stay inside projectRoot.");
    let path: string;
    try { path = realpathSync(candidate); } catch { fail("diagnostic_file_not_found", "A requested diagnostic file does not exist."); }
    if (!inside(root, path)) fail("path_outside_allowed_root", "Diagnostic paths must not escape projectRoot through links.");
    const extension = extname(path).toLowerCase();
    const binary = [".dmp", ".nv-gpudmp"].includes(extension);
    if (![".log", ".txt", ".xml", ".runtime-xml", ".dmp", ".nv-gpudmp"].includes(extension)) fail("diagnostic_extension_forbidden", "Only log, CrashContext XML and dump artifacts are supported.");
    if (!statSync(path).isFile()) fail("diagnostic_file_invalid", "Diagnostic input must be a regular file.");
    const fd = openSync(path, "r");
    try {
      const before = fstatSync(fd);
      if (!before.isFile()) fail("diagnostic_file_invalid", "Diagnostic input must be a regular file.");
      const source: Json = { path: relative(root, path).replace(/\\/g, "/"), sizeBytes: before.size, modifiedUtc: before.mtime.toISOString() };
      if (binary) {
        source.analysisStatus = "metadataOnly";
        source.reason = "Binary dump decoding and symbol resolution require an external debugger or vendor adapter.";
        sources.push(source);
        continue;
      }
      const xml = [".xml", ".runtime-xml"].includes(extension);
      if (xml && before.size > maxBytes) fail("diagnostic_file_too_large", "CrashContext XML exceeds maxBytesPerFile; partial XML is not analyzed.");
      let offset = Math.max(0, before.size - maxBytes);
      let bytes = Buffer.alloc(Math.min(maxBytes, before.size));
      let read = 0;
      while (read < bytes.length) {
        const count = readSync(fd, bytes, read, bytes.length - read, offset + read);
        if (count === 0) fail("diagnostic_source_changed", "Diagnostic source changed during the bounded read.");
        read += count;
      }
      if (offset > 0) {
        const newline = bytes.indexOf(10);
        if (newline < 0) fail("diagnostic_line_too_large", "The bounded tail contains no complete log line.");
        offset += newline + 1;
        bytes = bytes.subarray(newline + 1);
      }
      // A UTF-8 BOM is not part of the first log line, but offsets remain byte-exact.
      if (offset === 0 && bytes.subarray(0, 3).equals(Buffer.from([0xef, 0xbb, 0xbf]))) { bytes = bytes.subarray(3); offset = 3; }
      let text: string;
      try { text = new TextDecoder("utf-8", { fatal: true }).decode(bytes); } catch { fail("diagnostic_encoding_unsupported", "Diagnostic text must be UTF-8."); }
      if (text.includes("\0")) fail("diagnostic_encoding_unsupported", "Diagnostic input contains binary or unsupported text encoding.");
      const after = fstatSync(fd);
      const current = statSync(path);
      if (before.size !== after.size || before.mtimeMs !== after.mtimeMs || before.ino !== current.ino || before.dev !== current.dev || current.size !== before.size || current.mtimeMs !== before.mtimeMs || realpathSync(candidate) !== path) {
        fail("diagnostic_source_changed", "Diagnostic source changed during analysis; retry a finalized artifact.");
      }
      Object.assign(source, { analysisStatus: "analyzed", byteOffset: offset, bytesRead: bytes.length, contentSha256: digest(bytes), hashScope: "analyzedByteRange", coverage: before.size > maxBytes ? "tail" : "complete", lineNumberScope: before.size > maxBytes ? "analyzedSlice" : "file" });
      if (xml) {
        source.crashContext = parseCrashContext(text);
      } else {
        const parsed = parseRenderLog(text, Math.max(0, limit - retained), offset);
        for (const signal of parsed.signals) kinds.add(signal.kind);
        retained += parsed.signals.length;
        Object.assign(source, parsed);
      }
      sources.push(source);
    } finally { closeSync(fd); }
  }
  return {
    schema: "ue.render-failure-evidence.v1",
    sources,
    observedSignalKinds: [...kinds].sort(),
    retainedSignalCount: retained,
    conclusion: retained > 0 ? "diagnosticSignalsObserved" : "inconclusive",
    rootCause: { status: "unproven", note: "Device removal, Ensures, nearby allocations and active breadcrumbs do not individually establish a cause. Files remain independent evidence sources." },
    nextSteps: ["Correlate the crash process/build and matching symbols before source-level attribution.", "Inspect the reported queue/pass and resource lifetime; reproduce with a bounded capture when needed."],
  };
}
