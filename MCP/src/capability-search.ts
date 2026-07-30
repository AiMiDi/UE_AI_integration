import type { CapabilityDescriptor } from "./capability-catalog.js";

export interface CapabilitySearchMatch {
  score: number;
  matchedFields: Array<
    "id" | "title" | "keywords" | "aliases" | "description"
  >;
  matchedTokens: string[];
}

export function compareCapabilityIds(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

function isAsciiUpper(code: number): boolean {
  return code >= 0x41 && code <= 0x5a;
}

function isAsciiLower(code: number): boolean {
  return code >= 0x61 && code <= 0x7a;
}

function isAsciiDigit(code: number): boolean {
  return code >= 0x30 && code <= 0x39;
}

function lowerAscii(text: string): string {
  let result = "";
  for (let index = 0; index < text.length; ++index) {
    const code = text.charCodeAt(index);
    result += String.fromCharCode(
      isAsciiUpper(code) ? code - 0x41 + 0x61 : code,
    );
  }
  return result;
}

function trimAsciiWhitespace(text: string): string {
  let begin = 0;
  while (begin < text.length && text.charCodeAt(begin) <= 0x20) {
    ++begin;
  }
  let end = text.length;
  while (end > begin && text.charCodeAt(end - 1) <= 0x20) {
    --end;
  }
  return text.slice(begin, end);
}

export function tokenizeCapabilitySearch(text: string): string[] {
  const tokens: string[] = [];
  let current = "";
  const flush = (): void => {
    if (current.length !== 0 && !tokens.includes(current)) {
      tokens.push(current);
    }
    current = "";
  };

  for (let index = 0; index < text.length; ++index) {
    const code = text.charCodeAt(index);
    if (code >= 0x80) {
      current += text[index];
      continue;
    }
    const alphanumeric =
      isAsciiUpper(code) || isAsciiLower(code) || isAsciiDigit(code);
    if (!alphanumeric) {
      flush();
      continue;
    }
    if (isAsciiUpper(code) && current.length !== 0) {
      const previous = text.charCodeAt(index - 1);
      const next =
        index + 1 < text.length ? text.charCodeAt(index + 1) : 0;
      if (
        isAsciiLower(previous) ||
        isAsciiDigit(previous) ||
        (isAsciiUpper(previous) && isAsciiLower(next))
      ) {
        flush();
      }
    }
    current += String.fromCharCode(
      isAsciiUpper(code) ? code - 0x41 + 0x61 : code,
    );
  }
  flush();
  return tokens;
}

function fieldScore(
  token: string,
  text: string | undefined,
  exactScore: number,
  substringScore: number,
): number {
  if (text === undefined) {
    return 0;
  }
  const tokens = tokenizeCapabilitySearch(text);
  if (tokens.includes(token)) {
    return exactScore;
  }
  return tokens.some((candidate) => candidate.includes(token))
    ? substringScore
    : 0;
}

export function matchCapabilitySearch(
  query: string,
  capability: Pick<
    CapabilityDescriptor,
    "id" | "description" | "search"
  >,
): CapabilitySearchMatch | undefined {
  const trimmedQuery = trimAsciiWhitespace(query);
  const queryTokens = tokenizeCapabilitySearch(trimmedQuery);
  if (queryTokens.length === 0) {
    return {
      score: 0,
      matchedFields: [],
      matchedTokens: [],
    };
  }
  if (lowerAscii(trimmedQuery) === lowerAscii(capability.id)) {
    return {
      score: 100000,
      matchedFields: ["id"],
      matchedTokens: queryTokens,
    };
  }

  const fieldOrder = [
    "id",
    "title",
    "keywords",
    "aliases",
    "description",
  ] as const;
  const matchedFields = new Set<(typeof fieldOrder)[number]>();
  let score = 0;
  for (const token of queryTokens) {
    let bestScore = 0;
    let bestField: (typeof fieldOrder)[number] = "description";
    const consider = (
      candidateScore: number,
      field: (typeof fieldOrder)[number],
    ): void => {
      if (candidateScore > bestScore) {
        bestScore = candidateScore;
        bestField = field;
      }
    };
    consider(fieldScore(token, capability.id, 5000, 4500), "id");
    consider(
      fieldScore(token, capability.search?.title, 4000, 3500),
      "title",
    );
    for (const keyword of capability.search?.keywords ?? []) {
      consider(fieldScore(token, keyword, 3000, 2500), "keywords");
    }
    for (const alias of capability.search?.aliases ?? []) {
      consider(fieldScore(token, alias, 3000, 2500), "aliases");
    }
    consider(
      fieldScore(token, capability.description, 1000, 500),
      "description",
    );
    if (bestScore === 0) {
      return undefined;
    }
    score += bestScore;
    matchedFields.add(bestField);
  }
  return {
    score,
    matchedFields: fieldOrder.filter((field) => matchedFields.has(field)),
    matchedTokens: queryTokens,
  };
}
