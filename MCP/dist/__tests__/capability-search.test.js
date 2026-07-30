import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { compareCapabilityIds, matchCapabilitySearch, } from "../capability-search.js";
const fixture = JSON.parse(readFileSync(new URL("../../../Resources/Contracts/capability-search-v1.json", import.meta.url), "utf8"));
test("matches the shared capability search golden vectors", () => {
    for (const vector of fixture.vectors) {
        const actual = fixture.documents
            .map((document) => {
            const match = matchCapabilitySearch(vector.query, document);
            return match === undefined
                ? undefined
                : {
                    id: document.id,
                    ...match,
                };
        })
            .filter((value) => value !== undefined)
            .sort((left, right) => right.score - left.score ||
            compareCapabilityIds(left.id, right.id));
        assert.deepEqual(actual, vector.matches, vector.query);
    }
});
//# sourceMappingURL=capability-search.test.js.map