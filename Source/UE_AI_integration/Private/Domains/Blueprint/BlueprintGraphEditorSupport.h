// Shared Blueprint Graph Editor resolution, geometry, and hashing helpers.
#pragma once

#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphEditor.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "JsonObjectConverter.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "Tools/MCPToolBase.h"
#include "Widgets/SWindow.h"

namespace UEAIIntegration::BlueprintGraph
{
inline TMap<FString, TWeakPtr<SGraphEditor>> GraphEditorCache;

struct FContext
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedPtr<IBlueprintEditor> BlueprintEditor;
	TSharedPtr<SGraphEditor> GraphEditor;
	FString BlueprintInput;
	FString GraphName;
	bool bEditorOpened = false;
};

struct FNodeBounds
{
	double X = 0.0;
	double Y = 0.0;
	double Width = 0.0;
	double Height = 0.0;
	FString Source = TEXT("stored");
	bool bExact = false;

	double Right() const
	{
		return X + Width;
	}

	double Bottom() const
	{
		return Y + Height;
	}
};

struct FEditorGeometryFingerprint
{
	FVector2D ViewOffset = FVector2D::ZeroVector;
	double Zoom = 1.0;
	double ApplicationScale = 1.0;
	double DPIScale = 1.0;
	int32 PassCount = 0;
	FString BoundsHash;
	bool bExact = false;
};

inline FMCPToolResult InvalidRequest(const FString& Message)
{
	return FMCPToolResult::Error(Message, TEXT("invalid_request"), 400);
}

inline FMCPToolResult NotFound(const FString& Message)
{
	return FMCPToolResult::Error(Message, TEXT("not_found"), 404);
}

inline FMCPToolResult EditorUnavailable(
	const FString& Message,
	const FString& Code = TEXT("editor_context_unavailable"))
{
	return FMCPToolResult::Error(Message, Code, 409);
}

inline UEdGraph* FindGraph(
	UBlueprint* Blueprint,
	const FString& EncodedGraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	const FString GraphName = MCPHelpers::UrlDecode(EncodedGraphName);
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph
			&& Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}
	return nullptr;
}

inline FMCPToolResult ResolveBlueprintGraph(
	const TSharedPtr<FJsonObject>& Params,
	FContext& OutContext)
{
	if (!Params.IsValid())
	{
		return InvalidRequest(TEXT("Request params are required."));
	}

	FString BlueprintInput;
	if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintInput))
	{
		Params->TryGetStringField(TEXT("name"), BlueprintInput);
	}
	FString GraphInput;
	if (BlueprintInput.IsEmpty()
		|| !Params->TryGetStringField(TEXT("graph"), GraphInput)
		|| GraphInput.IsEmpty())
	{
		return InvalidRequest(
			TEXT("Parameters 'blueprint' (or 'name') and 'graph' are required."));
	}

	FString LoadError;
	UBlueprint* Blueprint =
		MCPHelpers::LoadBlueprintByName(BlueprintInput, LoadError);
	if (!Blueprint)
	{
		return NotFound(LoadError);
	}

	UEdGraph* Graph = FindGraph(Blueprint, GraphInput);
	if (!Graph)
	{
		return NotFound(
			FString::Printf(
				TEXT("Graph '%s' was not found in Blueprint '%s'."),
				*MCPHelpers::UrlDecode(GraphInput),
				*BlueprintInput));
	}

	OutContext.Blueprint = Blueprint;
	OutContext.Graph = Graph;
	OutContext.BlueprintInput = MoveTemp(BlueprintInput);
	OutContext.GraphName = Graph->GetName();
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

/**
 * Requests a full visual refresh, runs the pending active timers owned by this
 * Graph Editor, then performs a targeted prepass/redraw. Graph mutations made
 * through UEdGraph can arrive while SGraphEditorImpl already has a deferred
 * refresh registered; merely ticking Slate in that state can leave a newly
 * added node absent from SGraphPanel::NodeToWidgetLookup. An explicit
 * NotifyGraphChanged coalesces the current model into one fresh visual rebuild
 * before callers ask for exact native bounds.
 *
 * A newly opened document tab needs a Slate widget tick before its Graph
 * Editor becomes part of the active layout. Two guarded widget-only ticks
 * activate the tab and run SGraphEditorImpl's private refresh timer; they do
 * not advance the Engine/Game tick. The targeted prepass/redraw then measures
 * the newly materialized node widgets. The guard prevents recursive refreshes
 * if an unrelated Slate pre-tick callback resolves another Graph Editor.
 */
