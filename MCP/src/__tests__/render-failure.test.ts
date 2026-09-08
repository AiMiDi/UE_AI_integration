import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, realpathSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test, type TestContext } from "node:test";
import { parseCrashContext, parseRenderLog, redactDiagnostic } from "../render-failure.js";
import { LocalProjectExecutor } from "../project-executor.js";
import { loadCapabilityCatalog } from "../capability-catalog.js";
import { BackendRoutingExecutor, runDomainOperation } from "../domain-router.js";
import { UEApiError } from "../ue-bridge.js";

const log = (message: string) => `[2026.09.08-01.00.00:000][123]LogD3D12RHI: Error: ${message}`;
const hasCode = (code: string) => (error: unknown) => error instanceof UEApiError && error.code === code;
function fixture(t: TestContext): string {
  const root = realpathSync(mkdtempSync(join(tmpdir(), "ueai-render-evidence-")));
  t.after(() => { assert.equal(realpathSync(root), root); rmSync(root, { recursive: true }); });
  return root;
}
async function analyze(root: string, files: string[], extra: Record<string, unknown> = {}) {
  return new LocalProjectExecutor().execute("scene.render.failure.analyze", { projectRoot: root, files, ...extra });
}

test("extracts this branch's DRED allocation, frame and address evidence without asserting cause", () => {
  const result = parseRenderLog([
    log("DXGI_ERROR_DEVICE_REMOVED with Reason: DXGI_ERROR_DEVICE_HUNG"),
    log('DRED: PageFault at VA GPUAddress "0xABC"'),
    log("DRED: Active objects with VA ranges that match the faulting VA:"),
    log("Name: Texture A (Type: RESOURCE)"),
    log("DRED: Recent freed objects with VA ranges that match the faulting VA:"),
    log("Name: Texture B (Type: RESOURCE)"),
    log("PageFault: Last completed frame ID: 12 (cached: 11) - Current frame ID: 14"),
    log("GPU Address: [0x100 .. 0x200] - Size: 256 bytes, 0.00 MB - Distance to page fault: -1 bytes, 0.00 MB - Transient: 1 - Name: TransientDepth - Desc: Texture2D"),
    log("GPU Address: [0x100 .. 0x200] - Size: 256 bytes, 0.00 MB - FrameID: 12 - DefragFree: 0 - Transient: 1 - Heap: 0 - Name: OldDepth - Desc: Texture2D"),
  ].join("\r\n"));
  assert.deepEqual(result.signals.filter(s => s.kind === "dredAllocation").map(s => s.details.lifetime), ["active", "recentlyFreed"]);
  assert.equal(result.signals.find(s => s.kind === "pageFault")!.details.gpuAddress, "0xABC");
  assert.equal(result.signals.find(s => s.kind === "gpuFrameProgress")!.details.currentFrame, "14");
  assert.equal(result.signals.find(s => s.kind === "trackedResource")!.details.distanceBytes, "-1");
  assert.equal(result.signals.find(s => s.kind === "releasedResource")!.details.frameId, "12");
  assert.equal(result.truncated, false);
});

test("keeps DRED operations attached to their command list and resets at a boundary", () => {
  const result = parseRenderLog([
    log('DRED: Commandlist "CL1" on CommandQueue "AsyncCompute", 3 completed of 10'),
    log("Op: 2, DISPATCH [Niagara] - LAST COMPLETED"),
    log("Op: 3, RESOURCEBARRIER"),
    log('DRED: Commandlist "CL2" on CommandQueue "Graphics", 1 completed of 4'),
    log("Op: 0, DRAWINSTANCED - LAST COMPLETED"),
    log('DRED: PageFault at VA GPUAddress "0x100"'),
    log("Op: 999, unrelated"),
  ].join("\n"));
  const ops = result.signals.filter(s => s.kind === "dredOperation");
  assert.equal(ops.length, 3);
  assert.deepEqual(ops.map(s => s.details.queue), ["AsyncCompute", "AsyncCompute", "Graphics"]);
  assert.deepEqual(ops.map(s => s.details.lastCompleted), [true, false, true]);
  assert.ok(ops.every(s => s.details.rootCauseProven === false));
});

test("missing DRED and Ensures remain qualified and unrelated categories do not become render evidence", () => {
  const result = parseRenderLog([
    "LogChat: DXGI_ERROR_DEVICE_REMOVED DRED: PageFault at VA GPUAddress 0x123",
    log("DRED: No breadcrumb head found."),
    log("DRED: No PageFault data."),
    "LogWindows: Error: Ensure condition failed: resource transition invalid",
  ].join("\n"));
  assert.equal(result.signals.filter(s => s.kind === "diagnosticUnavailable").length, 2);
  assert.equal(result.signals.find(s => s.kind === "ensure")!.details.fatality, "notEstablishedByEnsure");
  assert.ok(!result.signals.some(s => s.kind === "deviceRemoved" || s.kind === "pageFault"));
});

