import { createHash } from "node:crypto";
import { closeSync, existsSync, openSync, readSync, statSync } from "node:fs";
import { extname } from "node:path";

type AssetDetail = "summary" | "tables" | "full";
type AssetSection = "names" | "imports" | "exports" | "assetRegistry" | "dependencies";
type Request = {
  operation: string;
  assetPath: string;
  otherAssetPath?: string;
  engineVersion: string;
  detail?: AssetDetail;
  sections?: AssetSection[];
  offset?: number;
  limit?: number;
  targetTokens?: number;
  maxBytes?: number;
};
type JsonObject = Record<string, unknown>;

type OutputOptions = {
  detail: AssetDetail;
  sections: Set<AssetSection>;
  offset: number;
  limit: number;
  targetTokens: number;
  maxBytes: number;
  effectiveMaxBytes: number;
};

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
const MAX_TAG_VALUE_CHARACTERS = 4096;
const DEFAULT_TARGET_TOKENS = 2048;
const DEFAULT_MAX_BYTES = 64 * 1024;
const MIN_MAX_BYTES = 4 * 1024;
const MAX_MAX_BYTES = 1024 * 1024;
const DEFAULT_PAGE_LIMIT = 64;
const MAX_PAGE_LIMIT = 256;

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

function boundedInteger(
  value: unknown,
  fallback: number,
  minimum: number,
  maximum: number,
  label: string,
): number {
  const resolved = value === undefined ? fallback : value;
  if (!Number.isInteger(resolved) || Number(resolved) < minimum || Number(resolved) > maximum) {
    throw failure("invalid_params", `${label} must be an integer from ${minimum} to ${maximum}.`);
  }
  return Number(resolved);
}

function outputOptions(request: Request): OutputOptions {
  const detail = request.detail ?? "summary";
  if (!["summary", "tables", "full"].includes(detail)) {
    throw failure("invalid_params", "detail must be summary, tables, or full.");
  }
  const allowed: AssetSection[] = ["names", "imports", "exports", "assetRegistry", "dependencies"];
  const requestedSections = request.sections ?? (detail === "summary" ? [] : allowed);
  if (!Array.isArray(requestedSections) || requestedSections.some((section) => !allowed.includes(section))) {
    throw failure("invalid_params", "sections contains an unsupported Asset section.");
  }
  const targetTokens = boundedInteger(request.targetTokens, DEFAULT_TARGET_TOKENS, 256, 32768, "targetTokens");
  const maxBytes = boundedInteger(request.maxBytes, DEFAULT_MAX_BYTES, MIN_MAX_BYTES, MAX_MAX_BYTES, "maxBytes");
  return {
    detail,
    sections: new Set(requestedSections),
    offset: boundedInteger(request.offset, 0, 0, MAX_ARRAY_ITEMS, "offset"),
    limit: boundedInteger(request.limit, DEFAULT_PAGE_LIMIT, 1, MAX_PAGE_LIMIT, "limit"),
    targetTokens,
    maxBytes,
    effectiveMaxBytes: Math.min(maxBytes, targetTokens * 4),
  };
}

function byteLength(value: unknown): number {
  return Buffer.byteLength(JSON.stringify(value), "utf8");
}

