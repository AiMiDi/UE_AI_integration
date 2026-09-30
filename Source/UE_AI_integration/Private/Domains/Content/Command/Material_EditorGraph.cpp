#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Infrastructure/DeferredGraphMutation.h"
#include "Workflow/UEWorkflowRuntime.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "IMaterialEditor.h"
#include "MaterialEditorActions.h"
#include "Framework/Commands/UICommandList.h"
#include "MaterialEditingLibrary.h"
#include "MaterialEditorUtilities.h"
#include "MaterialShared.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "UObject/UObjectIterator.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitManager.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEAIIntegration::MaterialEditing
{
	namespace
	{
		using namespace MCPMaterialInfrastructure;

		FMCPToolResult Bad(const FString& Message, const TCHAR* Code = TEXT("invalid_preview_edit"))
		{
			return FMCPToolResult::Error(Message, Code, 400);
		}

		bool Integer(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, int32& Value)
		{
			double Number = Value;
			if (P->HasField(Field) && !P->TryGetNumberField(Field, Number)) return false;
			if (!FMath::IsFinite(Number) || Number < MIN_int32 || Number > MAX_int32 || Number !=
				FMath::FloorToDouble(Number))
				return false;
			Value = static_cast<int32>(Number);
			return true;
		}

		bool Number(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, float& Value)
		{
			double Number = Value;
			if (P->HasField(Field) && !P->TryGetNumberField(Field, Number)) return false;
			if (!FMath::IsFinite(Number) || FMath::Abs(Number) > MAX_flt) return false;
			Value = static_cast<float>(Number);
			return true;
		}

		UEdGraphNode* Node(const FTarget& Target, const FString& Id)
		{
			if (Id == TEXT("root")) return Target.Function ? nullptr : Target.Graph()->RootNode.Get();
			if (auto* E = Target.Find(Id)) return E->GraphNode;
			return nullptr;
		}

		UEdGraphPin* Pin(UEdGraphNode* Node, const FString& Name, TOptional<EEdGraphPinDirection> Direction = {})
		{
			if (!Node) return nullptr;
			TArray<UEdGraphPin*> Matches;
			int32 InIndex = 0, OutIndex = 0;
			for (auto* Pin : Node->Pins)
			{
				if (!Pin) continue;
				const int32 Index = Pin->Direction == EGPD_Input ? InIndex++ : OutIndex++;
				if (Direction.IsSet() && Pin->Direction != Direction.GetValue()) continue;
				if (Name == FString::Printf(TEXT("index:%d"), Index) || Name == Pin->PinName.ToString()
					|| (Pin->Direction == EGPD_Output && Index == 0 && Name == TEXT("Output")))
					Matches.Add(Pin);
			}
			return Matches.Num() == 1 ? Matches[0] : nullptr;
		}

		bool WouldCycle(UEdGraphNode* Source, UEdGraphNode* Target)
		{
			// Native material schema currently warns about loops but still permits them.
			// Walk upstream from Source before wiring Source -> Target; bound malformed graphs.
			TArray<UEdGraphNode*> Queue{Source};
			TSet<UEdGraphNode*> Seen;
			for (int32 I = 0; I < Queue.Num(); ++I)
			{
				auto* Current = Queue[I];
				if (Current == Target || Queue.Num() > 20000) return true;
				if (!Current || Seen.Contains(Current)) continue;
				Seen.Add(Current);
				for (auto* Input : Current->Pins)
					if (Input && Input->Direction == EGPD_Input)
						for (auto* Output : Input->LinkedTo) if (Output) Queue.Add(Output->GetOwningNode());
			}
			return false;
		}

		void ModifyConnections(UEdGraphNode* Node)
		{
			Node->Modify();
			if (auto* MaterialNode = Cast<UMaterialGraphNode>(Node); MaterialNode && MaterialNode->MaterialExpression)
				MaterialNode->MaterialExpression->Modify();
			for (auto* Pin : Node->Pins)
				if (Pin)
					for (auto* Linked : Pin->LinkedTo)
						if (Linked)
						{
							Linked->GetOwningNode()->Modify();
							if (auto* Other = Cast<UMaterialGraphNode>(Linked->GetOwningNode()); Other && Other->
								MaterialExpression)
								Other->MaterialExpression->Modify();
						}
		}

		void SyncWorkingCollection(const FTarget& Target)
		{
			if (Target.Function)
				Target.Function->
				       AssignExpressionCollection(Target.Material->GetExpressionCollection());
		}

		FMCPToolResult Finish(FTarget& Target, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& P,
		                      const TSharedRef<FJsonObject>& Result, bool bChanged = true)
		{
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("changed"), bChanged);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("applied"), false);
			Result->SetBoolField(TEXT("compileDeferred"), true);
			DescribeTarget(Target, Result);
			if (bChanged)
			{
				SyncWorkingCollection(Target);
				FinishEdit(Target, Expression, P, Result);
			}
			return FMCPToolResult::Ok(Result);
		}

		// Use only within a synchronous batch that requests a final refresh. Restoring
		// the native toggle updates the base preview, so it must happen AFTER the final
		// graph/parameter-panel update (or after rollback), never after another compile.
		class FScopedPreviewPause
		{
		public:
			explicit FScopedPreviewPause(TSharedPtr<IMaterialEditor> InEditor) : Editor(MoveTemp(InEditor))
			{
			}

			~FScopedPreviewPause() { Restore(); }
			bool IsPaused() const { return bPaused; }

			bool Pause()
			{
				if (bPaused || !IsLive()) return true;
				Editor->GetToolkitCommands()->ExecuteAction(
					FMaterialEditorCommands::Get().ToggleLivePreview.ToSharedRef());
				bPaused = !IsLive();
				return bPaused;
			}

			bool Restore()
			{
				if (!bPaused) return true;
				if (!IsLive())
					Editor->GetToolkitCommands()->ExecuteAction(
						FMaterialEditorCommands::Get().ToggleLivePreview.ToSharedRef());
				if (!IsLive()) return false;
				bPaused = false;
				return true;
			}

		private:
			bool IsLive() const
			{
				return Editor->GetToolkitCommands()->GetCheckState(
					FMaterialEditorCommands::Get().ToggleLivePreview.ToSharedRef()) == ECheckBoxState::Checked;
			}

			TSharedPtr<IMaterialEditor> Editor;
			bool bPaused = false;
		};

		FMCPToolResult DeletePreviewNodes(FTarget& Target, const TArray<FString>& Ids,
		                                  const TSharedPtr<FJsonObject>& Params, int32& FailedIndex,
		                                  TFunctionRef<bool()> BeforeDelete)
		{
			TArray<UEdGraphNode*> Nodes;
			TArray<UMaterialExpression*> Expressions;
			TSet<UEdGraphNode*> Seen;
			for (int32 Index = 0; Index < Ids.Num(); ++Index)
			{
				FailedIndex = Index;
				auto* Expression = Target.Find(Ids[Index]);
				auto* GraphNode = Node(Target, Ids[Index]);
				if (!Expression || !GraphNode) return Bad(TEXT("nodeId is not an expression in this open preview."));
				if (Seen.Contains(GraphNode))
					return Bad(
						TEXT("A consecutive deletion group cannot delete the same node twice."));
				if (!GraphNode->CanUserDeleteNode() || Expression->IsA<UMaterialExpressionComposite>() || Expression->
					IsA<UMaterialExpressionPinBase>())
					return Bad(
						TEXT("Composite interfaces require their native dedicated editor operation."));
				if (Expression->IsA<UMaterialExpressionFunctionInput>() || Expression->IsA<
					UMaterialExpressionFunctionOutput>())
					return Bad(TEXT(
						"Function interface deletion uses a native confirmation dialog; use the editor's delete command for these interface nodes."));
				Seen.Add(GraphNode);
				Nodes.Add(GraphNode);
				Expressions.Add(Expression);
			}
			FailedIndex = 0;
			if (!BeforeDelete()) return Bad(TEXT("Could not pause native Live Preview before deletion."));
			BeginEdit(Target);
			Target.Graph()->Modify();
			for (auto* GraphNode : Nodes) ModifyConnections(GraphNode);
			// Keep native selected-expression cleanup and graph bookkeeping; group only
			// adjacent deletes, without moving them across any intervening operation.
			Target.Editor->DeleteNodes(Nodes);
			SyncWorkingCollection(Target);
			for (int32 Index = 0; Index < Expressions.Num(); ++Index)
				if (Target.Material->GetExpressions().Contains(Expressions[Index]))
				{
					FailedIndex = Index;
					return Bad(TEXT("Native editor did not delete the node."));
				}
			auto Result = MakeShared<FJsonObject>();
			if (Ids.Num() == 1) Result->SetStringField(TEXT("nodeId"), Ids[0]);
			Result->SetNumberField(TEXT("deletedNodeCount"), Ids.Num());
			Finish(Target, nullptr, Params, Result);
			Result->SetBoolField(TEXT("compileDeferred"), false);
			Result->SetBoolField(TEXT("nativeEditorRefresh"), true);
			return FMCPToolResult::Ok(Result);
		}

		UObject* LoadMaterialEditorAsset(const FString& AssetPath, FString& OutError)
		{
			if (AssetPath.IsEmpty())
			{
				OutError = TEXT("assetPath is required.");
				return nullptr;
			}

			if (UMaterialInterface* MaterialInterface = MCPMaterialInfrastructure::LoadMaterialInterfaceByName(
				AssetPath, OutError))
			{
				return MaterialInterface;
			}

			FString FunctionError;
			if (UMaterialFunction* Function = MCPMaterialInfrastructure::LoadMaterialFunctionByName(
				AssetPath, FunctionError))
			{
				return Function;
			}

			OutError = FString::Printf(
				TEXT("Material, material instance, or material function '%s' was not found."), *AssetPath);
			return nullptr;
		}

		bool IsMaterialEditorAsset(const UObject* Asset)
		{
			return Asset && (Asset->IsA<UMaterialInterface>() || Asset->IsA<UMaterialFunctionInterface>());
		}

		TSharedPtr<IMaterialEditor> FindMaterialEditor(UObject* Asset, bool bFocusIfOpen = false)
		{
			if (!Asset || !GEditor)
			{
				return nullptr;
			}

			UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditors)
			{
				return nullptr;
			}

			IAssetEditorInstance* Instance = AssetEditors->FindEditorForAsset(Asset, bFocusIfOpen);
			if (!Instance || Instance->GetEditorName() != TEXT("MaterialEditor"))
			{
				return nullptr;
			}

			TSharedPtr<IToolkit> Toolkit = FToolkitManager::Get().FindEditorForAsset(Asset);
			return Toolkit.IsValid() && Toolkit->GetToolkitFName() == TEXT("MaterialEditor")
				       ? StaticCastSharedPtr<IMaterialEditor>(Toolkit)
				       : nullptr;
		}

		FString MaterialEditorNodeId(const UObject* SelectedObject)
		{
			const UMaterialGraphNode* GraphNode = Cast<UMaterialGraphNode>(SelectedObject);
			if (GraphNode)
			{
				if (GraphNode->MaterialExpression)
				{
					return ExpressionNodeId(GraphNode->MaterialExpression);
				}
				return GraphNode->NodeGuid.ToString();
			}

			return ExpressionNodeId(Cast<UMaterialExpression>(const_cast<UObject*>(SelectedObject)));
		}

		void AppendMaterialEditorNodeIds(const TSet<UObject*>& SelectedObjects, TArray<FString>& OutIds)
		{
			TSet<FString> Seen;
			for (UObject* SelectedObject : SelectedObjects)
			{
				const FString NodeId = MaterialEditorNodeId(SelectedObject);
				if (!NodeId.IsEmpty() && !Seen.Contains(NodeId))
				{
					Seen.Add(NodeId);
					OutIds.Add(NodeId);
				}
			}
			OutIds.Sort();
		}

		FString MaterialEditorOpenMethod(const IAssetEditorInstance* Instance)
		{
			if (!Instance)
			{
				return TEXT("unknown");
			}
			switch (Instance->GetOpenMethod())
			{
			case EAssetOpenMethod::Edit:
				return TEXT("edit");
			case EAssetOpenMethod::View:
				return TEXT("view");
			default:
				return TEXT("unknown");
			}
		}

		TSharedPtr<FJsonObject> BuildMaterialEditorState(UObject* Asset, const FString& RequestedPath)
		{
			auto Result = MakeShared<FJsonObject>();
			const FString CanonicalPath = Asset ? Asset->GetPathName() : RequestedPath;
			Result->SetStringField(TEXT("assetPath"), CanonicalPath);
			Result->SetStringField(TEXT("assetClass"), Asset ? Asset->GetClass()->GetPathName() : FString());

			int32 OpenEditorCount = 0;
			int32 MaterialEditorCount = 0;
			bool bRecognizedMaterialEditor = false;
			TArray<TSharedPtr<FJsonValue>> OpenEditors;
			if (GEditor && Asset)
			{
				if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					for (IAssetEditorInstance* Instance : AssetEditors->FindEditorsForAsset(Asset))
					{
						if (!Instance) continue;
						++OpenEditorCount;
						const FString EditorName = Instance->GetEditorName().ToString();
						if (EditorName == TEXT("MaterialEditor"))
						{
							++MaterialEditorCount;
							bRecognizedMaterialEditor = true;
						}
						auto EditorState = MakeShared<FJsonObject>();
						EditorState->SetStringField(TEXT("editorName"), EditorName);
						EditorState->SetBoolField(TEXT("isPrimaryEditor"), Instance->IsPrimaryEditor());
						EditorState->SetStringField(TEXT("openMethod"), MaterialEditorOpenMethod(Instance));
						OpenEditors.Add(MakeShared<FJsonValueObject>(EditorState));
					}
				}
			}

			Result->SetBoolField(TEXT("editorOpen"), MaterialEditorCount > 0);
			Result->SetBoolField(TEXT("isOpen"), MaterialEditorCount > 0);
			Result->SetNumberField(TEXT("openEditorCount"), OpenEditorCount);
			Result->SetNumberField(TEXT("materialEditorCount"), MaterialEditorCount);
			Result->SetArrayField(TEXT("openEditors"), OpenEditors);
			Result->SetBoolField(TEXT("trackedMaterialEditor"), bRecognizedMaterialEditor);
			Result->SetBoolField(TEXT("supportsEditorDirtyProbe"), false);
			Result->SetStringField(TEXT("editorDirtyState"), TEXT("unknown_without_explicit_probe"));
			Result->SetStringField(
				TEXT("editorKind"),
				bRecognizedMaterialEditor
					? (Asset && Asset->IsA<UMaterialFunctionInterface>()
						   ? TEXT("material_function_editor")
						   : TEXT("material_editor"))
					: TEXT("untracked_or_non_material_editor"));

			TArray<TSharedPtr<FJsonValue>> SelectedIds;
			if (TSharedPtr<IMaterialEditor> Editor = FindMaterialEditor(Asset))
			{
				TArray<FString> Ids;
				AppendMaterialEditorNodeIds(Editor->GetSelectedNodes(), Ids);
				for (const FString& Id : Ids) SelectedIds.Add(MakeShared<FJsonValueString>(Id));
				Result->SetBoolField(
					TEXT("hasUnappliedChanges"),
					Editor->GetToolkitCommands()->CanExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef()));
				Result->SetBoolField(
					TEXT("livePreviewEnabled"),
					Editor->GetToolkitCommands()->GetCheckState(
						FMaterialEditorCommands::Get().ToggleLivePreview.ToSharedRef()) == ECheckBoxState::Checked);
				if (UMaterial* PreviewMaterial = Cast<UMaterial>(Editor->GetMaterialInterface()))
					Result->SetStringField(TEXT("previewPath"), PreviewMaterial->GetPathName());
			}
			else
			{
				Result->SetBoolField(TEXT("hasUnappliedChanges"), false);
				Result->SetBoolField(TEXT("livePreviewEnabled"), false);
			}

			Result->SetArrayField(TEXT("selectedNodeIds"), SelectedIds);
			Result->SetNumberField(TEXT("selectedNodeCount"), SelectedIds.Num());
			return Result;
		}

		bool ResolveMaterialEditorNode(UMaterial* PreviewMaterial, const FString& RequestedId,
		                               UMaterialExpression*& OutExpression)
		{
			OutExpression = nullptr;
			if (!PreviewMaterial) return false;
			for (UMaterialExpression* Expression : PreviewMaterial->GetExpressions())
			{
				if (!Expression) continue;
				const UMaterialGraphNode* GraphNode = Cast<UMaterialGraphNode>(Expression->GraphNode.Get());
				if (ExpressionNodeId(Expression) == RequestedId
					|| Expression->GetName() == RequestedId
					|| Expression->MaterialExpressionGuid.ToString() == RequestedId
					|| (GraphNode && GraphNode->NodeGuid.ToString() == RequestedId))
				{
					OutExpression = Expression;
					return true;
				}
			}
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> JsonStringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
			return Result;
		}
	}

	FMCPToolResult MutatePreviewGraph(const FString& Capability, const TSharedPtr<FJsonObject>& Params)
	{
		FTarget Target;
		FString Error;
		if (!Resolve(Params, Target, Error, true)) return Bad(Error);
		if (!Target.Editor) return Bad(TEXT("This operation requires targetContext=editorPreview."));
		if (Target.Expressions.Num() > 20000) return Bad(TEXT("Preview exceeds the 20,000 expression edit budget."));
		auto Result = MakeShared<FJsonObject>();
		if (Capability == TEXT("content.material.expression.add"))
		{
			FString Name = Params->GetStringField(TEXT("expressionClass"));
			if (Name == TEXT("Lerp")) Name = TEXT("LinearInterpolate");
			UClass* Class = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
				if (It->GetName() == TEXT("MaterialExpression") + Name && It->
					IsChildOf(UMaterialExpression::StaticClass()))
				{
					Class = *It;
					break;
				}
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				|| !IsAllowedExpressionType(Class, Target.Function != nullptr) || Class->
				IsChildOf(UMaterialExpressionComposite::StaticClass()) || Class->
				IsChildOf(UMaterialExpressionPinBase::StaticClass()) || Class->IsChildOf(
					UMaterialExpressionComment::StaticClass()))
				return Bad(TEXT("Unsupported expression class for ordinary preview-node creation."));
			int32 X = 0, Y = 0;
			if (!Integer(Params, TEXT("posX"), X) || !Integer(Params, TEXT("posY"), Y))
				return Bad(
					TEXT("Positions must be finite int32 values."));
			if (Target.Expressions.Num() >= 20000) return Bad(TEXT("Preview expression budget reached."));
			BeginEdit(Target);
			Target.Graph()->Modify();
			auto* Expression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
				Target.Material, Target.Function, Class, nullptr, X, Y, false);
			if (!Expression) return Bad(TEXT("Could not create expression."));
			Expression->Function = Target.Function;
			auto* GraphNode = Target.Graph()->AddExpression(Expression, false);
			if (GraphNode)
			{
				GraphNode->SetFlags(RF_Transactional);
				GraphNode->Modify();
			}
			Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
			Result->SetStringField(TEXT("expressionClass"), Name);
			Result->SetNumberField(TEXT("posX"), X);
			Result->SetNumberField(TEXT("posY"), Y);
			return Finish(Target, Expression, Params, Result);
		}
		if (Capability == TEXT("content.material.pin.connect"))
		{
			auto* Source = Node(Target, Params->GetStringField(TEXT("sourceNodeId")));
			auto* Destination = Node(Target, Params->GetStringField(TEXT("targetNodeId")));
			auto* Output = Pin(Source, Params->GetStringField(TEXT("sourcePinName")), EGPD_Output);
			auto* Input = Pin(Destination, Params->GetStringField(TEXT("targetPinName")), EGPD_Input);
			if (!Output || !Input || Source->GetGraph() != Destination->GetGraph())
				return Bad(
					TEXT("Pins must resolve uniquely in the same preview graph; use names or index:N."));
			if (Input->LinkedTo.Contains(Output))
			{
				Result->SetBoolField(TEXT("connected"), true);
				return Finish(Target, nullptr, Params, Result, false);
			}
			if (WouldCycle(Source, Destination))
				return Bad(
					TEXT("Connection would form a loop or exceeds the traversal budget."),
					TEXT("material_graph_cycle"));
			const auto* Schema = Source->GetGraph()->GetSchema();
			const auto Response = Schema->CanCreateConnection(Output, Input);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW) return Bad(Response.Message.ToString());
			BeginEdit(Target);
			Source->GetGraph()->Modify();
			ModifyConnections(Source);
			ModifyConnections(Destination);
			if (!Infrastructure::TryCreateConnection(Schema, Output, Input, true))
				return Bad(
					TEXT("Native material schema rejected the connection."));
			Target.Graph()->LinkMaterialExpressionsFromGraph();
			Result->SetBoolField(TEXT("connected"), true);
			return Finish(Target, nullptr, Params, Result);
		}
		const FString Id = Params->GetStringField(TEXT("nodeId"));
		auto* GraphNode = Node(Target, Id);
		auto* Expression = Target.Find(Id);
		if (!GraphNode) return Bad(TEXT("nodeId is not in this open preview."));
		Result->SetStringField(TEXT("nodeId"), Id);
		if (Capability == TEXT("content.material.pin.disconnect"))
		{
			FString DirectionName;
			Params->TryGetStringField(TEXT("direction"), DirectionName);
			TOptional<EEdGraphPinDirection> Direction;
			if (!DirectionName.IsEmpty())
			{
				if (DirectionName == TEXT("input")) Direction = EGPD_Input;
				else if (DirectionName == TEXT("output")) Direction = EGPD_Output;
				else return Bad(TEXT("direction is input or output."));
			}
			auto* Selected = Pin(GraphNode, Params->GetStringField(TEXT("pinName")), Direction);
			if (!Selected) return Bad(TEXT("Pin is missing or ambiguous; specify direction with index:N when needed."));
			const int32 Count = Selected->LinkedTo.Num();
			Result->SetNumberField(TEXT("disconnectedCount"), Count);
			if (!Count) return Finish(Target, Expression, Params, Result, false);
			BeginEdit(Target);
			GraphNode->GetGraph()->Modify();
			ModifyConnections(GraphNode);
			GraphNode->GetSchema()->UEdGraphSchema::BreakPinLinks(*Selected, true);
			Target.Graph()->LinkMaterialExpressionsFromGraph();
			return Finish(Target, Expression, Params, Result);
		}
		if (!Expression) return Bad(TEXT("The material root is not an expression."));
		if (Capability == TEXT("content.material.function.interface.set"))
		{
			auto* Input = Cast<UMaterialExpressionFunctionInput>(Expression);
			auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression);
			if (!Target.Function || (!Input && !Output))
				return Bad(
					TEXT("Select a FunctionInput or FunctionOutput in the function preview."));
			const FName OldName = Input ? Input->InputName : Output->OutputName;
			FString Name = OldName.ToString();
			Params->TryGetStringField(TEXT("name"), Name);
			const FName NewName(*Name);
			if (Name.TrimStartAndEnd().IsEmpty() || Name.Len() > 128 || NewName.IsNone() || (NewName == OldName && Name
				!= OldName.ToString()))
				return Bad(
					TEXT("Use a distinct nonempty interface name; case-only renames are unsupported."));
			for (auto* Other : Target.Expressions)
				if (Other && Other != Expression)
					if ((Input && Cast<UMaterialExpressionFunctionInput>(Other) && CastChecked<
							UMaterialExpressionFunctionInput>(Other)->InputName == NewName)
						|| (Output && Cast<UMaterialExpressionFunctionOutput>(Other) && CastChecked<
							UMaterialExpressionFunctionOutput>(Other)->OutputName == NewName))
						return Bad(TEXT("Duplicate function interface name."));
			if (Output && (Params->HasField(TEXT("inputType")) || Params->HasField(TEXT("usePreviewValueAsDefault"))))
				return Bad(TEXT("Input-only settings cannot target FunctionOutput."));
			int32 Sort = Input ? Input->SortPriority : Output->SortPriority;
			if (!Integer(Params, TEXT("sortPriority"), Sort)) return Bad(TEXT("sortPriority must be a finite int32."));
			int64 Type = Input ? static_cast<int64>(Input->InputType) : 0;
			FString TypeName;
			if (Params->TryGetStringField(TEXT("inputType"), TypeName))
				Type = StaticEnum<EFunctionInputType>()->
					GetValueByNameString(TEXT("FunctionInput_") + TypeName);
			if (Type < 0 || Type >= FunctionInput_MAX) return Bad(TEXT("Unknown function input type."));
			bool bDefault = Input && Input->bUsePreviewValueAsDefault;
			if (Params->HasField(TEXT("usePreviewValueAsDefault")) && !Params->
				TryGetBoolField(TEXT("usePreviewValueAsDefault"), bDefault))
				return Bad(
					TEXT("usePreviewValueAsDefault must be boolean."));
			const bool bChanged = NewName != OldName || Sort != (Input ? Input->SortPriority : Output->SortPriority) ||
				(Input && (Type != Input->InputType || bDefault != Input->bUsePreviewValueAsDefault));
			if (!bChanged) return Finish(Target, Expression, Params, Result, false);
			BeginEdit(Target);
			Expression->Modify();
			GraphNode->Modify();
			Target.Graph()->Modify();
			Target.Graph()->LinkMaterialExpressionsFromGraph();
			if (Input)
			{
				Input->InputName = NewName;
				Input->InputType = static_cast<EFunctionInputType>(Type);
				Input->SortPriority = Sort;
				Input->bUsePreviewValueAsDefault = bDefault;
			}
			else
			{
				Output->OutputName = NewName;
				Output->SortPriority = Sort;
			}
			GraphNode->ReconstructNode();
			Target.Graph()->LinkGraphNodesFromMaterial();
			Result->SetStringField(TEXT("name"), Name);
			return Finish(Target, Expression, Params, Result);
		}
		if (Capability == TEXT("content.material.expression.delete"))
		{
			int32 FailedIndex = 0;
			return DeletePreviewNodes(Target, {Id}, Params, FailedIndex, []() { return true; });
		}
		if (Capability == TEXT("content.material.expression.move"))
		{
			int32 X = GraphNode->NodePosX, Y = GraphNode->NodePosY;
			if (!Params->HasField(TEXT("posX")) || !Params->HasField(TEXT("posY")) || !Integer(Params, TEXT("posX"), X)
				|| !Integer(Params, TEXT("posY"), Y))
				return Bad(TEXT("Both finite int32 coordinates are required."));
			if (X == GraphNode->NodePosX && Y == GraphNode->NodePosY)
				return Finish(
					Target, Expression, Params, Result, false);
			BeginEdit(Target);
			Expression->Modify();
			GraphNode->Modify();
			GraphNode->NodePosX = X;
			GraphNode->NodePosY = Y;
			Expression->MaterialExpressionEditorX = X;
			Expression->MaterialExpressionEditorY = Y;
			return Finish(Target, Expression, Params, Result);
		}
		if (Capability == TEXT("content.material.expression.value.set"))
		{
			if (Expression->HasAParameterName() || Expression->IsA<UMaterialExpressionCustom>())
			{
				auto P = MakeShared<FJsonObject>();
				P->Values = Params->Values;
				const bool bCustom = Expression->IsA<UMaterialExpressionCustom>();
				P->SetField(bCustom ? TEXT("code") : TEXT("defaultValue"), P->TryGetField(TEXT("value")));
				P->RemoveField(TEXT("value"));
				FString Name;
				if (P->TryGetStringField(TEXT("parameterName"), Name))
				{
					P->SetStringField(TEXT("name"), Name);
					P->RemoveField(TEXT("parameterName"));
				}
				auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
				const FString ToolId = bCustom
					                       ? TEXT("content.material.custom.set")
					                       : TEXT("content.material.parameter.set");
				TArray<FString> Errors;
				if (!Registry->ValidateParams(ToolId, P, Errors)) return Bad(FString::Join(Errors, TEXT("; ")));
				return Registry->FindTool(ToolId)->Execute(P);
			}
			TFunction<void()> Apply;
			bool bChanged = false;
			if (auto* Constant = Cast<UMaterialExpressionConstant>(Expression))
			{
				float Value = Constant->R;
				if (!Number(Params, TEXT("value"), Value))
					return
						Bad(TEXT("Constant requires a finite numeric value."));
				bChanged = Value != Constant->R;
				Apply = [Constant, Value]() { Constant->R = Value; };
			}
			else
			{
				const TSharedPtr<FJsonObject>* Value = nullptr;
				if (!Params->TryGetObjectField(TEXT("value"), Value))
					return Bad(
						TEXT("Vector and mask values require an object."));
				auto* V2 = Cast<UMaterialExpressionConstant2Vector>(Expression);
				auto* V3 = Cast<UMaterialExpressionConstant3Vector>(Expression);
				auto* V4 = Cast<UMaterialExpressionConstant4Vector>(Expression);
				if (V2 || V3 || V4)
				{
					FLinearColor Before = V2 ? FLinearColor(V2->R, V2->G, 0, 1) : V3 ? V3->Constant : V4->Constant,
					             After = Before;
					if (!Number(*Value, TEXT("r"), After.R) || !Number(*Value, TEXT("g"), After.G) || (!V2 && !
						Number(*Value, TEXT("b"), After.B)) || (V4 && !Number(*Value, TEXT("a"), After.A)))
						return Bad(
							TEXT("Vector channels must be finite."));
					bChanged = Before != After;
					Apply = [V2, V3, V4, After]()
					{
						if (V2)
						{
							V2->R = After.R;
							V2->G = After.G;
						}
						else if (V3) V3->Constant = After;
						else V4->Constant = After;
					};
				}
				else if (auto* Mask = Cast<UMaterialExpressionComponentMask>(Expression))
				{
					bool R = Mask->R, G = Mask->G, B = Mask->B, A = Mask->A;
					auto Read = [&](const TCHAR* Field, bool& Out)
					{
						return !(*Value)->HasField(Field) || (*Value)->TryGetBoolField(Field, Out);
					};
					if (!Read(TEXT("r"), R) || !Read(TEXT("g"), G) || !Read(TEXT("b"), B) || !Read(TEXT("a"), A))
						return
							Bad(TEXT("Mask channels must be booleans."));
					bChanged = R != Mask->R || G != Mask->G || B != Mask->B || A != Mask->A;
					Apply = [Mask, R, G, B, A]()
					{
						Mask->R = R;
						Mask->G = G;
						Mask->B = B;
						Mask->A = A;
					};
				}
				else return Bad(TEXT("Expression type does not support direct value editing."));
			}
			if (!bChanged) return Finish(Target, Expression, Params, Result, false);
			BeginEdit(Target);
			Expression->Modify();
			Apply();
			return Finish(Target, Expression, Params, Result);
		}
		return Bad(TEXT("Unsupported preview graph operation."));
	}
}

