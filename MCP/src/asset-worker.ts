import { createHash } from "node:crypto";
import { closeSync, existsSync, openSync, readSync, statSync } from "node:fs";
import { extname } from "node:path";

type Request = { operation: string; assetPath: string; otherAssetPath?: string; engineVersion: string };
type JsonObject = Record<string, unknown>;

const PACKAGE_TAG = 0x9e2a83c1;
const LEGACY_VERSION_UE5 = -8;
const UE53_FILE_VERSION_UE4 = 522;
const UE5_MIN_VERSION = 1000;
const UE5_MAX_VERSION = 1009;
const UE5_NAMES_REFERENCED = 1001;
const UE5_PAYLOAD_TOC = 1002;
const UE5_SOFT_OBJECT_PATHS = 1008;
const UE5_DATA_RESOURCES = 1009;
const PKG_COOKED = 0x00000200;
const PKG_FILTER_EDITOR_ONLY = 0x80000000;
const MAX_HEADER_BYTES = 64 * 1024 * 1024;
const MAX_ARRAY_ITEMS = 1_000_000;
const MAX_STRING_UNITS = 1_048_576;
const MAX_PREVIEW_NAMES = 256;
const MAX_PREVIEW_ASSETS = 64;
const MAX_PREVIEW_TAGS = 512;
const MAX_TAG_VALUE_CHARACTERS = 4096;

class PackageReader {
  position = 0;

  constructor(readonly bytes: Buffer) {}

  private require(length: number, label: string): void {
    if (!Number.isSafeInteger(length) || length < 0 || this.position + length > this.bytes.length) {
      throw failure("asset_corrupt", `${label} exceeds the bounded package header.`);
    }
  }

  seek(position: number, label: string): void {
    if (!Number.isSafeInteger(position) || position < 0 || position > this.bytes.length) {
      throw failure("asset_corrupt", `${label} offset is outside the bounded package header.`);
    }
    this.position = position;
  }

  skip(length: number, label: string): void {
    this.require(length, label);
    this.position += length;
  }

  i32(label: string): number {
    this.require(4, label);
    const value = this.bytes.readInt32LE(this.position);
    this.position += 4;
    return value;
  }

  u32(label: string): number {
    this.require(4, label);
    const value = this.bytes.readUInt32LE(this.position);
    this.position += 4;
    return value;
  }

  u16(label: string): number {
    this.require(2, label);
    const value = this.bytes.readUInt16LE(this.position);
    this.position += 2;
    return value;
  }

  i64(label: string): number {
    this.require(8, label);
    const value = this.bytes.readBigInt64LE(this.position);
    this.position += 8;
    const numeric = Number(value);
    if (!Number.isSafeInteger(numeric)) throw failure("asset_corrupt", `${label} is outside the safe offset range.`);
    return numeric;
  }

  guid(label: string): string {
    const words = [this.u32(label), this.u32(label), this.u32(label), this.u32(label)];
    return words.map((word) => word.toString(16).padStart(8, "0")).join("-");
  }

  string(label: string): string {
    const serializedLength = this.i32(`${label}.length`);
    if (serializedLength === 0) return "";
    const units = Math.abs(serializedLength);
    if (units > MAX_STRING_UNITS) throw failure("asset_corrupt", `${label} exceeds the string limit.`);
    const wide = serializedLength < 0;
    const byteLength = units * (wide ? 2 : 1);
    this.require(byteLength, label);
    const start = this.position;
    this.position += byteLength;
    const contentEnd = Math.max(start, this.position - (wide ? 2 : 1));
    return this.bytes.subarray(start, contentEnd).toString(wide ? "utf16le" : "latin1");
  }

  count(label: string, maximum = MAX_ARRAY_ITEMS): number {
    const value = this.i32(label);
    if (value < 0 || value > maximum) throw failure("asset_corrupt", `${label} is outside the supported range.`);
    return value;
  }
}

function failure(code: string, message: string): { code: string; message: string } {
  return { code, message };
}

function sha256(bytes: Buffer): string {
  return `sha256:${createHash("sha256").update(bytes).digest("hex")}`;
}