inline bool RefreshGraphEditorLayout(
	const TSharedPtr<SGraphEditor>& GraphEditor)
{
	if (!GraphEditor.IsValid()
		|| !FSlateApplication::IsInitialized()
		|| !FApp::CanEverRender())
	{
		return false;
	}

	static thread_local bool bRefreshingGraphLayout = false;
	if (bRefreshingGraphLayout)
	{
		return false;
	}
	TGuardValue<bool> RefreshGuard(bRefreshingGraphLayout, true);

	FSlateApplication& SlateApplication = FSlateApplication::Get();
	GraphEditor->NotifyGraphChanged();
	if (SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel())
	{
		// TriggerRefresh normally reaches this on a later Slate active timer.
		// Exact geometry requests are synchronous, so update the lookup now.
		GraphPanel->Update();
	}
	SlateApplication.Tick(ESlateTickType::Widgets);
	SlateApplication.Tick(ESlateTickType::Widgets);
	SlateApplication.Tick(ESlateTickType::Widgets);
	const TSharedPtr<SWindow> Window =
		SlateApplication.FindWidgetWindow(GraphEditor.ToSharedRef());
	if (!Window.IsValid())
	{
		return false;
	}

	const TSharedPtr<FGenericWindow> NativeWindow = Window->GetNativeWindow();
	if (!NativeWindow.IsValid())
	{
		return false;
	}
	const float LayoutScale =
		SlateApplication.GetApplicationScale()
		* NativeWindow->GetDPIScaleFactor();
	if (SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel())
	{
		GraphPanel->SlatePrepass(LayoutScale);
		if (UEdGraph* Graph = GraphEditor->GetCurrentGraph())
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				const TSharedPtr<SGraphNode> NodeWidget =
					GraphPanel->GetNodeWidgetFromGuid(Node->NodeGuid);
				if (NodeWidget.IsValid())
				{
					// Off-screen graph nodes remain valid panel children but
					// are not arranged, so their desired size can still be
					// zero. Prepass each concrete SGraphNode without changing
					// the user's view transform.
					NodeWidget->SlatePrepass(LayoutScale);
				}
			}
		}
	}
	GraphEditor->SlatePrepass(LayoutScale);
	Window->SlatePrepass(LayoutScale);
	SlateApplication.ForceRedrawWindow(Window.ToSharedRef());
	Window->SlatePrepass(LayoutScale);
	SlateApplication.ForceRedrawWindow(Window.ToSharedRef());
	return true;
}