class FTool_EditMaterialPreviewBatch : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.batch"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::MaterialEditing;
		static const TSet<FString> Allowed{
			TEXT("content.material.expression.add"), TEXT("content.material.expression.delete"),
			TEXT("content.material.expression.move"), TEXT("content.material.expression.value.set"),
			TEXT("content.material.pin.connect"), TEXT("content.material.pin.disconnect"),
			TEXT("content.material.custom.set"), TEXT("content.material.parameter.set"),
			TEXT("content.material.function.call.set"), TEXT("content.material.function.interface.set")
		};
		FString Text;
		FJsonSerializer::Serialize(Params.ToSharedRef(),
		                           TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		if (FTCHARToUTF8(*Text).Length() > 512 * 1024) return Bad(TEXT("Preview batch exceeds 512 KiB UTF-8 JSON."));
		const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
		if (!Params->TryGetArrayField(TEXT("operations"), Operations) || Operations->IsEmpty() || Operations->Num() >
			128)
			return Bad(TEXT("A batch needs 1..128 operations."));
		if (!GEditor || !GEditor->Trans || GEditor->IsTransactionActive())
			return Bad(
				TEXT("Wait for the current native transaction to finish before starting a preview batch."));
		auto Scoped = MakeShared<FJsonObject>();
		Scoped->Values = Params->Values;
		Scoped->SetStringField(TEXT("targetContext"), TEXT("editorPreview"));
		FTarget Target;
		FString Error;
		if (!Resolve(Scoped, Target, Error, true)) return Bad(Error);
		bool bRefresh = true, bRequireShader = false;
		Params->TryGetBoolField(TEXT("refresh"), bRefresh);
		Params->TryGetBoolField(TEXT("requireValidShader"), bRequireShader);
		if (bRequireShader && (!bRefresh || Target.Function))
			return Bad(TEXT(
				"requireValidShader requires a material preview and refresh=true; function validity needs an actual applied host."));
		if (Target.Expressions.Num() > 20000) return Bad(TEXT("Preview exceeds the 20,000 expression batch budget."));
		auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
		struct FOperation
		{
			FString Id, Capability;
			TSharedPtr<FJsonObject> Params;
		};
		TArray<FOperation> Prepared;
		TSet<FString> UsedIds, AddIds;
		// Validate every public schema before the first edit. Never let nested args
		// replace the scope, preview identity, or trusted execution metadata.
		for (const auto& Value : *Operations)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object) return Bad(TEXT("Operations must be objects."));
			auto Op = Value->AsObject();
			FOperation Item;
			const TSharedPtr<FJsonObject>* Input = nullptr;
			if (!Op->TryGetStringField(TEXT("id"), Item.Id) || Item.Id.IsEmpty() || Item.Id.Len() > 64 || UsedIds.
				Contains(Item.Id)
				|| !Op->TryGetStringField(TEXT("capability"), Item.Capability) || !Allowed.Contains(Item.Capability) ||
				!Op->TryGetObjectField(TEXT("params"), Input))
				return Bad(TEXT("Each operation needs a unique id, supported capability, and params object."));
			Item.Params = MakeShared<FJsonObject>();
			Item.Params->Values = (*Input)->Values;
			for (const auto& Field : Item.Params->Values)
				if (Field.Key.StartsWith(TEXT("__")) || Field.Key == TEXT("material") || Field.Key ==
					TEXT("materialFunction") || Field.Key == TEXT("targetContext") || Field.Key ==
					TEXT("expectedPreviewId"))
					return Bad(
						TEXT("Operation params cannot override batch scope or execution context."));
			for (const TCHAR* Field : {TEXT("nodeId"), TEXT("sourceNodeId"), TEXT("targetNodeId")})
			{
				FString Reference;
				if (Item.Params->TryGetStringField(Field, Reference) && Reference.StartsWith(TEXT("$")) && !AddIds.
					Contains(Reference.Mid(1)))
					return Bad(
						TEXT("Node aliases must refer to an earlier expression.add id."));
			}
			Item.Params->SetStringField(Target.Function ? TEXT("materialFunction") : TEXT("material"),
			                            Target.OriginalAsset->GetPathName());
			Item.Params->SetStringField(TEXT("targetContext"), TEXT("editorPreview"));
			Item.Params->SetStringField(TEXT("expectedPreviewId"), PreviewId(Target));
			TArray<FString> Errors;
			if (!Registry->ValidateParams(Item.Capability, Item.Params, Errors))
				return Bad(
					FString::Join(Errors, TEXT("; ")));
			UsedIds.Add(Item.Id);
			if (Item.Capability == TEXT("content.material.expression.add")) AddIds.Add(Item.Id);
			Prepared.Add(MoveTemp(Item));
		}
		const FString Before = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Target.Material);
		FString Expected;
		if (Params->TryGetStringField(TEXT("expectedStateHash"), Expected) && Expected != Before)
			return Bad(
				TEXT("Preview graph changed; capture its editor context again."), TEXT("material_edit_conflict"));
		const FText Title = FText::FromString(
			TEXT("UE AI material preview batch ") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		auto Transaction = MakeUnique<FScopedTransaction>(TEXT("UEAIMaterialPreview"), Title, Target.Material);
		FScopedPreviewPause PreviewPause(Target.Editor);
		TMap<FString, FString> Aliases;
		TArray<TSharedPtr<FJsonValue>> Results;
		FString FailedId, Failure;
		int32 NativeRefreshes = 0;
		bool bDeletionCompileDeferred = false;
		auto ResolveAliases = [&](FOperation& Op)
		{
			for (const TCHAR* Field : {TEXT("nodeId"), TEXT("sourceNodeId"), TEXT("targetNodeId")})
			{
				FString Reference;
				if (Op.Params->TryGetStringField(Field, Reference) && Reference.StartsWith(TEXT("$")))
					Op.Params->
					   SetStringField(Field, Aliases.FindRef(Reference.Mid(1)));
			}
		};
		auto AppendSummary = [&](const FOperation& Op, const TSharedPtr<FJsonObject>& Data)
		{
			auto Summary = MakeShared<FJsonObject>();
			Summary->SetStringField(TEXT("id"), Op.Id);
			Summary->SetStringField(TEXT("capability"), Op.Capability);
			for (const TCHAR* Field : {
				     TEXT("nodeId"), TEXT("stateHash"), TEXT("changed"), TEXT("saved"), TEXT("compileDeferred"),
				     TEXT("nativeEditorRefresh")
			     })
				if (Data->HasField(Field)) Summary->SetField(Field, Data->TryGetField(Field));
			Results.Add(MakeShared<FJsonValueObject>(Summary));
			return Summary;
		};
		for (int32 Index = 0; Index < Prepared.Num(); ++Index)
		{
			auto& Op = Prepared[Index];
			ResolveAliases(Op);
			if (Op.Capability == TEXT("content.material.expression.delete"))
			{
				int32 End = Index;
				TArray<FString> Ids;
				while (End < Prepared.Num() && Prepared[End].Capability == Op.Capability)
				{
					ResolveAliases(Prepared[End]);
					Ids.Add(Prepared[End].Params->GetStringField(TEXT("nodeId")));
					++End;
				}
				FTarget DeleteTarget;
				if (!Resolve(Op.Params, DeleteTarget, Error, true))
				{
					FailedId = Op.Id;
					Failure = Error;
					break;
				}
				int32 FailedIndex = 0;
				const auto Deleted = DeletePreviewNodes(DeleteTarget, Ids, Op.Params, FailedIndex,
				                                        [&]() { return !bRefresh || PreviewPause.Pause(); });
				bDeletionCompileDeferred |= PreviewPause.IsPaused();
				if (!Deleted.bSuccess)
				{
					FailedId = Prepared[Index + FailedIndex].Id;
					Failure = Deleted.ErrorMessage;
					break;
				}
				++NativeRefreshes;
				for (int32 Row = Index; Row < End; ++Row)
				{
					auto Summary = AppendSummary(Prepared[Row], Deleted.Data);
					Summary->SetStringField(TEXT("nodeId"), Ids[Row - Index]);
					Summary->SetStringField(TEXT("deletionGroupId"), Op.Id);
					Summary->SetNumberField(TEXT("deletionGroupSize"), End - Index);
					Summary->SetBoolField(TEXT("nativeEditorRefresh"), Row == End - 1);
					Summary->SetBoolField(TEXT("compileDeferred"), PreviewPause.IsPaused());
				}
				Index = End - 1;
				continue;
			}
			const auto Executed = Registry->FindTool(Op.Capability)->Execute(Op.Params);
			if (!Executed.bSuccess)
			{
				FailedId = Op.Id;
				Failure = Executed.ErrorMessage;
				break;
			}
			AppendSummary(Op, Executed.Data);
			FString Id;
			if (Op.Capability == TEXT("content.material.expression.add") && Executed.Data->
				TryGetStringField(TEXT("nodeId"), Id))
				Aliases.Add(Op.Id, Id);
		}
		auto Outcome = MakeShared<FJsonObject>();
		DescribeTarget(Target, Outcome);
		Outcome->SetArrayField(TEXT("operations"), Results);
		Outcome->SetBoolField(TEXT("saved"), false);
		Outcome->SetBoolField(TEXT("applied"), false);
		Outcome->SetNumberField(TEXT("nativeDeletionRefreshes"), NativeRefreshes);
		Outcome->SetBoolField(TEXT("nativeDeletionCompileDeferred"), bDeletionCompileDeferred);
		const bool bChanged = Before != UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(
			Target.Material);
		if (Failure.IsEmpty() && bRefresh && (bChanged || bRequireShader))
		{
			auto Refresh = MakeShared<FJsonObject>();
			Refresh->SetStringField(Target.Function ? TEXT("materialFunction") : TEXT("material"),
			                        Target.OriginalAsset->GetPathName());
			Refresh->SetStringField(TEXT("expectedPreviewId"), PreviewId(Target));
			Refresh->SetBoolField(TEXT("waitForCompilation"), true);
			FMCPToolResult Refreshed;
			if (PreviewPause.IsPaused())
			{
				const TCHAR* ErrorCode = nullptr;
				if (!ValidatePreviewDependencies(Target, Error, ErrorCode)) Refreshed = Bad(Error, ErrorCode);
				else
				{
					// Update the graph and parameter panel with Live Preview paused.
					// Restoring the original native toggle then compiles the base ONCE.
					const bool bShaderCacheRefreshed = PrepareMaterialSourceValidation(Target.Material);
					Target.Editor->UpdateMaterialAfterGraphChange();
					Refreshed = PreviewPause.Restore()
						            ? FMCPToolResult::Ok(CompletePreviewUpdate(Target, true))
						            : Bad(TEXT("Could not restore Live Preview."));
					if (Refreshed.bSuccess)
						Refreshed.Data->SetBoolField(
							TEXT("shaderFileCacheRefreshed"), bShaderCacheRefreshed);
				}
			}
			else Refreshed = Registry->FindTool(TEXT("content.material.editor.refresh"))->Execute(Refresh);
			if (!Refreshed.bSuccess)
			{
				Failure = Refreshed.ErrorMessage;
				FailedId = TEXT("refresh");
			}
			else
			{
				Outcome->SetObjectField(TEXT("diagnostics"), Refreshed.Data);
				bool bValid = false;
				if (bRequireShader && (!Refreshed.Data->TryGetBoolField(TEXT("valid"), bValid) || !bValid))
				{
					Failure = TEXT("Preview shader validation did not succeed.");
					FailedId = TEXT("refresh");
				}
			}
		}
		if (!bChanged) Transaction->Cancel();
		Transaction.Reset();
		if (!Failure.IsEmpty())
		{
			const auto Undo = GEditor->Trans->GetUndoContext(false);
			// OperationId is cleared when recording ends; only TransactionId remains
			// valid on a completed undo record. Never undo another owner's transaction.
			const bool bOwned = Undo.TransactionId.IsValid() && Undo.Context == TEXT("UEAIMaterialPreview") && Undo.
				Title.EqualTo(Title);
			const bool bUndone = bChanged && bOwned && GEditor->UndoTransaction(false);
			const bool bPreviewRestored = PreviewPause.Restore();
			const bool bVerified = bPreviewRestored && (!bChanged || bUndone) && Before ==
				UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Target.Material);
			Outcome->SetBoolField(TEXT("livePreviewRestored"), bPreviewRestored);
			Outcome->SetBoolField(TEXT("undoRecordOwned"), bOwned);
			Outcome->SetBoolField(TEXT("nativeUndoApplied"), bUndone);
			Outcome->SetStringField(TEXT("undoContext"), Undo.Context);
			Outcome->SetStringField(TEXT("undoTitle"), Undo.Title.ToString());
			Outcome->SetBoolField(TEXT("success"), false);
			Outcome->SetStringField(TEXT("status"), bVerified ? TEXT("rolledBack") : TEXT("rollbackFailed"));
			Outcome->SetStringField(TEXT("failedOperation"), FailedId);
			Outcome->SetStringField(TEXT("error"), Failure);
			Outcome->SetBoolField(TEXT("rollbackVerified"), bVerified);
			Outcome->SetStringField(
				TEXT("rollbackCoverage"), TEXT("previewGraphStructure; native UI dirty state may remain set"));
			if (!bVerified)
			{
				auto Failed = Bad(
					FString::Printf(
						TEXT(
							"Preview rollback could not be verified (owned=%d, undo=%d, active=%d, context=%s, title=%s): %s"),
						bOwned, bUndone, GEditor->IsTransactionActive(), *Undo.Context, *Undo.Title.ToString(),
						*Failure), TEXT("preview_batch_rollback_failed"));
				Failed.Data = Outcome;
				return Failed;
			}
		}
		else
		{
			if (!PreviewPause.Restore())
				return Bad(
					TEXT("Could not restore Live Preview."), TEXT("preview_refresh_restore_failed"));
			Outcome->SetBoolField(TEXT("livePreviewRestored"), true);
			Outcome->SetBoolField(TEXT("success"), true);
			Outcome->SetStringField(TEXT("status"), TEXT("completed"));
		}
		Outcome->SetBoolField(TEXT("changed"), bChanged && Failure.IsEmpty());
		Outcome->SetStringField(
			TEXT("stateHash"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Target.Material));
		return FMCPToolResult::Ok(Outcome);
	}
};