function readPrefix(path: string, maximum: number): Buffer {
  const size = statSync(path).size;
  const length = Math.min(size, maximum);
  const bytes = Buffer.alloc(length);
  const descriptor = openSync(path, "r");
  try {
    let total = 0;
    while (total < length) {
      const count = readSync(descriptor, bytes, total, length - total, total);
      if (count === 0) break;
      total += count;
    }
    return total === length ? bytes : bytes.subarray(0, total);
  } finally {
    closeSync(descriptor);
  }
}

function headerPath(requestedPath: string): { headerPath: string; companion?: JsonObject } {
  if (extname(requestedPath).toLowerCase() !== ".uexp") return { headerPath: requestedPath };
  const sibling = `${requestedPath.slice(0, -5)}.uasset`;
  if (!existsSync(sibling)) {
    throw failure("asset_companion_unavailable", "A .uexp summary requires the matching sibling .uasset package header.");
  }
  const stat = statSync(requestedPath);
  return {
    headerPath: sibling,
    companion: {
      path: requestedPath,
      extension: ".uexp",
      sizeBytes: stat.size,
      prefixSha256: sha256(readPrefix(requestedPath, 1024 * 1024)),
    },
  };
}

function engineVersion(reader: PackageReader, label: string): JsonObject {
  return {
    major: reader.u16(`${label}.major`),
    minor: reader.u16(`${label}.minor`),
    patch: reader.u16(`${label}.patch`),
    changelist: reader.u32(`${label}.changelist`),
    branch: reader.string(`${label}.branch`),
  };
}

type ParsedSummary = {
  packageSummary: JsonObject;
  totalHeaderSize: number;
  packageFlags: number;
  nameCount: number;
  nameOffset: number;
  exportCount: number;
  exportOffset: number;
  importCount: number;
  importOffset: number;
  dependsOffset: number;
  softPackageReferencesCount: number;
  softPackageReferencesOffset: number;
  assetRegistryDataOffset: number;
  allOffsets: number[];
};

