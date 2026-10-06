#!/usr/bin/env python3
"""Static contract checks for the Niagara direct-GDF first-frame guard."""

from __future__ import annotations

import pathlib
import os
import unittest


def _find_engine_root() -> pathlib.Path:
    configured = os.environ.get("UEAI_ENGINE_ROOT")
    if configured:
        candidate = pathlib.Path(configured).resolve()
        if (candidate / "Plugins" / "FX" / "Niagara").is_dir():
            return candidate

    for parent in pathlib.Path(__file__).resolve().parents:
        if (parent / "Plugins" / "FX" / "Niagara").is_dir():
            return parent
        if (parent / "Engine" / "Plugins" / "FX" / "Niagara").is_dir():
            return parent / "Engine"
    raise FileNotFoundError(
        "UEAI_ENGINE_ROOT is not configured and the Unreal Engine root could not be found"
    )


ENGINE_DISPATCH = (
    _find_engine_root()
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

    def test_guard_is_local_to_the_cache_validation_path(self) -> None:
        start = self.source.index("const bool bGDFResourcesReady =")
        assignment = self.source.index(
            "CachedGDFData.bCacheValid\t\t\t= bGDFResourcesReady;", start
        )
        self.assertGreater(assignment, start)
        self.assertLess(assignment - start, 4096)
        self.assertNotIn("CachedGDFData.bCacheValid\t\t\t= true;", self.source[start:assignment])


if __name__ == "__main__":
    unittest.main()