function pageMetadata(total: number, offset: number, returned: number, limit: number): JsonObject {
  const next = offset + returned < total ? offset + returned : null;
  return { total, offset, limit, returned, nextOffset: next, truncated: next !== null };
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

function tableDescriptor(
  bytes: Buffer,
  summary: ParsedSummary,
  count: number,
  offset: number,
  options: OutputOptions,
): JsonObject {
  if (count === 0 || offset === 0) {
    return {
      available: count === 0,
      offsetBytes: offset,
      items: [],
      ...pageMetadata(count, options.offset, 0, options.limit),
    };
  }
  if (offset < 0 || offset >= summary.totalHeaderSize) throw failure("asset_corrupt", "Package table offset is outside TotalHeaderSize.");
  const endOffset = nextOffset(summary, offset);
  if (endOffset < offset || endOffset > bytes.length) throw failure("asset_corrupt", "Package table range is invalid.");
  const byteLength = endOffset - offset;
  const fixedEntryBytes = byteLength % count === 0 ? byteLength / count : null;
  const returned = Math.max(0, Math.min(options.limit, count - options.offset));
  const items: JsonObject[] = [];
  if (fixedEntryBytes !== null) {
    for (let index = options.offset; index < options.offset + returned; index += 1) {
      const itemOffset = offset + index * fixedEntryBytes;
      items.push({
        index,
        offsetBytes: itemOffset,
        byteLength: fixedEntryBytes,
        sha256: sha256(bytes.subarray(itemOffset, itemOffset + fixedEntryBytes)),
      });
    }
  } else {
    for (let index = options.offset; index < options.offset + returned; index += 1) {
      items.push({ index, unavailable: true, reason: "entry_layout_not_fixed" });
    }
  }
  return {
    available: true,
    ...pageMetadata(count, options.offset, items.length, options.limit),
    offsetBytes: offset,
    endOffset,
    byteLength,
    fixedEntryBytes,
    items,
    structuredEntriesAvailable: fixedEntryBytes !== null,
    sha256: sha256(bytes.subarray(offset, endOffset)),
  };
}

function parseNames(bytes: Buffer, summary: ParsedSummary, options: OutputOptions): JsonObject {
  if (summary.nameCount === 0) return { offsetBytes: summary.nameOffset, items: [], ...pageMetadata(0, options.offset, 0, options.limit) };
  const reader = new PackageReader(bytes);
  reader.seek(summary.nameOffset, "name table");
  const items: string[] = [];
  const digest = createHash("sha256");
  const start = reader.position;
  for (let index = 0; index < summary.nameCount; index += 1) {
    const value = reader.string(`names[${index}]`);
    reader.skip(4, `names[${index}].hashes`);
    digest.update(value, "utf8").update("\0", "utf8");
    if (index >= options.offset && index < options.offset + options.limit) items.push(value);
  }
  if (reader.position > nextOffset(summary, summary.nameOffset)) throw failure("asset_corrupt", "Name table overlaps the next package section.");
  return {
    ...pageMetadata(summary.nameCount, options.offset, items.length, options.limit),
    offsetBytes: summary.nameOffset,
    endOffset: reader.position,
    byteLength: reader.position - start,
    items,
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

function parseDependencies(bytes: Buffer, summary: ParsedSummary, names: string[], options: OutputOptions): JsonObject {
  const result: JsonObject = {
    dependsMap: {
      exportCount: summary.exportCount,
      offsetBytes: summary.dependsOffset,
      edges: 0,
      perExportCounts: [],
      ...pageMetadata(summary.exportCount, options.offset, 0, options.limit),
    },
  };
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
      ...pageMetadata(summary.exportCount, options.offset, perExportCounts.slice(options.offset, options.offset + options.limit).length, options.limit),
      exportCount: summary.exportCount,
      offsetBytes: summary.dependsOffset,
      endOffset: reader.position,
      edges: edgeCount,
      perExportCounts: perExportCounts.slice(options.offset, options.offset + options.limit),
      sha256: sha256(bytes.subarray(summary.dependsOffset, reader.position)),
    };
  }
  const softPackageReferences: Array<string | null> = [];
  const softReferenceDigest = createHash("sha256");
  if (summary.softPackageReferencesCount > 0 && summary.softPackageReferencesOffset > 0) {
    const reader = new PackageReader(bytes);
    reader.seek(summary.softPackageReferencesOffset, "soft package references");
    for (let index = 0; index < summary.softPackageReferencesCount; index += 1) {
      const nameIndex = reader.i32(`softPackageReferences[${index}].nameIndex`);
      const number = reader.i32(`softPackageReferences[${index}].number`);
      const value = nameAt(names, nameIndex, number);
      softReferenceDigest.update(value ?? "<invalid>", "utf8").update("\0", "utf8");
      if (index >= options.offset && index < options.offset + options.limit) softPackageReferences.push(value);
    }
  }
  result.softPackageReferences = {
    ...pageMetadata(summary.softPackageReferencesCount, options.offset, softPackageReferences.length, options.limit),
    values: softPackageReferences,
    normalizedSha256: `sha256:${softReferenceDigest.digest("hex")}`,
  };
  const depends = result.dependsMap as JsonObject;
  const soft = result.softPackageReferences as JsonObject;
  const total = Math.max(Number(depends.total ?? 0), Number(soft.total ?? 0));
  const returned = Math.max(Number(depends.returned ?? 0), Number(soft.returned ?? 0));
  Object.assign(result, pageMetadata(total, options.offset, returned, options.limit));
  return result;
}

function parseAssetRegistry(bytes: Buffer, summary: ParsedSummary, options: OutputOptions): JsonObject {
  if (summary.assetRegistryDataOffset <= 0) {
    return {
      available: false,
      reason: "asset_registry_section_absent",
      assets: [],
      ...pageMetadata(0, options.offset, 0, options.limit),
    };
  }
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
    const retainAsset = objectIndex >= options.offset && objectIndex < options.offset + options.limit;
    const tags: JsonObject[] = [];
    for (let tagIndex = 0; tagIndex < tagCount; tagIndex += 1) {
      const key = reader.string(`assetRegistry.assets[${objectIndex}].tags[${tagIndex}].key`);
      const value = reader.string(`assetRegistry.assets[${objectIndex}].tags[${tagIndex}].value`);
      if (retainAsset && options.detail === "full" && tagIndex < options.limit) {
        const truncated = value.length > MAX_TAG_VALUE_CHARACTERS;
        tags.push({
          key,
          value: truncated ? `${value.slice(0, MAX_TAG_VALUE_CHARACTERS)}…` : value,
          ...(truncated ? { valueTruncated: true, originalCharacters: value.length, sha256: sha256(Buffer.from(value, "utf8")) } : {}),
        });
      }
    }
    if (retainAsset) {
      assets.push({
        objectPath,
        objectClass,
        tagCount,
        ...(options.detail === "full"
          ? { tags, tagsTruncated: tags.length < tagCount }
          : {}),
      });
    }
  }
  return {
    available: true,
    ...pageMetadata(objectCount, options.offset, assets.length, options.limit),
    offsetBytes: summary.assetRegistryDataOffset,
    endOffset: reader.position,
    dependencyDataOffset,
    objectCount,
    totalTagCount,
    assets,
    sha256: sha256(bytes.subarray(summary.assetRegistryDataOffset, reader.position)),
  };
}

