#!/usr/bin/env python3
"""Source contracts for non-destructive Niagara simulation-stage recovery.

These checks run without UE. Native graph topology, compilation, and failure
recovery are exercised separately by NiagaraSimulationStageContractTests.cpp.
"""

from pathlib import Path
import unittest


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
SOURCE = PLUGIN_ROOT / "Source/UE_AI_integration/Private/Domains/Content/Command/Niagara_SimulationStage.cpp"
NATIVE_TESTS = PLUGIN_ROOT / "Source/UE_AI_integration/Private/Tests/NiagaraSimulationStageContractTests.cpp"


def section(text: str, start: str, end: str) -> str:
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


class SimulationStageRecoverySourceContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.tests = NATIVE_TESTS.read_text(encoding="utf-8")
        cls.restore = section(cls.source, "bool RestoreGraphSnapshot(", "TMap<FString, FStageReceipt>& Receipts()")
        cls.remove = section(cls.source, "bool RemoveStageOutput(", "FString CompileStatusName(")
        cls.apply = section(cls.source, "class FTool_NiagaraSimulationStageRemoveApply", "class FTool_NiagaraSimulationStageRemoveRollback")
        cls.rollback = section(cls.source, "class FTool_NiagaraSimulationStageRemoveRollback", "class FTool_NiagaraSimulationStageAddPlan")

    def test_retained_graph_objects_replace_unsupported_text_import(self) -> None:
        self.assertIn("TStrongObjectPtr<UEdGraphNode> Node", self.source)
        self.assertIn("TStrongObjectPtr<UNiagaraSimulationStageBase> RetainedStage", self.source)
        self.assertIn("TSharedPtr<FGraphSnapshot> BeforeGraphSnapshot", self.source)
        self.assertNotIn("ImportNodesFromText", self.source)
        self.assertNotIn("DestroyNode", self.restore)

    def test_reconstruction_is_refused_before_saved_pins_are_used(self) -> None:
        pins = section(self.source, "bool SnapshotPinsSurvive(", "bool GraphSnapshotMatches(")
        self.assertIn("State.Node->Pins != State.Pins", pins)
        self.assertLess(self.restore.index("SnapshotPinsSurvive(Graph, Snapshot)"),
                        self.restore.index("ApplyRetainedGraphSnapshot(Graph, Snapshot)"))

    def test_full_graph_verdict_requires_identity_order_and_authored_digest(self) -> None:
        match = section(self.source, "bool GraphSnapshotMatches(", "void ApplyRetainedGraphSnapshot(")
        self.assertIn("Graph->Nodes[Index] != Snapshot.Nodes[Index].Node.Get()", match)
        self.assertIn("ReadBack.Digest == Snapshot.Digest", match)
        self.assertIn("ReadBack.NodeCount == Snapshot.NodeCount", match)
        self.assertIn("GraphSnapshotMatches(Graph, BeforeGraph)", self.rollback)

    def test_failed_graph_restore_can_restore_the_current_graph(self) -> None:
        self.assertLess(self.restore.index("CaptureGraphSnapshot(Graph, CurrentGraph)"),
                        self.restore.index("ApplyRetainedGraphSnapshot(Graph, Snapshot)"))
        self.assertIn("ApplyRetainedGraphSnapshot(Graph, CurrentGraph)", self.restore)
        self.assertIn("Editor Undo", self.rollback)
        self.assertNotIn("Transaction.Cancel()", self.rollback)

    def test_remove_protects_crossing_consumers_and_their_dependencies(self) -> None:
        self.assertIn("CollectUpstreamNodes(Output, Graph, Candidates)", self.remove)
        self.assertIn("!Candidates.Contains(LinkedPin->GetOwningNode())", self.remove)
        self.assertIn("CollectUpstreamNodes(Node, Graph, ProtectedNodes)", self.remove)
        self.assertIn("Candidates.Contains(Node) && !ProtectedNodes.Contains(Node)", self.remove)

    def test_failure_never_substitutes_an_empty_stage_output(self) -> None:
        self.assertNotIn("CreateStageOutput", self.apply)
        self.assertNotIn("CreateStageOutput", self.rollback)
        self.assertLess(self.rollback.index("RestoreGraphSnapshot(Graph, BeforeGraph)"),
                        self.rollback.index("Emitter->AddSimulationStage"))
        self.assertLess(self.rollback.index("if (!bReadBack || !Compile.bCompiled)"),
                        self.rollback.index("Receipt->bFullGraphRestored = true"))

    def test_native_regressions_cover_shared_nodes_and_refused_restoration(self) -> None:
        self.assertIn("UE_AI_integration.Niagara.SimulationStage.SharedGraphRecovery", self.tests)
        self.assertIn("Exclusive upstream branch leaves the graph", self.tests)
        self.assertIn("Shared upstream dependency remains", self.tests)
        self.assertIn("Every graph node property and link is restored", self.tests)
        self.assertIn("Failed restoration preserves the current complete graph", self.tests)
        self.assertIn("Failed restoration retains Editor Undo", self.tests)


if __name__ == "__main__":
    unittest.main()