test("recognizes LogOutputDevice rendering stacks and keeps symbol/load diagnostics unverified", () => {
  const result = parseRenderLog([
    "LogOutputDevice: Error: Ensure condition failed: ReferencedGeometries.Num() == TSet(ReferencedGeometries).Num()",
    "LogOutputDevice: Error: [Callstack] 0x00007ff8 UnrealEditor-RenderCore.dll!FRenderingThread::Run() [S:/Engine/RenderingThread.cpp:399]",
    "LogWindows: Error: Unhandled Exception: EXCEPTION_ACCESS_VIOLATION reading address 0x00000000",
    "LogWindows: Failed to preload 'S:/Engine/UnrealEditor-Niagara.dll' (GetLastError=127)",
  ].join("\n"));
  assert.deepEqual(result.signals.map(s => s.kind), ["ensure", "renderCallstack", "cpuException", "renderModuleLoadFailure"]);
  assert.equal(result.signals[1]!.details.symbolVerification, "notPerformed");
  assert.equal(result.signals[2]!.details.access, "reading");
  assert.equal(result.signals[3]!.details.systemError, "127");
});

test("bounded output preserves total matches and byte-exact multibyte line offsets", () => {
  const first = "中文 diagnostic\r\n";
  const result = parseRenderLog(first + log("GPU crashed") + "\n" + log("GPU crashed"), 1, 512);
  assert.equal(result.signals.length, 1);
  assert.equal(result.matchedSignalCount, 2);
  assert.equal(result.signals[0]!.byteOffset, 512 + Buffer.byteLength(first));
  assert.equal(result.signals[0]!.line, 2);
  assert.equal(result.truncated, true);
  assert.equal(parseRenderLog("x".repeat(5000)).shortenedLineCount, 1);
  assert.equal(parseRenderLog("x".repeat(5000)).truncated, true);
});

test("redacts launch arguments, authorization, URL and secrets in both excerpts and fields", () => {
  const value = redactDiagnostic('token=abc password="a b" Bearer abc.xyz https://x.invalid/?secret=a CommandLine: -secret=xyz');
  for (const secret of ["abc", "a b", "x.invalid", "xyz"]) assert.ok(!value.includes(secret));
  assert.ok(!redactDiagnostic("Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==").includes("QWxh"));
  const result = parseRenderLog(log("DRED: Active objects with VA ranges that match the faulting VA:") + "\n" + log("Name: token=mysecret (Type: RESOURCE)"));
  assert.ok(!JSON.stringify(result).includes("mysecret"));
});

test("CrashContext extracts only whitelisted leaf fields and never resolves entities", () => {
  const result = parseCrashContext('<FGenericCrashContext><RuntimeProperties><EngineVersion>5.4.1</EngineVersion><ErrorMessage>A &lt; B token=private</ErrorMessage><CommandLine>privateCommand</CommandLine><UserName>privateUser</UserName></RuntimeProperties></FGenericCrashContext>');
  assert.equal(result.EngineVersion, "5.4.1");
  assert.equal(result.ErrorMessage, "A < B token=<redacted>");
  assert.ok(!JSON.stringify(result).includes("private"));
  assert.throws(() => parseCrashContext('<!DOCTYPE x [<!ENTITY x SYSTEM "file:///a">]><FGenericCrashContext/>'), hasCode("crash_context_unsupported_xml"));
  assert.throws(() => parseCrashContext("<FGenericCrashContext><ErrorMessage>partial"), hasCode("crash_context_unsupported_xml"));
  assert.throws(() => parseCrashContext("<unrelated/>"), hasCode("crash_context_unsupported_xml"));
});

test("offline executor analyzes finalized evidence without a uproject or Editor and hashes exactly the returned range", async t => {
  const root = fixture(t);
  const bytes = Buffer.from("\uFEFF" + log("GPU crashed") + "\n");
  writeFileSync(join(root, "Crash.log"), bytes);
  const result = await analyze(root, ["Crash.log"]);
  const source = (result.sources as any[])[0];
  assert.equal(result.schema, "ue.render-failure-evidence.v1");
  assert.equal(source.byteOffset, 3);
  assert.equal(source.contentSha256, createHash("sha256").update(bytes.subarray(3)).digest("hex"));
  assert.equal(source.signals[0].byteOffset, 3);
  assert.equal(source.coverage, "complete");
  assert.equal((result.rootCause as any).status, "unproven");
});