function normalizedDigest(value: JsonObject): string {
  const normalized = {
    packageSummary: value.packageSummary,
    sectionDigests: value.sectionDigests,
    companion: value.companion ?? null,
  };
  return `sha256:${createHash("sha256").update(JSON.stringify(normalized), "utf8").digest("hex")}`;
}

function compactPackageSummary(value: JsonObject): JsonObject {
  const result = { ...value };
  const chunkIds = Array.isArray(result.chunkIds) ? result.chunkIds as unknown[] : [];
  result.chunkIds = {
    count: chunkIds.length,
    normalizedSha256: sha256(Buffer.from(JSON.stringify(chunkIds), "utf8")),
  };
  return result;
}

function updatePageAfterTrim(section: JsonObject, itemField: string): void {
  const items = Array.isArray(section[itemField]) ? section[itemField] as unknown[] : [];
  section.returned = items.length;
  const total = Number(section.total ?? 0);
  const offset = Number(section.offset ?? 0);
  section.nextOffset = offset + items.length < total ? offset + items.length : null;
  section.truncated = section.nextOffset !== null;
}

function updateDependencyPageAfterTrim(section: JsonObject): void {
  const depends = section.dependsMap as JsonObject | undefined;
  const soft = section.softPackageReferences as JsonObject | undefined;
  const total = Math.max(Number(depends?.total ?? 0), Number(soft?.total ?? 0));
  const returned = Math.max(Number(depends?.returned ?? 0), Number(soft?.returned ?? 0));
  const offset = Number(section.offset ?? depends?.offset ?? soft?.offset ?? 0);
  const limit = Number(section.limit ?? depends?.limit ?? soft?.limit ?? DEFAULT_PAGE_LIMIT);
  Object.assign(section, pageMetadata(total, offset, returned, limit));
}

