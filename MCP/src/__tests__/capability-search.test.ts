import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";

import type {
  CapabilityDescriptor,
  CapabilitySearchMetadata,
} from "../capability-catalog.js";
import {
  compareCapabilityIds,
  matchCapabilitySearch,
} from "../capability-search.js";

interface SearchFixtureDocument {
  id: string;
  description: string;
  search?: CapabilitySearchMetadata;
}

interface ExpectedMatch {
  id: string;
  score: number;
  matchedFields: Array<
    "id" | "title" | "keywords" | "aliases" | "description"
  >;
  matchedTokens: string[];
}

interface SearchVector {
  query: string;
  matches: ExpectedMatch[];
}

const fixture = JSON.parse(
  readFileSync(
    new URL(
      "../../../Resources/Contracts/capability-search-v1.json",
      import.meta.url,
    ),
    "utf8",
  ),
) as {
  documents: SearchFixtureDocument[];
  vectors: SearchVector[];
};

test("matches the shared capability search golden vectors", () => {
  for (const vector of fixture.vectors) {
    const actual = fixture.documents
      .map((document) => {
        const match = matchCapabilitySearch(
          vector.query,
          document as Pick<
            CapabilityDescriptor,
            "id" | "description" | "search"
          >,
        );
        return match === undefined
          ? undefined
          : {
              id: document.id,
              ...match,
            };
      })
      .filter((value): value is ExpectedMatch => value !== undefined)
      .sort(
        (left, right) =>
          right.score - left.score ||
          compareCapabilityIds(left.id, right.id),
      );
    assert.deepEqual(actual, vector.matches, vector.query);
  }
});