function parseSummary(reader: PackageReader): ParsedSummary {
  const tag = reader.u32("tag");
  if (tag !== PACKAGE_TAG) throw failure("asset_corrupt", "Asset package tag is invalid, encrypted, or unsupported.");
  const legacyFileVersion = reader.i32("legacyFileVersion");
  if (legacyFileVersion !== LEGACY_VERSION_UE5) {
    throw failure("asset_version_incompatible", "The offline Asset Worker accepts only the UE5 package-summary layout (legacy version -8).");
  }
  const legacyUE3Version = reader.i32("legacyUE3Version");
  const fileVersionUE4 = reader.i32("fileVersionUE4");
  const fileVersionUE5 = reader.i32("fileVersionUE5");
  const fileVersionLicenseeUE = reader.i32("fileVersionLicenseeUE");
  if (fileVersionUE4 === 0 && fileVersionUE5 === 0 && fileVersionLicenseeUE === 0) {
    throw failure("asset_unversioned_unavailable", "Unversioned packages cannot be interpreted safely outside their exact Engine build.");
  }
  if (fileVersionUE4 !== UE53_FILE_VERSION_UE4 || fileVersionUE5 < UE5_MIN_VERSION || fileVersionUE5 > UE5_MAX_VERSION) {
    throw failure("asset_version_incompatible", `Package versions UE4=${fileVersionUE4}, UE5=${fileVersionUE5} are outside the UE 5.3 worker contract.`);
  }

  const customVersionCount = reader.count("customVersions", 4096);
  reader.skip(customVersionCount * 20, "customVersion entries");
  const totalHeaderSize = reader.i32("totalHeaderSize");
  if (totalHeaderSize < reader.position || totalHeaderSize > reader.bytes.length || totalHeaderSize > MAX_HEADER_BYTES) {
    throw failure("asset_header_too_large", "TotalHeaderSize is invalid or exceeds the 64 MiB offline header limit.");
  }
  const packageName = reader.string("packageName");
  const packageFlags = reader.u32("packageFlags");
  if ((packageFlags & PKG_COOKED) !== 0 || (packageFlags & PKG_FILTER_EDITOR_ONLY) !== 0) {
    throw failure("asset_cooked_unavailable", "Cooked or editor-filtered packages are intentionally unavailable to the offline Asset Worker.");
  }

  const nameCount = reader.count("nameCount");
  const nameOffset = reader.i32("nameOffset");
  let softObjectPathsCount = 0;
  let softObjectPathsOffset = 0;
  if (fileVersionUE5 >= UE5_SOFT_OBJECT_PATHS) {
    softObjectPathsCount = reader.count("softObjectPathsCount");
    softObjectPathsOffset = reader.i32("softObjectPathsOffset");
  }
  const localizationId = reader.string("localizationId");
  const gatherableTextDataCount = reader.count("gatherableTextDataCount");
  const gatherableTextDataOffset = reader.i32("gatherableTextDataOffset");
  const exportCount = reader.count("exportCount");
  const exportOffset = reader.i32("exportOffset");
  const importCount = reader.count("importCount");
  const importOffset = reader.i32("importOffset");
  const dependsOffset = reader.i32("dependsOffset");
  const softPackageReferencesCount = reader.count("softPackageReferencesCount");
  const softPackageReferencesOffset = reader.i32("softPackageReferencesOffset");
  const searchableNamesOffset = reader.i32("searchableNamesOffset");
  const thumbnailTableOffset = reader.i32("thumbnailTableOffset");
  const guid = reader.guid("guid");
  const persistentGuid = reader.guid("persistentGuid");
  const generationCount = reader.count("generationCount", 4096);
  reader.skip(generationCount * 8, "generation entries");
  const savedByEngineVersion = engineVersion(reader, "savedByEngineVersion");
  const compatibleWithEngineVersion = engineVersion(reader, "compatibleWithEngineVersion");
  const compressionFlags = reader.u32("compressionFlags");
  const compressedChunkCount = reader.count("compressedChunks", 4096);
  if (compressionFlags !== 0 || compressedChunkCount !== 0) {
    throw failure("asset_compressed_unavailable", "Package-level compressed assets are unavailable to the offline Asset Worker.");
  }
  const packageSource = reader.u32("packageSource");
  const additionalPackageCount = reader.count("additionalPackagesToCook", 4096);
  for (let index = 0; index < additionalPackageCount; index += 1) reader.string(`additionalPackagesToCook[${index}]`);
  const assetRegistryDataOffset = reader.i32("assetRegistryDataOffset");
  const bulkDataStartOffset = reader.i64("bulkDataStartOffset");
  const worldTileInfoDataOffset = reader.i32("worldTileInfoDataOffset");
  const chunkIdCount = reader.count("chunkIds", 4096);
  const chunkIds = Array.from({ length: chunkIdCount }, (_, index) => reader.i32(`chunkIds[${index}]`));
  const preloadDependencyCount = reader.i32("preloadDependencyCount");
  if (preloadDependencyCount < -1 || preloadDependencyCount > MAX_ARRAY_ITEMS) {
    throw failure("asset_corrupt", "preloadDependencyCount is outside the supported range.");
  }
  const preloadDependencyOffset = reader.i32("preloadDependencyOffset");
  const namesReferencedFromExportDataCount = fileVersionUE5 >= UE5_NAMES_REFERENCED ? reader.count("namesReferencedFromExportDataCount") : nameCount;
  const payloadTocOffset = fileVersionUE5 >= UE5_PAYLOAD_TOC ? reader.i64("payloadTocOffset") : -1;
  const dataResourceOffset = fileVersionUE5 >= UE5_DATA_RESOURCES ? reader.i32("dataResourceOffset") : -1;

  const allOffsets = [
    totalHeaderSize, nameOffset, softObjectPathsOffset, gatherableTextDataOffset, exportOffset, importOffset, dependsOffset,
    softPackageReferencesOffset, searchableNamesOffset, thumbnailTableOffset, assetRegistryDataOffset, worldTileInfoDataOffset,
    preloadDependencyOffset, payloadTocOffset, dataResourceOffset,
  ].filter((offset) => Number.isSafeInteger(offset) && offset > 0 && offset <= totalHeaderSize);

  return {
    totalHeaderSize,
    packageFlags,
    nameCount,
    nameOffset,
    exportCount,
    exportOffset,
    importCount,
    importOffset,
    dependsOffset,
    softPackageReferencesCount,
    softPackageReferencesOffset,
    assetRegistryDataOffset,
    allOffsets,
    packageSummary: {
      tag: "0x9e2a83c1",
      legacyFileVersion,
      legacyUE3Version,
      fileVersionUE4,
      fileVersionUE5,
      fileVersionLicenseeUE,
      customVersionCount,
      totalHeaderSize,
      packageName,
      packageFlags: `0x${packageFlags.toString(16).padStart(8, "0")}`,
      localizationId,
      softObjectPaths: { count: softObjectPathsCount, offset: softObjectPathsOffset },
      gatherableTextData: { count: gatherableTextDataCount, offset: gatherableTextDataOffset },
      guid,
      persistentGuid,
      generationCount,
      savedByEngineVersion,
      compatibleWithEngineVersion,
      packageSource,
      bulkDataStartOffset,
      chunkIds,
      preloadDependencies: { count: preloadDependencyCount, offset: preloadDependencyOffset },
      namesReferencedFromExportDataCount,
      payloadTocOffset,
      dataResourceOffset,
    },
  };
}