function enforceBudget(value: JsonObject, options: OutputOptions): JsonObject {
  const omittedSections: string[] = [];
  value.budget = {
    targetTokens: options.targetTokens,
    maxBytes: options.maxBytes,
    effectiveMaxBytes: options.effectiveMaxBytes,
    estimatedBytes: 0,
    truncated: false,
    omittedSections,
  };
  value.truncated = false;
  value.omittedSections = omittedSections;
  const sections = value.sections as JsonObject | undefined;
  const trimOne = (): boolean => {
    const assetRegistry = sections?.assetRegistry as JsonObject | undefined;
    const assets = Array.isArray(assetRegistry?.assets) ? assetRegistry.assets as JsonObject[] : [];
    for (let index = assets.length - 1; index >= 0; index -= 1) {
      const tags = Array.isArray(assets[index]?.tags) ? assets[index]!.tags as unknown[] : [];
      if (tags.length > 0) {
        tags.pop();
        assets[index]!.tagsTruncated = true;
        return true;
      }
    }
    const candidates: Array<[JsonObject | undefined, string]> = [
      [sections?.dependencies as JsonObject | undefined, "perExportCounts"],
      [sections?.dependencies as JsonObject | undefined, "softPackageReferences"],
      [assetRegistry, "assets"],
      [sections?.exports as JsonObject | undefined, "items"],
      [sections?.imports as JsonObject | undefined, "items"],
      [sections?.names as JsonObject | undefined, "items"],
    ];
    for (const [section, field] of candidates) {
      if (!section) continue;
      if (field === "perExportCounts") {
        const depends = section.dependsMap as JsonObject | undefined;
        const items = Array.isArray(depends?.perExportCounts) ? depends.perExportCounts as unknown[] : [];
        if (items.length > 0) {
          items.pop();
          updatePageAfterTrim(depends!, "perExportCounts");
          updateDependencyPageAfterTrim(section);
          return true;
        }
        continue;
      }
      if (field === "softPackageReferences") {
        const soft = section.softPackageReferences as JsonObject | undefined;
        const items = Array.isArray(soft?.values) ? soft.values as unknown[] : [];
        if (items.length > 0) {
          items.pop();
          updatePageAfterTrim(soft!, "values");
          updateDependencyPageAfterTrim(section);
          return true;
        }
        continue;
      }
      const items = Array.isArray(section[field]) ? section[field] as unknown[] : [];
      if (items.length > 0) { items.pop(); updatePageAfterTrim(section, field); return true; }
    }
    if (sections) {
      for (const name of ["dependencies", "assetRegistry", "exports", "imports", "names"] as const) {
        if (name in sections) {
          delete sections[name];
          omittedSections.push(name);
          return true;
        }
      }
      if ("left" in sections || "right" in sections) {
        delete sections.left;
        delete sections.right;
        omittedSections.push("diffSections");
        return true;
      }
    }
    return false;
  };
  while (byteLength(value) > options.effectiveMaxBytes && trimOne()) {
    (value.budget as JsonObject).truncated = true;
    value.truncated = true;
  }
  const budget = value.budget as JsonObject;
  budget.estimatedBytes = byteLength(value);
  if (Number(budget.estimatedBytes) > options.effectiveMaxBytes) {
    throw failure("asset_output_budget_exceeded", "The compact Asset summary cannot fit inside the requested output budget.");
  }
  return value;
}