inline FMCPToolResult ResolveGraphEditor(
	FContext& Context,
	const bool bOpenEditor,
	const bool bRequireFocused = false)
{
	TSharedPtr<IBlueprintEditor> BlueprintEditor =
		FKismetEditorUtilities::GetIBlueprintEditorForObject(
			Context.Blueprint,
			false);
	const bool bEditorWasOpen = BlueprintEditor.IsValid();
	if (!BlueprintEditor.IsValid() && bOpenEditor)
	{
		BlueprintEditor =
			FKismetEditorUtilities::GetIBlueprintEditorForObject(
				Context.Blueprint,
				true);
	}
	if (!BlueprintEditor.IsValid())
	{
		return EditorUnavailable(
			FString::Printf(
				TEXT("Blueprint Editor for '%s' is not open."),
				*Context.BlueprintInput));
	}
	if (bRequireFocused && BlueprintEditor->GetFocusedGraph() != Context.Graph)
	{
		return EditorUnavailable(
			FString::Printf(
				TEXT("Graph '%s' is not the focused graph in Blueprint '%s'."),
				*Context.GraphName,
				*Context.BlueprintInput));
	}

	const FString CacheKey = Context.Graph->GetPathName();
	TSharedPtr<SGraphEditor> GraphEditor;
	if (BlueprintEditor->GetFocusedGraph() == Context.Graph)
	{
		if (const TWeakPtr<SGraphEditor>* Cached =
				GraphEditorCache.Find(CacheKey))
		{
			GraphEditor = Cached->Pin();
		}
		if (!GraphEditor.IsValid()
			|| GraphEditor->GetCurrentGraph() != Context.Graph)
		{
			GraphEditor =
				BlueprintEditor->OpenGraphAndBringToFront(
					Context.Graph,
					false);
		}
	}
	else if (bOpenEditor)
	{
		GraphEditor =
			BlueprintEditor->OpenGraphAndBringToFront(
				Context.Graph,
				true);
	}
	if (!GraphEditor.IsValid()
		|| GraphEditor->GetCurrentGraph() != Context.Graph)
	{
		GraphEditorCache.Remove(CacheKey);
		return EditorUnavailable(
			FString::Printf(
				TEXT("Graph Editor context for '%s' is unavailable."),
				*Context.GraphName));
	}
	if (bOpenEditor)
	{
		RefreshGraphEditorLayout(GraphEditor);
	}

	GraphEditorCache.Add(CacheKey, GraphEditor);
	Context.BlueprintEditor = MoveTemp(BlueprintEditor);
	Context.GraphEditor = MoveTemp(GraphEditor);
	Context.bEditorOpened = !bEditorWasOpen;
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

inline UEdGraphNode* FindNode(UEdGraph* Graph, const FGuid& NodeGuid)
{
	if (!Graph)
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == NodeGuid)
		{
			return Node;
		}
	}
	return nullptr;
}

inline bool TryParseNodeGuid(
	const FString& NodeId,
	FGuid& OutGuid,
	FString& OutError)
{
	OutGuid.Invalidate();
	if (!FGuid::Parse(NodeId, OutGuid) || !OutGuid.IsValid())
	{
		OutError = FString::Printf(
			TEXT("Node ID '%s' is not a valid GUID."),
			*NodeId);
		return false;
	}
	return true;
}

inline bool TryGetNodeBounds(
	const FContext& Context,
	const UEdGraphNode* Node,
	const bool bPreferEditor,
	FNodeBounds& OutBounds)
{
	if (!Node)
	{
		return false;
	}

	if (bPreferEditor && Context.GraphEditor.IsValid())
	{
		FSlateRect Rect;
		if (Context.GraphEditor->GetBoundsForNode(Node, Rect, 0.0f)
			&& Rect.Right > Rect.Left
			&& Rect.Bottom > Rect.Top)
		{
			OutBounds.X = Rect.Left;
			OutBounds.Y = Rect.Top;
			OutBounds.Width = Rect.Right - Rect.Left;
			OutBounds.Height = Rect.Bottom - Rect.Top;
			OutBounds.Source = TEXT("editor");
			OutBounds.bExact = true;
			return true;
		}
	}

	OutBounds.X = Node->NodePosX;
	OutBounds.Y = Node->NodePosY;
	OutBounds.Width = FMath::Max(0, Node->NodeWidth);
	OutBounds.Height = FMath::Max(0, Node->NodeHeight);
	OutBounds.Source = TEXT("stored");
	OutBounds.bExact = !bPreferEditor
		&& Cast<UEdGraphNode_Comment>(Node) != nullptr
		&& OutBounds.Width > 0.0
		&& OutBounds.Height > 0.0;
	return true;
}

inline TSharedRef<FJsonObject> BoundsToJson(const FNodeBounds& Bounds)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetNumberField(TEXT("x"), Bounds.X);
	Json->SetNumberField(TEXT("y"), Bounds.Y);
	Json->SetNumberField(TEXT("width"), Bounds.Width);
	Json->SetNumberField(TEXT("height"), Bounds.Height);
	Json->SetStringField(TEXT("source"), Bounds.Source);
	Json->SetBoolField(TEXT("exact"), Bounds.bExact);
	return Json;
}