function nextOffset(summary: ParsedSummary, offset: number): number {
  return summary.allOffsets.filter((candidate) => candidate > offset).sort((left, right) => left - right)[0] ?? summary.totalHeaderSize;
}

function tableDescriptor(bytes: Buffer, summary: ParsedSummary, count: number, offset: number): JsonObject {
  if (count === 0 || offset === 0) return { count, offset, available: count === 0 };
  if (offset < 0 || offset >= summary.totalHeaderSize) throw failure("asset_corrupt", "Package table offset is outside TotalHeaderSize.");
  const endOffset = nextOffset(summary, offset);
  if (endOffset < offset || endOffset > bytes.length) throw failure("asset_corrupt", "Package table range is invalid.");
  return { count, offset, endOffset, byteLength: endOffset - offset, sha256: sha256(bytes.subarray(offset, endOffset)) };
}

function parseNames(bytes: Buffer, summary: ParsedSummary): JsonObject {
  if (summary.nameCount === 0) return { count: 0, offset: summary.nameOffset, names: [] };
  const reader = new PackageReader(bytes);
  reader.seek(summary.nameOffset, "name table");
  const names: string[] = [];
  const digest = createHash("sha256");
  const start = reader.position;
  for (let index = 0; index < summary.nameCount; index += 1) {
    const value = reader.string(`names[${index}]`);
    reader.skip(4, `names[${index}].hashes`);
    digest.update(value, "utf8").update("\0", "utf8");
    if (names.length < MAX_PREVIEW_NAMES) names.push(value);
  }
  if (reader.position > nextOffset(summary, summary.nameOffset)) throw failure("asset_corrupt", "Name table overlaps the next package section.");
  return {
    count: summary.nameCount,
    offset: summary.nameOffset,
    endOffset: reader.position,
    byteLength: reader.position - start,
    names,
    truncated: summary.nameCount > names.length,
    normalizedSha256: `sha256:${digest.digest("hex")}`,
  };
}

function nameAt(names: string[], index: number, number: number): string | null {
  if (index < 0 || index >= names.length) return null;
  return number > 0 ? `${names[index]}_${number - 1}` : names[index]!;
}

function parseNameList(bytes: Buffer, summary: ParsedSummary): string[] {
  if (summary.nameCount === 0) return [];
  const reader = new PackageReader(bytes);
  reader.seek(summary.nameOffset, "name table");
  const names: string[] = [];
  for (let index = 0; index < summary.nameCount; index += 1) {
    names.push(reader.string(`names[${index}]`));
    reader.skip(4, `names[${index}].hashes`);
  }
  return names;
}