class FTool_OpenMaterialEditor final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.open"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
			return FMCPToolResult::Error(TEXT("assetPath is required."), TEXT("invalid_params"), 400);
		FString Error;
		UObject* Asset = UEAIIntegration::MaterialEditing::LoadMaterialEditorAsset(AssetPath, Error);
		if (!Asset || !UEAIIntegration::MaterialEditing::IsMaterialEditorAsset(Asset))
			return FMCPToolResult::Error(Error.IsEmpty() ? TEXT("Asset is not a material editor asset.") : Error,
			                             TEXT("asset_not_found"), 404);
		if (!GEditor) return FMCPToolResult::Error(TEXT("GEditor is unavailable."), TEXT("editor_unavailable"), 503);
		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditors)
			return FMCPToolResult::Error(
				TEXT("AssetEditorSubsystem is unavailable."), TEXT("editor_unavailable"), 503);
		const bool bOpened = AssetEditors->OpenEditorForAsset(Asset);
		auto Result = UEAIIntegration::MaterialEditing::BuildMaterialEditorState(Asset, AssetPath);
		const bool bOpenVerified = Result->GetBoolField(TEXT("editorOpen"));
		Result->SetBoolField(TEXT("success"), bOpened || bOpenVerified);
		Result->SetBoolField(TEXT("openVerified"), bOpenVerified);
		Result->SetBoolField(TEXT("openRequested"), true);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_CloseMaterialEditor final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.close"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
			return FMCPToolResult::Error(TEXT("assetPath is required."), TEXT("invalid_params"), 400);
		FString Error;
		UObject* Asset = UEAIIntegration::MaterialEditing::LoadMaterialEditorAsset(AssetPath, Error);
		if (!Asset || !UEAIIntegration::MaterialEditing::IsMaterialEditorAsset(Asset))
			return FMCPToolResult::Error(Error.IsEmpty() ? TEXT("Asset is not a material editor asset.") : Error,
			                             TEXT("asset_not_found"), 404);
		if (!GEditor) return FMCPToolResult::Error(TEXT("GEditor is unavailable."), TEXT("editor_unavailable"), 503);
		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditors)
			return FMCPToolResult::Error(
				TEXT("AssetEditorSubsystem is unavailable."), TEXT("editor_unavailable"), 503);
		int32 ClosedCount = 0;
		for (IAssetEditorInstance* Instance : AssetEditors->FindEditorsForAssetAndSubObjects(Asset))
			if (Instance && Instance->CloseWindow(EAssetEditorCloseReason::AssetEditorHostClosed)) ++ClosedCount;
		auto Result = UEAIIntegration::MaterialEditing::BuildMaterialEditorState(Asset, AssetPath);
		const bool bStillOpen = Result->GetBoolField(TEXT("editorOpen"));
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("closedCount"), ClosedCount);
		Result->SetBoolField(TEXT("closeRequested"), true);
		Result->SetBoolField(TEXT("stillOpen"), bStillOpen);
		Result->SetBoolField(TEXT("closeVerified"), !bStillOpen);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_GetMaterialEditorState final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.state"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
			return FMCPToolResult::Error(TEXT("assetPath is required."), TEXT("invalid_params"), 400);
		FString Error;
		UObject* Asset = UEAIIntegration::MaterialEditing::LoadMaterialEditorAsset(AssetPath, Error);
		if (!Asset || !UEAIIntegration::MaterialEditing::IsMaterialEditorAsset(Asset))
			return FMCPToolResult::Error(Error.IsEmpty() ? TEXT("Asset is not a material editor asset.") : Error,
			                             TEXT("asset_not_found"), 404);
		return FMCPToolResult::Ok(UEAIIntegration::MaterialEditing::BuildMaterialEditorState(Asset, AssetPath));
	}
};