inline TSharedRef<FJsonObject> GeometryFingerprintToJson(
	const FEditorGeometryFingerprint& Fingerprint)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(
		TEXT("schema"),
		TEXT("ue.blueprint-editor-geometry-fingerprint.v1"));
	Json->SetStringField(TEXT("coordinateSpace"), TEXT("graph"));
	Json->SetBoolField(TEXT("exact"), Fingerprint.bExact);
	Json->SetStringField(TEXT("boundsHash"), Fingerprint.BoundsHash);
	Json->SetNumberField(TEXT("zoom"), Fingerprint.Zoom);
	TSharedRef<FJsonObject> ViewOffset = MakeShared<FJsonObject>();
	ViewOffset->SetNumberField(TEXT("x"), Fingerprint.ViewOffset.X);
	ViewOffset->SetNumberField(TEXT("y"), Fingerprint.ViewOffset.Y);
	Json->SetObjectField(TEXT("viewOffset"), ViewOffset);
	Json->SetNumberField(
		TEXT("applicationScale"),
		Fingerprint.ApplicationScale);
	Json->SetNumberField(TEXT("dpiScale"), Fingerprint.DPIScale);
	Json->SetNumberField(TEXT("passCount"), Fingerprint.PassCount);
	return Json;
}

/**
 * Captures native SGraphEditor bounds in Graph-space. UE 5.3's
 * GetBoundsForNode already returns SNode positions and desired sizes in Graph
 * Units, so callers must not divide these values by the current zoom.
 *
 * A capture is accepted only after two consecutive refresh passes produce the
 * same bounds hash. This avoids consuming a transient Slate desired size while
 * a newly opened Graph tab or a changed zoom level is still settling.
 */