function parseDependencies(bytes: Buffer, summary: ParsedSummary, names: string[]): JsonObject {
  const result: JsonObject = { dependsMap: { exportCount: summary.exportCount, offset: summary.dependsOffset, edges: 0 } };
  if (summary.exportCount > 0 && summary.dependsOffset > 0) {
    const reader = new PackageReader(bytes);
    reader.seek(summary.dependsOffset, "depends map");
    let edgeCount = 0;
    const perExportCounts: number[] = [];
    for (let index = 0; index < summary.exportCount; index += 1) {
      const count = reader.count(`dependsMap[${index}]`, MAX_ARRAY_ITEMS - edgeCount);
      edgeCount += count;
      perExportCounts.push(count);
      reader.skip(count * 4, `dependsMap[${index}] entries`);
    }
    result.dependsMap = {
      exportCount: summary.exportCount,
      offset: summary.dependsOffset,
      endOffset: reader.position,
      edges: edgeCount,
      perExportCounts: perExportCounts.slice(0, 256),
      truncated: perExportCounts.length > 256,
      sha256: sha256(bytes.subarray(summary.dependsOffset, reader.position)),
    };
  }
  const softPackageReferences: Array<string | null> = [];
  if (summary.softPackageReferencesCount > 0 && summary.softPackageReferencesOffset > 0) {
    const reader = new PackageReader(bytes);
    reader.seek(summary.softPackageReferencesOffset, "soft package references");
    for (let index = 0; index < summary.softPackageReferencesCount; index += 1) {
      const nameIndex = reader.i32(`softPackageReferences[${index}].nameIndex`);
      const number = reader.i32(`softPackageReferences[${index}].number`);
      softPackageReferences.push(nameAt(names, nameIndex, number));
    }
  }
  result.softPackageReferences = {
    count: summary.softPackageReferencesCount,
    values: softPackageReferences.slice(0, 256),
    truncated: softPackageReferences.length > 256,
  };
  return result;
}

function parseAssetRegistry(bytes: Buffer, summary: ParsedSummary): JsonObject {
  if (summary.assetRegistryDataOffset <= 0) return { available: false, reason: "asset_registry_section_absent", assets: [] };
  const reader = new PackageReader(bytes);
  reader.seek(summary.assetRegistryDataOffset, "asset registry");
  const dependencyDataOffset = reader.i64("assetRegistry.dependencyDataOffset");
  const objectCount = reader.count("assetRegistry.objectCount");
  const assets: JsonObject[] = [];
  let totalTagCount = 0;
  for (let objectIndex = 0; objectIndex < objectCount; objectIndex += 1) {
    const objectPath = reader.string(`assetRegistry.assets[${objectIndex}].objectPath`);
    const objectClass = reader.string(`assetRegistry.assets[${objectIndex}].objectClass`);
    const tagCount = reader.count(`assetRegistry.assets[${objectIndex}].tagCount`, MAX_ARRAY_ITEMS - totalTagCount);
    totalTagCount += tagCount;
    const tags: Record<string, string> = {};
    const tagMetadata: Record<string, JsonObject> = {};
    let retained = 0;
    for (let tagIndex = 0; tagIndex < tagCount; tagIndex += 1) {
      const key = reader.string(`assetRegistry.assets[${objectIndex}].tags[${tagIndex}].key`);
      const value = reader.string(`assetRegistry.assets[${objectIndex}].tags[${tagIndex}].value`);
      if (assets.length < MAX_PREVIEW_ASSETS && retained < MAX_PREVIEW_TAGS) {
        tags[key] = value.length <= MAX_TAG_VALUE_CHARACTERS ? value : `${value.slice(0, MAX_TAG_VALUE_CHARACTERS)}…`;
        if (value.length > MAX_TAG_VALUE_CHARACTERS) {
          tagMetadata[key] = { truncated: true, originalCharacters: value.length, sha256: sha256(Buffer.from(value, "utf8")) };
        }
        retained += 1;
      }
    }
    if (assets.length < MAX_PREVIEW_ASSETS) assets.push({ objectPath, objectClass, tagCount, tags, tagMetadata, tagsTruncated: retained < tagCount });
  }
  return {
    available: true,
    offset: summary.assetRegistryDataOffset,
    endOffset: reader.position,
    dependencyDataOffset,
    objectCount,
    totalTagCount,
    assets,
    truncated: assets.length < objectCount,
    sha256: sha256(bytes.subarray(summary.assetRegistryDataOffset, reader.position)),
  };
}