function summary(path: string, requestedEngineVersion: string, options: OutputOptions): JsonObject {
  if (!requestedEngineVersion.startsWith("5.3")) throw failure("asset_version_incompatible", "This bundled Asset Worker is pinned to UE 5.3 package headers.");
  const resolved = headerPath(path);
  const stat = statSync(resolved.headerPath);
  if (stat.size < 32) throw failure("asset_corrupt", "Asset package is too small to contain a package summary.");
  const bytes = readPrefix(resolved.headerPath, MAX_HEADER_BYTES);
  const parsed = parseSummary(new PackageReader(bytes));
  const names = parseNameList(bytes, parsed);
  const nameSection = parseNames(bytes, parsed, options);
  const imports = tableDescriptor(bytes, parsed, parsed.importCount, parsed.importOffset, options);
  const exports = tableDescriptor(bytes, parsed, parsed.exportCount, parsed.exportOffset, options);
  const assetRegistry = parseAssetRegistry(bytes, parsed, options);
  const dependencies = parseDependencies(bytes, parsed, names, options);
  const value: JsonObject = {
    schema: "ue.local-asset-summary.v3",
    available: true,
    assetPath: path,
    packageHeaderPath: resolved.headerPath,
    extension: extname(path).toLowerCase(),
    sizeBytes: statSync(path).size,
    headerSizeBytes: parsed.totalHeaderSize,
    packageSummary: compactPackageSummary(parsed.packageSummary),
    headerSha256: sha256(bytes.subarray(0, parsed.totalHeaderSize)),
    counts: {
      names: parsed.nameCount,
      imports: parsed.importCount,
      exports: parsed.exportCount,
      assetRegistryObjects: Number(assetRegistry.objectCount ?? 0),
      assetRegistryTags: Number(assetRegistry.totalTagCount ?? 0),
      dependencyEdges: Number((dependencies.dependsMap as JsonObject)?.edges ?? 0),
      softPackageReferences: parsed.softPackageReferencesCount,
    },
    sectionDigests: {
      names: nameSection.normalizedSha256,
      imports: imports.sha256 ?? null,
      exports: exports.sha256 ?? null,
      assetRegistry: assetRegistry.sha256 ?? null,
      dependencies: {
        dependsMap: (dependencies.dependsMap as JsonObject)?.sha256 ?? null,
        softPackageReferences: (dependencies.softPackageReferences as JsonObject)?.normalizedSha256 ?? null,
      },
    },
    companion: resolved.companion,
  };
  const sections: JsonObject = {};
  if (options.sections.has("names")) sections.names = nameSection;
  if (options.sections.has("imports")) sections.imports = imports;
  if (options.sections.has("exports")) sections.exports = exports;
  if (options.sections.has("assetRegistry")) sections.assetRegistry = assetRegistry;
  if (options.sections.has("dependencies")) sections.dependencies = dependencies;
  if (Object.keys(sections).length > 0) value.sections = sections;
  value.normalizedDigest = normalizedDigest(value);
  return enforceBudget(value, options);
}

function compactSide(value: JsonObject): JsonObject {
  return {
    assetPath: value.assetPath,
    sizeBytes: value.sizeBytes,
    headerSizeBytes: value.headerSizeBytes,
    headerSha256: value.headerSha256,
    normalizedDigest: value.normalizedDigest,
    counts: value.counts,
    sectionDigests: value.sectionDigests,
    packageSummary: value.packageSummary,
    companion: value.companion ?? null,
  };
}

function diff(left: JsonObject, right: JsonObject, options: OutputOptions): JsonObject {
  const leftDigests = left.sectionDigests as JsonObject;
  const rightDigests = right.sectionDigests as JsonObject;
  const fields = ["packageSummary", "names", "imports", "exports", "assetRegistry", "dependencies", "companion"];
  const changes = fields.filter((field) => {
    if (field === "packageSummary" || field === "companion") return JSON.stringify(left[field] ?? null) !== JSON.stringify(right[field] ?? null);
    return JSON.stringify(leftDigests[field] ?? null) !== JSON.stringify(rightDigests[field] ?? null);
  });
  const value: JsonObject = {
    schema: "ue.local-asset-diff.v3",
    left: compactSide(left),
    right: compactSide(right),
    equalNormalized: left.normalizedDigest === right.normalizedDigest,
    equalHeader: left.headerSha256 === right.headerSha256,
    sizeDelta: Number(right.sizeBytes) - Number(left.sizeBytes),
    changedSections: changes,
  };
  if (options.detail !== "summary") {
    value.sections = {
      left: left.sections ?? {},
      right: right.sections ?? {},
    };
  }
  return enforceBudget(value, options);
}

let input = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => { input += chunk; if (input.length > 1024 * 1024) process.exit(2); });
process.stdin.on("end", () => {
  try {
    const request = JSON.parse(input) as Request;
    const options = outputOptions(request);
    const first = summary(request.assetPath, request.engineVersion, options);
    const data = request.operation === "production.asset.package.diff"
      ? (() => {
          if (typeof request.otherAssetPath !== "string" || !existsSync(request.otherAssetPath)) throw failure("asset_diff_target_required", "otherAssetPath is required for package diff.");
          return diff(first, summary(request.otherAssetPath, request.engineVersion, options), options);
        })()
      : first;
    process.stdout.write(JSON.stringify({ ok: true, data }));
  } catch (error) {
    const payload = error as { code?: string; message?: string };
    process.stdout.write(JSON.stringify({ ok: false, error: { code: payload.code ?? "asset_worker_failed", message: payload.message ?? String(error) } }));
    process.exitCode = 1;
  }
});