inline bool TryCaptureSettledEditorGeometry(
	const FContext& Context,
	const TArray<UEdGraphNode*>& RequestedNodes,
	TMap<FGuid, FNodeBounds>& OutBounds,
	FEditorGeometryFingerprint& OutFingerprint,
	FString& OutError,
	const int32 MaxPasses = 6)
{
	OutBounds.Reset();
	OutFingerprint = FEditorGeometryFingerprint();
	OutError.Reset();
	if (!Context.GraphEditor.IsValid() || RequestedNodes.IsEmpty())
	{
		OutError = TEXT("A live Graph Editor and at least one node are required.");
		return false;
	}

	// SGraphNode can switch its Slate LOD at low zoom, which changes desired
	// size even though the node's Graph-space layout has not changed. Measure at
	// the canonical 1.0 editor zoom, then restore the caller's view. The
	// fingerprint still records the requested view so diagnostics can reproduce
	// the session state without making its transform part of the bounds hash.
	FVector2D RequestedViewOffset = FVector2D::ZeroVector;
	float RequestedZoom = 1.0f;
	Context.GraphEditor->GetViewLocation(RequestedViewOffset, RequestedZoom);
	Context.GraphEditor->SetViewLocation(RequestedViewOffset, 1.0f);
	ON_SCOPE_EXIT
	{
		if (Context.GraphEditor.IsValid())
		{
			Context.GraphEditor->SetViewLocation(
				RequestedViewOffset,
				RequestedZoom);
			RefreshGraphEditorLayout(Context.GraphEditor);
		}
	};

	TArray<UEdGraphNode*> Nodes;
	Nodes.Reserve(RequestedNodes.Num());
	for (UEdGraphNode* Node : RequestedNodes)
	{
		if (Node && Node->GetGraph() == Context.Graph)
		{
			Nodes.AddUnique(Node);
		}
	}
	Nodes.Sort(
		[](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
		});
	if (Nodes.IsEmpty())
	{
		OutError = TEXT("None of the requested nodes belong to the Graph.");
		return false;
	}

	FString PreviousBoundsHash;
	for (int32 PassIndex = 1; PassIndex <= FMath::Max(2, MaxPasses); ++PassIndex)
	{
		if (!RefreshGraphEditorLayout(Context.GraphEditor))
		{
			OutError = TEXT("The Graph Editor could not refresh native geometry.");
			return false;
		}

		TMap<FGuid, FNodeBounds> CurrentBounds;
		TArray<TSharedPtr<FJsonValue>> BoundsValues;
		for (const UEdGraphNode* Node : Nodes)
		{
			FNodeBounds Bounds;
			if (!TryGetNodeBounds(Context, Node, true, Bounds) || !Bounds.bExact)
			{
				OutError = FString::Printf(
					TEXT("Exact Graph geometry is unavailable for node '%s'."),
					*Node->NodeGuid.ToString());
				return false;
			}
			CurrentBounds.Add(Node->NodeGuid, Bounds);
			TSharedRef<FJsonObject> Entry = BoundsToJson(Bounds);
			Entry->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
			BoundsValues.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(
			TEXT("schema"),
			TEXT("ue.blueprint-editor-graph-bounds.v1"));
		Evidence->SetStringField(TEXT("coordinateSpace"), TEXT("graph"));
		Evidence->SetArrayField(TEXT("nodeBounds"), BoundsValues);
		FString BoundsDigest;
		if (!Infrastructure::TryDigestJson(Evidence, BoundsDigest))
		{
			OutError = TEXT("Could not compute the Graph bounds fingerprint.");
			return false;
		}
		const FString CurrentBoundsHash = TEXT("sha256:") + BoundsDigest;

		double ApplicationScale = FSlateApplication::Get().GetApplicationScale();
		double DPIScale = 1.0;
		const TSharedPtr<SWindow> Window =
			FSlateApplication::Get().FindWidgetWindow(
				Context.GraphEditor.ToSharedRef());
		if (Window.IsValid())
		{
			const TSharedPtr<FGenericWindow> NativeWindow =
				Window->GetNativeWindow();
			if (NativeWindow.IsValid())
			{
				DPIScale = NativeWindow->GetDPIScaleFactor();
			}
		}

		if (!PreviousBoundsHash.IsEmpty()
			&& PreviousBoundsHash == CurrentBoundsHash)
		{
			OutBounds = MoveTemp(CurrentBounds);
			OutFingerprint.ViewOffset = RequestedViewOffset;
			OutFingerprint.Zoom = RequestedZoom;
			OutFingerprint.ApplicationScale = ApplicationScale;
			OutFingerprint.DPIScale = DPIScale;
			OutFingerprint.PassCount = PassIndex;
			OutFingerprint.BoundsHash = CurrentBoundsHash;
			OutFingerprint.bExact = true;
			return true;
		}
		PreviousBoundsHash = CurrentBoundsHash;
	}

	OutError = FString::Printf(
		TEXT("Graph geometry did not settle in %d refresh passes."),
		FMath::Max(2, MaxPasses));
	return false;
}

inline bool Contains(
	const FNodeBounds& Outer,
	const FNodeBounds& Inner,
	const double Padding = 0.0)
{
	return Inner.X >= Outer.X + Padding
		&& Inner.Y >= Outer.Y + Padding
		&& Inner.Right() <= Outer.Right() - Padding
		&& Inner.Bottom() <= Outer.Bottom() - Padding;
}

inline bool Intersects(const FNodeBounds& A, const FNodeBounds& B)
{
	return A.X < B.Right()
		&& A.Right() > B.X
		&& A.Y < B.Bottom()
		&& A.Bottom() > B.Y;
}

inline FString ComputeGraphHash(UEdGraph* Graph)
{
	// Package reload keeps the superseded Graph object alive long enough to
	// replace references to it. IsValid() intentionally still accepts that
	// object, but its TObjectPtr node handles may already contain reload
	// sentinels. Never inspect a Graph after a newer package version exists.
	if (!IsValid(Graph) || Graph->HasAnyFlags(RF_NewerVersionExists))
	{
		return FString();
	}

	TArray<UEdGraphNode*> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (IsValid(Node) && !Node->HasAnyFlags(RF_NewerVersionExists))
		{
			Nodes.Add(Node);
		}
	}
	Nodes.Sort(
		[](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
		});

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	for (const UEdGraphNode* Node : Nodes)
	{
		TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
		NodeJson->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
		NodeJson->SetStringField(
			TEXT("title"),
			Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);
		NodeJson->SetNumberField(TEXT("width"), Node->NodeWidth);
		NodeJson->SetNumberField(TEXT("height"), Node->NodeHeight);
		NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);

		TArray<const UEdGraphPin*> NodePins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				NodePins.Add(Pin);
			}
		}
		NodePins.Sort(
			[](const UEdGraphPin& Left, const UEdGraphPin& Right)
			{
				const FString LeftId = Left.PinId.ToString();
				const FString RightId = Right.PinId.ToString();
				if (LeftId != RightId)
				{
					return LeftId < RightId;
				}
				if (Left.Direction != Right.Direction)
				{
					return static_cast<int32>(Left.Direction)
						< static_cast<int32>(Right.Direction);
				}
				return Left.PinName.ToString() < Right.PinName.ToString();
			});

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : NodePins)
		{
			TArray<FString> Links;
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode())
				{
					Links.Add(
						Linked->GetOwningNode()->NodeGuid.ToString()
						+ TEXT(":")
						+ Linked->PinId.ToString()
						+ TEXT(":")
						+ Linked->PinName.ToString());
				}
			}
			Links.Sort();

			TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("pinId"), Pin->PinId.ToString());
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetNumberField(
				TEXT("direction"),
				static_cast<int32>(Pin->Direction));
			const TSharedPtr<FJsonObject> PinTypeJson =
				FJsonObjectConverter::UStructToJsonObject(Pin->PinType);
			if (PinTypeJson.IsValid())
			{
				PinJson->SetObjectField(TEXT("type"), PinTypeJson);
			}
			else
			{
				// A conversion failure must still affect the approval hash.
				// These fields cover the core type identity without silently
				// falling back to category-only hashing.
				TSharedRef<FJsonObject> FallbackType =
					MakeShared<FJsonObject>();
				FallbackType->SetStringField(
					TEXT("category"),
					Pin->PinType.PinCategory.ToString());
				FallbackType->SetStringField(
					TEXT("subCategory"),
					Pin->PinType.PinSubCategory.ToString());
				FallbackType->SetStringField(
					TEXT("subCategoryObject"),
					GetPathNameSafe(
						Pin->PinType.PinSubCategoryObject.Get()));
				FallbackType->SetNumberField(
					TEXT("containerType"),
					static_cast<int32>(Pin->PinType.ContainerType));
				FallbackType->SetBoolField(
					TEXT("isReference"),
					Pin->PinType.bIsReference);
				FallbackType->SetBoolField(
					TEXT("isConst"),
					Pin->PinType.bIsConst);
				FallbackType->SetBoolField(
					TEXT("isWeakPointer"),
					Pin->PinType.bIsWeakPointer);
				FallbackType->SetBoolField(
					TEXT("isUObjectWrapper"),
					Pin->PinType.bIsUObjectWrapper);
				PinJson->SetObjectField(TEXT("type"), FallbackType);
			}
			PinJson->SetStringField(
				TEXT("autogeneratedDefaultValue"),
				Pin->AutogeneratedDefaultValue);
			PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
			PinJson->SetStringField(
				TEXT("defaultObject"),
				GetPathNameSafe(Pin->DefaultObject));
			PinJson->SetStringField(
				TEXT("defaultTextValue"),
				Pin->DefaultTextValue.ToString());
			PinJson->SetBoolField(TEXT("hidden"), Pin->bHidden);
			PinJson->SetBoolField(
				TEXT("notConnectable"),
				Pin->bNotConnectable);
			PinJson->SetBoolField(
				TEXT("defaultValueIsIgnored"),
				Pin->bDefaultValueIsIgnored);
			PinJson->SetBoolField(
				TEXT("defaultValueIsReadOnly"),
				Pin->bDefaultValueIsReadOnly);
			PinJson->SetBoolField(TEXT("advancedView"), Pin->bAdvancedView);
			PinJson->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
			TArray<TSharedPtr<FJsonValue>> LinkValues;
			for (const FString& Link : Links)
			{
				LinkValues.Add(MakeShared<FJsonValueString>(Link));
			}
			PinJson->SetArrayField(TEXT("links"), LinkValues);
			Pins.Add(MakeShared<FJsonValueObject>(PinJson));
		}
		NodeJson->SetArrayField(TEXT("pins"), Pins);
		NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
	Identity->SetStringField(TEXT("graph"), Graph->GetName());
	Identity->SetArrayField(TEXT("nodes"), NodeValues);
	FString Digest;
	return Infrastructure::TryDigestJson(Identity, Digest)
		? TEXT("sha256:") + Digest
		: FString();
}
}
