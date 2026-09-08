#!/usr/bin/env python3
"""Static contract checks for the Niagara direct-GDF first-frame guard."""

from __future__ import annotations

import pathlib
import unittest


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[4]
ENGINE_DISPATCH = (
    PROJECT_ROOT
    / "unrealengine"
    / "Engine"
    / "Plugins"
    / "FX"
    / "Niagara"
    / "Source"
    / "Niagara"
    / "Private"
    / "NiagaraGpuComputeDispatch.cpp"
)


class NiagaraGDFReadinessGateSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = ENGINE_DISPATCH.read_text(encoding="utf-8")

    def test_cache_valid_requires_complete_direct_gdf_resources(self) -> None:
        start = self.source.index("const bool bGDFResourcesReady =")
        end = self.source.index("CachedGDFData.PageAtlasTexture", start)
        guard = self.source[start:end]

        for token in (
            "NumGlobalSDFClipmaps > 0",
            "ClipmapSizeInPages > 0",
            "GlobalDFResolution > 0.0f",
            "MaxDFAOConeDistance > 0.0f",
            "PageAtlasTexture != nullptr",
            "PageTableTexture != nullptr",
            "MipTexture != nullptr",
        ):
            with self.subTest(token=token):
                self.assertIn(token, guard)

        self.assertIn("CachedGDFData.bCacheValid\t\t\t= bGDFResourcesReady;", guard)
        self.assertNotIn("CachedGDFData.bCacheValid\t\t\t= true;", guard)

    def test_guard_is_explicitly_scoped_to_the_silverpalace_change(self) -> None:
        start = self.source.index("const bool bGDFResourcesReady =")
        begin = self.source.rfind(
            "//++[SilverPalace] Begin add by wuziye 2026/09/05", 0, start
        )
        end = self.source.index(
            "//--[SilverPalace] End add by wuziye 2026/09/05", start
        )
        self.assertGreaterEqual(begin, 0)
        self.assertLess(begin, start)
        self.assertGreater(end, start)


if __name__ == "__main__":
    unittest.main()
