#!/usr/bin/env python3
"""Static contract checks for Niagara collision graph classification.

These checks intentionally do not require an Unreal Editor process. They keep
the inspect and detailed-audit token sets aligned while the optional Niagara
feature is unavailable in a local build.
"""

from __future__ import annotations

import pathlib
import json
import re
import unittest


PLUGIN_ROOT = pathlib.Path(__file__).resolve().parents[1]
GRAPH_SOURCE = (
    PLUGIN_ROOT
    / "Source"
    / "UE_AI_integration"
    / "Private"
    / "Domains"
    / "Content"
    / "Command"
    / "Niagara_Graph.cpp"
)
AUDIT_SOURCE = GRAPH_SOURCE.with_name("Niagara_Graph_Audit.cpp")
ADVANCED_SOURCE = GRAPH_SOURCE.with_name("Niagara_Graph_Advanced.cpp")
MODULE_SOURCE = GRAPH_SOURCE.with_name("Niagara_Graph_Module.cpp")
EDIT_SOURCE = GRAPH_SOURCE.with_name("Niagara_Graph_Edit.cpp")
PROJECT_AUDIT_SOURCE = GRAPH_SOURCE.with_name(
    "Niagara_CollisionProjectAudit.cpp"
)
CAPABILITY_MANIFEST = PLUGIN_ROOT / "Resources" / "Capabilities" / "content.json"


def section(source: str, start: str, end: str) -> str:
    start_index = source.index(start)
    end_index = source.index(end, start_index)
    return source[start_index:end_index]


class NiagaraGraphClassificationSourceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.graph = GRAPH_SOURCE.read_text(encoding="utf-8")
        cls.audit = AUDIT_SOURCE.read_text(encoding="utf-8")
        cls.advanced = ADVANCED_SOURCE.read_text(encoding="utf-8")
        cls.module = MODULE_SOURCE.read_text(encoding="utf-8")
        cls.edit = EDIT_SOURCE.read_text(encoding="utf-8")
        cls.project_audit = PROJECT_AUDIT_SOURCE.read_text(encoding="utf-8")
        cls.graph_classifier = section(
            cls.graph,
            "void ClassifyNode(",
            "TSharedRef<FJsonObject> SerializePin",
        )
        cls.audit_classifier = section(
            cls.audit,
            "void ClassifyFunctionNode(",
            "bool IsNestedUnder",
        )

    def test_collision_aliases_are_classified_by_both_paths(self) -> None:
        aliases = (
            "CollisionQuery",
            "PerformCollisionQuerySyncCPU",
            "PerformCollisionQueryAsyncCPU",
            "Collision.Collision",
            "CollisionQuery.CollisionQuery",
            "/Collision/Collision",
            "/Collision/CollisionQuery",
        )
        for token in aliases:
            with self.subTest(token=token):
                self.assertIn(token, self.graph_classifier)
                self.assertIn(token, self.audit_classifier)

    def test_collision_module_provider_matching_is_exact(self) -> None:
        source_classifier_pairs = (
            (
                self.graph,
                self.graph_classifier,
                "FString GetPinPath(",
                "bOutOrdinaryCollisionQuery =",
            ),
            (
                self.audit,
                self.audit_classifier,
                "void AddUniqueString(",
                "OutClassification.bOrdinaryCollisionQuery =",
            ),
        )
        for source, classifier, helper_end, ordinary_start in source_classifier_pairs:
            with self.subTest(classifier=classifier[:24]):
                operation_helper = section(
                    source,
                    "bool HasExactOperationPathOrName(",
                    "bool HasExactCollisionModuleIdentity(",
                )
                self.assertIn("DottedToken", operation_helper)
                helper = section(
                    source,
                    "bool HasExactCollisionModuleIdentity(",
                    helper_end,
                )
                self.assertIn("DottedModuleName", helper)
                self.assertIn("AssetPathSuffix", helper)
                self.assertIn("FunctionScript.EndsWith", helper)
                provider = section(
                    classifier,
                    "const bool bCollisionModule =",
                    "const bool bCollisionResponseOnly =",
                )
                self.assertIn("HasExactCollisionModuleIdentity", provider)
                self.assertIn('TEXT("Collision.Collision")', provider)
                self.assertIn('TEXT("CollisionQuery.CollisionQuery")', provider)
                self.assertIn('TEXT("/Collision/Collision.Collision")', provider)
                self.assertIn(
                    'TEXT("/Collision/CollisionQuery.CollisionQuery")',
                    provider,
                )
                self.assertIn("bCollisionQueryAndResponseModule", provider)
                self.assertNotIn("ContainsAnyToken", provider)

            ordinary = section(
                classifier,
                ordinary_start,
                "if ("
                + (
                    "bOutOrdinaryCollisionQuery)"
                    if ordinary_start.startswith("bOut")
                    else "OutClassification.bOrdinaryCollisionQuery)"
                ),
            )
            self.assertIn("!bCollisionResponseOnly", ordinary)

    def test_response_only_collision_modules_are_not_ordinary_providers(self) -> None:
        response_tokens = (
            "CollisionRest",
            "CollisionLinearImpulse",
        )
        classifier_pairs = (
            (self.graph_classifier, "bOutOrdinaryCollisionQuery =", "if (bOutOrdinaryCollisionQuery)"),
            (
                self.audit_classifier,
                "OutClassification.bOrdinaryCollisionQuery =",
                "if (OutClassification.bOrdinaryCollisionQuery)",
            ),
        )
        for classifier, ordinary_start, ordinary_end in classifier_pairs:
            with self.subTest(classifier=classifier[:24]):
                response = section(
                    classifier,
                    "const bool bCollisionResponseOnly =",
                    "const bool bCollisionResponse =",
                )
                for token in response_tokens:
                    self.assertIn(f'TEXT("{token}")', response)
                self.assertIn("bCollisionResponseOnly", response)
                ordinary = section(
                    classifier,
                    ordinary_start,
                    ordinary_end,
                )
                self.assertIn("!bCollisionResponseOnly", ordinary)

    def test_collision_query_and_response_remains_an_exact_provider(self) -> None:
        for classifier in (self.graph_classifier, self.audit_classifier):
            with self.subTest(classifier=classifier[:24]):
                provider = section(
                    classifier,
                    "const bool bCollisionModule =",
                    "const bool bCollisionResponseOnly =",
                )
                self.assertIn(
                    'TEXT("CollisionQueryAndResponse")',
                    provider,
                )
                self.assertIn(
                    "|| bCollisionQueryAndResponseModule",
                    provider,
                )
                response_only = section(
                    classifier,
                    "const bool bCollisionResponseOnly =",
                    "const bool bCollisionResponse =",
                )
                self.assertNotIn(
                    'TEXT("CollisionQueryAndResponse")',
                    response_only,
                )

    def test_deprecated_async_gpu_aliases_are_classified_by_both_paths(self) -> None:
        aliases = (
            "IssueAsyncRayTraceGpu",
            "CreateAsyncRayTraceGpu",
            "ReserveAsyncRayTraceGpu",
            "ReserveRayTraceIndex",
            "ReadAsyncRayTraceGpu",
            "IsAsyncGpuTraceReadyGpu",
            "IsHardwareRayTracingEnabledGpu",
            "IsHardwareRayTracingAvailableGpu",
        )
        for token in aliases:
            with self.subTest(token=token):
                self.assertIn(token, self.graph_classifier)
                self.assertIn(token, self.audit_classifier)

    def test_distance_field_operations_are_explicit(self) -> None:
        operations = (
            "NiagaraDistanceFieldCollisions",
            "QueryMeshDistanceFieldGPU",
            "QueryDistanceField",
            "GetElementPointMeshDistanceFieldNoNormal",
            "GetClosestPointMeshDistanceField",
            "GetClosestPointMeshDistanceFieldAccurate",
            "GetClosestPointMeshDistanceFieldNoNormal",
            "GetMaxEncodedDistanceMeshDistanceField",
            "Fn_SphereTraceDistanceField",
            "AvoidDistanceFieldSurfaces_GPU",
            "MoveToNearestDistanceFieldSurface_GPU",
            "FindNearestDistanceFieldSurface_GPU",
            "RayTraceDistanceField_GPU",
            "ReadDistanceField_GPU",
            "SphereTraceDistanceField_GPU",
            "SphereCast_GlobalDistanceField",
            "CalculateTheGlobalDistanceFieldSurfaceNormal_GPU",
            "CalculateTheGlobalDistanceFieldSurfaceNormal",
            "CalculateGlobalDistanceFieldSurfaceNormal",
            "CalculateGlobalDistanceFieldIsoSurfaceNormal",
            "FindNearestDistanceFieldSurface",
            "FindNearestDistanceFieldIsoSurface",
            "FindDistanceFieldVolumeTextureGradient",
            "CalculateA_VolumeTexturesDistanceFieldGradient",
            "GetClosestPointMeshDistanceFieldAccurate",
        )
        for token in operations:
            with self.subTest(token=token):
                self.assertIn(token, self.graph_classifier)
                self.assertIn(token, self.audit_classifier)

    def test_non_collision_distance_field_interfaces_are_not_policy_classified(self) -> None:
        for token in (
            "GetLocalWindFieldForceAndDistanceField",
            "GetLocalWindFieldDistanceFieldGradient",
        ):
            with self.subTest(token=token):
                self.assertNotIn(token, self.graph_classifier)
                self.assertNotIn(token, self.audit_classifier)

    def test_distance_field_detection_is_not_a_bare_substring(self) -> None:
        graph_distance_tokens = section(
            self.graph_classifier,
            "const bool bNamedDistanceFieldCollision =",
            "const bool bCollisionPathDistanceField =",
        )
        audit_distance_tokens = section(
            self.audit_classifier,
            "const bool bNamedDistanceFieldCollision =",
            "const bool bCollisionPathDistanceField =",
        )
        self.assertNotIn('TEXT("DistanceField"),', graph_distance_tokens)
        self.assertNotIn('TEXT("Distance Field"),', graph_distance_tokens)
        self.assertNotIn('TEXT("DistanceField"),', audit_distance_tokens)
        self.assertNotIn('TEXT("Distance Field"),', audit_distance_tokens)

    def test_unsuffixed_distance_field_helpers_use_exact_identity_fallback(self) -> None:
        for classifier in (self.graph_classifier, self.audit_classifier):
            with self.subTest(classifier=classifier[:24]):
                exact_helpers = section(
                    classifier,
                    "const bool bExactDistanceFieldHelper =",
                    "const bool bCollisionPathDistanceField =",
                )
                self.assertIn("HasExactOperationPathOrName", exact_helpers)
                self.assertIn('TEXT("FindNearestDistanceFieldSurface")', exact_helpers)
                self.assertIn(
                    'TEXT("CalculateTheGlobalDistanceFieldSurfaceNormal")',
                    exact_helpers,
                )
                self.assertIn("bExactDistanceFieldHelper", classifier)

    def test_collision_events_are_not_ordinary_world_query_providers(self) -> None:
        provider_sections = (
            section(
                self.graph_classifier,
                "const bool bExplicitCpuCollisionQuery =",
                "const bool bCollisionResponse =",
            ),
            section(
                self.audit_classifier,
                "const bool bExplicitCpuQuery =",
                "const bool bCollisionResponse =",
            ),
        )
        for provider_section in provider_sections:
            for operation in (
                "GenerateCollisionEvent",
                "ReceiveCollisionEvent",
                "PBD_IntraParticleCollision",
            ):
                with self.subTest(operation=operation):
                    self.assertNotIn(operation, provider_section)

    def test_collision_helpers_are_inventory_only(self) -> None:
        helpers = (
            "AddRotationalVelocity",
            "CalculateLinePlaneInt",
            "DebugCollisionEvents",
            "FindTangentialVelocityOnSphere",
            "InitialRotationalVelocity",
            "InitializeNeighborGrid",
            "PBD_IntraParticleCollision",
            "PopulateNeighborGrid",
            "GenerateCollisionEvent",
            "ReceiveCollisionEvent",
        )
        operation_inventory = section(
            self.audit,
            "void AddBestOperationMatch(",
            "FString FunctionSearchText(",
        )
        provider_sections = (
            section(
                self.graph_classifier,
                "const bool bExplicitCpuCollisionQuery =",
                "const bool bCollisionResponse =",
            ),
            section(
                self.audit_classifier,
                "const bool bExplicitCpuQuery =",
                "const bool bCollisionResponse =",
            ),
        )
        for helper in helpers:
            with self.subTest(helper=helper):
                self.assertIn(helper, operation_inventory)
                self.assertIn(helper, self.graph_classifier)
                self.assertIn(helper, self.audit_classifier)
                for provider_section in provider_sections:
                    self.assertNotIn(helper, provider_section)

    def test_depth_and_response_operations_are_retained(self) -> None:
        operations = (
            "SceneDepthTest",
            "PlaceParticlesOnDepthBuffer_GPU",
            "AlignParticlesWithCollisionPlane",
            "CollisionQueryAndResponse",
        )
        for token in operations:
            with self.subTest(token=token):
                self.assertIn(token, self.graph_classifier)
                self.assertIn(token, self.audit_classifier)

    def test_analytical_and_support_operations_are_inventory_tokens(self) -> None:
        operation_inventory = section(
            self.audit,
            "void AddBestOperationMatch(",
            "FString FunctionSearchText(",
        )
        analytical = (
            "AnalyticalCollisionQuery",
            "PlaneSphereCollisionDetection",
        )
        support = (
            "RandomizeCollisionNormals",
            "SetupRigidBodyDI",
            "RayTrace",
        )
        for token in (*analytical, *support):
            with self.subTest(token=token):
                self.assertIn(token, operation_inventory)
                self.assertIn(token, self.graph_classifier)
                self.assertIn(token, self.audit_classifier)
        self.assertIn("analyticalCollision", self.graph_classifier)
        self.assertIn("collisionSupport", self.graph_classifier)
        self.assertIn("analyticalCollision", self.audit_classifier)
        self.assertIn("collisionSupport", self.audit_classifier)

    def test_analytical_and_support_nodes_are_excluded_from_ordinary_provider(self) -> None:
        for classifier in (self.graph_classifier, self.audit_classifier):
            with self.subTest(classifier=classifier[:24]):
                self.assertIn("!bAnalyticalCollision", classifier)
                self.assertIn("!bCollisionSupport", classifier)
        policy = section(
            self.graph,
            "bool IsCollisionPolicyNodeManaged(",
            "bool CollisionPolicyNodeWouldChange(",
        )
        self.assertNotIn("bAnalyticalCollision", policy)
        self.assertNotIn("bCollisionSupport", policy)

    def test_neighbor_operations_are_inventory_only_collision_support(self) -> None:
        neighbor_operations = (
            "CalculateNeighbors",
            "SampleNeighbors",
            "NeighborBehaviours",
            "NeighborBehaviors",
        )
        operation_inventory = section(
            self.audit,
            "void AddBestOperationMatch(",
            "FString FunctionSearchText(",
        )
        provider_sections = (
            section(
                self.graph_classifier,
                "const bool bExplicitCpuCollisionQuery =",
                "const bool bCollisionResponse =",
            ),
            section(
                self.audit_classifier,
                "const bool bExplicitCpuQuery =",
                "const bool bCollisionResponse =",
            ),
        )
        for operation in neighbor_operations:
            with self.subTest(operation=operation):
                self.assertIn(operation, operation_inventory)
                self.assertIn(operation, self.graph_classifier)
                self.assertIn(operation, self.audit_classifier)
                for provider_section in provider_sections:
                    self.assertNotIn(operation, provider_section)
        self.assertIn("collisionSupport", self.graph_classifier)
        self.assertIn("collisionSupport", self.audit_classifier)

    def test_collision_audit_serializes_analytical_and_support_counts(self) -> None:
        self.assertIn('TEXT("analyticalCollision")', self.audit)
        self.assertIn('TEXT("collisionSupport")', self.audit)
        self.assertIn('TEXT("analyticalCollisionNodeCount")', self.audit)
        self.assertIn('TEXT("collisionSupportNodeCount")', self.audit)

    def test_collision_query_classification_is_mutually_exclusive(self) -> None:
        self.assertIn("!bOutDistanceField", self.graph_classifier)
        self.assertIn("!bOutAsyncGpuTrace", self.graph_classifier)
        self.assertIn("!bDepthBufferCollisionQuery", self.graph_classifier)
        self.assertIn("!bDistanceField", self.audit_classifier)
        self.assertIn("!bAsyncGpuTrace", self.audit_classifier)
        self.assertIn("!bDepthBufferQuery", self.audit_classifier)

    def test_audit_operation_inventory_contains_exact_names(self) -> None:
        operation_inventory = section(
            self.audit,
            "void AddBestOperationMatch(",
            "FString FunctionSearchText(",
        )
        for token in (
            "Collision.Collision",
            "CollisionQuery.CollisionQuery",
            "NiagaraDistanceFieldCollisions",
            "MoveToNearestDistanceFieldSurface_GPU",
            "FindNearestDistanceFieldSurface_GPU",
            "QueryDistanceField",
            "GetElementPointMeshDistanceFieldNoNormal",
            "GetClosestPointMeshDistanceField",
            "GetClosestPointMeshDistanceFieldNoNormal",
            "GetMaxEncodedDistanceMeshDistanceField",
            "Fn_SphereTraceDistanceField",
            "SphereTraceDistanceField_GPU",
            "CalculateTheGlobalDistanceFieldSurfaceNormal_GPU",
            "CalculateTheGlobalDistanceFieldSurfaceNormal",
            "FindNearestDistanceFieldSurface",
            "CalculateA_VolumeTexturesDistanceFieldGradient",
            "AddRotationalVelocity",
            "CalculateLinePlaneInt",
            "DebugCollisionEvents",
            "FindTangentialVelocityOnSphere",
            "InitialRotationalVelocity",
            "InitializeNeighborGrid",
            "PBD_IntraParticleCollision",
            "PopulateNeighborGrid",
            "GenerateCollisionEvent",
            "ReceiveCollisionEvent",
            "IssueAsyncRayTraceGpu",
            "ReserveAsyncRayTraceGpu",
            "ReserveRayTraceIndex",
            "IsAsyncGpuTraceReadyGpu",
            "IsHardwareRayTracingEnabledGpu",
            "IsHardwareRayTracingAvailableGpu",
            "SceneDepthTest",
        ):
            with self.subTest(token=token):
                self.assertIn(token, operation_inventory)

    def test_rigid_mesh_and_physics_asset_operations_are_inventory_tokens(self) -> None:
        rigid_mesh_operations = (
            "FindActors",
            "GetNumBoxes",
            "GetNumSpheres",
            "GetNumCapsules",
            "GetNumElements",
            "GetBoxElementsStartIndex",
            "GetSphereElementsStartIndex",
            "GetCapsuleElementsStartIndex",
            "GetSphereRadius",
            "GetCapsuleSize",
            "GetBoxSize",
            "IsWorldPositionInsideCombinedBounds",
            "GetClosestElement",
            "GetElementPoint",
            "GetElementPointMeshDistanceFieldNoNormal",
            "GetElementDistance",
            "GetClosestPoint",
            "GetClosestPointSimple",
            "GetClosestDistance",
            "GetClosestPointMeshDistanceField",
            "GetClosestPointMeshDistanceFieldAccurate",
            "GetClosestPointMeshDistanceFieldNoNormal",
            "GetMaxEncodedDistanceMeshDistanceField",
        )
        physics_asset_operations = (
            "GetNumBoxes",
            "GetNumSpheres",
            "GetNumCapsules",
            "GetClosestElement",
            "GetElementPoint",
            "GetElementDistance",
            "GetClosestPoint",
            "GetClosestDistance",
            "GetRestDistance",
            "GetTexturePoint",
            "GetProjectionPoint",
        )
        operation_inventory = section(
            self.audit,
            "void AddBestOperationMatch(",
            "FString FunctionSearchText(",
        )
        for token in (*rigid_mesh_operations, *physics_asset_operations):
            with self.subTest(token=token):
                self.assertIn(token, operation_inventory)
                self.assertIn(token, self.graph_classifier)

    def test_rigid_mesh_distance_field_variants_are_provider_tokens(self) -> None:
        graph_rigid_provider = section(
            self.graph_classifier,
            "const bool bRigidMeshOperation =",
            "const bool bRigidMeshSharedOperation =",
        )
        audit_rigid_provider = section(
            self.audit,
            "bool HasRigidMeshCollisionOperation(",
            "bool HasPhysicsAssetOperation(",
        )
        for provider in (graph_rigid_provider, audit_rigid_provider):
            with self.subTest(provider=provider[:24]):
                self.assertIn(
                    'TEXT("GetClosestPointMeshDistanceFieldAccurate")',
                    provider,
                )
                self.assertIn(
                    'TEXT("GetClosestPointMeshDistanceFieldNoNormal")',
                    provider,
                )

    def test_rigid_mesh_and_physics_asset_kinds_are_explicit(self) -> None:
        for source in (self.graph_classifier, self.audit_classifier):
            self.assertIn("rigidMeshCollisionQuery", source)
            self.assertIn("physicsAsset", source)
        self.assertIn("!bRigidMeshCollisionQuery", self.graph_classifier)
        self.assertIn("!bPhysicsAsset", self.graph_classifier)
        self.assertIn("!OutClassification.bRigidMeshCollisionQuery", self.audit_classifier)
        self.assertIn("!OutClassification.bPhysicsAsset", self.audit_classifier)
        self.assertIn("GetCollisionDataInterfaceKind", self.audit)
        self.assertIn("IsSupportedCollisionDataInterface", self.audit)
        self.assertIn('TEXT("operationInterfaceKinds")', self.graph)
        self.assertIn('TEXT("operationInterfaceKinds")', self.audit)

    def test_collision_audit_collects_rigid_mesh_and_physics_asset_interfaces(self) -> None:
        self.assertIn("NiagaraDataInterfacePhysicsAsset.h", self.audit)
        self.assertIn("bool IsClassNamed(const UObject* Object", self.audit)
        self.assertIn(
            'FName(TEXT("NiagaraDataInterfaceRigidMeshCollisionQuery"))',
            self.audit,
        )
        self.assertNotIn(
            "UNiagaraDataInterfaceRigidMeshCollisionQuery::StaticClass()",
            self.audit,
        )
        self.assertIn("RigidMeshDataInterfaceCount", self.audit)
        self.assertIn("PhysicsAssetDataInterfaceCount", self.audit)
        self.assertIn('TEXT("rigidMeshCollisionQueryDataInterfaceCount")', self.audit)
        self.assertIn('TEXT("physicsAssetDataInterfaceCount")', self.audit)
        self.assertIn('TEXT("interfaceKinds")', self.audit)

    def test_operation_inventory_schema_is_versioned(self) -> None:
        self.assertIn('TEXT("operationInventorySchema")', self.audit)
        self.assertIn("ue.niagara-operation-inventory.v2", self.audit)

    def test_operation_inventory_writes_include_provider_kinds(self) -> None:
        inventory_body = section(
            self.audit,
            "void AppendDetailedCollisionAudit(",
            "void GetCollisionOperationNames(",
        )
        calls = re.findall(
            r"AddOperationInventoryEntry\((.*?)\);",
            inventory_body,
            flags=re.DOTALL,
        )
        self.assertEqual(2, len(calls))
        for call in calls:
            self.assertIn("InterfaceKinds", call)
            self.assertIn("SafeLimit", call)

    def test_operation_detail_api_preserves_name_compatibility(self) -> None:
        api = section(
            self.audit,
            "void GetCollisionOperationNames(",
            "void SetCollisionOperationInventory(",
        )
        self.assertIn("void GetCollisionOperationDetails(", api)
        self.assertIn(
            "GetCollisionOperationNames(FunctionCall, OutOperations, OutInterfaceKinds)",
            api,
        )
        self.assertEqual(1, api.count("ClassifyFunctionNode(FunctionCall, Classification)"))

    def test_collision_audit_operation_selector_is_exact_and_canonical(self) -> None:
        self.assertIn("MatchCollisionOperationSelector", self.audit)
        self.assertIn("OperationSelector", self.audit)
        self.assertIn("OperationSelector", self.graph)
        self.assertIn("Operation.Equals(NormalizedSelector", self.audit)
        self.assertNotIn("Operations.Contains(NormalizedSelector", self.audit)
        self.assertIn('TEXT("operationSelectorMatched")', self.audit)
        self.assertIn('TEXT("operationSelectorCanonical")', self.graph)

    def test_collision_query_selector_covers_both_ordinary_module_names(self) -> None:
        selector = section(
            self.audit,
            "bool MatchCollisionOperationSelector(",
            "void SetCollisionOperationInventory(",
        )
        self.assertIn('TEXT("CollisionQuery.CollisionQuery")', selector)
        self.assertIn('TEXT("Collision.Collision")', selector)
        self.assertNotIn('TEXT("CollisionQueryAndResponse")', selector)

    def test_manifest_exposes_optional_collision_operation_selector(self) -> None:
        manifest = json.loads(CAPABILITY_MANIFEST.read_text(encoding="utf-8"))
        capabilities = {
            capability["id"]: capability for capability in manifest["capabilities"]
        }
        properties = capabilities[
            "content.niagara.graph.collision.audit"
        ]["inputSchema"]["properties"]
        self.assertIn("operation", properties)
        self.assertEqual("string", properties["operation"]["type"])
        self.assertIn("exact", properties["operation"]["description"])

    def test_manifest_advertises_provider_specific_operation_inventory(self) -> None:
        manifest = json.loads(CAPABILITY_MANIFEST.read_text(encoding="utf-8"))
        capabilities = {
            capability["id"]: capability for capability in manifest["capabilities"]
        }
        inspect = capabilities["content.niagara.graph.inspect"]
        audit = capabilities["content.niagara.graph.collision.audit"]
        self.assertIn("RigidMeshCollisionQuery", inspect["description"])
        self.assertIn("PhysicsAsset", inspect["description"])
        self.assertIn("inventory-only helper/event/PBD", inspect["description"])
        self.assertIn("operation inventory", audit["description"])
        self.assertIn("inventory-only helper/event/PBD", audit["description"])
        self.assertIn("provider kinds", audit["description"])
        for capability in (inspect, audit):
            self.assertIn("search", capability)
            keywords = " ".join(capability["search"]["keywords"]).lower()
            self.assertIn("rigid mesh collision query", keywords)
            self.assertIn("physics asset", keywords)

    def test_manifest_aliases_cover_collision_provider_operations(self) -> None:
        manifest = json.loads(CAPABILITY_MANIFEST.read_text(encoding="utf-8"))
        capabilities = {
            capability["id"]: capability for capability in manifest["capabilities"]
        }
        required_aliases = {
            "CalculateA_VolumeTexturesDistanceFieldGradient",
            "QueryMeshDistanceFieldGPU",
            "FindNearestDistanceFieldSurface",
            "FindNearestDistanceFieldSurface_GPU",
            "MoveToNearestDistanceFieldSurface_GPU",
            "CalculateTheGlobalDistanceFieldSurfaceNormal",
            "CreateAsyncRayTraceGpu",
            "ReadAsyncRayTraceGpu",
            "AsyncGpuTrace",
            "AnalyticalCollisionQuery",
            "PlaneSphereCollisionDetection",
            "AddRotationalVelocity",
            "CalculateLinePlaneInt",
            "DebugCollisionEvents",
            "FindTangentialVelocityOnSphere",
            "InitialRotationalVelocity",
            "InitializeNeighborGrid",
            "PBD_IntraParticleCollision",
            "PopulateNeighborGrid",
            "GenerateCollisionEvent",
            "ReceiveCollisionEvent",
            "RandomizeCollisionNormals",
            "SetupRigidBodyDI",
            "RayTrace",
        }
        for capability_id in (
            "content.niagara.graph.inspect",
            "content.niagara.graph.collision.audit",
        ):
            with self.subTest(capability_id=capability_id):
                aliases = set(capabilities[capability_id]["search"]["aliases"])
                self.assertTrue(required_aliases <= aliases)
                keywords = " ".join(
                    capabilities[capability_id]["search"]["keywords"]
                ).lower()
                self.assertIn("async gpu trace", keywords)
                self.assertIn("analytical collision", keywords)
                self.assertIn("collision support", keywords)

    def test_project_audit_is_asset_registry_bounded_and_read_only(self) -> None:
        self.assertIn(
            'TEXT("content.niagara.collision.audit_project")',
            self.project_audit,
        )
        self.assertIn("GetAssetsByClass", self.project_audit)
        self.assertIn("WaitForCompletion", self.project_audit)
        self.assertIn("MaxSystemLimit", self.project_audit)
        self.assertIn("MaxGraphLimit", self.project_audit)
        self.assertIn("MaxDetailLimit", self.project_audit)
        self.assertIn("SetCollisionOperationInventory", self.project_audit)
        self.assertNotIn("MarkPackageDirty", self.project_audit)
        self.assertNotIn("RequestCompile", self.project_audit)

    def test_project_audit_manifest_contract(self) -> None:
        manifest = json.loads(CAPABILITY_MANIFEST.read_text(encoding="utf-8"))
        capabilities = {
            capability["id"]: capability for capability in manifest["capabilities"]
        }
        capability = capabilities["content.niagara.collision.audit_project"]
        self.assertEqual("query", capability["kind"])
        self.assertIn("AssetRegistry", capability["description"])
        properties = capability["inputSchema"]["properties"]
        for field in (
            "packagePath",
            "filter",
            "operation",
            "limit",
            "graphLimit",
            "detailLimit",
        ):
            self.assertIn(field, properties)
        self.assertEqual("project", capability["dsl"]["scopeKinds"][0])
        self.assertFalse(capability["traits"]["destructive"])
        self.assertEqual("read", capability["effects"]["asset"])
        aliases = set(capability["search"]["aliases"])
        self.assertIn("CollisionQuery", aliases)
        self.assertIn("MoveToNearestDistanceFieldSurface_GPU", aliases)

    def test_project_audit_is_registered_with_content_tools(self) -> None:
        subsystem = (
            PLUGIN_ROOT
            / "Source"
            / "UE_AI_integration"
            / "Private"
            / "UEAIIntegrationSubsystem.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "RegisterNiagaraCollisionProjectAuditTools",
            subsystem,
        )

    def test_audit_operation_inventory_prefers_exact_longest_match(self) -> None:
        self.assertIn("SignatureName.Equals(Token", self.audit)
        self.assertIn("FunctionName.Equals(Token", self.audit)
        self.assertIn("TokenLength > BestMatchLength", self.audit)
        self.assertNotIn(
            'TEXT("GetClosestPointMeshDistanceFieldAccurate"),\n\t\tTEXT("AlignParticlesWithCollisionPlane")',
            self.audit,
        )

    def test_collision_policy_manages_ordinary_nodes_only_for_hardware_rt(self) -> None:
        policy = section(
            self.graph,
            "bool IsCollisionPolicyNodeManaged(",
            "bool CollisionPolicyNodeWouldChange(",
        )
        self.assertIn("State.bOrdinaryCollisionQuery", policy)
        self.assertIn('TEXT("hardwareRayTracing")', policy)
        self.assertIn("bOutDesiredEnabled = !Policy.Equals", policy)

    def test_non_hardware_policies_leave_ordinary_state_unmanaged(self) -> None:
        policy = section(
            self.graph,
            "if (State.bOrdinaryCollisionQuery)",
            "return false;\n}",
        )
        self.assertIn("if (!Policy.Equals", policy)
        self.assertIn('TEXT("hardwareRayTracing")', policy)
        self.assertIn("return false;", policy)
        self.assertIn("bOutDesiredEnabled = false;", policy)

    def test_collision_policy_plan_separates_before_and_after_state(self) -> None:
        serializer = section(
            self.graph,
            "TSharedRef<FJsonObject> SerializeCollisionPolicyDataInterface(",
            "void AddCollisionPolicyRisk(",
        )
        plan = section(
            self.graph,
            "TSharedRef<FJsonObject> BuildCollisionPolicyPlanJson(",
            "bool ApplyCollisionPolicyNodeState(",
        )
        self.assertIn("const bool bAfterState", serializer)
        self.assertIn("provider", serializer)
        self.assertIn("AfterNodeValues", plan)
        self.assertIn("SerializeCollisionPolicyNode(Node, Data.Policy, false)", plan)
        self.assertIn("SerializeCollisionPolicyNode(Node, Data.Policy, true)", plan)
        self.assertIn("SerializeCollisionPolicyDataInterface(DI, false)", plan)
        self.assertIn("SerializeCollisionPolicyDataInterface(DI, true)", plan)

    def test_collision_policy_reports_preserved_ordinary_state(self) -> None:
        self.assertIn('TEXT("leaveUnchanged")', self.graph)
        self.assertIn('TEXT("disableEditablePreserveUnmanaged")', self.graph)
        self.assertNotIn('TEXT("leaveEnabled")', self.graph)
        self.assertIn(
            "distanceField and hybrid policies leave ordinary CollisionQuery modules unchanged",
            self.graph,
        )

    def test_collision_policy_counts_only_policy_managed_nodes(self) -> None:
        builder = section(
            self.graph,
            "bool BuildCollisionPolicyPlanData(",
            "TSharedRef<FJsonObject> BuildCollisionPolicyPlanJson(",
        )
        self.assertIn("const bool bManagedByPolicy", builder)
        self.assertIn(
            "GraphState.ManagedNodeCount += bManagedByPolicy ? 1 : 0;",
            builder,
        )
        self.assertIn(
            "if (!CollisionPolicyPlanWouldChangeState(OutData))",
            builder,
        )

    def test_collision_policy_receipt_tracks_only_mutations(self) -> None:
        apply_receipt = section(
            self.graph,
            "FCollisionPolicyReceipt Receipt;",
            "class FTool_NiagaraCollisionPolicyRollback",
        )
        self.assertIn("TSet<UNiagaraGraph*> AffectedGraphs", apply_receipt)
        self.assertIn("!IsCollisionPolicyNodeManaged", apply_receipt)
        self.assertIn("!DIState.bEditable || !DIState.bProviderChanged", apply_receipt)
        self.assertIn("Receipt.OrdinaryCollisionQueryCount = Data.OrdinaryCollisionQueryCount", apply_receipt)

    def test_collision_policy_noop_restore_is_verified_without_compile(self) -> None:
        restore = section(
            self.graph,
            "bool RestoreCollisionPolicyStates(",
            "TSharedRef<FJsonObject> MakeCollisionPolicyResult(",
        )
        self.assertIn('OutCompileSummary.AggregateStatus = TEXT("not_required")', restore)
        self.assertIn("OutCompileSummary.bCompiled = true", restore)

    def test_policy_manifest_exposes_read_only_ordinary_override(self) -> None:
        manifest = json.loads(CAPABILITY_MANIFEST.read_text(encoding="utf-8"))
        capabilities = {
            capability["id"]: capability for capability in manifest["capabilities"]
        }
        for capability_id in (
            "content.niagara.graph.collision.policy.plan",
            "content.niagara.graph.collision.policy.apply",
        ):
            with self.subTest(capability_id=capability_id):
                properties = capabilities[capability_id]["inputSchema"]["properties"]
                self.assertIn("allowUnmanagedOrdinaryCollisionQuery", properties)
                self.assertIn(
                    "read-only/shared",
                    properties["allowUnmanagedOrdinaryCollisionQuery"]["description"],
                )

    def test_policy_manifest_exposes_read_only_distance_field_override(self) -> None:
        manifest = json.loads(CAPABILITY_MANIFEST.read_text(encoding="utf-8"))
        capabilities = {
            capability["id"]: capability for capability in manifest["capabilities"]
        }
        for capability_id in (
            "content.niagara.graph.collision.policy.plan",
            "content.niagara.graph.collision.policy.apply",
        ):
            with self.subTest(capability_id=capability_id):
                properties = capabilities[capability_id]["inputSchema"]["properties"]
                self.assertIn("allowUnmanagedDistanceField", properties)
                self.assertIn(
                    "read-only/shared",
                    properties["allowUnmanagedDistanceField"]["description"],
                )

    def test_hardware_rt_blocks_unmanaged_distance_field_by_default(self) -> None:
        self.assertIn("UnmanagedEnabledDistanceFieldCount", self.graph)
        self.assertIn("bAllowUnmanagedDistanceField", self.graph)
        self.assertIn(
            "hardwareRayTracing cannot disable %d enabled Distance Field node(s)",
            self.graph,
        )

    def test_collision_policy_reports_unmanaged_ordinary_action(self) -> None:
        self.assertIn("CollisionPolicyOrdinaryAction(", self.graph)
        self.assertIn("disableEditablePreserveUnmanaged", self.graph)

    def test_graph_inspect_exposes_specific_operation_names(self) -> None:
        serialize_node = section(
            self.graph,
            "TSharedRef<FJsonObject> SerializeNode(",
            "bool ResolveNode(",
        )
        self.assertIn("GetCollisionOperationNames", serialize_node)
        self.assertIn('TEXT("operations")', serialize_node)
        self.assertIn("OperationValues", serialize_node)

    def test_collision_audit_exposes_bounded_operation_inventory(self) -> None:
        audit = section(
            self.audit,
            "void AppendDetailedCollisionAudit(",
            "} // namespace UEAINiagaraGraphAuditExtensions",
        )
        for token in (
            "FNiagaraCollisionOperationInventory",
            "GraphOperationInventory",
            "AggregateInventory",
            "SetCollisionOperationInventory",
            'TEXT("locationsTruncated")',
            'TEXT("operationInventoryLocationsTruncated")',
        ):
            with self.subTest(token=token):
                self.assertIn(token, audit)

    def test_collision_audit_inventory_is_wired_to_top_level_and_graph_scope(self) -> None:
        self.assertIn(
            "FNiagaraCollisionOperationInventory OperationInventory",
            self.graph,
        )
        self.assertIn(
            "&OperationInventory",
            self.graph,
        )
        self.assertIn(
            'TEXT("operationInventory")',
            self.audit,
        )

    def test_niagara_manifest_and_registrars_cover_same_graph_capabilities(self) -> None:
        manifest = json.loads(CAPABILITY_MANIFEST.read_text(encoding="utf-8"))
        manifest_ids = {
            capability["id"]
            for capability in manifest["capabilities"]
            if capability["id"].startswith("content.niagara.graph.")
        }

        def active_source(source: str) -> str:
            # Each optional Niagara translation unit has one top-level
            # WITH_UEAI_NIAGARA branch and an unavailable-tool #else at EOF.
            # Keep the concrete tool classes and registrar in the active side.
            return source.rsplit("#else", 1)[0]

        def registered_ids(source: str, function_name: str) -> set[str]:
            active = active_source(source)
            registration = re.search(
                rf"void {re.escape(function_name)}\([^)]*\)\s*\{{(?P<body>.*?)\n[ \t]*\}}",
                active,
                flags=re.DOTALL,
            )
            self.assertIsNotNone(registration)
            class_names = re.findall(
                r"\bRegistry\s*\.\s*Register\s*\(\s*MakeShared\s*<\s*"
                r"([A-Za-z_][A-Za-z0-9_]*)\s*>",
                registration.group("body"),
            )
            self.assertTrue(class_names, function_name)
            ids: set[str] = set()
            for class_name in class_names:
                class_block = re.search(
                    # A forward declaration ends in ';', so it must not
                    # capture a later, unrelated class's capability body.
                    rf"\bclass\s+{re.escape(class_name)}\b[^;{{]*\{{"
                    rf".*?\n[ \t]*\}};",
                    active,
                    flags=re.DOTALL,
                )
                self.assertIsNotNone(class_block, class_name)
                capability = re.search(
                    r'GetCapabilityId\(\).*?TEXT\("(content\.niagara\.[^"]+)"\)',
                    class_block.group(0),
                    flags=re.DOTALL,
                )
                self.assertIsNotNone(capability, class_name)
                if capability.group(1).startswith("content.niagara.graph."):
                    ids.add(capability.group(1))
            return ids

        # Graph-edit tools use names such as FPlan rather than FTool_*. Keep
        # both that naming freedom and declaration/definition boundaries in
        # the scanner contract without relaxing manifest equality below.
        scanner_fixture = '''
void RegisterFixtureTools(FMCPToolRegistry& Registry);
class FPlan;
class FPlan final : public FMCPToolBase
{
    FString GetCapabilityId() const override
    { return TEXT("content.niagara.graph.edit.plan"); }
};
void RegisterFixtureTools(FMCPToolRegistry& Registry)
{
    Registry.Register(MakeShared<FPlan>());
}
'''
        self.assertEqual(
            registered_ids(scanner_fixture, "RegisterFixtureTools"),
            {"content.niagara.graph.edit.plan"},
        )

        registered = set()
        registered.update(
            registered_ids(self.graph, "RegisterNiagaraGraphTools")
        )
        registered.update(
            registered_ids(self.advanced, "RegisterNiagaraGraphAdvancedTools")
        )
        registered.update(
            registered_ids(self.module, "RegisterNiagaraGraphModuleTools")
        )
        registered.update(
            registered_ids(self.module, "RegisterNiagaraDynamicInputTools")
        )
        registered.update(
            registered_ids(self.edit, "RegisterNiagaraGraphEditTools")
        )
        self.assertEqual(manifest_ids, registered)

    def test_async_trace_plan_serializes_distinct_before_and_after_values(self) -> None:
        serializer = section(
            self.advanced,
            "TSharedRef<FJsonObject> SerializeTarget(",
            "TSharedRef<FJsonObject> SerializeRequest(",
        )
        plan = section(
            self.advanced,
            "TSharedRef<FJsonObject> BuildPlanJson(",
            "FString CompileStatusName(",
        )
        self.assertIn("const bool bAfterState", serializer)
        self.assertIn('TEXT("traceProvider")', serializer)
        self.assertIn('TEXT("maxTracesPerParticle")', serializer)
        self.assertIn('TEXT("maxRetraces")', serializer)
        self.assertIn("SerializeTarget(State, false)", plan)
        self.assertIn("SerializeTarget(State, true)", plan)

    def test_async_trace_result_reports_the_current_side_of_the_receipt(self) -> None:
        result = section(
            self.advanced,
            "TSharedRef<FJsonObject> MakeResult(",
            "class FTool_AsyncTraceConfigurePlan",
        )
        self.assertIn("SerializeTarget(State, !Receipt.bRolledBack)", result)

    def test_graph_mutation_entries_reject_null_params_before_dereference(self) -> None:
        for class_name in (
            "FTool_NiagaraPinDefaultApply",
            "FTool_NiagaraPinDefaultRollback",
            "FTool_NiagaraNodeEnabledApply",
            "FTool_NiagaraNodeEnabledRollback",
        ):
            with self.subTest(class_name=class_name):
                class_block = re.search(
                    rf"class {re.escape(class_name)}\b.*?\n\}};",
                    self.graph,
                    flags=re.DOTALL,
                )
                self.assertIsNotNone(class_block, class_name)
                self.assertRegex(
                    class_block.group(0),
                    r"if \(!Params\.IsValid\(\)\s*\|\|\s*!Params->",
                )

    def test_node_enabled_rollback_requires_compile_success_when_state_changes(self) -> None:
        rollback = section(
            self.graph,
            "class FTool_NiagaraNodeEnabledRollback",
            "} // namespace UEAINiagaraGraphPrivate",
        )
        self.assertIn(
            "bRollbackChangesState && !CompileSummary.bCompiled",
            rollback,
        )
        self.assertIn('TEXT("rollback_verification_failed")', rollback)

    def test_module_duplicate_detection_uses_full_script_identity(self) -> None:
        identity = section(
            self.module,
            "bool ScriptObjectPathsEqual(",
            "UEdGraphPin* FindMapPin",
        )
        self.assertIn("NormalizeObjectPath", identity)
        self.assertIn("FunctionScriptAssetObjectPath", identity)
        duplicate_check = section(
            self.module,
            "Target.ExistingModuleCount = 0;",
            "if (Request.bHasTargetIndex)",
        )
        self.assertIn("ModuleScriptIdentityMatches(FunctionCall, RequestedModulePath)", duplicate_check)
        self.assertNotIn("NodeScriptPath(FunctionCall).EndsWith", duplicate_check)

    def test_module_target_refuses_ambiguous_stack_graph(self) -> None:
        target = section(
            self.module,
            "bool FindTarget(",
            "bool ResolveInsertion(",
        )
        self.assertIn("if (Candidates.Num() > 1)", target)
        self.assertIn('TEXT("ambiguous_graph")', target)
        self.assertIn("specify outputNodePath", target)

    def test_module_plan_uses_common_change_plan_contract(self) -> None:
        plan = section(
            self.module,
            "TSharedRef<FJsonObject> BuildPlanJson(",
            "FString CompileStatusName(",
        )
        for field in (
            'TEXT("ue.change-plan.v1")',
            'TEXT("planKind")',
            'TEXT("action")',
            'TEXT("scope")',
            'TEXT("risk")',
            'TEXT("confirmWriteRequired")',
            'TEXT("rollbackBoundary")',
            'TEXT("rollbackDurability")',
            'TEXT("before")',
        ):
            with self.subTest(field=field):
                self.assertIn(field, plan)


if __name__ == "__main__":
    unittest.main()