class FTool_SelectMaterialEditorNodes final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.nodes.select"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
			return FMCPToolResult::Error(TEXT("assetPath is required."), TEXT("invalid_params"), 400);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(TEXT("nodeIds"), Values) || !Values || Values->IsEmpty() || Values->Num() > 256)
			return FMCPToolResult::Error(TEXT("nodeIds must contain 1..256 entries."), TEXT("invalid_params"), 400);
		TArray<FString> Requested;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().IsEmpty())
				return FMCPToolResult::Error(
					TEXT("nodeIds entries must be non-empty strings."), TEXT("invalid_params"), 400);
			Requested.Add(Value->AsString());
		}
		FString Error;
		UObject* Asset = UEAIIntegration::MaterialEditing::LoadMaterialEditorAsset(AssetPath, Error);
		if (!Asset || !UEAIIntegration::MaterialEditing::IsMaterialEditorAsset(Asset))
			return FMCPToolResult::Error(Error.IsEmpty() ? TEXT("Asset is not a material editor asset.") : Error,
			                             TEXT("asset_not_found"), 404);
		if (!GEditor) return FMCPToolResult::Error(TEXT("GEditor is unavailable."), TEXT("editor_unavailable"), 503);
		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditors)
			return FMCPToolResult::Error(
				TEXT("AssetEditorSubsystem is unavailable."), TEXT("editor_unavailable"), 503);
		AssetEditors->OpenEditorForAsset(Asset);
		TSharedPtr<IMaterialEditor> Editor = UEAIIntegration::MaterialEditing::FindMaterialEditor(Asset, true);
		if (!Editor.IsValid())
			return FMCPToolResult::Error(
				TEXT("No active Material Editor was resolved."), TEXT("editor_not_open"), 409);
		UMaterial* PreviewMaterial = Cast<UMaterial>(Editor->GetMaterialInterface());
		if (!PreviewMaterial)
			return FMCPToolResult::Error(
				TEXT("Material Editor preview material is unavailable."), TEXT("editor_invalid"), 409);
		TArray<UMaterialExpression*> Expressions;
		for (const FString& Id : Requested)
		{
			UMaterialExpression* Expression = nullptr;
			if (!UEAIIntegration::MaterialEditing::ResolveMaterialEditorNode(PreviewMaterial, Id, Expression) || !
				Expression || !Expression->GraphNode)
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Node '%s' was not found in the open Material Editor."), *Id),
					TEXT("node_not_found"), 404);
			Expressions.Add(Expression);
		}
		// AddToSelection is additive; validating all expressions before this point
		// keeps a failed request from partially changing the native selection.
		for (UMaterialExpression* Expression : Expressions) Editor->AddToSelection(Expression);
		TArray<FString> Selected;
		UEAIIntegration::MaterialEditing::AppendMaterialEditorNodeIds(Editor->GetSelectedNodes(), Selected);
		TSet<FString> SelectedSet;
		for (const FString& SelectedId : Selected) SelectedSet.Add(SelectedId);
		TArray<FString> Missing;
		for (const FString& RequestedId : Requested)
			if (!SelectedSet.Contains(RequestedId)) Missing.Add(RequestedId);
		auto Result = MakeShared<FJsonObject>();
		const bool bSelectionVerified = Missing.IsEmpty();
		Result->SetBoolField(TEXT("success"), bSelectionVerified);
		Result->SetBoolField(TEXT("selectionVerified"), bSelectionVerified);
		Result->SetStringField(TEXT("assetPath"), Asset->GetPathName());
		Result->SetArrayField(TEXT("requestedNodeIds"), UEAIIntegration::MaterialEditing::JsonStringArray(Requested));
		Result->SetArrayField(TEXT("selectedNodeIds"), UEAIIntegration::MaterialEditing::JsonStringArray(Selected));
		Result->SetNumberField(TEXT("selectedObjectCount"), Editor->GetSelectedNodes().Num());
		Result->SetArrayField(TEXT("missingNodeIds"), UEAIIntegration::MaterialEditing::JsonStringArray(Missing));
		if (!bSelectionVerified)
		{
			Result->SetStringField(
				TEXT("error"), TEXT("Material Editor selection readback did not contain every requested node."));
			FMCPToolResult Failure = FMCPToolResult::Error(
				TEXT("Material Editor selection readback did not contain every requested node."),
				TEXT("selection_readback_failed"), 409);
			Failure.Data = Result;
			return Failure;
		}
		return FMCPToolResult::Ok(Result);
	}
};

namespace UEAIIntegrationTools
{
	void RegisterMaterialEditorGraphTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_EditMaterialPreviewBatch>());
		Registry.Register(MakeShared<FTool_OpenMaterialEditor>());
		Registry.Register(MakeShared<FTool_CloseMaterialEditor>());
		Registry.Register(MakeShared<FTool_GetMaterialEditorState>());
		Registry.Register(MakeShared<FTool_SelectMaterialEditorNodes>());
	}
}