function normalizedDigest(value: JsonObject): string {
  const normalized = {
    packageSummary: value.packageSummary,
    tables: value.tables,
    assetRegistry: value.assetRegistry,
    dependencies: value.dependencies,
    companion: value.companion ?? null,
  };
  return `sha256:${createHash("sha256").update(JSON.stringify(normalized), "utf8").digest("hex")}`;
}

function summary(path: string, requestedEngineVersion: string): JsonObject {
  if (!requestedEngineVersion.startsWith("5.3")) throw failure("asset_version_incompatible", "This bundled Asset Worker is pinned to UE 5.3 package headers.");
  const resolved = headerPath(path);
  const stat = statSync(resolved.headerPath);
  if (stat.size < 32) throw failure("asset_corrupt", "Asset package is too small to contain a package summary.");
  const bytes = readPrefix(resolved.headerPath, MAX_HEADER_BYTES);
  const parsed = parseSummary(new PackageReader(bytes));
  const names = parseNameList(bytes, parsed);
  const value: JsonObject = {
    schema: "ue.local-asset-summary.v2",
    available: true,
    assetPath: path,
    packageHeaderPath: resolved.headerPath,
    extension: extname(path).toLowerCase(),
    sizeBytes: statSync(path).size,
    headerSizeBytes: parsed.totalHeaderSize,
    packageSummary: parsed.packageSummary,
    headerSha256: sha256(bytes.subarray(0, parsed.totalHeaderSize)),
    tables: {
      names: parseNames(bytes, parsed),
      imports: tableDescriptor(bytes, parsed, parsed.importCount, parsed.importOffset),
      exports: tableDescriptor(bytes, parsed, parsed.exportCount, parsed.exportOffset),
    },
    assetRegistry: parseAssetRegistry(bytes, parsed),
    dependencies: parseDependencies(bytes, parsed, names),
    companion: resolved.companion,
  };
  value.normalizedDigest = normalizedDigest(value);
  return value;
}

function diff(left: JsonObject, right: JsonObject): JsonObject {
  const fields = ["packageSummary", "tables", "assetRegistry", "dependencies", "companion"];
  const changes = fields.filter((field) => JSON.stringify(left[field] ?? null) !== JSON.stringify(right[field] ?? null));
  return {
    schema: "ue.local-asset-diff.v2",
    left,
    right,
    equalNormalized: left.normalizedDigest === right.normalizedDigest,
    equalHeader: left.headerSha256 === right.headerSha256,
    sizeDelta: Number(right.sizeBytes) - Number(left.sizeBytes),
    changedSections: changes,
  };
}

let input = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => { input += chunk; if (input.length > 1024 * 1024) process.exit(2); });
process.stdin.on("end", () => {
  try {
    const request = JSON.parse(input) as Request;
    const first = summary(request.assetPath, request.engineVersion);
    const data = request.operation === "production.asset.package.diff"
      ? (() => {
          if (typeof request.otherAssetPath !== "string" || !existsSync(request.otherAssetPath)) throw failure("asset_diff_target_required", "otherAssetPath is required for package diff.");
          return diff(first, summary(request.otherAssetPath, request.engineVersion));
        })()
      : first;
    process.stdout.write(JSON.stringify({ ok: true, data }));
  } catch (error) {
    const payload = error as { code?: string; message?: string };
    process.stdout.write(JSON.stringify({ ok: false, error: { code: payload.code ?? "asset_worker_failed", message: payload.message ?? String(error) } }));
    process.exitCode = 1;
  }
});