test("tail reads discard partial first lines and retain precise source offsets", async t => {
  const root = fixture(t);
  const bytes = Buffer.from("discard".repeat(1000) + "\n" + log("GPU crashed") + "\n");
  writeFileSync(join(root, "Large.log"), bytes);
  const result = await analyze(root, ["Large.log"], { maxBytesPerFile: 4096 });
  const source = (result.sources as any[])[0];
  assert.equal(source.coverage, "tail");
  assert.equal(source.lineNumberScope, "analyzedSlice");
  assert.equal(source.byteOffset, 7001);
  assert.equal(source.signals[0].byteOffset, 7001);
  assert.equal(source.contentSha256, createHash("sha256").update(bytes.subarray(7001)).digest("hex"));
});

test("global signal limit applies across files; dump binaries remain metadata only", async t => {
  const root = fixture(t);
  writeFileSync(join(root, "A.log"), log("GPU crashed"));
  writeFileSync(join(root, "B.log"), log("GPU crashed"));
  writeFileSync(join(root, "Crash.dmp"), Buffer.from([0, 255, 1]));
  const result = await analyze(root, ["A.log", "B.log", "Crash.dmp"], { limit: 1 });
  const sources = result.sources as any[];
  assert.equal(result.retainedSignalCount, 1);
  assert.equal(sources[1].matchedSignalCount, 1);
  assert.equal(sources[1].truncated, true);
  assert.equal(sources[2].analysisStatus, "metadataOnly");
  assert.equal(sources[2].contentSha256, undefined);
});

test("rejects path traversal and junction escapes", async t => {
  const root = fixture(t);
  await assert.rejects(analyze(root, ["../outside.log"]), hasCode("path_outside_allowed_root"));
  const outside = fixture(t);
  writeFileSync(join(outside, "Crash.log"), log("GPU crashed"));
  symlinkSync(outside, join(root, "Escape"), "junction");
  await assert.rejects(analyze(root, ["Escape/Crash.log"]), hasCode("path_outside_allowed_root"));
});

test("rejects unsupported text, oversized XML, no complete tail lines, invalid budgets and nonfiles", async t => {
  const root = fixture(t);
  writeFileSync(join(root, "Binary.log"), Buffer.from([255, 254, 0, 0]));
  writeFileSync(join(root, "Huge.xml"), "x".repeat(5000));
  writeFileSync(join(root, "Line.log"), "x".repeat(5000));
  mkdirSync(join(root, "Directory.log"));
  await assert.rejects(analyze(root, ["Binary.log"]), hasCode("diagnostic_encoding_unsupported"));
  await assert.rejects(analyze(root, ["Huge.xml"], { maxBytesPerFile: 4096 }), hasCode("diagnostic_file_too_large"));
  await assert.rejects(analyze(root, ["Line.log"], { maxBytesPerFile: 4096 }), hasCode("diagnostic_line_too_large"));
  await assert.rejects(analyze(root, ["Line.log"], { limit: 257 }), hasCode("invalid_parameters"));
  await assert.rejects(analyze(root, ["Directory.log"]), hasCode("diagnostic_file_invalid"));
  await assert.rejects(analyze(root, []), hasCode("invalid_parameters"));
});

test("empty logs cannot establish a healthy render or a root cause", async t => {
  const root = fixture(t);
  writeFileSync(join(root, "Empty.log"), "");
  const result = await analyze(root, ["Empty.log"]);
  assert.equal(result.conclusion, "inconclusive");
  assert.equal((result.rootCause as any).status, "unproven");
});

test("the scene domain routes failure analysis locally even when Editor is unreachable", async t => {
  const root = fixture(t);
  writeFileSync(join(root, "Crash.log"), log("GPU crashed"));
  const catalog = loadCapabilityCatalog();
  let editorCalls = 0;
  const offline = { execute: async () => { editorCalls++; throw new UEApiError({ code: "editor_unreachable", message: "offline" }); } };
  const router = new BackendRoutingExecutor(catalog, offline, offline, undefined, new LocalProjectExecutor());
  const response = await runDomainOperation(catalog, router, "scene", "scene.render.failure.analyze",
    { projectRoot: root, files: ["Crash.log"] });
  assert.notEqual(response.isError, true);
  assert.equal(editorCalls, 0);
  assert.ok(JSON.stringify(response).includes("diagnosticSignalsObserved"));
});
