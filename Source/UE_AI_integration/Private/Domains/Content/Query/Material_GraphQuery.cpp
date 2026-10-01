#include "Infrastructure/MaterialGraphSnapshot.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Infrastructure/DeferredGraphMutation.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/Sha256.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Tools/MCPToolRegistry.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "Engine/Texture.h"

#include "Materials/MaterialExpressionNamedReroute.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Misc/Base64.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "String/LexFromString.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UEAIIntegration::MaterialQuery
{
	namespace
	{
		constexpr int32 MaxCaptureNodes = 20000;
		constexpr int32 MaxCaptureEdges = 100000;
		constexpr int32 MaxPinsPerNode = 256;
		constexpr int32 MaxEntries = 8;
		constexpr int32 MaxBoundaryProofs = 64;
		constexpr double LifetimeSeconds = 300;
		constexpr uint64 MaxCacheBytes = 64ull * 1024 * 1024;
		constexpr uint64 MaxSnapshotBytes = 32ull * 1024 * 1024;
		constexpr int32 ResponseBudget = 256 * 1024;
		constexpr int32 PageScanBudget = 4096;
		constexpr int32 MaxDefinitionProperties = 32;
		constexpr int32 MaxDefinitionInputs = 64;
		constexpr int32 MaxDefinitionOutputs = 64;
		constexpr int32 MaxDefinitionEnumValues = 64;
		constexpr int32 MaxDefinitionDefaultTextCharacters = 512;
		constexpr int32 DefinitionResponseReserveBytes = 16 * 1024;
		constexpr int32 MaxDefinitionClassesScanned = 50000;
		constexpr int32 MaxDefinitionContractsBuilt = 1024;
		// All public entry points execute on the Editor game thread, like domain tools.
		TArray<TSharedPtr<const FSnapshot>> Snapshots;
		TArray<TSharedPtr<const FBoundaryProof>> BoundaryProofs;

		FString JsonText(const TSharedPtr<FJsonObject>& Object)
		{
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
			return Text;
		}

		FString Digest(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			FString Hash;
			Infrastructure::TrySha256Hex(Utf8.Get(), Utf8.Length(), Hash);
			return Hash;
		}

		FString CanonicalJsonText(const TSharedPtr<FJsonObject>& Object)
		{
			return UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
				MakeShared<FJsonValueObject>(Object));
		}

		bool JsonObjectsEqual(
			const TSharedPtr<FJsonObject>& Left,
			const TSharedPtr<FJsonObject>& Right)
		{
			return CanonicalJsonText(Left) == CanonicalJsonText(Right);
		}

		TArray<FString> JsonObjectChangedFields(
			const TSharedPtr<FJsonObject>& Left,
			const TSharedPtr<FJsonObject>& Right)
		{
			TSet<FString> Keys;
			if (Left.IsValid())
			{
				for (const auto& Entry : Left->Values)
				{
					Keys.Add(Entry.Key);
				}
			}
			if (Right.IsValid())
			{
				for (const auto& Entry : Right->Values)
				{
					Keys.Add(Entry.Key);
				}
			}

			TArray<FString> SortedKeys = Keys.Array();
			SortedKeys.Sort();
			TArray<FString> Changed;
			for (const FString& Key : SortedKeys)
			{
				const TSharedPtr<FJsonValue>* LeftValue = Left.IsValid()
					                                          ? Left->Values.Find(Key)
					                                          : nullptr;
				const TSharedPtr<FJsonValue>* RightValue = Right.IsValid()
					                                           ? Right->Values.Find(Key)
					                                           : nullptr;
				if (!LeftValue || !RightValue || !LeftValue->IsValid() || !RightValue->IsValid()
					|| !FJsonValue::CompareEqual(**LeftValue, **RightValue))
				{
					Changed.Add(Key);
				}
			}
			return Changed;
		}

		FString MaterialFunctionType(const UMaterialFunction& Function)
		{
			if (Function.IsA<UMaterialFunctionMaterialLayer>()) return TEXT("MaterialLayer");
			if (Function.IsA<UMaterialFunctionMaterialLayerBlend>()) return TEXT("MaterialLayerBlend");
			return TEXT("MaterialFunction");
		}

		TSharedPtr<FJsonObject> ProjectFunctionMetadata(const UMaterialFunction& Function)
		{
			auto Metadata = MakeShared<FJsonObject>();
			Metadata->SetStringField(TEXT("type"), MaterialFunctionType(Function));
			Metadata->SetStringField(TEXT("description"), Function.Description);
			Metadata->SetStringField(TEXT("userExposedCaption"), Function.UserExposedCaption);
			Metadata->SetBoolField(TEXT("exposeToLibrary"), Function.bExposeToLibrary);
			Metadata->SetBoolField(TEXT("prefixParameterNames"), Function.bPrefixParameterNames);
			Metadata->SetBoolField(TEXT("enableExecWire"), Function.bEnableExecWire);
			Metadata->SetBoolField(TEXT("enableNewHLSLGenerator"), Function.bEnableNewHLSLGenerator);
			TArray<TSharedPtr<FJsonValue>> Categories;
			for (const FText& Category : Function.LibraryCategoriesText)
			{
				Categories.Add(MakeShared<FJsonValueString>(Category.ToString()));
			}
			Metadata->SetArrayField(TEXT("libraryCategories"), Categories);
			return Metadata;
		}

		// Compact typed projection for the settings editable through our material APIs.
		// Large text contributes hashes; it is never copied into each graph node response.
		bool ProjectEditableSettings(UMaterialExpression* E, const TSharedPtr<FJsonObject>& Data, int32& TextChars)
		{
			auto Spend = [&](const FString& Text)
			{
				if (Text.Len() > 1024 * 1024 || Text.Len() > 8 * 1024 * 1024 - TextChars) return false;
				TextChars += Text.Len();
				return true;
			};
			if (!Spend(E->Desc)) return false;
			Data->SetStringField(TEXT("descriptionHash"), Digest(E->Desc));
			auto Number = [&](const TCHAR* Key, float Value)
			{
				if (!FMath::IsFinite(Value)) return false;
				Data->SetNumberField(Key, Value);
				return true;
			};
			auto Color = [&](const TCHAR* Key, const FLinearColor& Value)
			{
				if (!FMath::IsFinite(Value.R) || !FMath::IsFinite(Value.G) || !FMath::IsFinite(Value.B) || !
					FMath::IsFinite(Value.A))
					return false;
				auto V = MakeShared<FJsonObject>();
				V->SetNumberField(TEXT("r"), Value.R);
				V->SetNumberField(TEXT("g"), Value.G);
				V->SetNumberField(TEXT("b"), Value.B);
				V->SetNumberField(TEXT("a"), Value.A);
				Data->SetObjectField(Key, V);
				return true;
			};
			if (const auto* P = Cast<UMaterialExpressionParameter>(E))
			{
				Data->SetStringField(TEXT("group"), P->Group.ToString());
				Data->SetNumberField(TEXT("sortPriority"), P->SortPriority);
				Data->SetStringField(TEXT("parameterGuid"), P->ExpressionGUID.ToString());
			}
			if (const auto* P = Cast<UMaterialExpressionScalarParameter>(E))
			{
				if (!Number(TEXT("value"), P->DefaultValue) || !Number(TEXT("sliderMin"), P->SliderMin) || !Number(
					TEXT("sliderMax"), P->SliderMax))
					return false;
			}
			if (const auto* P = Cast<UMaterialExpressionVectorParameter>(E))
				if (!Color(TEXT("value"), P->DefaultValue))
					return false;
			if (const auto* P = Cast<UMaterialExpressionStaticBoolParameter>(E))
				Data->SetBoolField(
					TEXT("value"), P->DefaultValue);
			if (const auto* P = Cast<UMaterialExpressionTextureSampleParameter>(E))
			{
				Data->SetStringField(TEXT("value"), P->Texture ? P->Texture->GetPathName() : FString());
				Data->SetNumberField(TEXT("samplerType"), P->SamplerType);
				Data->SetStringField(TEXT("group"), P->Group.ToString());
				Data->SetNumberField(TEXT("sortPriority"), P->SortPriority);
				Data->SetStringField(TEXT("parameterGuid"), P->ExpressionGUID.ToString());
			}
			if (const auto* C = Cast<UMaterialExpressionConstant>(E)) if (!Number(TEXT("value"), C->R)) return false;
			if (const auto* C = Cast<UMaterialExpressionConstant2Vector>(E))
			{
				if (!Number(TEXT("valueR"), C->R) || !Number(TEXT("valueG"), C->G)) return false;
			}
			if (const auto* C = Cast<UMaterialExpressionConstant3Vector>(E))
				if (!Color(TEXT("value"), C->Constant))
					return false;
			if (const auto* C = Cast<UMaterialExpressionConstant4Vector>(E))
				if (!Color(TEXT("value"), C->Constant))
					return false;
			if (const auto* Mask = Cast<UMaterialExpressionComponentMask>(E))
			{
				auto Value = MakeShared<FJsonObject>();
				Value->SetBoolField(TEXT("r"), Mask->R != 0);
				Value->SetBoolField(TEXT("g"), Mask->G != 0);
				Value->SetBoolField(TEXT("b"), Mask->B != 0);
				Value->SetBoolField(TEXT("a"), Mask->A != 0);
				Data->SetObjectField(TEXT("componentMask"), Value);
			}
			if (const auto* Input = Cast<UMaterialExpressionFunctionInput>(E))
			{
				Data->SetStringField(TEXT("interfaceId"), Input->Id.ToString());
				if (!Color(TEXT("previewValue"), FLinearColor(Input->PreviewValue.X, Input->PreviewValue.Y,
				                                              Input->PreviewValue.Z,
				                                              Input->PreviewValue.W)))
					return false;
			}
			if (const auto* Output = Cast<UMaterialExpressionFunctionOutput>(E))
				Data->SetStringField(
					TEXT("interfaceId"), Output->Id.ToString());
			if (const auto* C = Cast<UMaterialExpressionCustom>(E))
			{
				if (!Spend(C->Code) || !Spend(C->Description) || C->Inputs.Num() > 256 || C->AdditionalOutputs.Num() >
					256 || C->AdditionalDefines.Num() > 256 || C->IncludeFilePaths.Num() > 256)
					return false;
				Data->SetNumberField(TEXT("codeCharacters"), C->Code.Len());
				Data->SetStringField(TEXT("codeHash"), Digest(C->Code));
				Data->SetNumberField(TEXT("customOutputType"), C->OutputType);
				Data->SetStringField(TEXT("customDescriptionHash"), Digest(C->Description));
				FString Config;
				auto Append = [&](const FString& Text)
				{
					if (!Spend(Text)) return false;
					const FString Prefix = FString::FromInt(Text.Len()) + TEXT(":");
					if (Config.Len() + Prefix.Len() + Text.Len() > 2 * 1024 * 1024) return false;
					Config += Prefix;
					Config += Text;
					return true;
				};
				if (!Append(FString::FromInt(C->OutputType)) || !Append(FString::FromInt(C->Inputs.Num())))
					return
						false;
				for (const auto& I : C->Inputs) if (!Append(I.InputName.ToString())) return false;
				if (!Append(FString::FromInt(C->AdditionalOutputs.Num()))) return false;
				for (const auto& O : C->AdditionalOutputs)
					if (!Append(O.OutputName.ToString()) || !Append(
						FString::FromInt(O.OutputType)))
						return false;
				if (!Append(FString::FromInt(C->AdditionalDefines.Num()))) return false;
				for (const auto& D : C->AdditionalDefines)
					if (!Append(D.DefineName) || !Append(D.DefineValue))
						return
							false;
				if (!Append(FString::FromInt(C->IncludeFilePaths.Num()))) return false;
				for (const auto& I : C->IncludeFilePaths) if (!Append(I)) return false;
				Data->SetStringField(TEXT("customConfigurationHash"), Digest(Config));
				Data->SetNumberField(TEXT("defineCount"), C->AdditionalDefines.Num());
				Data->SetNumberField(TEXT("includeCount"), C->IncludeFilePaths.Num());
			}
			return true;
		}

		void Expire()
		{
			const double Now = FPlatformTime::Seconds();
			Snapshots.RemoveAll([Now](const TSharedPtr<const FSnapshot>& Item)
			{
				return Now - Item->CapturedSeconds >= LifetimeSeconds;
			});
			BoundaryProofs.RemoveAll([Now](const TSharedPtr<const FBoundaryProof>& Item)
			{
				if (!Item || Now - Item->CreatedSeconds >= LifetimeSeconds)
				{
					return true;
				}
				return !Snapshots.ContainsByPredicate(
					[&Item](const TSharedPtr<const FSnapshot>& Snapshot)
					{
						return Snapshot && Snapshot->Id == Item->SnapshotId;
					});
			});
		}

		TSharedPtr<const FSnapshot> FindSnapshot(const FString& Id)
		{
			Expire();
			for (const auto& Item : Snapshots)
			{
				if (Item->Id == Id) return Item;
			}
			return nullptr;
		}

		FMCPToolResult MissingSnapshot()
		{
			return FMCPToolResult::Error(
				TEXT(
					"Snapshot expired, was evicted/released, or belongs to another Editor. Capture again and restart pagination."),
				TEXT("graph_snapshot_unavailable"), 410);
		}

		FMCPToolResult Invalid(const FString& Message)
		{
			return FMCPToolResult::Error(Message, TEXT("invalid_graph_query"), 400);
		}

		bool ReadInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, int32 Default,
		             int32 Min, int32 Max, int32& Out)
		{
			double Value = Default;
			if (Params->HasField(Key) && !Params->TryGetNumberField(Key, Value)) return false;
			if (!FMath::IsFinite(Value) || Value < Min || Value > Max || Value != FMath::FloorToDouble(Value))
				return
					false;
			Out = static_cast<int32>(Value);
			return true;
		}

		bool HasTypedIdentity(const FSnapshot& Snapshot)
		{
			return !Snapshot.Id.IsEmpty() && !Snapshot.ProjectionHash.IsEmpty();
		}

		FString SnapshotAssetKind(const FSnapshot& Snapshot)
		{
			switch (Snapshot.AssetKind)
			{
			case ESnapshotAssetKind::MaterialFunction:
				return TEXT("materialFunction");
			case ESnapshotAssetKind::Material:
				return TEXT("material");
			default:
				return FString();
			}
		}

		FString SnapshotAssetClassName(const FSnapshot& Snapshot)
		{
			return Snapshot.AssetClass;
		}

		TSharedPtr<FJsonObject> Metadata(const FSnapshot& Snapshot)
		{
			auto Result = MakeShared<FJsonObject>();
			if (HasTypedIdentity(Snapshot))
			{
				const FString Kind = SnapshotAssetKind(Snapshot);
				if (!Kind.IsEmpty())
				{
					auto AssetRef = MakeShared<FJsonObject>();
					AssetRef->SetStringField(TEXT("kind"), Kind);
					AssetRef->SetStringField(TEXT("path"), Snapshot.AssetPath);
					AssetRef->SetStringField(TEXT("snapshotId"), Snapshot.Id);
					AssetRef->SetStringField(TEXT("projectionHash"), Snapshot.ProjectionHash);
					if (!Snapshot.PreviewId.IsEmpty()) AssetRef->SetStringField(TEXT("previewId"), Snapshot.PreviewId);
					Result->SetObjectField(TEXT("assetRef"), AssetRef);
				}
			}
			Result->SetStringField(TEXT("schema"), TEXT("ue.material.graph-query/1"));
			Result->SetStringField(TEXT("snapshotId"), Snapshot.Id);
			Result->SetStringField(TEXT("assetPath"), Snapshot.AssetPath);
			Result->SetStringField(TEXT("assetClass"), SnapshotAssetClassName(Snapshot));
			Result->SetStringField(TEXT("capturedAt"), Snapshot.CapturedAt);
			Result->SetStringField(TEXT("projectionHash"), Snapshot.ProjectionHash);
			Result->SetStringField(TEXT("source"), Snapshot.PreviewId.IsEmpty()
				                                       ? TEXT("assetExpressions")
				                                       : TEXT("editorPreviewExpressions"));
			Result->SetStringField(
				TEXT("targetContext"), Snapshot.PreviewId.IsEmpty() ? TEXT("asset") : TEXT("editorPreview"));
			if (!Snapshot.PreviewId.IsEmpty()) Result->SetStringField(TEXT("previewId"), Snapshot.PreviewId);
			Result->SetStringField(TEXT("topologyCoverage"), Snapshot.bIncludeNamedReroutes
				                                                 ? TEXT("explicitExpressionInputsAndNamedReroutes")
				                                                 : TEXT("explicitExpressionInputs"));
			Result->SetBoolField(TEXT("includeNamedReroutes"), Snapshot.bIncludeNamedReroutes);
			Result->SetNumberField(TEXT("namedRerouteEdges"), Snapshot.NamedRerouteEdges);
			Result->SetNumberField(TEXT("unresolvedNamedReroutes"), Snapshot.UnresolvedNamedReroutes);
			Result->SetStringField(TEXT("namedRerouteCoverage"), !Snapshot.bIncludeNamedReroutes
				                                                     ? TEXT("notRequested")
				                                                     : Snapshot.UnresolvedNamedReroutes
				                                                     ? TEXT("incomplete")
				                                                     : TEXT("completeWithinSnapshot"));
			Result->SetBoolField(TEXT("referencesExpanded"), false);
			Result->SetStringField(TEXT("snapshotSemantics"), TEXT("immutable"));
			Result->SetBoolField(TEXT("liveStateChecked"), false);
			Result->SetNumberField(TEXT("totalNodes"), Snapshot.Nodes.Num());
			Result->SetNumberField(TEXT("totalEdges"), Snapshot.Edges.Num());
			if (Snapshot.FunctionMetadata.IsValid())
			{
				TSharedPtr<FJsonObject> FunctionMetadata;
				FJsonSerializer::Deserialize(
					TJsonReaderFactory<>::Create(JsonText(Snapshot.FunctionMetadata)),
					FunctionMetadata);
				if (FunctionMetadata.IsValid())
				{
					Result->SetObjectField(TEXT("functionMetadata"), FunctionMetadata);
				}
			}
			return Result;
		}

		TSharedPtr<FJsonObject> NodeRef(const FSnapshot& Snapshot, const FString& Id)
		{
			// Do not publish a typed handle that cannot be bound to an immutable
			// projection.  Capture rejects this state as well; this guard keeps the
			// serializer safe if a future caller constructs a partial snapshot.
			if (!HasTypedIdentity(Snapshot)) return nullptr;
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("kind"), TEXT("materialNode"));
			Result->SetStringField(TEXT("id"), Id);
			Result->SetStringField(TEXT("snapshotId"), Snapshot.Id);
			Result->SetStringField(TEXT("projectionHash"), Snapshot.ProjectionHash);
			return Result;
		}

		TSharedPtr<FJsonObject> EdgeJson(
			const FSnapshot& Snapshot,
			const FEdge& Edge,
			bool bIncludeTypedRefs)
		{
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("sourceNodeId"), Snapshot.Nodes[Edge.Source].Id);
			Result->SetStringField(TEXT("targetNodeId"), Snapshot.Nodes[Edge.Target].Id);
			if (bIncludeTypedRefs && HasTypedIdentity(Snapshot))
			{
				Result->SetObjectField(TEXT("sourceRef"), NodeRef(Snapshot, Snapshot.Nodes[Edge.Source].Id));
				Result->SetObjectField(TEXT("targetRef"), NodeRef(Snapshot, Snapshot.Nodes[Edge.Target].Id));
			}
			Result->SetNumberField(TEXT("outputIndex"), Edge.OutputIndex);
			Result->SetNumberField(TEXT("inputIndex"), Edge.InputIndex);
			Result->SetStringField(TEXT("inputName"), Edge.InputName);
			Result->SetStringField(TEXT("kind"), Edge.bNamedRerouteReference
				                                     ? TEXT("namedRerouteReference")
				                                     : TEXT("expressionInput"));
			Result->SetBoolField(TEXT("editableConnection"), !Edge.bNamedRerouteReference);
			Result->SetNumberField(TEXT("mask"), Edge.Mask);
			Result->SetNumberField(TEXT("maskR"), Edge.MaskR);
			Result->SetNumberField(TEXT("maskG"), Edge.MaskG);
			Result->SetNumberField(TEXT("maskB"), Edge.MaskB);
			Result->SetNumberField(TEXT("maskA"), Edge.MaskA);
			return Result;
		}

		// Deep clone prevents tool consumers (including tests) from mutating cache data.
		TSharedPtr<FJsonObject> NodeJson(const FSnapshot& Snapshot, const FNode& Node)
		{
			TSharedPtr<FJsonObject> Result;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText(Node.Data)), Result);
			if (HasTypedIdentity(Snapshot)) Result->SetObjectField(TEXT("nodeRef"), NodeRef(Snapshot, Node.Id));
			Result->SetNumberField(TEXT("incomingCount"), Node.Incoming.Num());
			Result->SetNumberField(TEXT("outgoingCount"), Node.Outgoing.Num());
			return Result;
		}

		FString EncodeCursor(const FSnapshot& Snapshot, const FString& FilterHash, int32 Offset)
		{
			auto Cursor = MakeShared<FJsonObject>();
			Cursor->SetStringField(TEXT("snapshot"), Snapshot.Id);
			Cursor->SetStringField(TEXT("filter"), FilterHash);
			Cursor->SetNumberField(TEXT("offset"), Offset);
			return FBase64::Encode(JsonText(Cursor));
		}

		TSharedPtr<FJsonObject> Continuation(
			const FSnapshot& Snapshot,
			const FString& FilterHash,
			int32 Offset,
			const FString& NextCursor)
		{
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("snapshotId"), Snapshot.Id);
			Result->SetStringField(TEXT("projectionHash"), Snapshot.ProjectionHash);
			Result->SetStringField(TEXT("filterHash"), FilterHash);
			Result->SetNumberField(TEXT("offset"), Offset);
			if (!NextCursor.IsEmpty()) Result->SetStringField(TEXT("nextCursor"), NextCursor);
			return Result;
		}
	}

	// Defined later in this namespace; used by ValidateBoundaryWrite before the
	// definition site. Keep the declaration in the named namespace to match.
	TSharedPtr<const FBoundaryProof> FindBoundaryProof(const FString& Id);

	FMCPToolResult Capture(
		UObject* Asset,
		const FString& OriginalPath,
		const FString& PreviewId,
		const TArray<UMaterialExpression*>* WorkingExpressions,
		bool bIncludeNamedReroutes,
		TSharedPtr<const FSnapshot>* OutSnapshot,
		bool bPublish)
	{
		check(IsInGameThread());
		const double Start = FPlatformTime::Seconds();
		UMaterial* Material = Cast<UMaterial>(Asset);
		UMaterialFunction* Function = Cast<UMaterialFunction>(Asset);
		if (!Material && !Function)
			return Invalid(
				TEXT("assetPath must identify a Material or MaterialFunction asset."));
		const int32 ExpressionCount = WorkingExpressions
			                              ? WorkingExpressions->Num()
			                              : Material
			                              ? Material->GetExpressions().Num()
			                              : Function->GetExpressions().Num();
		if (ExpressionCount + (Material ? 1 : 0) > MaxCaptureNodes)
			return FMCPToolResult::Error(
				TEXT("Graph exceeds 20000 nodes; capture was not published."), TEXT("graph_capture_limit"), 413);

		auto Snapshot = MakeShared<FSnapshot>();
		Snapshot->bIncludeNamedReroutes = bIncludeNamedReroutes;
		Snapshot->Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TArray<UMaterialExpression*> Expressions;
		if (WorkingExpressions) Expressions = *WorkingExpressions;
		else if (Material) for (UMaterialExpression* E : Material->GetExpressions()) Expressions.Add(E);
		else for (UMaterialExpression* E : Function->GetExpressions()) Expressions.Add(E);
		Snapshot->AssetPath = OriginalPath.IsEmpty() ? Asset->GetPathName() : OriginalPath;
		Snapshot->PreviewId = PreviewId;
		// Preserve the exact capture-time asset kind in the immutable snapshot so
		// later pages can publish typed handles without reloading the UObject.
		Snapshot->AssetKind = Function
		                         ? ESnapshotAssetKind::MaterialFunction
		                         : Material
		                         ? ESnapshotAssetKind::Material
		                         : ESnapshotAssetKind::Unknown;
		Snapshot->AssetClass = Asset->GetClass()->GetName();
		Snapshot->CapturedAt = FDateTime::UtcNow().ToIso8601();
		Snapshot->CapturedSeconds = Start;
		if (Function)
		{
			Snapshot->FunctionMetadata = ProjectFunctionMetadata(*Function);
			Snapshot->ApproximateBytes += JsonText(Snapshot->FunctionMetadata).Len() * 8ull;
		}
		TMap<FString, UMaterialExpression*> Authored;
		TSet<UMaterialExpression*> Members(Expressions);
		int32 PinCount = 0, ProjectionTextChars = 0;
		FString HashInput = bIncludeNamedReroutes ? TEXT("namedReroutes:1\n") : TEXT("namedReroutes:0\n");
		if (Snapshot->FunctionMetadata.IsValid())
		{
			// Function metadata is a write boundary. Hash its recursively sorted
			// projection so TMap iteration order can never create a false stale or
			// equal result while still covering every projected field.
			HashInput += TEXT("functionMetadata:") + CanonicalJsonText(Snapshot->FunctionMetadata) + TEXT("\n");
		}
		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression) continue;
			FNode Node;
			Node.Id = MCPMaterialInfrastructure::ExpressionNodeId(Expression);
			if (Authored.Contains(Node.Id)) return Invalid(TEXT("Duplicate expression identity in asset."));
			Authored.Add(Node.Id, Expression);
			Node.ClassName = Expression->GetClass()->GetName();
			Node.Data = MakeShared<FJsonObject>();
			Node.Data->SetStringField(TEXT("nodeId"), Node.Id);
			Node.Data->SetStringField(TEXT("className"), Node.ClassName);
			Node.Data->SetStringField(TEXT("name"), Expression->GetName());
			Node.Data->SetNumberField(TEXT("x"), !PreviewId.IsEmpty() && Expression->GraphNode
				                                     ? Expression->GraphNode->NodePosX
				                                     : Expression->MaterialExpressionEditorX);
			Node.Data->SetNumberField(TEXT("y"), !PreviewId.IsEmpty() && Expression->GraphNode
				                                     ? Expression->GraphNode->NodePosY
				                                     : Expression->MaterialExpressionEditorY);
			Node.Data->SetStringField(TEXT("description"), Expression->Desc.Left(256));
			Node.SearchText = Node.Id + TEXT(" ") + Node.ClassName + TEXT(" ") + Expression->Desc.Left(256);
			if (Expression->HasAParameterName())
			{
				const FString Name = Expression->GetParameterName().ToString();
				Node.Data->SetStringField(TEXT("parameterName"), Name);
				Node.SearchText += TEXT(" ") + Name;
			}
			if (const auto* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				Node.Data->SetStringField(TEXT("interfaceName"), Input->InputName.ToString());
				Node.Data->SetNumberField(TEXT("inputType"), Input->InputType);
				Node.Data->SetNumberField(TEXT("sortPriority"), Input->SortPriority);
				Node.Data->SetBoolField(TEXT("usePreviewValueAsDefault"), Input->bUsePreviewValueAsDefault);
				Node.SearchText += TEXT(" ") + Input->InputName.ToString();
			}
			if (const auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				Node.Data->SetStringField(TEXT("interfaceName"), Output->OutputName.ToString());
				Node.Data->SetNumberField(TEXT("sortPriority"), Output->SortPriority);
				Node.SearchText += TEXT(" ") + Output->OutputName.ToString();
			}
			if (const auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				Node.Data->SetStringField(
					TEXT("functionPath"), Call->MaterialFunction ? Call->MaterialFunction->GetPathName() : FString());
			if (const auto* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expression))
			{
				Node.Data->SetStringField(TEXT("rerouteName"), Declaration->Name.ToString());
				Node.Data->SetStringField(TEXT("variableGuid"), Declaration->VariableGuid.ToString());
				Node.SearchText += TEXT(" ") + Declaration->Name.ToString();
			}
			if (const auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression))
			{
				// The compiler uses the actual declaration pointer, not a GUID/name search.
				const auto* Declaration = Usage->Declaration.Get();
				const bool bValid = IsValid(Declaration);
				const bool bLocal = bValid && Members.Contains(Usage->Declaration.Get());
				Node.Data->SetStringField(TEXT("declarationGuid"), Usage->DeclarationGuid.ToString());
				Node.Data->SetStringField(TEXT("referenceStatus"), !Declaration
					                                                   ? TEXT("missingDeclaration")
					                                                   : !bValid
					                                                   ? TEXT("invalidDeclaration")
					                                                   : !bLocal
					                                                   ? TEXT("outsideSnapshot")
					                                                   : TEXT("resolved"));
				if (bValid)
				{
					Node.Data->SetStringField(TEXT("declarationPath"), Declaration->GetPathName());
					Node.Data->SetStringField(TEXT("rerouteName"), Declaration->Name.ToString());
					Node.SearchText += TEXT(" ") + Declaration->Name.ToString();
					if (bLocal)
						Node.Data->SetStringField(
							TEXT("declarationNodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Declaration));
				}
				if (bIncludeNamedReroutes && !bLocal) ++Snapshot->UnresolvedNamedReroutes;
			}
			if (!ProjectEditableSettings(Expression, Node.Data, ProjectionTextChars))
				return FMCPToolResult::Error(
					TEXT("Cannot capture editable settings for ") + Node.Id + TEXT(
						": non-finite numeric value or bounded text/configuration limit exceeded. No snapshot published."),
					TEXT("graph_node_projection_unavailable"), 409);

			const int32 InputCount = Expression->GetInputsView().Num();
			const auto& Outputs = Expression->GetOutputs();
			if (InputCount > MaxPinsPerNode || Outputs.Num() > MaxPinsPerNode || (PinCount += InputCount + Outputs.
				Num()) > 4 * MaxCaptureEdges)
				return FMCPToolResult::Error(
					TEXT("Graph exceeds capture pin budget."), TEXT("graph_capture_limit"), 413);
			const TArray<FString> InputNames = UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expression);
			const auto Inputs = Expression->GetInputsView();
			TArray<TSharedPtr<FJsonValue>> InputJson, OutputJson;
			for (int32 Index = 0; Index < Inputs.Num(); ++Index)
			{
				auto Pin = MakeShared<FJsonObject>();
				Pin->SetNumberField(TEXT("index"), Index);
				Pin->SetStringField(TEXT("name"), InputNames.IsValidIndex(Index)
					                                  ? InputNames[Index].Left(128)
					                                  : FString());
				InputJson.Add(MakeShared<FJsonValueObject>(Pin));
			}
			for (int32 Index = 0; Index < Outputs.Num(); ++Index)
			{
				auto Pin = MakeShared<FJsonObject>();
				Pin->SetNumberField(TEXT("index"), Index);
				Pin->SetStringField(TEXT("name"), Outputs[Index].OutputName.IsNone()
					                                  ? TEXT("Output")
					                                  : Outputs[Index].OutputName.ToString().Left(128));
				OutputJson.Add(MakeShared<FJsonValueObject>(Pin));
			}
			Node.Data->SetArrayField(TEXT("inputs"), InputJson);
			Node.Data->SetArrayField(TEXT("outputs"), OutputJson);
			// Include JSON tree overhead, strings, hash indexes and adjacency capacity.
			Snapshot->ApproximateBytes += 2048 + JsonText(Node.Data).Len() * 8ull;
			if (Snapshot->ApproximateBytes > MaxSnapshotBytes)
				return FMCPToolResult::Error(
					TEXT("Graph exceeds snapshot memory budget."), TEXT("graph_capture_limit"), 413);
			Snapshot->Nodes.Add(MoveTemp(Node));
		}
		if (Material)
		{
			FNode Root;
			Root.Id = TEXT("root");
			Root.ClassName = TEXT("MaterialOutput");
			Root.SearchText = TEXT("root MaterialOutput");
			Root.Data = MakeShared<FJsonObject>();
			Root.Data->SetStringField(TEXT("nodeId"), Root.Id);
			Root.Data->SetStringField(TEXT("className"), Root.ClassName);
			Root.Data->SetStringField(TEXT("name"), Material->GetName());
			Snapshot->Nodes.Add(MoveTemp(Root));
		}
		Snapshot->Nodes.Sort([](const FNode& A, const FNode& B)
		{
			return A.Id.Compare(B.Id, ESearchCase::CaseSensitive) < 0;
		});
		for (int32 Index = 0; Index < Snapshot->Nodes.Num(); ++Index)
		{
			FNode& Node = Snapshot->Nodes[Index];
			Node.DataHash = Digest(JsonText(Node.Data));
			if (Node.DataHash.IsEmpty())
				return FMCPToolResult::Error(
					TEXT("Snapshot hashing unavailable."), TEXT("graph_hash_unavailable"), 500);
			Snapshot->ApproximateBytes += Node.DataHash.Len() * sizeof(TCHAR);
			Snapshot->ById.Add(Node.Id, Index);
			Snapshot->ByClass.FindOrAdd(Node.ClassName).Add(Index);
			HashInput += JsonText(Node.Data) + TEXT("\n");
		}
		bool bInvalidEdge = false;
		auto AddInput = [&](int32 Target, int32 InputIndex, const FString& Name, const FExpressionInput* Input)
		{
			if (!Input || !Input->Expression) return;
			const int32* Source = Snapshot->ById.Find(MCPMaterialInfrastructure::ExpressionNodeId(Input->Expression));
			if (!Source || Authored.FindRef(Snapshot->Nodes[*Source].Id) != Input->Expression
				|| Input->OutputIndex < 0 || Input->OutputIndex >= Input->Expression->GetOutputs().Num())
			{
				bInvalidEdge = true;
				return;
			}
			const int32 EdgeIndex = Snapshot->Edges.Num();
			Snapshot->Edges.Add({
				*Source, Target, Input->OutputIndex, InputIndex, Name.Left(128), false, Input->Mask, Input->MaskR,
				Input->MaskG, Input->MaskB, Input->MaskA
			});
			Snapshot->Nodes[*Source].Outgoing.Add(EdgeIndex);
			Snapshot->Nodes[Target].Incoming.Add(EdgeIndex);
		};
		for (int32 Target = 0; Target < Snapshot->Nodes.Num(); ++Target)
		{
			if (auto* Expression = Authored.FindRef(Snapshot->Nodes[Target].Id))
			{
				const auto Names = UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expression);
				// Custom/FunctionCall rebuild CachedInputs on each GetInputsView call.
				// GetMaterialExpressionInputNames calls it too; acquire the view last.
				const auto Inputs = Expression->GetInputsView();
				for (int32 Index = 0; Index < Inputs.Num(); ++Index)
					AddInput(Target, Index, Names.IsValidIndex(Index) ? Names[Index] : FString(), Inputs[Index]);
				if (bIncludeNamedReroutes)
				{
					if (const auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression); Usage &&
						IsValid(Usage->Declaration) && Members.Contains(Usage->Declaration))
					{
						const int32 Source = Snapshot->ById.FindChecked(
							MCPMaterialInfrastructure::ExpressionNodeId(Usage->Declaration));
						const int32 EdgeIndex = Snapshot->Edges.Num();
						// This is a semantic dependency, not a real input pin on Usage.
						Snapshot->Edges.Add({Source, Target, 0, INDEX_NONE, FString(), true});
						Snapshot->Nodes[Source].Outgoing.Add(EdgeIndex);
						Snapshot->Nodes[Target].Incoming.Add(EdgeIndex);
						++Snapshot->NamedRerouteEdges;
					}
				}
			}
			else if (Material)
			{
				for (int32 Index = 0; Index < MP_MAX; ++Index)
					AddInput(Target, Index, StaticEnum<EMaterialProperty>()->GetNameStringByValue(Index),
					         Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Index)));
			}
			if (Snapshot->Edges.Num() > MaxCaptureEdges)
				return FMCPToolResult::Error(
					TEXT("Graph exceeds capture edge budget."), TEXT("graph_capture_limit"), 413);
		}
		if (bInvalidEdge)
			return FMCPToolResult::Error(
				TEXT(
					"Graph contains an external/dangling expression input or invalid output index. No partial snapshot was published."),
				TEXT("graph_invalid_topology"), 409);
		for (int32 I = 0; I < Snapshot->Edges.Num(); ++I)
		{
			auto& Edge = Snapshot->Edges[I];
			Edge.Key = JsonText(EdgeJson(*Snapshot, Edge, false));
			Snapshot->EdgesByKey.Add(I);
			Snapshot->ApproximateBytes += Edge.Key.Len() * sizeof(TCHAR) + sizeof(int32);
		}
		Snapshot->EdgesByKey.Sort([&](int32 A, int32 B)
		{
			return Snapshot->Edges[A].Key.Compare(Snapshot->Edges[B].Key, ESearchCase::CaseSensitive) < 0;
		});
		for (int32 I : Snapshot->EdgesByKey) HashInput += Snapshot->Edges[I].Key + TEXT("\n");
		Snapshot->ProjectionHash = Digest(HashInput);
		if (Snapshot->Id.IsEmpty() || Snapshot->ProjectionHash.IsEmpty())
			return FMCPToolResult::Error(
				TEXT("Snapshot typed identity could not be established; no snapshot published."),
				TEXT("graph_hash_unavailable"),
				500);
		Snapshot->ApproximateBytes += Snapshot->Edges.Num() * 1024ull;
		if (Snapshot->ApproximateBytes > MaxSnapshotBytes)
			return FMCPToolResult::Error(
				TEXT("Graph exceeds snapshot memory budget."), TEXT("graph_capture_limit"), 413);
		Snapshot->CaptureMilliseconds = (FPlatformTime::Seconds() - Start) * 1000;
		if (OutSnapshot)
		{
			*OutSnapshot = Snapshot;
		}
		if (bPublish)
		{
			Expire();
			uint64 CacheBytes = Snapshot->ApproximateBytes;
			for (const auto& Existing : Snapshots) CacheBytes += Existing->ApproximateBytes;
			while (Snapshots.Num() && (Snapshots.Num() >= MaxEntries || CacheBytes > MaxCacheBytes))
			{
				CacheBytes -= Snapshots[0]->ApproximateBytes;
				Snapshots.RemoveAt(0);
			}
			Snapshots.Add(Snapshot);
		}
		auto Result = Metadata(*Snapshot);
		Result->SetNumberField(TEXT("captureMilliseconds"), Snapshot->CaptureMilliseconds);
		Result->SetNumberField(TEXT("approximateBytes"), static_cast<double>(Snapshot->ApproximateBytes));
		Result->SetNumberField(TEXT("ttlSeconds"), LifetimeSeconds);
		Result->SetNumberField(TEXT("responseByteLimit"), ResponseBudget);
		return FMCPToolResult::Ok(Result);
	}

	FMCPToolResult ListNodes(const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());
		const auto Snapshot = FindSnapshot(Params->GetStringField(TEXT("snapshotId")));
		if (!Snapshot) return MissingSnapshot();
		int32 Limit;
		if (!ReadInt(Params, TEXT("limit"), 50, 1, 200, Limit))
			return Invalid(
				TEXT("limit must be an integer in [1, 200]."));
		FString ClassName, Search, CursorText;
		Params->TryGetStringField(TEXT("className"), ClassName);
		Params->TryGetStringField(TEXT("search"), Search);
		Params->TryGetStringField(TEXT("cursor"), CursorText);
		if (ClassName.Len() > 256 || Search.Len() > 256 || CursorText.Len() > 2048)
			return Invalid(
				TEXT("Query text or cursor exceeds its size limit."));
		Search = Search.ToLower();
		auto Filter = MakeShared<FJsonObject>();
		Filter->SetStringField(TEXT("className"), ClassName);
		Filter->SetStringField(TEXT("search"), Search);
		const FString FilterHash = Digest(JsonText(Filter));
		const TArray<int32>* Candidates = ClassName.IsEmpty() ? nullptr : Snapshot->ByClass.Find(ClassName);
		const int32 Count = ClassName.IsEmpty() ? Snapshot->Nodes.Num() : (Candidates ? Candidates->Num() : 0);
		int32 Offset = 0;
		if (!CursorText.IsEmpty())
		{
			FString Decoded;
			TSharedPtr<FJsonObject> Cursor;
			if (!FBase64::Decode(CursorText, Decoded) || !FJsonSerializer::Deserialize(
					TJsonReaderFactory<>::Create(Decoded), Cursor)
				|| !Cursor || Cursor->GetStringField(TEXT("snapshot")) != Snapshot->Id
				|| Cursor->GetStringField(TEXT("filter")) != FilterHash
				|| !ReadInt(Cursor, TEXT("offset"), -1, 0, Count, Offset))
				return Invalid(TEXT("Cursor does not match this snapshot and filter, or is malformed."));
		}
		TArray<TSharedPtr<FJsonValue>> Nodes;
		int32 Scanned = 0;
		int32 Bytes = 4096;
		while (Offset < Count && Nodes.Num() < Limit && Scanned < PageScanBudget)
		{
			const FNode& Node = Snapshot->Nodes[Candidates ? (*Candidates)[Offset] : Offset];
			if (!Search.IsEmpty() && !Node.SearchText.Contains(Search, ESearchCase::IgnoreCase))
			{
				++Offset;
				++Scanned;
				continue;
			}
			auto Data = NodeJson(*Snapshot, Node);
			const int32 NodeBytes = FTCHARToUTF8(*JsonText(Data)).Length();
			if (Bytes + NodeBytes > ResponseBudget) break;
			Bytes += NodeBytes;
			Nodes.Add(MakeShared<FJsonValueObject>(Data));
			++Offset;
			++Scanned;
		}
		if (Scanned == 0 && Offset < Count)
			return FMCPToolResult::Error(
				TEXT("A single node exceeds the response budget."), TEXT("graph_response_limit"), 413);
		auto Result = Metadata(*Snapshot);
		Result->SetArrayField(TEXT("nodes"), Nodes);
		Result->SetNumberField(TEXT("returnedCount"), Nodes.Num());
		Result->SetNumberField(TEXT("scannedCount"), Scanned);
		Result->SetNumberField(TEXT("candidateCount"), Count);
		Result->SetBoolField(TEXT("hasMore"), Offset < Count);
		if (Offset < Count)
		{
			const FString NextCursor = EncodeCursor(*Snapshot, FilterHash, Offset);
			Result->SetStringField(TEXT("nextCursor"), NextCursor);
			if (HasTypedIdentity(*Snapshot))
				Result->SetObjectField(TEXT("continuation"), Continuation(*Snapshot, FilterHash, Offset, NextCursor));
		}
		return FMCPToolResult::Ok(Result);
	}

	FMCPToolResult Subgraph(const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());
		const auto Snapshot = FindSnapshot(Params->GetStringField(TEXT("snapshotId")));
		if (!Snapshot) return MissingSnapshot();
		int32 Depth, MaxNodes, MaxEdges;
		if (!ReadInt(Params, TEXT("depth"), 2, 0, 32, Depth)
			|| !ReadInt(Params, TEXT("maxNodes"), 100, 1, 200, MaxNodes)
			|| !ReadInt(Params, TEXT("maxEdges"), 200, 1, 1000, MaxEdges))
			return Invalid(TEXT("depth/maxNodes/maxEdges must be bounded integers (0..32 / 1..200 / 1..1000)."));
		FString Direction = TEXT("upstream");
		Params->TryGetStringField(TEXT("direction"), Direction);
		if (Direction != TEXT("upstream") && Direction != TEXT("downstream") && Direction != TEXT("both"))
			return
				Invalid(TEXT("Invalid traversal direction."));
		const TArray<TSharedPtr<FJsonValue>>* Seeds = nullptr;
		if (!Params->TryGetArrayField(TEXT("nodeIds"), Seeds) || !Seeds || Seeds->IsEmpty() || Seeds->Num() >
			FMath::Min(MaxNodes, 32))
			return Invalid(TEXT("nodeIds must contain 1..min(maxNodes,32) seed IDs."));
		TMap<int32, int32> Distances;
		TArray<int32> Queue;
		for (const auto& Seed : *Seeds)
		{
			FString Id;
			if (!Seed->TryGetString(Id)) return Invalid(TEXT("nodeIds must contain strings."));
			const int32* Index = Snapshot->ById.Find(Id);
			if (!Index) return Invalid(TEXT("Unknown seed node: ") + Id.Left(128));
			if (!Distances.Contains(*Index))
			{
				Distances.Add(*Index, 0);
				Queue.Add(*Index);
			}
		}
		// Inspect each adjacency at most once per visited endpoint; stop even at hubs.
		const int32 ScanBudget = FMath::Min(10000, FMath::Max(256, MaxEdges * 4));
		int32 Scanned = 0;
		bool bNodeLimited = false, bDepthLimited = false, bScanLimited = false;
		TSet<int32> SeenEdges;
		TArray<int32> EdgeOrder;
		for (int32 Head = 0; Head < Queue.Num() && !bScanLimited; ++Head)
		{
			const int32 Current = Queue[Head];
			const int32 Distance = Distances.FindChecked(Current);
			auto Visit = [&](const TArray<int32>& Adjacency)
			{
				for (int32 EdgeIndex : Adjacency)
				{
					if (Scanned >= ScanBudget)
					{
						bScanLimited = true;
						break;
					}
					++Scanned;
					if (!SeenEdges.Contains(EdgeIndex))
					{
						SeenEdges.Add(EdgeIndex);
						EdgeOrder.Add(EdgeIndex);
					}
					const FEdge& Edge = Snapshot->Edges[EdgeIndex];
					const int32 Neighbor = Edge.Source == Current ? Edge.Target : Edge.Source;
					if (Distances.Contains(Neighbor)) continue;
					if (Distance >= Depth)
					{
						bDepthLimited = true;
						continue;
					}
					if (Queue.Num() >= MaxNodes)
					{
						bNodeLimited = true;
						continue;
					}
					Distances.Add(Neighbor, Distance + 1);
					Queue.Add(Neighbor);
				}
			};
			if (Direction != TEXT("downstream")) Visit(Snapshot->Nodes[Current].Incoming);
			if (!bScanLimited && Direction != TEXT("upstream")) Visit(Snapshot->Nodes[Current].Outgoing);
		}

		int32 Bytes = 8192;
		bool bByteLimited = false;
		TSet<int32> Returned;
		TArray<TSharedPtr<FJsonValue>> Nodes, Edges, Boundary;
		for (int32 Index : Queue)
		{
			auto Data = NodeJson(*Snapshot, Snapshot->Nodes[Index]);
			Data->SetNumberField(TEXT("distance"), Distances.FindChecked(Index));
			const int32 Size = FTCHARToUTF8(*JsonText(Data)).Length();
			if (Bytes + Size > ResponseBudget)
			{
				bByteLimited = true;
				break;
			}
			Bytes += Size;
			Returned.Add(Index);
			Nodes.Add(MakeShared<FJsonValueObject>(Data));
		}
		int32 OmittedEdges = 0;
		for (int32 Index : EdgeOrder)
		{
			const FEdge& Edge = Snapshot->Edges[Index];
			if (!Returned.Contains(Edge.Source) && !Returned.Contains(Edge.Target)) continue;
			auto Data = EdgeJson(*Snapshot, Edge, true);
			const int32 Size = FTCHARToUTF8(*JsonText(Data)).Length();
			if (Edges.Num() + Boundary.Num() >= MaxEdges || Bytes + Size > ResponseBudget)
			{
				bByteLimited |= Bytes + Size > ResponseBudget;
				++OmittedEdges;
				continue;
			}
			Bytes += Size;
			if (Returned.Contains(Edge.Source) && Returned.Contains(Edge.Target))
				Edges.Add(
					MakeShared<FJsonValueObject>(Data));
			else Boundary.Add(MakeShared<FJsonValueObject>(Data));
		}
		auto Result = Metadata(*Snapshot);
		Result->SetArrayField(TEXT("nodes"), Nodes);
		Result->SetArrayField(TEXT("edges"), Edges);
		Result->SetArrayField(TEXT("boundaryEdges"), Boundary);
		Result->SetNumberField(TEXT("inspectedAdjacencies"), Scanned);
		Result->SetNumberField(TEXT("omittedObservedEdges"), OmittedEdges);
		Result->SetBoolField(TEXT("edgeCoverageComplete"), !bScanLimited && OmittedEdges == 0 && !bByteLimited);
		Result->SetBoolField(
			TEXT("truncated"), bNodeLimited || bDepthLimited || bScanLimited || bByteLimited || OmittedEdges > 0);
		auto Limits = MakeShared<FJsonObject>();
		Limits->SetBoolField(TEXT("depth"), bDepthLimited);
		Limits->SetBoolField(TEXT("nodes"), bNodeLimited);
		Limits->SetBoolField(TEXT("scan"), bScanLimited);
		Limits->SetBoolField(TEXT("bytes"), bByteLimited);
		Limits->SetBoolField(TEXT("edges"), OmittedEdges > 0);
		Result->SetObjectField(TEXT("limitsReached"), Limits);
		return FMCPToolResult::Ok(Result);
	}

	FMCPToolResult Boundary(const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());
		const FString SnapshotId = Params->GetStringField(TEXT("snapshotId"));
		const auto Snapshot = FindSnapshot(SnapshotId);
		if (!Snapshot)
		{
			return MissingSnapshot();
		}

		const FMCPToolResult Selection = Subgraph(Params);
		if (!Selection.bSuccess || !Selection.Data)
		{
			return Selection;
		}

		bool bTruncated = false;
		bool bEdgeCoverageComplete = false;
		Selection.Data->TryGetBoolField(TEXT("truncated"), bTruncated);
		Selection.Data->TryGetBoolField(TEXT("edgeCoverageComplete"), bEdgeCoverageComplete);
		if (bTruncated || !bEdgeCoverageComplete)
		{
			return FMCPToolResult::Error(
				TEXT(
					"A writable boundary requires complete node and edge coverage. Increase the bounded traversal limits or split the edit."),
				TEXT("material_boundary_incomplete"),
				409);
		}

		const TArray<TSharedPtr<FJsonValue>>* SelectedNodes = nullptr;
		if (!Selection.Data->TryGetArrayField(TEXT("nodes"), SelectedNodes) || !SelectedNodes)
		{
			return FMCPToolResult::Error(
				TEXT("Subgraph result is missing boundary data."), TEXT("material_boundary_internal_error"), 500);
		}

		TSet<FString> SelectedIds;
		TArray<FString> WritableIds;
		for (const TSharedPtr<FJsonValue>& Value : *SelectedNodes)
		{
			const TSharedPtr<FJsonObject> Node = Value.IsValid() && Value->Type == EJson::Object
				                                     ? Value->AsObject()
				                                     : nullptr;
			FString NodeId;
			if (!Node || !Node->TryGetStringField(TEXT("nodeId"), NodeId) || NodeId.IsEmpty())
			{
				return FMCPToolResult::Error(
					TEXT("Subgraph returned an invalid node identity."), TEXT("material_boundary_internal_error"), 500);
			}
			SelectedIds.Add(NodeId);
			if (NodeId != TEXT("root"))
			{
				WritableIds.Add(NodeId);
			}
		}
		if (WritableIds.IsEmpty())
		{
			return Invalid(TEXT("A writable boundary must contain at least one material expression node."));
		}
		WritableIds.Sort();

		TSet<FString> ExternalDependencies;
		TSet<FString> ExternallyConsumed;
		TArray<FString> BoundaryEdgeKeys;
		TArray<TSharedPtr<FJsonValue>> BoundaryEdges;
		int32 BoundaryBytes = 0;
		// Subgraph traversal follows the requested direction. A write boundary must
		// additionally observe every crossing edge in both directions so shared
		// consumers cannot be hidden by an upstream-only selection.
		for (const FEdge& SnapshotEdge : Snapshot->Edges)
		{
			const FString& SourceId = Snapshot->Nodes[SnapshotEdge.Source].Id;
			const FString& TargetId = Snapshot->Nodes[SnapshotEdge.Target].Id;
			const bool bSourceInside = SelectedIds.Contains(SourceId);
			const bool bTargetInside = SelectedIds.Contains(TargetId);
			if (bSourceInside == bTargetInside)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Edge = EdgeJson(*Snapshot, SnapshotEdge, true);
			const FString EdgeKey = JsonText(Edge);
			BoundaryBytes += FTCHARToUTF8(*EdgeKey).Length();
			if (BoundaryEdges.Num() >= 1000 || BoundaryBytes > ResponseBudget - 8192)
			{
				return FMCPToolResult::Error(
					TEXT(
						"Boundary has too many crossing edges for a complete writable contract. Split the selected node set."),
					TEXT("material_boundary_too_large"),
					413);
			}
			if (bSourceInside && SourceId != TEXT("root"))
			{
				ExternallyConsumed.Add(SourceId);
			}
			if (bTargetInside)
			{
				ExternalDependencies.Add(SourceId);
			}
			BoundaryEdgeKeys.Add(EdgeKey);
			BoundaryEdges.Add(MakeShared<FJsonValueObject>(Edge));
		}
		BoundaryEdgeKeys.Sort();

		auto BoundaryIdentity = MakeShared<FJsonObject>();
		BoundaryIdentity->SetStringField(TEXT("schema"), TEXT("ue.material.graph-boundary/1"));
		BoundaryIdentity->SetStringField(TEXT("snapshotId"), Snapshot->Id);
		BoundaryIdentity->SetStringField(TEXT("projectionHash"), Snapshot->ProjectionHash);
		BoundaryIdentity->SetStringField(TEXT("assetPath"), Snapshot->AssetPath);
		TArray<TSharedPtr<FJsonValue>> IdentityNodes;
		for (const FString& NodeId : WritableIds)
		{
			IdentityNodes.Add(MakeShared<FJsonValueString>(NodeId));
		}
		BoundaryIdentity->SetArrayField(TEXT("nodeIds"), IdentityNodes);
		TArray<TSharedPtr<FJsonValue>> IdentityEdges;
		for (const FString& EdgeKey : BoundaryEdgeKeys)
		{
			IdentityEdges.Add(MakeShared<FJsonValueString>(EdgeKey));
		}
		BoundaryIdentity->SetArrayField(TEXT("boundaryEdges"), IdentityEdges);
		const FString BoundaryDigest = Digest(JsonText(BoundaryIdentity));
		if (BoundaryDigest.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Boundary hashing is unavailable."), TEXT("graph_hash_unavailable"), 500);
		}
		const FString BoundaryId = TEXT("boundary:") + BoundaryDigest;
		TArray<FString> SelectedIdList = SelectedIds.Array();
		SelectedIdList.Sort();
		TArray<FString> ExternalDependencyIds = ExternalDependencies.Array();
		ExternalDependencyIds.Sort();
		TArray<FString> ExternallyConsumedIds = ExternallyConsumed.Array();
		ExternallyConsumedIds.Sort();
		auto Proof = MakeShared<FBoundaryProof>();
		Proof->BoundaryId = BoundaryId;
		Proof->SnapshotId = Snapshot->Id;
		Proof->AssetPath = Snapshot->AssetPath;
		Proof->PreviewId = Snapshot->PreviewId;
		Proof->ProjectionHash = Snapshot->ProjectionHash;
		Proof->CreatedSeconds = FPlatformTime::Seconds();
		Proof->bRequiresSharedNodeConfirmation = !ExternallyConsumedIds.IsEmpty();
		Proof->SelectedNodeIds = MoveTemp(SelectedIdList);
		Proof->WritableNodeIds = WritableIds;
		Proof->ExternalDependencyNodeIds = MoveTemp(ExternalDependencyIds);
		Proof->ExternallyConsumedNodeIds = MoveTemp(ExternallyConsumedIds);
		Proof->BoundaryEdgeKeys = BoundaryEdgeKeys;
		Expire();
		BoundaryProofs.RemoveAll([&BoundaryId](const TSharedPtr<const FBoundaryProof>& Existing)
		{
			return Existing && Existing->BoundaryId == BoundaryId;
		});
		while (BoundaryProofs.Num() >= MaxBoundaryProofs)
		{
			BoundaryProofs.RemoveAt(0);
		}
		BoundaryProofs.Add(Proof);

		auto Result = Metadata(*Snapshot);
		Result->SetStringField(TEXT("boundaryId"), BoundaryId);
		Result->SetStringField(TEXT("boundaryDigest"), BoundaryDigest);
		Result->SetStringField(TEXT("sourceSnapshotId"), Snapshot->Id);
		Result->SetStringField(TEXT("sourceProjectionHash"), Snapshot->ProjectionHash);
		Result->SetArrayField(TEXT("writableNodeIds"), IdentityNodes);
		TArray<TSharedPtr<FJsonValue>> WritableNodeRefs;
		if (HasTypedIdentity(*Snapshot))
		{
			for (const FString& NodeId : WritableIds)
				WritableNodeRefs.Add(MakeShared<FJsonValueObject>(NodeRef(*Snapshot, NodeId)));
			Result->SetArrayField(TEXT("writableNodeRefs"), WritableNodeRefs);
		}
		Result->SetArrayField(TEXT("boundaryEdges"), BoundaryEdges);
		Result->SetNumberField(TEXT("writableNodeCount"), WritableIds.Num());
		Result->SetNumberField(TEXT("boundaryEdgeCount"), BoundaryEdges.Num());

		auto ToSortedJson = [](const TSet<FString>& Values)
		{
			TArray<FString> Sorted = Values.Array();
			Sorted.Sort();
			TArray<TSharedPtr<FJsonValue>> ResultValues;
			for (const FString& Value : Sorted)
			{
				ResultValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return ResultValues;
		};
		Result->SetArrayField(TEXT("externalDependencyNodeIds"), ToSortedJson(ExternalDependencies));
		Result->SetArrayField(TEXT("externallyConsumedNodeIds"), ToSortedJson(ExternallyConsumed));
		Result->SetBoolField(TEXT("hasExternalDependencies"), !ExternalDependencies.IsEmpty());
		Result->SetBoolField(TEXT("hasExternallyConsumedNodes"), !ExternallyConsumed.IsEmpty());
		Result->SetBoolField(TEXT("requiresSharedNodeConfirmation"), !ExternallyConsumed.IsEmpty());
		Result->SetBoolField(TEXT("liveStateChecked"), false);
		Result->SetBoolField(TEXT("requiresLiveFingerprintCheck"), true);
		Result->SetStringField(TEXT("freshness"), TEXT("boundToImmutableSnapshot"));
		Result->SetNumberField(TEXT("boundaryTtlSeconds"), LifetimeSeconds);
		Result->SetStringField(
			TEXT("writeContract"),
			TEXT(
				"A writer must re-capture the same target and reject a changed projectionHash before applying this boundary."));
		return FMCPToolResult::Ok(Result);
	}

	FMCPToolResult ValidateBoundaryWrite(
		UObject* Asset,
		const TSharedPtr<FJsonObject>& Params,
		FBoundaryWriteValidation& OutValidation)
	{
		check(IsInGameThread());
		OutValidation = FBoundaryWriteValidation();
		FString BoundaryId;
		FString SnapshotId;
		FString ExpectedProjectionHash;
		if (!Asset || !Params.IsValid()
			|| !Params->TryGetStringField(TEXT("boundaryId"), BoundaryId)
			|| !Params->TryGetStringField(TEXT("snapshotId"), SnapshotId)
			|| !Params->TryGetStringField(
				TEXT("expectedProjectionHash"), ExpectedProjectionHash)
			|| BoundaryId.IsEmpty() || SnapshotId.IsEmpty()
			|| ExpectedProjectionHash.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("boundaryId, snapshotId and expectedProjectionHash are required."),
				TEXT("invalid_material_boundary_write"),
				422);
		}
		bool bConfirmSharedNodeImpact = false;
		if (Params->HasField(TEXT("confirmSharedNodeImpact"))
			&& !Params->TryGetBoolField(
				TEXT("confirmSharedNodeImpact"), bConfirmSharedNodeImpact))
		{
			return FMCPToolResult::Error(
				TEXT("confirmSharedNodeImpact must be a boolean."),
				TEXT("invalid_material_boundary_write"),
				422);
		}

		const TSharedPtr<const FBoundaryProof> Proof = FindBoundaryProof(BoundaryId);
		const TSharedPtr<const FSnapshot> Source = FindSnapshot(SnapshotId);
		if (!Proof || !Source)
		{
			return FMCPToolResult::Error(
				TEXT(
					"The boundary or its source snapshot expired, was evicted/released, or belongs to another Editor."),
				TEXT("material_boundary_unavailable"),
				410);
		}
		if (Proof->SnapshotId != SnapshotId
			|| Proof->AssetPath != Source->AssetPath
			|| Proof->PreviewId != Source->PreviewId
			|| Proof->ProjectionHash != Source->ProjectionHash
			|| ExpectedProjectionHash != Source->ProjectionHash)
		{
			return FMCPToolResult::Error(
				TEXT("The boundary, snapshot and expected projection identities do not match."),
				TEXT("material_boundary_identity_mismatch"),
				409);
		}
		if (Proof->AssetPath != Asset->GetPathName())
		{
			return FMCPToolResult::Error(
				TEXT("The boundary belongs to a different material asset."),
				TEXT("material_boundary_asset_mismatch"),
				409);
		}
		if (!Proof->PreviewId.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT(
					"This writer only accepts authored-asset boundaries; use editor.batch for a preview working copy."),
				TEXT("material_boundary_preview_writer_unsupported"),
				409);
		}
		if (Proof->bRequiresSharedNodeConfirmation && !bConfirmSharedNodeImpact)
		{
			return FMCPToolResult::Error(
				TEXT(
					"The boundary contains nodes consumed outside the selection. Re-read the boundary and explicitly confirm shared-node impact."),
				TEXT("material_boundary_shared_node_confirmation_required"),
				409);
		}

		TSharedPtr<const FSnapshot> Fresh;
		const FMCPToolResult FreshCapture = Capture(
			Asset,
			Source->AssetPath,
			FString(),
			nullptr,
			Source->bIncludeNamedReroutes,
			&Fresh,
			false);
		if (!FreshCapture.bSuccess || !Fresh)
		{
			return FreshCapture.bSuccess
				       ? FMCPToolResult::Error(
					       TEXT("Fresh boundary capture returned no snapshot."),
					       TEXT("material_boundary_capture_failed"),
					       500)
				       : FreshCapture;
		}
		if (Fresh->AssetPath != Source->AssetPath
			|| Fresh->PreviewId != Source->PreviewId
			|| Fresh->ProjectionHash != Source->ProjectionHash)
		{
			return FMCPToolResult::Error(
				TEXT("The material graph changed after the boundary was read. Capture a new graph and boundary."),
				TEXT("material_boundary_stale"),
				409);
		}

		OutValidation.SourceSnapshot = Source;
		OutValidation.FreshSnapshot = Fresh;
		OutValidation.Boundary = Proof;
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("boundaryId"), Proof->BoundaryId);
		Result->SetStringField(TEXT("snapshotId"), Source->Id);
		Result->SetStringField(TEXT("assetPath"), Source->AssetPath);
		Result->SetStringField(TEXT("targetContext"), TEXT("asset"));
		Result->SetStringField(TEXT("sourceProjectionHash"), Source->ProjectionHash);
		Result->SetStringField(TEXT("freshProjectionHash"), Fresh->ProjectionHash);
		Result->SetBoolField(TEXT("freshLiveProjectionVerified"), true);
		Result->SetBoolField(
			TEXT("sharedNodeImpactConfirmed"),
			!Proof->bRequiresSharedNodeConfirmation || bConfirmSharedNodeImpact);
		return FMCPToolResult::Ok(Result);
	}

	FString DescribeDefinitionPropertyValueType(const FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("unknown");
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum ? TEXT("enum") : TEXT("integer");
		}
		if (Property->IsA<FEnumProperty>()) return TEXT("enum");
		if (Property->IsA<FBoolProperty>()) return TEXT("boolean");
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return NumericProperty->IsFloatingPoint() ? TEXT("number") : TEXT("integer");
		}
		if (Property->IsA<FStrProperty>() || Property->IsA<FTextProperty>()) return TEXT("string");
		if (Property->IsA<FNameProperty>()) return TEXT("name");
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const FName StructName = StructProperty->Struct
				                         ? StructProperty->Struct->GetFName()
				                         : NAME_None;
			if (StructName == NAME_Vector || StructName == TEXT("Vector3f")) return TEXT("vector3");
			if (StructName == TEXT("Vector2D") || StructName == TEXT("Vector2f")) return TEXT("vector2");
			if (StructName == TEXT("Vector4") || StructName == TEXT("Vector4f")) return TEXT("vector4");
			if (StructName == TEXT("LinearColor")) return TEXT("linearColor");
			if (StructName == TEXT("Color")) return TEXT("color");
			return TEXT("struct");
		}
		if (Property->IsA<FObjectPropertyBase>()) return TEXT("objectPath");
		if (Property->IsA<FArrayProperty>()) return TEXT("array");
		if (Property->IsA<FSetProperty>()) return TEXT("set");
		if (Property->IsA<FMapProperty>()) return TEXT("map");
		return TEXT("unknown");
	}

	TArray<TSharedPtr<FJsonValue>> DefinitionAllowedValueKinds(const FString& ValueType)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		auto Add = [&Values](const TCHAR* Value)
		{
			Values.Add(MakeShared<FJsonValueString>(Value));
		};
		if (ValueType == TEXT("boolean")) Add(TEXT("boolean"));
		else if (ValueType == TEXT("number") || ValueType == TEXT("integer")) Add(TEXT("number"));
		else if (ValueType == TEXT("enum"))
		{
			Add(TEXT("string"));
			Add(TEXT("number"));
		}
		else if (ValueType == TEXT("string") || ValueType == TEXT("name")) Add(TEXT("string"));
		else if (ValueType == TEXT("objectPath"))
		{
			Add(TEXT("string"));
			Add(TEXT("null"));
		}
		else if (ValueType == TEXT("array") || ValueType == TEXT("set")) Add(TEXT("array"));
		else if (ValueType == TEXT("map") || ValueType == TEXT("struct")
			|| ValueType.StartsWith(TEXT("vector")) || ValueType == TEXT("linearColor")
			|| ValueType == TEXT("color"))
			Add(TEXT("object"));
		return Values;
	}

	const UEnum* DefinitionPropertyEnum(const FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum;
		}
		return nullptr;
	}

	TSharedPtr<const FBoundaryProof> FindBoundaryProof(const FString& Id)
	{
		Expire();
		for (const TSharedPtr<const FBoundaryProof>& Item : BoundaryProofs)
		{
			if (Item && Item->BoundaryId == Id)
			{
				return Item;
			}
		}
		return nullptr;
	}

	bool ShouldExportDefinitionProperty(const FProperty* Property)
	{
		if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit)
			|| Property->HasAnyPropertyFlags(
				CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
		{
			return false;
		}
		static const TSet<FName> SkippedNames = {
			FName(TEXT("MaterialExpressionEditorX")),
			FName(TEXT("MaterialExpressionEditorY")),
			FName(TEXT("MaterialExpressionGuid")),
			FName(TEXT("Desc")),
			FName(TEXT("bRealtimePreview")),
			FName(TEXT("bNeedToUpdatePreview")),
			FName(TEXT("bIsParameterExpression")),
			FName(TEXT("bShowOutputNameOnPin")),
			FName(TEXT("bShowMaskColorsOnPin")),
			FName(TEXT("bCollapsed")),
			FName(TEXT("bShaderInputData")),
			FName(TEXT("bShowInputs")),
			FName(TEXT("bShowOutputs"))
		};
		return !SkippedNames.Contains(Property->GetFName());
	}

	FString DefinitionPropertyWriterRole(UClass* Class, const FName PropertyName)
	{
		if (!Class)
		{
			return FString();
		}
		if (Class->IsChildOf(UMaterialExpressionConstant::StaticClass())
			&& PropertyName == TEXT("R"))
			return TEXT("primaryValue");
		if (Class->IsChildOf(UMaterialExpressionConstant3Vector::StaticClass())
			&& PropertyName == TEXT("Constant"))
			return TEXT("primaryValue");
		if (Class->IsChildOf(UMaterialExpressionConstant4Vector::StaticClass())
			&& PropertyName == TEXT("Constant"))
			return TEXT("primaryValue");
		if (Class->IsChildOf(UMaterialExpressionScalarParameter::StaticClass()))
		{
			if (PropertyName == TEXT("DefaultValue")) return TEXT("primaryValue");
			if (PropertyName == TEXT("ParameterName")) return TEXT("optionalSideInput");
		}
		if (Class->IsChildOf(UMaterialExpressionVectorParameter::StaticClass()))
		{
			if (PropertyName == TEXT("DefaultValue")) return TEXT("primaryValue");
			if (PropertyName == TEXT("ParameterName")) return TEXT("optionalSideInput");
		}
		if (Class->IsChildOf(UMaterialExpressionCustom::StaticClass())
			&& PropertyName == TEXT("Code"))
			return TEXT("primaryValue");
		if (Class->IsChildOf(UMaterialExpressionComponentMask::StaticClass())
			&& (PropertyName == TEXT("R") || PropertyName == TEXT("G")
				|| PropertyName == TEXT("B") || PropertyName == TEXT("A")))
		{
			return TEXT("aggregateComponent");
		}
		return FString();
	}

	bool TryReadFinitePropertyMetadata(
		const FProperty* Property,
		const TCHAR* MetadataKey,
		double& OutValue)
	{
		if (!Property || !Property->HasMetaData(MetadataKey))
		{
			return false;
		}
		const FString Text = Property->GetMetaData(MetadataKey).TrimStartAndEnd();
		if (Text.IsEmpty())
		{
			return false;
		}
		double Value = 0.0;
		if (!LexTryParseString(Value, *Text) || !FMath::IsFinite(Value))
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	TSharedPtr<FJsonObject> BuildDefinitionPropertyContract(
		UClass* Class,
		const FProperty* Property,
		UObject* ClassDefaultObject)
	{
		auto Contract = MakeShared<FJsonObject>();
		const FString ValueType = DescribeDefinitionPropertyValueType(Property);
		const FString WriterRole = DefinitionPropertyWriterRole(Class, Property->GetFName());
		const bool bWriterSupported = !WriterRole.IsEmpty();
		Contract->SetStringField(TEXT("propertyKey"), Property->GetName());
		Contract->SetStringField(TEXT("cppType"), Property->GetCPPType());
		Contract->SetStringField(TEXT("valueType"), ValueType);
		Contract->SetBoolField(TEXT("requiredOnCreate"), false);
		Contract->SetBoolField(TEXT("editorReadOnly"), Property->HasAnyPropertyFlags(CPF_EditConst));
		Contract->SetBoolField(TEXT("readOnly"), !bWriterSupported);
		Contract->SetBoolField(TEXT("writerSupported"), bWriterSupported);
		Contract->SetStringField(TEXT("writerRole"), bWriterSupported ? WriterRole : TEXT("none"));
		if (bWriterSupported)
		{
			Contract->SetStringField(
				TEXT("writerCapability"), TEXT("content.material.expression.value.set"));
		}
		Contract->SetArrayField(
			TEXT("allowedValueKinds"), DefinitionAllowedValueKinds(ValueType));

		if (const UEnum* Enum = DefinitionPropertyEnum(Property))
		{
			Contract->SetStringField(TEXT("enumType"), Enum->GetPathName());
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			int32 EligibleValues = 0;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
				++EligibleValues;
				if (EnumValues.Num() >= MaxDefinitionEnumValues)
				{
					continue;
				}
				auto Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
				Value->SetNumberField(TEXT("value"), Enum->GetValueByIndex(Index));
				const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
				if (DisplayName.Len() <= 256)
				{
					Value->SetStringField(TEXT("displayName"), DisplayName);
				}
				EnumValues.Add(MakeShared<FJsonValueObject>(Value));
			}
			Contract->SetArrayField(TEXT("enumValues"), EnumValues);
			Contract->SetNumberField(TEXT("enumValueCount"), EligibleValues);
			Contract->SetBoolField(
				TEXT("enumValuesTruncated"), EligibleValues > EnumValues.Num());
		}
		if (const FObjectPropertyBase* ObjectProperty =
			CastField<FObjectPropertyBase>(Property))
		{
			if (ObjectProperty->PropertyClass)
			{
				Contract->SetStringField(
					TEXT("expectedClass"), ObjectProperty->PropertyClass->GetPathName());
			}
		}

		auto Constraints = MakeShared<FJsonObject>();
		auto AddConstraint = [&](const TCHAR* MetadataKey, const TCHAR* JsonKey)
		{
			double Value = 0.0;
			if (TryReadFinitePropertyMetadata(Property, MetadataKey, Value))
			{
				Constraints->SetNumberField(JsonKey, Value);
			}
		};
		AddConstraint(TEXT("ClampMin"), TEXT("clampMin"));
		AddConstraint(TEXT("ClampMax"), TEXT("clampMax"));
		AddConstraint(TEXT("UIMin"), TEXT("uiMin"));
		AddConstraint(TEXT("UIMax"), TEXT("uiMax"));
		if (!Constraints->Values.IsEmpty())
		{
			Contract->SetObjectField(TEXT("constraints"), Constraints);
		}

		const bool bBoundedDefaultType = !Property->IsA<FArrayProperty>()
			&& !Property->IsA<FSetProperty>()
			&& !Property->IsA<FMapProperty>()
			&& Property->ArrayDim == 1;
		if (ClassDefaultObject && bBoundedDefaultType)
		{
			FString DefaultText;
			if (Property->ExportText_InContainer(
				0,
				DefaultText,
				ClassDefaultObject,
				nullptr,
				ClassDefaultObject,
				PPF_None))
			{
				Contract->SetNumberField(
					TEXT("defaultValueCharacters"), DefaultText.Len());
				if (DefaultText.Len() <= MaxDefinitionDefaultTextCharacters)
				{
					Contract->SetStringField(TEXT("defaultValueText"), DefaultText);
					Contract->SetBoolField(TEXT("defaultValueOmitted"), false);
				}
				else
				{
					Contract->SetStringField(TEXT("defaultValueHash"), Digest(DefaultText));
					Contract->SetBoolField(TEXT("defaultValueOmitted"), true);
				}
			}
		}
		return Contract;
	}

	FString DescribeDefinitionPinValueType(const uint32 ValueType)
	{
		switch (ValueType)
		{
		case MCT_Float1: return TEXT("float1");
		case MCT_Float2: return TEXT("float2");
		case MCT_Float3: return TEXT("float3");
		case MCT_Float4: return TEXT("float4");
		case MCT_Float: return TEXT("float");
		case MCT_Texture2D: return TEXT("texture2D");
		case MCT_TextureCube: return TEXT("textureCube");
		case MCT_Texture2DArray: return TEXT("texture2DArray");
		case MCT_TextureCubeArray: return TEXT("textureCubeArray");
		case MCT_VolumeTexture: return TEXT("volumeTexture");
		case MCT_StaticBool: return TEXT("staticBoolean");
		case MCT_Bool: return TEXT("boolean");
		case MCT_MaterialAttributes: return TEXT("materialAttributes");
		case MCT_ShadingModel: return TEXT("shadingModel");
		case MCT_LWCScalar: return TEXT("lwcScalar");
		case MCT_LWCVector2: return TEXT("lwcVector2");
		case MCT_LWCVector3: return TEXT("lwcVector3");
		case MCT_LWCVector4: return TEXT("lwcVector4");
		case MCT_UInt1: return TEXT("uint1");
		case MCT_UInt2: return TEXT("uint2");
		case MCT_UInt3: return TEXT("uint3");
		case MCT_UInt4: return TEXT("uint4");
		case MCT_Execution: return TEXT("execution");
		case MCT_VoidStatement: return TEXT("voidStatement");
		default: break;
		}
		if (ValueType & MCT_MaterialAttributes) return TEXT("materialAttributes");
		if (ValueType & MCT_Texture) return TEXT("texture");
		if (ValueType & MCT_LWCType) return TEXT("lwcNumeric");
		if (ValueType & MCT_UInt) return TEXT("uint");
		if (ValueType & MCT_Numeric) return TEXT("numeric");
		return TEXT("unknown");
	}

	TSharedPtr<FJsonObject> BuildDefinitionDirectValueWriter(UClass* Class)
	{
		auto Writer = MakeShared<FJsonObject>();
		Writer->SetStringField(
			TEXT("capability"), TEXT("content.material.expression.value.set"));
		Writer->SetBoolField(TEXT("genericReflectedPropertyWriter"), false);
		FString ValueKind;
		TArray<TSharedPtr<FJsonValue>> RequiredFields;
		TArray<TSharedPtr<FJsonValue>> OptionalFields;
		auto Require = [&RequiredFields](const TCHAR* Name)
		{
			RequiredFields.Add(MakeShared<FJsonValueString>(Name));
		};
		if (Class->IsChildOf(UMaterialExpressionConstant::StaticClass())
			|| Class->IsChildOf(UMaterialExpressionScalarParameter::StaticClass()))
		{
			ValueKind = TEXT("number");
		}
		else if (Class->IsChildOf(UMaterialExpressionConstant3Vector::StaticClass()))
		{
			ValueKind = TEXT("object");
			Require(TEXT("r"));
			Require(TEXT("g"));
			Require(TEXT("b"));
		}
		else if (Class->IsChildOf(UMaterialExpressionConstant4Vector::StaticClass())
			|| Class->IsChildOf(UMaterialExpressionVectorParameter::StaticClass()))
		{
			ValueKind = TEXT("object");
			Require(TEXT("r"));
			Require(TEXT("g"));
			Require(TEXT("b"));
			Require(TEXT("a"));
		}
		else if (Class->IsChildOf(UMaterialExpressionCustom::StaticClass()))
		{
			ValueKind = TEXT("string");
		}
		else if (Class->IsChildOf(UMaterialExpressionComponentMask::StaticClass()))
		{
			ValueKind = TEXT("object");
			Require(TEXT("r"));
			Require(TEXT("g"));
			Require(TEXT("b"));
			Require(TEXT("a"));
		}
		if (Class->IsChildOf(UMaterialExpressionScalarParameter::StaticClass())
			|| Class->IsChildOf(UMaterialExpressionVectorParameter::StaticClass()))
		{
			OptionalFields.Add(MakeShared<FJsonValueString>(TEXT("parameterName")));
		}
		Writer->SetBoolField(TEXT("supported"), !ValueKind.IsEmpty());
		Writer->SetStringField(
			TEXT("valueKind"), ValueKind.IsEmpty() ? TEXT("none") : ValueKind);
		Writer->SetArrayField(TEXT("requiredObjectFields"), RequiredFields);
		Writer->SetArrayField(TEXT("optionalRequestFields"), OptionalFields);
		return Writer;
	}

	bool DefinitionUsesDynamicPinConfiguration(UClass* Class)
	{
		if (!Class)
		{
			return false;
		}
		if (Class->IsChildOf(UMaterialExpressionCustom::StaticClass())
			|| Class->IsChildOf(UMaterialExpressionMaterialFunctionCall::StaticClass())
			|| Class->IsChildOf(UMaterialExpressionFunctionInput::StaticClass())
			|| Class->IsChildOf(UMaterialExpressionFunctionOutput::StaticClass()))
		{
			return true;
		}
		const FName ClassName = Class->GetFName();
		return ClassName == TEXT("MaterialExpressionGetMaterialAttributes")
			|| ClassName == TEXT("MaterialExpressionSetMaterialAttributes");
	}

	TSharedPtr<FJsonObject> BuildDefinitionContract(
		UClass* Class,
		const FString& AssetKind,
		const FString& DefinitionKey,
		const FString& DisplayName)
	{
		auto Contract = MakeShared<FJsonObject>();
		Contract->SetStringField(TEXT("definitionKey"), DefinitionKey);
		Contract->SetStringField(TEXT("className"), Class->GetName());
		Contract->SetStringField(TEXT("displayName"), DisplayName);
		Contract->SetStringField(TEXT("creationMode"), TEXT("ordinaryExpression"));
		Contract->SetStringField(TEXT("assetKind"), AssetKind);
		Contract->SetStringField(
			TEXT("definitionKind"),
			Class->IsChildOf(UMaterialExpressionCustom::StaticClass())
				? TEXT("customHlslNode")
				: TEXT("builtInExpression"));
		Contract->SetObjectField(
			TEXT("directValueWriter"), BuildDefinitionDirectValueWriter(Class));
		const bool bDynamicPinConfiguration = DefinitionUsesDynamicPinConfiguration(Class);
		Contract->SetStringField(TEXT("templateSource"), TEXT("classDefaultObject"));
		Contract->SetBoolField(
			TEXT("dynamicInstanceConfigurationRequired"), bDynamicPinConfiguration);
		Contract->SetBoolField(TEXT("templatesCompleteForInstances"), false);

		UObject* ClassDefaultObject = Class->GetDefaultObject();
		TArray<const FProperty*> Properties;
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			if (ShouldExportDefinitionProperty(*It))
			{
				Properties.Add(*It);
			}
		}
		Properties.Sort([](const FProperty& Left, const FProperty& Right)
		{
			return Left.GetName().Compare(Right.GetName(), ESearchCase::CaseSensitive) < 0;
		});
		TArray<TSharedPtr<FJsonValue>> PropertyContracts;
		for (int32 Index = 0;
		     Index < Properties.Num() && PropertyContracts.Num() < MaxDefinitionProperties;
		     ++Index)
		{
			PropertyContracts.Add(MakeShared<FJsonValueObject>(
				BuildDefinitionPropertyContract(Class, Properties[Index], ClassDefaultObject)));
		}
		Contract->SetArrayField(TEXT("propertyContracts"), PropertyContracts);
		Contract->SetNumberField(TEXT("propertyContractCount"), Properties.Num());
		Contract->SetBoolField(
			TEXT("propertyContractsTruncated"), Properties.Num() > PropertyContracts.Num());

		TArray<TSharedPtr<FJsonValue>> InputTemplates;
		TArray<TSharedPtr<FJsonValue>> OutputTemplates;
		int32 TotalInputs = 0;
		int32 TotalOutputs = 0;
		if (UMaterialExpression* DefaultExpression =
			Cast<UMaterialExpression>(ClassDefaultObject))
		{
			const TArrayView<FExpressionInput*> Inputs = DefaultExpression->GetInputsView();
			TotalInputs = Inputs.Num();
			for (int32 Index = 0;
			     Index < Inputs.Num() && InputTemplates.Num() < MaxDefinitionInputs;
			     ++Index)
			{
				const uint32 InputType = DefaultExpression->GetInputType(Index);
				auto Input = MakeShared<FJsonObject>();
				Input->SetNumberField(TEXT("index"), Index);
				Input->SetStringField(
					TEXT("portName"), DefaultExpression->GetInputName(Index).ToString());
				Input->SetStringField(
					TEXT("valueType"), DescribeDefinitionPinValueType(InputType));
				Input->SetNumberField(TEXT("rawMaterialValueType"), InputType);
				Input->SetBoolField(TEXT("supportsLiteralValue"), false);
				Input->SetBoolField(TEXT("instancePresenceGuaranteed"), false);
				Input->SetBoolField(TEXT("connectionWriterSupportedWhenPresent"), true);
				Input->SetStringField(
					TEXT("connectionWriterCapability"), TEXT("content.material.pin.connect"));
				Input->SetArrayField(
					TEXT("allowedValueKinds"), TArray<TSharedPtr<FJsonValue>>());
				InputTemplates.Add(MakeShared<FJsonValueObject>(Input));
			}

			TotalOutputs = DefaultExpression->Outputs.Num();
			for (int32 Index = 0;
			     Index < DefaultExpression->Outputs.Num()
			     && OutputTemplates.Num() < MaxDefinitionOutputs;
			     ++Index)
			{
				const uint32 OutputType = DefaultExpression->GetOutputType(Index);
				auto Output = MakeShared<FJsonObject>();
				Output->SetNumberField(TEXT("index"), Index);
				Output->SetStringField(
					TEXT("portName"),
					DefaultExpression->Outputs[Index].OutputName.ToString());
				Output->SetStringField(
					TEXT("valueType"), DescribeDefinitionPinValueType(OutputType));
				Output->SetNumberField(TEXT("rawMaterialValueType"), OutputType);
				Output->SetBoolField(TEXT("instancePresenceGuaranteed"), false);
				Output->SetBoolField(TEXT("connectionWriterSupportedWhenPresent"), true);
				Output->SetStringField(
					TEXT("connectionWriterCapability"), TEXT("content.material.pin.connect"));
				OutputTemplates.Add(MakeShared<FJsonValueObject>(Output));
			}
		}
		Contract->SetArrayField(TEXT("inputTemplates"), InputTemplates);
		Contract->SetArrayField(TEXT("outputTemplates"), OutputTemplates);
		Contract->SetNumberField(TEXT("inputTemplateCount"), TotalInputs);
		Contract->SetNumberField(TEXT("outputTemplateCount"), TotalOutputs);
		Contract->SetBoolField(
			TEXT("inputTemplatesTruncated"), TotalInputs > InputTemplates.Num());
		Contract->SetBoolField(
			TEXT("outputTemplatesTruncated"), TotalOutputs > OutputTemplates.Num());
		Contract->SetBoolField(
			TEXT("classDefaultTemplatesComplete"),
			TotalInputs <= InputTemplates.Num() && TotalOutputs <= OutputTemplates.Num());
		return Contract;
	}

	FMCPToolResult ListDefinitions(const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());
		int32 Limit = 50;
		if (!ReadInt(Params, TEXT("limit"), 50, 1, 200, Limit))
		{
			return Invalid(TEXT("limit must be an integer in [1, 200]."));
		}

		FString AssetKind = TEXT("material");
		FString DefinitionKey;
		FString Search;
		FString CursorText;
		Params->TryGetStringField(TEXT("assetKind"), AssetKind);
		Params->TryGetStringField(TEXT("definitionKey"), DefinitionKey);
		Params->TryGetStringField(TEXT("search"), Search);
		Params->TryGetStringField(TEXT("cursor"), CursorText);
		if (AssetKind != TEXT("material") && AssetKind != TEXT("materialFunction"))
		{
			return Invalid(TEXT("assetKind must be material or materialFunction."));
		}
		if (DefinitionKey.Len() > 256 || Search.Len() > 256 || CursorText.Len() > 2048)
		{
			return Invalid(TEXT("Definition filter or cursor exceeds its size limit."));
		}
		if (!DefinitionKey.IsEmpty() && !Search.IsEmpty())
		{
			return Invalid(TEXT("Use definitionKey for exact lookup or search for fuzzy lookup, not both."));
		}

		struct FDefinition
		{
			FString Key;
			FString ClassName;
			FString DisplayName;
			FString ContractHash;
			TSharedPtr<FJsonObject> Contract;
		};
		TArray<FDefinition> Definitions;
		int32 ClassesScanned = 0;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (++ClassesScanned > MaxDefinitionClassesScanned)
			{
				return FMCPToolResult::Error(
					TEXT(
						"The loaded class set exceeded the bounded definition scan budget; no partial catalog was returned."),
					TEXT("material_definition_scan_budget_exceeded"),
					413);
			}
			UClass* Class = *It;
			if (!Class->IsChildOf(UMaterialExpression::StaticClass())
				|| Class == UMaterialExpression::StaticClass()
				|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				|| Class->IsChildOf(UMaterialExpressionComposite::StaticClass())
				|| Class->IsChildOf(UMaterialExpressionPinBase::StaticClass())
				|| Class->IsChildOf(UMaterialExpressionComment::StaticClass())
				|| !IsAllowedExpressionType(Class, AssetKind == TEXT("materialFunction")))
			{
				continue;
			}
			FDefinition Definition;
			Definition.ClassName = Class->GetName();
			Definition.Key = Definition.ClassName;
			Definition.Key.RemoveFromStart(TEXT("MaterialExpression"));
			Definition.DisplayName = Class->GetDisplayNameText().ToString();
			if (!DefinitionKey.IsEmpty()
				&& !DefinitionKey.Equals(Definition.Key, ESearchCase::IgnoreCase)
				&& !DefinitionKey.Equals(Definition.ClassName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (!Search.IsEmpty()
				&& !Definition.Key.Contains(Search, ESearchCase::IgnoreCase)
				&& !Definition.ClassName.Contains(Search, ESearchCase::IgnoreCase)
				&& !Definition.DisplayName.Contains(Search, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (Definitions.Num() >= MaxDefinitionContractsBuilt)
			{
				return FMCPToolResult::Error(
					TEXT("The matching definition set exceeded the bounded contract-build budget; refine the filter."),
					TEXT("material_definition_contract_budget_exceeded"),
					413);
			}
			Definition.Contract = BuildDefinitionContract(
				Class,
				AssetKind,
				Definition.Key,
				Definition.DisplayName);
			Definition.ContractHash = Digest(JsonText(Definition.Contract));
			if (Definition.ContractHash.IsEmpty())
			{
				return FMCPToolResult::Error(
					TEXT("Definition contract hashing is unavailable."),
					TEXT("graph_hash_unavailable"),
					500);
			}
			Definitions.Add(MoveTemp(Definition));
		}
		Definitions.Sort([](const FDefinition& Left, const FDefinition& Right)
		{
			const int32 KeyOrder = Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive);
			return KeyOrder != 0 ? KeyOrder < 0 : Left.ClassName < Right.ClassName;
		});
		FString CatalogIdentity;
		for (const FDefinition& Definition : Definitions)
		{
			CatalogIdentity += FString::FromInt(Definition.Key.Len()) + TEXT(":") + Definition.Key;
			CatalogIdentity += FString::FromInt(Definition.ClassName.Len()) + TEXT(":") + Definition.ClassName;
			CatalogIdentity += FString::FromInt(Definition.ContractHash.Len()) + TEXT(":") + Definition.ContractHash;
		}
		const FString CatalogHash = Digest(CatalogIdentity);
		if (CatalogHash.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("Definition catalog hashing is unavailable."), TEXT("graph_hash_unavailable"), 500);
		}

		auto Filter = MakeShared<FJsonObject>();
		Filter->SetStringField(TEXT("assetKind"), AssetKind);
		Filter->SetStringField(TEXT("definitionKey"), DefinitionKey.ToLower());
		Filter->SetStringField(TEXT("search"), Search.ToLower());
		const FString FilterHash = Digest(JsonText(Filter));
		int32 Offset = 0;
		if (!CursorText.IsEmpty())
		{
			FString Decoded;
			TSharedPtr<FJsonObject> Cursor;
			FString CursorFilter;
			FString CursorCatalog;
			if (!FBase64::Decode(CursorText, Decoded)
				|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Decoded), Cursor)
				|| !Cursor || !Cursor->TryGetStringField(TEXT("filter"), CursorFilter)
				|| !Cursor->TryGetStringField(TEXT("catalog"), CursorCatalog)
				|| CursorFilter != FilterHash
				|| CursorCatalog != CatalogHash
				|| !ReadInt(Cursor, TEXT("offset"), -1, 0, Definitions.Num(), Offset))
			{
				return Invalid(TEXT(
					"Cursor does not match this definition filter/catalog, or is malformed. Restart pagination."));
			}
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		int32 ResponseBytes = 4096;
		while (Offset < Definitions.Num() && Rows.Num() < Limit)
		{
			const FDefinition& Definition = Definitions[Offset];
			const FString RowText = JsonText(Definition.Contract);
			const int32 RowBytes = FTCHARToUTF8(*RowText).Length();
			if (ResponseBytes + RowBytes
				> ResponseBudget - DefinitionResponseReserveBytes)
			{
				if (Rows.IsEmpty())
				{
					return FMCPToolResult::Error(
						TEXT("A single bounded definition contract exceeds the response budget."),
						TEXT("material_definition_too_large"),
						413);
				}
				break;
			}
			ResponseBytes += RowBytes;
			Rows.Add(MakeShared<FJsonValueObject>(Definition.Contract));
			++Offset;
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.material.expression-definitions/1"));
		Result->SetStringField(TEXT("assetKind"), AssetKind);
		Result->SetStringField(TEXT("filterHash"), FilterHash);
		Result->SetStringField(TEXT("catalogHash"), CatalogHash);
		Result->SetNumberField(TEXT("total"), Definitions.Num());
		Result->SetNumberField(TEXT("returnedCount"), Rows.Num());
		Result->SetNumberField(TEXT("responseBytesApprox"), ResponseBytes);
		Result->SetNumberField(TEXT("classesScanned"), ClassesScanned);
		Result->SetNumberField(TEXT("contractsBuilt"), Definitions.Num());
		Result->SetBoolField(TEXT("catalogComplete"), true);
		Result->SetBoolField(TEXT("scanExhausted"), false);
		Result->SetArrayField(TEXT("definitions"), Rows);
		auto Limits = MakeShared<FJsonObject>();
		Limits->SetNumberField(
			TEXT("maxPropertyContractsPerDefinition"), MaxDefinitionProperties);
		Limits->SetNumberField(
			TEXT("maxInputTemplatesPerDefinition"), MaxDefinitionInputs);
		Limits->SetNumberField(
			TEXT("maxOutputTemplatesPerDefinition"), MaxDefinitionOutputs);
		Limits->SetNumberField(
			TEXT("maxEnumValuesPerProperty"), MaxDefinitionEnumValues);
		Limits->SetNumberField(
			TEXT("maxDefaultTextCharacters"), MaxDefinitionDefaultTextCharacters);
		Limits->SetNumberField(TEXT("responseBudgetBytes"), ResponseBudget);
		Limits->SetNumberField(
			TEXT("maxClassesScanned"), MaxDefinitionClassesScanned);
		Limits->SetNumberField(
			TEXT("maxContractsBuilt"), MaxDefinitionContractsBuilt);
		Result->SetObjectField(TEXT("limits"), Limits);
		Result->SetBoolField(TEXT("genericReflectedPropertyWriterSupported"), false);
		Result->SetStringField(
			TEXT("propertyWriterSemantics"),
			TEXT(
				"Only class-specific fields marked writerSupported are writable through content.material.expression.value.set."));
		Result->SetBoolField(TEXT("hasMore"), Offset < Definitions.Num());
		if (Offset < Definitions.Num())
		{
			auto Cursor = MakeShared<FJsonObject>();
			Cursor->SetStringField(TEXT("filter"), FilterHash);
			Cursor->SetStringField(TEXT("catalog"), CatalogHash);
			Cursor->SetNumberField(TEXT("offset"), Offset);
			Result->SetStringField(TEXT("nextCursor"), FBase64::Encode(JsonText(Cursor)));
		}
		return FMCPToolResult::Ok(Result);
	}

	FMCPToolResult ResolveNodeSource(const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());
		FString SnapshotId;
		FString RequestedNodeId;
		if (!Params->TryGetStringField(TEXT("snapshotId"), SnapshotId) || SnapshotId.IsEmpty())
			return Invalid(TEXT("snapshotId is required."));
		if (!Params->TryGetStringField(TEXT("nodeId"), RequestedNodeId) || RequestedNodeId.IsEmpty())
			return Invalid(TEXT("nodeId is required."));
		const TSharedPtr<const FSnapshot> Snapshot = FindSnapshot(SnapshotId);
		if (!Snapshot) return MissingSnapshot();
		const int32* NodeIndex = Snapshot->ById.Find(RequestedNodeId);
		if (!NodeIndex || !Snapshot->Nodes.IsValidIndex(*NodeIndex))
			return FMCPToolResult::Error(
				TEXT("The requested node is not present in the graph snapshot."), TEXT("graph_node_not_found"), 404);

		const FNode& SnapshotNode = Snapshot->Nodes[*NodeIndex];
		UMaterialExpression* Expression = nullptr;
		UObject* OriginalAsset = nullptr;
		MaterialEditing::FTarget PreviewTarget;
		FString ResolveError;
		const FString Package = FPackageName::ObjectPathToPackageName(Snapshot->AssetPath);
		const FString ObjectPath = Snapshot->AssetPath.Contains(TEXT("."))
			                           ? Snapshot->AssetPath
			                           : Package + TEXT(".") + FPackageName::GetShortName(Package);
		OriginalAsset = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		if (!OriginalAsset || (!OriginalAsset->IsA<UMaterial>() && !OriginalAsset->IsA<UMaterialFunction>()))
			return FMCPToolResult::Error(
				TEXT("The material asset for this snapshot is no longer loaded."), TEXT("material_asset_unavailable"),
				409);

		// A snapshot contains value data only. Resolve against a fresh live
		// projection before returning an object identity; otherwise a deleted or
		// replaced expression with the same stale node id could be reported as
		// live after PostEditChange or package reload.
		TSharedPtr<const FSnapshot> FreshSnapshot;
		FMCPToolResult FreshCapture;

		if (Snapshot->PreviewId.IsEmpty())
		{
			FreshCapture = Capture(
				OriginalAsset,
				Snapshot->AssetPath,
				FString(),
				nullptr,
				Snapshot->bIncludeNamedReroutes,
				&FreshSnapshot,
				false);
			if (RequestedNodeId != TEXT("root"))
			{
				if (UMaterial* Material = Cast<UMaterial>(OriginalAsset))
				{
					for (UMaterialExpression* Candidate : Material->GetExpressions())
						if (Candidate && (MCPMaterialInfrastructure::ExpressionNodeId(Candidate) == RequestedNodeId
							|| MCPMaterialInfrastructure::MatchesMaterialNode(Candidate->GraphNode, RequestedNodeId)))
						{
							Expression = Candidate;
							break;
						}
				}
				else if (UMaterialFunction* Function = Cast<UMaterialFunction>(OriginalAsset))
				{
					for (UMaterialExpression* Candidate : Function->GetExpressions())
						if (Candidate && (MCPMaterialInfrastructure::ExpressionNodeId(Candidate) == RequestedNodeId
							|| MCPMaterialInfrastructure::MatchesMaterialNode(Candidate->GraphNode, RequestedNodeId)))
						{
							Expression = Candidate;
							break;
						}
				}
			}
		}
		else
		{
			TSharedRef<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
			PreviewParams->SetStringField(OriginalAsset->IsA<UMaterial>() ? TEXT("material") : TEXT("materialFunction"),
			                              ObjectPath);
			PreviewParams->SetStringField(TEXT("targetContext"), TEXT("editorPreview"));
			PreviewParams->SetStringField(TEXT("expectedPreviewId"), Snapshot->PreviewId);
			if (!MaterialEditing::Resolve(PreviewParams, PreviewTarget, ResolveError))
				return FMCPToolResult::Error(ResolveError, TEXT("preview_unavailable"), 409);
			FreshCapture = Capture(
				PreviewTarget.Asset,
				Snapshot->AssetPath,
				Snapshot->PreviewId,
				&PreviewTarget.Expressions,
				Snapshot->bIncludeNamedReroutes,
				&FreshSnapshot,
				false);
			if (RequestedNodeId != TEXT("root")) Expression = PreviewTarget.Find(RequestedNodeId);
		}

		if (!FreshCapture.bSuccess || !FreshSnapshot)
		{
			return FreshCapture.bSuccess
				       ? FMCPToolResult::Error(
					       TEXT("The live material projection could not be captured."),
					       TEXT("material_graph_live_unavailable"),
					       409)
				       : FreshCapture;
		}
		if (FreshSnapshot->AssetPath != Snapshot->AssetPath
			|| FreshSnapshot->PreviewId != Snapshot->PreviewId
			|| FreshSnapshot->ProjectionHash != Snapshot->ProjectionHash)
		{
			auto StaleData = MakeShared<FJsonObject>();
			StaleData->SetStringField(TEXT("snapshotId"), SnapshotId);
			StaleData->SetStringField(TEXT("nodeId"), RequestedNodeId);
			StaleData->SetStringField(TEXT("sourceState"), TEXT("stale"));
			StaleData->SetStringField(TEXT("resolutionState"), TEXT("stale"));
			StaleData->SetStringField(TEXT("sourceProjectionHash"), Snapshot->ProjectionHash);
			StaleData->SetStringField(TEXT("freshProjectionHash"), FreshSnapshot->ProjectionHash);
			StaleData->SetBoolField(TEXT("liveStateChecked"), true);
			FMCPToolResult Stale = FMCPToolResult::Error(
				TEXT("The live material projection changed after this snapshot was captured; recapture the graph."),
				TEXT("graph_node_stale"),
				409);
			Stale.Data = StaleData;
			return Stale;
		}

		if (RequestedNodeId != TEXT("root") && !Expression)
		{
			auto StaleData = MakeShared<FJsonObject>();
			StaleData->SetStringField(TEXT("snapshotId"), SnapshotId);
			StaleData->SetStringField(TEXT("nodeId"), RequestedNodeId);
			StaleData->SetStringField(TEXT("sourceState"), TEXT("stale"));
			StaleData->SetStringField(TEXT("resolutionState"), TEXT("stale"));
			StaleData->SetStringField(TEXT("sourceProjectionHash"), Snapshot->ProjectionHash);
			StaleData->SetStringField(TEXT("freshProjectionHash"), FreshSnapshot->ProjectionHash);
			StaleData->SetBoolField(TEXT("liveStateChecked"), true);
			FMCPToolResult Stale = FMCPToolResult::Error(
				TEXT("The snapshot node no longer resolves to a live material expression; recapture the graph."),
				TEXT("graph_node_stale"),
				409);
			Stale.Data = StaleData;
			return Stale;
		}

		auto Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("ueObjectName"), Expression ? Expression->GetName() : OriginalAsset->GetName());
		Source->SetStringField(TEXT("ueClass"), Expression
			                                        ? Expression->GetClass()->GetName()
			                                        : OriginalAsset->GetClass()->GetName());
		Source->SetStringField(TEXT("rawExportId"), SnapshotNode.Id);
		Source->SetStringField(
			TEXT("materialExpressionGuid"),
			Expression ? Expression->MaterialExpressionGuid.ToString(EGuidFormats::Digits) : FString());
		Source->SetStringField(TEXT("assetPath"), Snapshot->AssetPath);
		Source->SetStringField(TEXT("sourceMappingState"), TEXT("unavailable"));
		Source->SetStringField(
			TEXT("sourceMappingReason"),
			TEXT(
				"The current integration exposes authored UE object identity; compiler authored-line mapping requires an optional engine accessor."));
		Source->SetBoolField(TEXT("diagnosticOnly"), true);

		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("snapshotId"), SnapshotId);
		Result->SetStringField(TEXT("nodeId"), RequestedNodeId);
		Result->SetObjectField(TEXT("source"), Source);
		Result->SetBoolField(TEXT("writeAuthorized"), false);
		Result->SetBoolField(TEXT("liveStateChecked"), true);
		Result->SetStringField(TEXT("sourceState"), TEXT("live"));
		Result->SetStringField(TEXT("resolutionState"), TEXT("live"));
		Result->SetStringField(TEXT("sourceProjectionHash"), Snapshot->ProjectionHash);
		Result->SetStringField(TEXT("mappingCoverage"), TEXT("ueObjectIdentityOnly"));
		return FMCPToolResult::Ok(Result);
	}

	// Merge the two immutable indexes. The cursor keeps progress, so sparse changes
	// do not make each page scan the graph from the beginning.
	FMCPToolResult Diff(const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());
		FString BeforeId, AfterId, CursorText;
		if (!Params->TryGetStringField(TEXT("beforeSnapshotId"), BeforeId) || !Params->
			TryGetStringField(TEXT("afterSnapshotId"), AfterId))
			return Invalid(
				TEXT("beforeSnapshotId and afterSnapshotId are required."));
		const auto Before = FindSnapshot(BeforeId), After = FindSnapshot(AfterId);
		if (!Before || !After) return MissingSnapshot();
		if (Before->AssetPath != After->AssetPath || Before->AssetClass != After->AssetClass
			|| Before->AssetKind != After->AssetKind || Before->PreviewId != After->PreviewId
			|| Before->bIncludeNamedReroutes != After->bIncludeNamedReroutes)
			return Invalid(
				TEXT("Diff requires the same asset, asset/preview session and named-reroute projection mode."));
		int32 Limit;
		if (!ReadInt(Params, TEXT("limit"), 50, 1, 200, Limit))
			return Invalid(
				TEXT("limit must be an integer in [1,200]."));
		int32 Left = 0, Right = 0;
		FString Phase = TEXT("metadata");
		if (Params->HasField(TEXT("cursor")))
		{
			if (!Params->TryGetStringField(TEXT("cursor"), CursorText) || CursorText.IsEmpty() || CursorText.Len() >
				2048)
				return Invalid(TEXT("Invalid diff cursor."));
			FString Decoded, BoundBefore, BoundAfter;
			TSharedPtr<FJsonObject> Cursor;
			if (!FBase64::Decode(CursorText, Decoded) || !FJsonSerializer::Deserialize(
					TJsonReaderFactory<>::Create(Decoded), Cursor) || !Cursor.IsValid()
				|| !Cursor->TryGetStringField(TEXT("before"), BoundBefore) || !Cursor->TryGetStringField(
					TEXT("after"), BoundAfter)
				|| BoundBefore != BeforeId || BoundAfter != AfterId || !Cursor->TryGetStringField(TEXT("phase"), Phase)
				|| (Phase != TEXT("metadata") && Phase != TEXT("nodes") && Phase != TEXT("edges")))
				return Invalid(TEXT("Cursor must belong to the same ordered pair of snapshots."));
			if (!ReadInt(Cursor, TEXT("left"), -1, 0,
			             Phase == TEXT("nodes") ? Before->Nodes.Num() : Before->EdgesByKey.Num(), Left)
				|| !ReadInt(Cursor, TEXT("right"), -1, 0,
				            Phase == TEXT("nodes") ? After->Nodes.Num() : After->EdgesByKey.Num(), Right))
				return Invalid(TEXT("Cursor positions are outside the snapshot indexes."));
		}
		const bool bEqual = Before->ProjectionHash == After->ProjectionHash;
		int32 Scanned = 0, Bytes = 8192;
		bool bByteLimited = false;
		TArray<TSharedPtr<FJsonValue>> Changes;
		if (!bEqual && Phase == TEXT("metadata"))
		{
			if (!JsonObjectsEqual(Before->FunctionMetadata, After->FunctionMetadata))
			{
				auto Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("entity"), TEXT("functionMetadata"));
				Change->SetStringField(TEXT("change"), Before->FunctionMetadata.IsValid()
					                                       ? After->FunctionMetadata.IsValid()
						                                         ? TEXT("modified")
						                                         : TEXT("removed")
					                                       : TEXT("added"));
				Change->SetStringField(
					TEXT("beforeHash"),
					Before->FunctionMetadata.IsValid()
						? Digest(CanonicalJsonText(Before->FunctionMetadata))
						: FString());
				Change->SetStringField(
					TEXT("afterHash"),
					After->FunctionMetadata.IsValid()
						? Digest(CanonicalJsonText(After->FunctionMetadata))
						: FString());
				TArray<TSharedPtr<FJsonValue>> ChangedFields;
				for (const FString& Field : JsonObjectChangedFields(
					     Before->FunctionMetadata, After->FunctionMetadata))
				{
					ChangedFields.Add(MakeShared<FJsonValueString>(Field));
				}
				Change->SetArrayField(TEXT("changedFields"), ChangedFields);
				if (Before->FunctionMetadata.IsValid())
				{
					Change->SetObjectField(TEXT("beforeMetadata"), Before->FunctionMetadata);
				}
				if (After->FunctionMetadata.IsValid())
				{
					Change->SetObjectField(TEXT("afterMetadata"), After->FunctionMetadata);
				}

				const int32 Size = FTCHARToUTF8(*JsonText(Change)).Length();
				if (Bytes + Size > ResponseBudget)
				{
					bByteLimited = true;
				}
				else
				{
					Bytes += Size;
					Changes.Add(MakeShared<FJsonValueObject>(Change));
				}
			}
			Phase = TEXT("nodes");
			Left = Right = 0;
		}
		while (!bEqual && !bByteLimited && Scanned < PageScanBudget && Changes.Num() < Limit)
		{
			const bool bNodes = Phase == TEXT("nodes");
			const int32 LeftCount = bNodes ? Before->Nodes.Num() : Before->EdgesByKey.Num();
			const int32 RightCount = bNodes ? After->Nodes.Num() : After->EdgesByKey.Num();
			if (Left >= LeftCount && Right >= RightCount)
			{
				if (!bNodes) break;
				Phase = TEXT("edges");
				Left = Right = 0;
				continue;
			}
			const FString* A = Left < LeftCount
				                   ? (bNodes ? &Before->Nodes[Left].Id : &Before->Edges[Before->EdgesByKey[Left]].Key)
				                   : nullptr;
			const FString* B = Right < RightCount
				                   ? (bNodes ? &After->Nodes[Right].Id : &After->Edges[After->EdgesByKey[Right]].Key)
				                   : nullptr;
			const int32 Compare = !A ? 1 : !B ? -1 : A->Compare(*B, ESearchCase::CaseSensitive);
			TSharedPtr<FJsonObject> Change;
			if (Compare != 0 || (bNodes && Before->Nodes[Left].DataHash != After->Nodes[Right].DataHash))
			{
				Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("entity"), bNodes ? TEXT("node") : TEXT("edge"));
				Change->SetStringField(
					TEXT("change"), Compare < 0 ? TEXT("removed") : Compare > 0 ? TEXT("added") : TEXT("modified"));
				if (bNodes)
				{
					const FNode* Old = Compare <= 0 ? &Before->Nodes[Left] : nullptr;
					const FNode* New = Compare >= 0 ? &After->Nodes[Right] : nullptr;
					Change->SetStringField(TEXT("nodeId"), Old ? Old->Id : New->Id);
					const FSnapshot& NodeRefSnapshot = New ? *After : *Before;
					if (HasTypedIdentity(NodeRefSnapshot))
						Change->SetObjectField(TEXT("nodeRef"), NodeRef(NodeRefSnapshot, Old ? Old->Id : New->Id));
					Change->SetStringField(TEXT("beforeHash"), Old ? Old->DataHash : FString());
					Change->SetStringField(TEXT("afterHash"), New ? New->DataHash : FString());
					TArray<TSharedPtr<FJsonValue>> Fields;
					if (Old && New)
					{
						TSet<FString> Keys;
						for (const auto& P : Old->Data->Values) Keys.Add(P.Key);
						for (const auto& P : New->Data->Values) Keys.Add(P.Key);
						TArray<FString> Sorted = Keys.Array();
						Sorted.Sort();
						for (const auto& Key : Sorted)
						{
							const auto* X = Old->Data->Values.Find(Key);
							const auto* Y = New->Data->Values.Find(Key);
							if (!X || !Y || !FJsonValue::CompareEqual(**X, **Y))
								Fields.Add(
									MakeShared<FJsonValueString>(Key));
						}
					}
					Change->SetArrayField(TEXT("changedFields"), Fields);
				}
				else
				{
					const auto& S = Compare < 0 ? *Before : *After;
					Change->SetObjectField(
						TEXT("edge"), EdgeJson(S, S.Edges[S.EdgesByKey[Compare < 0 ? Left : Right]], true));
				}
				const int32 Size = FTCHARToUTF8(*JsonText(Change)).Length();
				if (Bytes + Size > ResponseBudget)
				{
					bByteLimited = true;
					break;
				}
				Bytes += Size;
				Changes.Add(MakeShared<FJsonValueObject>(Change));
			}
			if (Compare <= 0) ++Left;
			if (Compare >= 0) ++Right;
			++Scanned;
		}
		if (Phase == TEXT("nodes") && Left == Before->Nodes.Num() && Right == After->Nodes.Num())
		{
			Phase = TEXT("edges");
			Left = Right = 0;
		}
		const bool bMore = !bEqual && (Phase == TEXT("nodes") || Left < Before->EdgesByKey.Num() || Right < After->
			EdgesByKey.Num());
		if (bByteLimited && Scanned == 0)
			return FMCPToolResult::Error(
				TEXT("A diff record exceeds the response budget."), TEXT("graph_response_limit"), 413);
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.material.graph-diff/1"));
		Result->SetObjectField(TEXT("before"), Metadata(*Before));
		Result->SetObjectField(TEXT("after"), Metadata(*After));
		Result->SetBoolField(TEXT("projectionEqual"), bEqual);
		Result->SetBoolField(TEXT("liveStateChecked"), false);
		Result->SetArrayField(TEXT("changes"), Changes);
		Result->SetNumberField(TEXT("returnedCount"), Changes.Num());
		Result->SetNumberField(TEXT("scannedCount"), Scanned);
		Result->SetBoolField(TEXT("hasMore"), bMore);
		Result->SetBoolField(TEXT("scanLimited"), bMore && Scanned >= PageScanBudget);
		Result->SetBoolField(TEXT("byteLimited"), bByteLimited);
		Result->SetStringField(
			TEXT("comparisonCoverage"),
			TEXT("capturedNodePropertiesAndTypedEdgesIncludingMasks;notFullAssetOrShaderEquivalence"));
		if (bMore)
		{
			auto C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("before"), BeforeId);
			C->SetStringField(TEXT("after"), AfterId);
			C->SetStringField(TEXT("phase"), Phase);
			C->SetNumberField(TEXT("left"), Left);
			C->SetNumberField(TEXT("right"), Right);
			const FString NextCursor = FBase64::Encode(JsonText(C));
			Result->SetStringField(TEXT("nextCursor"), NextCursor);
			auto ContinuationData = MakeShared<FJsonObject>();
			ContinuationData->SetStringField(TEXT("snapshotId"), BeforeId);
			ContinuationData->SetStringField(TEXT("projectionHash"), Before->ProjectionHash);
			ContinuationData->SetStringField(TEXT("filterHash"), Digest(BeforeId + TEXT("\n") + AfterId));
			ContinuationData->SetNumberField(TEXT("offset"), Phase == TEXT("nodes") ? Left : Right);
			ContinuationData->SetStringField(TEXT("offsetAxis"), Phase == TEXT("nodes") ? TEXT("left") : TEXT("right"));
			ContinuationData->SetStringField(TEXT("phase"), Phase);
			ContinuationData->SetNumberField(TEXT("left"), Left);
			ContinuationData->SetNumberField(TEXT("right"), Right);
			ContinuationData->SetStringField(TEXT("beforeSnapshotId"), BeforeId);
			ContinuationData->SetStringField(TEXT("afterSnapshotId"), AfterId);
			ContinuationData->SetStringField(TEXT("beforeProjectionHash"), Before->ProjectionHash);
			ContinuationData->SetStringField(TEXT("afterProjectionHash"), After->ProjectionHash);
			ContinuationData->SetStringField(TEXT("nextCursor"), NextCursor);
			if (HasTypedIdentity(*Before) && HasTypedIdentity(*After))
				Result->SetObjectField(TEXT("continuation"), ContinuationData);
		}
		return FMCPToolResult::Ok(Result);
	}

	FMCPToolResult ExecutePlan(const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());
		if (!Params.IsValid())
		{
			return FMCPToolResult::Error(TEXT("A plan object is required."), TEXT("invalid_material_graph_plan"), 422);
		}

		FString AssetPath;
		FString SnapshotId;
		FString ExpectedProjectionHash;
		if (!Params->TryGetStringField(TEXT("assetPath"), AssetPath)
			|| !Params->TryGetStringField(TEXT("snapshotId"), SnapshotId)
			|| !Params->TryGetStringField(TEXT("expectedProjectionHash"), ExpectedProjectionHash)
			|| AssetPath.IsEmpty() || SnapshotId.IsEmpty() || ExpectedProjectionHash.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("assetPath, snapshotId and expectedProjectionHash are required."),
				TEXT("invalid_material_graph_plan"), 422);
		}

		const TSharedPtr<const FSnapshot> SourceSnapshot = FindSnapshot(SnapshotId);
		if (!SourceSnapshot)
		{
			return MissingSnapshot();
		}
		if (!SourceSnapshot->PreviewId.IsEmpty()
			|| ExpectedProjectionHash != SourceSnapshot->ProjectionHash)
		{
			return FMCPToolResult::Error(
				TEXT("ExecutePlan accepts only an authored snapshot with its exact projection hash."),
				TEXT("material_graph_snapshot_mismatch"), 409);
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FPackageName::IsValidLongPackageName(PackageName) || AssetPath.Len() > 1024)
		{
			return FMCPToolResult::Error(TEXT("assetPath must be a valid Material or MaterialFunction object path."),
			                             TEXT("invalid_asset_path"), 400);
		}
		const FString ObjectPath = AssetPath.Contains(TEXT("."))
			                           ? AssetPath
			                           : PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		UMaterial* Material = Cast<UMaterial>(Asset);
		UMaterialFunction* Function = Cast<UMaterialFunction>(Asset);
		if (!Material && !Function)
		{
			return FMCPToolResult::Error(
				TEXT("assetPath must resolve to a Material or MaterialFunction."),
				TEXT("material_asset_not_found"), 404);
		}
		if (SourceSnapshot->AssetPath != Asset->GetPathName())
		{
			return FMCPToolResult::Error(
				TEXT("The snapshot belongs to a different material asset."),
				TEXT("material_graph_asset_mismatch"), 409);
		}

		if (Material)
		{
			MCPMaterialInfrastructure::EnsureMaterialGraph(Material);
		}
		UMaterialGraph* Graph = nullptr;
		if (Material)
		{
			Graph = Material->MaterialGraph.Get();
		}
		else if (Function)
		{
			Graph = Function->MaterialGraph;
		}
		if (!Graph)
		{
			return FMCPToolResult::Error(
				TEXT("The material asset has no editor graph projection."),
				TEXT("material_graph_unavailable"), 409);
		}

		FBoundaryWriteValidation BoundaryValidation;
		const bool bHasBoundary = Params->HasField(TEXT("boundaryId"));
		if (bHasBoundary)
		{
			const FMCPToolResult BoundaryResult = ValidateBoundaryWrite(Asset, Params, BoundaryValidation);
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
		}

		TSharedPtr<const FSnapshot> FreshSnapshot;
		if (bHasBoundary)
		{
			FreshSnapshot = BoundaryValidation.FreshSnapshot;
		}
		else
		{
			const FMCPToolResult FreshResult = Capture(
				Asset,
				SourceSnapshot->AssetPath,
				FString(),
				nullptr,
				SourceSnapshot->bIncludeNamedReroutes,
				&FreshSnapshot,
				false);
			if (!FreshResult.bSuccess || !FreshSnapshot)
			{
				return FreshResult.bSuccess
					       ? FMCPToolResult::Error(TEXT("Fresh graph capture returned no snapshot."),
					                               TEXT("material_graph_capture_failed"), 500)
					       : FreshResult;
			}
		}
		if (!FreshSnapshot || FreshSnapshot->ProjectionHash != SourceSnapshot->ProjectionHash)
		{
			return FMCPToolResult::Error(
				TEXT("The material graph changed after the snapshot was captured. Capture a new graph and retry."),
				TEXT("material_graph_stale"), 409);
		}

		const TArray<TSharedPtr<FJsonValue>>* OperationValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("operations"), OperationValues) || !OperationValues
			|| OperationValues->Num() > 128)
		{
			return FMCPToolResult::Error(TEXT("operations must be an array containing at most 128 entries."),
			                             TEXT("invalid_material_graph_plan"), 422);
		}

		TMap<FString, UEdGraphNode*> NodesById;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (Node->IsA<UMaterialGraphNode_Root>())
			{
				NodesById.Add(TEXT("root"), Node);
				NodesById.Add(Node->NodeGuid.ToString(), Node);
			}
			else if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
			{
				if (MaterialNode->MaterialExpression)
				{
					NodesById.Add(MCPMaterialInfrastructure::ExpressionNodeId(MaterialNode->MaterialExpression), Node);
				}
				NodesById.Add(Node->NodeGuid.ToString(), Node);
			}
		}

		const auto ReadNodeId = [](const TSharedPtr<FJsonObject>& Operation, const TCHAR* Key, FString& OutId)
		{
			return Operation.IsValid() && Operation->TryGetStringField(Key, OutId) && !OutId.IsEmpty();
		};
		const auto ReadIndex = [](const TSharedPtr<FJsonObject>& Operation, const TCHAR* Key, int32& OutIndex)
		{
			double Number = 0.0;
			return Operation.IsValid() && Operation->TryGetNumberField(Key, Number)
				&& FMath::IsFinite(Number) && Number >= 0.0 && Number <= 256.0
				&& Number == FMath::FloorToDouble(Number)
				&& (OutIndex = static_cast<int32>(Number), true);
		};
		const auto ReadPoint = [](const TSharedPtr<FJsonObject>& Operation, const TCHAR* Key,
		                          int32& OutX, int32& OutY, bool& bPresent, FString& Error)
		{
			bPresent = Operation.IsValid() && Operation->HasField(Key);
			if (!bPresent)
			{
				return true;
			}

			double X = 0.0;
			double Y = 0.0;
			bool bRead = false;
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Operation->TryGetArrayField(Key, Array) && Array && Array->Num() == 2
				&& (*Array)[0].IsValid() && (*Array)[1].IsValid()
				&& (*Array)[0]->Type == EJson::Number && (*Array)[1]->Type == EJson::Number)
			{
				X = (*Array)[0]->AsNumber();
				Y = (*Array)[1]->AsNumber();
				bRead = true;
			}
			else
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Operation->TryGetObjectField(Key, Object) && Object
					&& (*Object)->TryGetNumberField(TEXT("x"), X)
					&& (*Object)->TryGetNumberField(TEXT("y"), Y))
				{
					bRead = true;
				}
			}

			if (!bRead || !FMath::IsFinite(X) || !FMath::IsFinite(Y)
				|| X < TNumericLimits<int32>::Lowest() || X > TNumericLimits<int32>::Max()
				|| Y < TNumericLimits<int32>::Lowest() || Y > TNumericLimits<int32>::Max()
				|| X != FMath::FloorToDouble(X) || Y != FMath::FloorToDouble(Y))
			{
				Error = FString::Printf(TEXT("%s must be [x,y] or {x,y} with finite integer-range coordinates."), Key);
				return false;
			}

			OutX = FMath::RoundToInt(X);
			OutY = FMath::RoundToInt(Y);
			return true;
		};

		struct FPlanOperation
		{
			FString Name;
			FString NodeId;
			FString ResultId;
			UEdGraphNode* Node = nullptr;
			UEdGraphPin* SourcePin = nullptr;
			UEdGraphPin* TargetPin = nullptr;
			FString SourceNodeId;
			FString TargetNodeId;
			FString SourcePinName;
			FString TargetPinName;
			int32 SourcePinIndex = INDEX_NONE;
			int32 TargetPinIndex = INDEX_NONE;
			int32 X = 0;
			int32 Y = 0;
			bool bHasDescription = false;
			FString Description;
			bool bHasExposeToLibrary = false;
			bool bExposeToLibrary = false;
			bool bHasLibraryCategories = false;
			TArray<FText> LibraryCategories;
			bool bHasDuplicatePosition = false;
			bool bHasDuplicateOffset = false;
			int32 DuplicateOffsetX = 0;
			int32 DuplicateOffsetY = 0;
		};
		TArray<FPlanOperation> Plan;
		Plan.Reserve(OperationValues->Num());
		TMap<FString, int32> DeclaredResultOperations;
		for (int32 OperationIndex = 0; OperationIndex < OperationValues->Num(); ++OperationIndex)
		{
			const TSharedPtr<FJsonValue>& Value = (*OperationValues)[OperationIndex];
			const TSharedPtr<FJsonObject> Operation = Value.IsValid() && Value->Type == EJson::Object
				                                          ? Value->AsObject()
				                                          : nullptr;
			if (!Operation.IsValid())
			{
				continue;
			}
			FString Name;
			FString ResultId;
			if (Operation->TryGetStringField(TEXT("op"), Name) && Name == TEXT("duplicate_node"))
			{
				Operation->TryGetStringField(TEXT("resultId"), ResultId);
				if (ResultId.IsEmpty())
				{
					Operation->TryGetStringField(TEXT("result_id"), ResultId);
				}
			}
			if (ResultId.IsEmpty())
			{
				continue;
			}
			if (DeclaredResultOperations.Contains(ResultId) || NodesById.Contains(ResultId))
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("duplicate_node resultId '%s' is already in use."), *ResultId),
					TEXT("invalid_material_graph_operation"), 422);
			}
			DeclaredResultOperations.Add(ResultId, OperationIndex);
		}
		TMap<FString, FString> ResultAliases;
		const auto RefreshNodesById = [&]()
		{
			NodesById.Reset();
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (Node->IsA<UMaterialGraphNode_Root>())
				{
					NodesById.Add(TEXT("root"), Node);
				}
				else if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
				{
					if (MaterialNode->MaterialExpression)
					{
						NodesById.Add(
							MCPMaterialInfrastructure::ExpressionNodeId(MaterialNode->MaterialExpression), Node);
					}
				}
				NodesById.Add(Node->NodeGuid.ToString(), Node);
			}
			for (const TPair<FString, FString>& Alias : ResultAliases)
			{
				if (UEdGraphNode* const* Node = NodesById.Find(Alias.Value))
				{
					NodesById.Add(Alias.Key, *Node);
				}
			}
		};

		TSet<FString> WritableIds;
		if (bHasBoundary)
		{
			for (const FString& NodeId : BoundaryValidation.Boundary->WritableNodeIds)
			{
				WritableIds.Add(NodeId);
			}
		}

		const auto FindPin = [](const TMap<FString, UEdGraphNode*>& NodeMap, UEdGraphNode* Node,
		                        EEdGraphPinDirection Direction, int32 Index,
		                        const FString& PinName, FString& Error) -> UEdGraphPin*
		{
			if (!Node)
			{
				Error = TEXT("The referenced graph node is unavailable.");
				return nullptr;
			}
			if (!PinName.IsEmpty())
			{
				UEdGraphPin* Pin = Node->FindPin(FName(*PinName), Direction);
				if (Pin)
				{
					return Pin;
				}
				// Captured authored input names and editor projection pin names can
				// differ for MaterialOutput properties. Fall back to the captured
				// stable index below, including the root-node property mapping.
				Error = FString::Printf(
					TEXT("Pin '%s' was not found on node '%s'; falling back to index."),
					*PinName, *Node->NodeGuid.ToString());
			}
			if (Index < 0)
			{
				Error = TEXT("A pin name or non-negative pin index is required.");
				return nullptr;
			}
			int32 SourceIndex = Index;
			if (Direction == EGPD_Input)
			{
				if (const UMaterialGraphNode_Root* Root = Cast<UMaterialGraphNode_Root>(Node))
				{
					SourceIndex = Root->GetSourceIndexForInputIndex(Index);
					if (SourceIndex == INDEX_NONE)
					{
						Error = FString::Printf(TEXT("Material property input index %d is not available."), Index);
						return nullptr;
					}
				}
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == Direction && Pin->SourceIndex == SourceIndex
					&& Pin->PinType.PinCategory != UMaterialGraphSchema::PC_Exec)
				{
					return Pin;
				}
			}
			Error = FString::Printf(
				TEXT("Pin index %d was not found on node '%s'."), Index, *Node->NodeGuid.ToString());
			return nullptr;
		};

		for (int32 OperationIndex = 0; OperationIndex < OperationValues->Num(); ++OperationIndex)
		{
			const TSharedPtr<FJsonValue>& Value = (*OperationValues)[OperationIndex];
			const TSharedPtr<FJsonObject> Operation = Value.IsValid() && Value->Type == EJson::Object
				                                          ? Value->AsObject()
				                                          : nullptr;
			if (!Operation.IsValid())
			{
				return FMCPToolResult::Error(FString::Printf(TEXT("Operation %d must be an object."), OperationIndex),
				                             TEXT("invalid_material_graph_plan"), 422);
			}
			FString Name;
			if (!Operation->TryGetStringField(TEXT("op"), Name)
				|| (Name != TEXT("move_node") && Name != TEXT("connect") && Name != TEXT("disconnect")
					&& Name != TEXT("set_function_metadata") && Name != TEXT("duplicate_node")))
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT(
							"Operation %d has unsupported op; use move_node, connect, disconnect, duplicate_node or set_function_metadata."),
						OperationIndex),
					TEXT("unsupported_material_graph_operation"), 422);
			}

			FPlanOperation Parsed;
			Parsed.Name = Name;
			FString Error;
			if (Name == TEXT("set_function_metadata"))
			{
				if (!Function)
				{
					return FMCPToolResult::Error(
						TEXT("set_function_metadata is only supported for MaterialFunction assets."),
						TEXT("invalid_material_graph_operation"), 422);
				}

				Parsed.bHasDescription = Operation->HasField(TEXT("description"));
				if (Parsed.bHasDescription
					&& !Operation->TryGetStringField(TEXT("description"), Parsed.Description))
				{
					return FMCPToolResult::Error(
						TEXT("set_function_metadata.description must be a string."),
						TEXT("invalid_material_graph_operation"), 422);
				}
				Parsed.bHasExposeToLibrary = Operation->HasField(TEXT("expose_to_library"));
				if (Parsed.bHasExposeToLibrary
					&& !Operation->TryGetBoolField(TEXT("expose_to_library"), Parsed.bExposeToLibrary))
				{
					return FMCPToolResult::Error(
						TEXT("set_function_metadata.expose_to_library must be boolean."),
						TEXT("invalid_material_graph_operation"), 422);
				}
				Parsed.bHasLibraryCategories = Operation->HasField(TEXT("library_categories"));
				if (Parsed.bHasLibraryCategories)
				{
					const TArray<TSharedPtr<FJsonValue>>* Categories = nullptr;
					if (!Operation->TryGetArrayField(TEXT("library_categories"), Categories) || !Categories)
					{
						return FMCPToolResult::Error(
							TEXT("set_function_metadata.library_categories must be an array of strings."),
							TEXT("invalid_material_graph_operation"), 422);
					}
					if (Categories->Num() > 64)
					{
						return FMCPToolResult::Error(
							TEXT("set_function_metadata.library_categories may contain at most 64 entries."),
							TEXT("invalid_material_graph_operation"), 422);
					}
					for (const TSharedPtr<FJsonValue>& CategoryValue : *Categories)
					{
						FString Category;
						if (!CategoryValue.IsValid() || !CategoryValue->TryGetString(Category)
							|| Category.Len() > 256)
						{
							return FMCPToolResult::Error(
								TEXT(
									"set_function_metadata.library_categories entries must be strings of at most 256 characters."),
								TEXT("invalid_material_graph_operation"), 422);
						}
						Parsed.LibraryCategories.Add(FText::FromString(Category));
					}
				}
				if (!Parsed.bHasDescription && !Parsed.bHasExposeToLibrary && !Parsed.bHasLibraryCategories)
				{
					return FMCPToolResult::Error(
						TEXT("set_function_metadata requires description, expose_to_library or library_categories."),
						TEXT("invalid_material_graph_operation"), 422);
				}
			}
			else if (Name == TEXT("move_node"))
			{
				if (!ReadNodeId(Operation, TEXT("nodeId"), Parsed.NodeId))
				{
					ReadNodeId(Operation, TEXT("node_id"), Parsed.NodeId);
				}
				if (Parsed.NodeId.IsEmpty())
				{
					return FMCPToolResult::Error(
						TEXT("move_node requires nodeId."), TEXT("invalid_material_graph_operation"), 422);
				}
				double X = 0.0;
				double Y = 0.0;
				bool bHasX = Operation->TryGetNumberField(TEXT("x"), X);
				bool bHasY = Operation->TryGetNumberField(TEXT("y"), Y);
				if ((!bHasX || !bHasY) && Operation->HasField(TEXT("position")))
				{
					const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
					if (Operation->TryGetArrayField(TEXT("position"), Position) && Position && Position->Num() == 2
						&& (*Position)[0].IsValid() && (*Position)[1].IsValid()
						&& (*Position)[0]->Type == EJson::Number && (*Position)[1]->Type == EJson::Number)
					{
						X = (*Position)[0]->AsNumber();
						Y = (*Position)[1]->AsNumber();
						bHasX = true;
						bHasY = true;
					}
				}
				if (!bHasX || !bHasY || !FMath::IsFinite(X) || !FMath::IsFinite(Y) || X < TNumericLimits<
						int32>::Lowest() || X >
					TNumericLimits<int32>::Max()
					|| Y < TNumericLimits<int32>::Lowest() || Y > TNumericLimits<int32>::Max())
				{
					return FMCPToolResult::Error(
						TEXT("move_node requires finite integer-range x and y."),
						TEXT("invalid_material_graph_operation"), 422);
				}
				Parsed.Node = NodesById.FindRef(Parsed.NodeId);
				const bool bDeferredNode = DeclaredResultOperations.Contains(Parsed.NodeId)
					&& DeclaredResultOperations.FindRef(Parsed.NodeId) < OperationIndex;
				if ((!Parsed.Node && !bDeferredNode) || (Parsed.Node && !Cast<UMaterialGraphNode>(Parsed.Node)))
				{
					return FMCPToolResult::Error(
						TEXT("move_node can target only an authored material expression node."),
						TEXT("invalid_material_graph_operation"), 422);
				}
				Parsed.X = FMath::RoundToInt(X);
				Parsed.Y = FMath::RoundToInt(Y);
			}
			else if (Name == TEXT("duplicate_node"))
			{
				if (!Material)
				{
					return FMCPToolResult::Error(
						TEXT("duplicate_node is supported only for Material assets."),
						TEXT("invalid_material_graph_operation"), 422);
				}
				if (!ReadNodeId(Operation, TEXT("nodeId"), Parsed.NodeId))
				{
					ReadNodeId(Operation, TEXT("node_id"), Parsed.NodeId);
				}
				if (Parsed.NodeId.IsEmpty())
				{
					return FMCPToolResult::Error(
						TEXT("duplicate_node requires nodeId."), TEXT("invalid_material_graph_operation"), 422);
				}
				Parsed.Node = NodesById.FindRef(Parsed.NodeId);
				const UMaterialGraphNode* SourceNode = Cast<UMaterialGraphNode>(Parsed.Node);
				const bool bDeferredSource = DeclaredResultOperations.Contains(Parsed.NodeId)
					&& DeclaredResultOperations.FindRef(Parsed.NodeId) < OperationIndex;
				if ((!SourceNode || !SourceNode->MaterialExpression) && !bDeferredSource)
				{
					return FMCPToolResult::Error(
						TEXT("duplicate_node can target only an authored material expression node."),
						TEXT("invalid_material_graph_operation"), 422);
				}
				if (!ReadPoint(Operation, TEXT("position"), Parsed.X, Parsed.Y,
				               Parsed.bHasDuplicatePosition, Error)
					|| !ReadPoint(Operation, TEXT("offset"), Parsed.DuplicateOffsetX,
					              Parsed.DuplicateOffsetY, Parsed.bHasDuplicateOffset, Error))
				{
					return FMCPToolResult::Error(Error, TEXT("invalid_material_graph_operation"), 422);
				}
				if (Parsed.bHasDuplicatePosition && Parsed.bHasDuplicateOffset)
				{
					return FMCPToolResult::Error(
						TEXT("duplicate_node accepts position or offset, not both."),
						TEXT("invalid_material_graph_operation"), 422);
				}
				Operation->TryGetStringField(TEXT("resultId"), Parsed.ResultId);
				if (Parsed.ResultId.IsEmpty())
				{
					Operation->TryGetStringField(TEXT("result_id"), Parsed.ResultId);
				}
			}
			else
			{
				const bool bHasSourceNode = ReadNodeId(Operation, TEXT("sourceNodeId"), Parsed.SourceNodeId);
				if (!bHasSourceNode && Name == TEXT("connect"))
				{
					return FMCPToolResult::Error(
						TEXT("connect requires sourceNodeId."), TEXT("invalid_material_graph_operation"), 422);
				}
				if (!ReadNodeId(Operation, TEXT("targetNodeId"), Parsed.TargetNodeId))
				{
					return FMCPToolResult::Error(
						TEXT("connect/disconnect requires targetNodeId."), TEXT("invalid_material_graph_operation"),
						422);
				}
				FString SourcePinName;
				FString TargetPinName;
				Operation->TryGetStringField(TEXT("sourcePinName"), SourcePinName);
				Operation->TryGetStringField(TEXT("targetPinName"), TargetPinName);
				if (Name == TEXT("disconnect"))
				{
					Operation->TryGetStringField(TEXT("pinName"), TargetPinName);
				}
				int32 SourceIndex = INDEX_NONE;
				int32 TargetIndex = INDEX_NONE;
				if (!ReadIndex(Operation, TEXT("outputIndex"), SourceIndex))
				{
					ReadIndex(Operation, TEXT("sourcePinIndex"), SourceIndex);
				}
				if (!ReadIndex(Operation, TEXT("inputIndex"), TargetIndex))
				{
					ReadIndex(Operation, TEXT("targetPinIndex"), TargetIndex);
				}
				Parsed.SourcePinName = SourcePinName;
				Parsed.TargetPinName = TargetPinName;
				Parsed.SourcePinIndex = SourceIndex;
				Parsed.TargetPinIndex = TargetIndex;
				if (Name == TEXT("connect") || bHasSourceNode)
				{
					Parsed.SourcePin = FindPin(NodesById, NodesById.FindRef(Parsed.SourceNodeId), EGPD_Output,
					                           SourceIndex,
					                           SourcePinName, Error);
					if (!Parsed.SourcePin)
					{
						return FMCPToolResult::Error(Error, TEXT("invalid_material_graph_operation"), 422);
					}
				}
				Parsed.TargetPin = FindPin(NodesById, NodesById.FindRef(Parsed.TargetNodeId), EGPD_Input, TargetIndex,
				                           TargetPinName, Error);
				if (!Parsed.TargetPin)
				{
					return FMCPToolResult::Error(Error, TEXT("invalid_material_graph_operation"), 422);
				}
				if (Name == TEXT("connect"))
				{
					const UEdGraphSchema* Schema = Graph->GetSchema();
					if (!Schema)
					{
						return FMCPToolResult::Error(
							TEXT("Material graph schema is unavailable."), TEXT("material_graph_unavailable"), 409);
					}
					const FPinConnectionResponse Response = Schema->CanCreateConnection(
						Parsed.SourcePin, Parsed.TargetPin);
					if (Response.Response == CONNECT_RESPONSE_DISALLOW)
					{
						return FMCPToolResult::Error(
							FString::Printf(TEXT("Connection is not valid: %s."), *Response.Message.ToString()),
							TEXT("invalid_material_graph_connection"), 409);
					}
				}
			}

			if (bHasBoundary)
			{
				const auto IsWritable = [&](const FString& Id)
				{
					if (Id.IsEmpty()) return true;
					if (WritableIds.Contains(Id)) return true;
					if (DeclaredResultOperations.Contains(Id)
						&& DeclaredResultOperations.FindRef(Id) < OperationIndex)
					{
						return true;
					}
					if (UEdGraphNode* Node = NodesById.FindRef(Id))
					{
						if (Node->IsA<UMaterialGraphNode_Root>()) return false;
						if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
							return MaterialNode->MaterialExpression
								&& WritableIds.Contains(
									MCPMaterialInfrastructure::ExpressionNodeId(MaterialNode->MaterialExpression));
					}
					return false;
				};
				if (!IsWritable(Parsed.NodeId) || !IsWritable(Parsed.SourceNodeId) || !IsWritable(Parsed.TargetNodeId))
				{
					return FMCPToolResult::Error(
						TEXT("The plan references a node outside the writable boundary."),
						TEXT("material_boundary_node_outside_selection"), 409);
				}
			}
			Plan.Add(MoveTemp(Parsed));
		}

		// Keep rollback state in snapshot identities and value data. Graph and pin
		// objects are editor projections and may be replaced by PostEditChange.
		TMap<FString, FIntPoint> OriginalPositions;
		for (const FPlanOperation& Operation : Plan)
		{
			if (const UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(Operation.Node))
			{
				if (Node->MaterialExpression)
				{
					OriginalPositions.FindOrAdd(
						MCPMaterialInfrastructure::ExpressionNodeId(Node->MaterialExpression),
						FIntPoint(
							Node->MaterialExpression->MaterialExpressionEditorX,
							Node->MaterialExpression->MaterialExpressionEditorY));
				}
			}
		}
		TArray<FString> CreatedNodeIds;
		const bool bHasFunctionMetadataOperation = Function && Plan.ContainsByPredicate(
			[](const FPlanOperation& Operation)
			{
				return Operation.Name == TEXT("set_function_metadata");
			});
		const FString OriginalFunctionDescription = Function ? Function->Description : FString();
		const bool bOriginalFunctionExposeToLibrary = Function && Function->bExposeToLibrary;
		const TArray<FText> OriginalFunctionLibraryCategories = Function
			                                                        ? Function->LibraryCategoriesText
			                                                        : TArray<FText>();
		const auto AreTextArraysEqual = [](const TArray<FText>& Left, const TArray<FText>& Right)
		{
			if (Left.Num() != Right.Num()) return false;
			for (int32 Index = 0; Index < Left.Num(); ++Index)
			{
				if (!Left[Index].EqualTo(Right[Index])) return false;
			}
			return true;
		};

		const bool bDeferCompile = UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		const bool bWasDirty = Asset->GetOutermost() && Asset->GetOutermost()->IsDirty();
		FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Execute Material Graph Plan")));
		Asset->Modify();
		if (!bDeferCompile) Asset->PreEditChange(nullptr);
		Graph->Modify();
		if (bHasFunctionMetadataOperation) Function->Modify();
		for (const FPlanOperation& Operation : Plan)
		{
			if (Operation.Node) Operation.Node->Modify();
			if (const UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(Operation.Node))
			{
				if (Node->MaterialExpression) Node->MaterialExpression->Modify();
			}
			if (Operation.SourcePin && Operation.SourcePin->GetOwningNode())
				Operation.SourcePin->GetOwningNode()->
				          Modify();
			if (Operation.TargetPin && Operation.TargetPin->GetOwningNode())
				Operation.TargetPin->GetOwningNode()->
				          Modify();
		}

		const auto Restore = [&]()
		{
			UMaterialGraph* RestoreGraph = nullptr;
			if (Material)
			{
				MCPMaterialInfrastructure::EnsureMaterialGraph(Material);
				for (const FString& CreatedNodeId : CreatedNodeIds)
				{
					UMaterialExpression* ExpressionToDelete = nullptr;
					for (UMaterialExpression* Candidate : Material->GetExpressions())
					{
						if (Candidate && MCPMaterialInfrastructure::ExpressionNodeId(Candidate) == CreatedNodeId)
						{
							ExpressionToDelete = Candidate;
							break;
						}
					}
					if (ExpressionToDelete)
					{
						UMaterialEditingLibrary::DeleteMaterialExpression(Material, ExpressionToDelete);
					}
				}
				for (const FString& CreatedNodeId : CreatedNodeIds)
				{
					for (UMaterialExpression* Candidate : Material->GetExpressions())
					{
						if (Candidate && MCPMaterialInfrastructure::ExpressionNodeId(Candidate) == CreatedNodeId)
						{
							return false;
						}
					}
				}
				RestoreGraph = Material->MaterialGraph.Get();
			}
			else if (Function)
			{
				RestoreGraph = Function->MaterialGraph;
			}
			if (!RestoreGraph)
			{
				return false;
			}
			// Material graph nodes and pins are editor projections. Rebuild the
			// projection after removing created expressions so the restore map is
			// based on the current authored expressions rather than stale nodes.
			RestoreGraph->RebuildGraph();

			TMap<FString, UEdGraphNode*> RestoreNodesById;
			for (UEdGraphNode* Node : RestoreGraph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				if (Node->IsA<UMaterialGraphNode_Root>())
				{
					RestoreNodesById.Add(TEXT("root"), Node);
				}
				else if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
				{
					if (MaterialNode->MaterialExpression)
					{
						RestoreNodesById.Add(
							MCPMaterialInfrastructure::ExpressionNodeId(MaterialNode->MaterialExpression), Node);
					}
				}
				RestoreNodesById.Add(Node->NodeGuid.ToString(), Node);
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin)
					{
						Pin->BreakAllPinLinks();
					}
				}
			}

			// Restore authored positions before rebuilding links. This keeps the
			// expression coordinates authoritative even if a pin lookup fails.
			for (const auto& Entry : OriginalPositions)
			{
				if (UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(RestoreNodesById.FindRef(Entry.Key)))
				{
					Node->NodePosX = Entry.Value.X;
					Node->NodePosY = Entry.Value.Y;
					if (Node->MaterialExpression)
					{
						Node->MaterialExpression->MaterialExpressionEditorX = Entry.Value.X;
						Node->MaterialExpression->MaterialExpressionEditorY = Entry.Value.Y;
					}
				}
				else
				{
					return false;
				}
			}

			for (const FEdge& Edge : SourceSnapshot->Edges)
			{
				if (Edge.bNamedRerouteReference
					|| !SourceSnapshot->Nodes.IsValidIndex(Edge.Source)
					|| !SourceSnapshot->Nodes.IsValidIndex(Edge.Target))
				{
					continue;
				}
				const FString& SourceNodeId = SourceSnapshot->Nodes[Edge.Source].Id;
				const FString& TargetNodeId = SourceSnapshot->Nodes[Edge.Target].Id;
				FString PinError;
				UEdGraphPin* SourcePin = FindPin(
					RestoreNodesById,
					RestoreNodesById.FindRef(SourceNodeId),
					EGPD_Output,
					Edge.OutputIndex,
					FString(),
					PinError);
				UEdGraphPin* TargetPin = FindPin(
					RestoreNodesById,
					RestoreNodesById.FindRef(TargetNodeId),
					EGPD_Input,
					Edge.InputIndex,
					Edge.InputName,
					PinError);
				if (!SourcePin || !TargetPin)
				{
					return false;
				}
				SourcePin->MakeLinkTo(TargetPin);
				if (!SourcePin->LinkedTo.Contains(TargetPin) || !TargetPin->LinkedTo.Contains(SourcePin))
				{
					return false;
				}
			}

			if (bHasFunctionMetadataOperation && Function)
			{
				Function->Description = OriginalFunctionDescription;
				Function->bExposeToLibrary = bOriginalFunctionExposeToLibrary;
				Function->LibraryCategoriesText = OriginalFunctionLibraryCategories;
			}
			RestoreGraph->LinkMaterialExpressionsFromGraph();
			RestoreGraph->NotifyGraphChanged();
			UEAIIntegration::MaterialEditing::NotifyMaterialSourceEdited(Asset);
			if (bDeferCompile)
			{
				Asset->MarkPackageDirty();
			}
			else
			{
				Asset->PostEditChange();
			}
			TSharedPtr<const FSnapshot> Restored;
			const FMCPToolResult Readback = Capture(Asset, SourceSnapshot->AssetPath, FString(), nullptr,
			                                        SourceSnapshot->bIncludeNamedReroutes, &Restored, false);
			const bool bVerified = Readback.bSuccess && Restored && Restored->ProjectionHash == SourceSnapshot->
				ProjectionHash;
			if (bVerified && Asset->GetOutermost()) Asset->GetOutermost()->SetDirtyFlag(bWasDirty);
			return bVerified;
		};

		const auto ResolveNodeId = [&](const FString& RequestedId) -> UEdGraphNode*
		{
			const FString* ResolvedId = ResultAliases.Find(RequestedId);
			return NodesById.FindRef(ResolvedId ? *ResolvedId : RequestedId);
		};
		const auto ResolveOperationProjection = [&](FPlanOperation& Operation) -> bool
		{
			if (Operation.Name == TEXT("move_node") || Operation.Name == TEXT("duplicate_node"))
			{
				Operation.Node = ResolveNodeId(Operation.NodeId);
				if (!Operation.Node || !Cast<UMaterialGraphNode>(Operation.Node))
				{
					return false;
				}
			}
			if (Operation.Name != TEXT("connect") && Operation.Name != TEXT("disconnect"))
			{
				return true;
			}
			FString Error;
			Operation.SourcePin = nullptr;
			Operation.TargetPin = nullptr;
			if (!Operation.SourceNodeId.IsEmpty())
			{
				Operation.SourcePin = FindPin(
					NodesById, ResolveNodeId(Operation.SourceNodeId), EGPD_Output,
					Operation.SourcePinIndex, Operation.SourcePinName, Error);
				if (!Operation.SourcePin)
				{
					return false;
				}
			}
			Operation.TargetPin = FindPin(
				NodesById, ResolveNodeId(Operation.TargetNodeId), EGPD_Input,
				Operation.TargetPinIndex, Operation.TargetPinName, Error);
			return Operation.TargetPin != nullptr;
		};

		TArray<TSharedPtr<FJsonValue>> OperationResults;
		bool bApplied = true;
		for (int32 Index = 0; Index < Plan.Num(); ++Index)
		{
			FPlanOperation& Operation = Plan[Index];
			bool bOperationSuccess = true;
			FString ProducedNodeId;
			RefreshNodesById();
			bOperationSuccess = ResolveOperationProjection(Operation);
			if (bOperationSuccess && Operation.Name == TEXT("move_node"))
			{
				if (UMaterialGraphNode* Node = Cast<UMaterialGraphNode>(Operation.Node))
				{
					Node->NodePosX = Operation.X;
					Node->NodePosY = Operation.Y;
					if (Node->MaterialExpression)
					{
						Node->MaterialExpression->MaterialExpressionEditorX = Operation.X;
						Node->MaterialExpression->MaterialExpressionEditorY = Operation.Y;
					}
				}
				else bOperationSuccess = false;
			}
			else if (bOperationSuccess && Operation.Name == TEXT("set_function_metadata"))
			{
				if (!Function)
				{
					bOperationSuccess = false;
				}
				else
				{
					if (Operation.bHasDescription) Function->Description = Operation.Description;
					if (Operation.bHasExposeToLibrary) Function->bExposeToLibrary = Operation.bExposeToLibrary;
					if (Operation.bHasLibraryCategories) Function->LibraryCategoriesText = Operation.LibraryCategories;
				}
			}
			else if (bOperationSuccess && Operation.Name == TEXT("duplicate_node"))
			{
				const UMaterialGraphNode* SourceNode = Cast<UMaterialGraphNode>(Operation.Node);
				UMaterialExpression* SourceExpression = SourceNode ? SourceNode->MaterialExpression : nullptr;
				UMaterialExpression* DuplicatedExpression = SourceExpression
					                                            ? UMaterialEditingLibrary::DuplicateMaterialExpression(
						                                            Material, nullptr, SourceExpression)
					                                            : nullptr;
				if (!DuplicatedExpression)
				{
					bOperationSuccess = false;
				}
				else
				{
					int32 PositionX = SourceExpression->MaterialExpressionEditorX + 50;
					int32 PositionY = SourceExpression->MaterialExpressionEditorY + 50;
					if (Operation.bHasDuplicatePosition)
					{
						PositionX = Operation.X;
						PositionY = Operation.Y;
					}
					else if (Operation.bHasDuplicateOffset)
					{
						PositionX = SourceExpression->MaterialExpressionEditorX + Operation.DuplicateOffsetX;
						PositionY = SourceExpression->MaterialExpressionEditorY + Operation.DuplicateOffsetY;
					}
					DuplicatedExpression->MaterialExpressionEditorX = PositionX;
					DuplicatedExpression->MaterialExpressionEditorY = PositionY;
					ProducedNodeId = MCPMaterialInfrastructure::ExpressionNodeId(DuplicatedExpression);
					CreatedNodeIds.Add(ProducedNodeId);
					if (!Operation.ResultId.IsEmpty())
					{
						ResultAliases.Add(Operation.ResultId, ProducedNodeId);
					}
					Graph->RebuildGraph();
					RefreshNodesById();
				}
			}
			else if (bOperationSuccess && Operation.Name == TEXT("connect"))
			{
				bOperationSuccess = UEAIIntegration::Infrastructure::TryCreateConnection(
					Graph->GetSchema(), Operation.SourcePin,
					Operation.TargetPin, bDeferCompile);
				bOperationSuccess = bOperationSuccess
					&& Operation.SourcePin->LinkedTo.Contains(Operation.TargetPin)
					&& Operation.TargetPin->LinkedTo.Contains(Operation.SourcePin);
			}
			else if (bOperationSuccess)
			{
				if (Operation.SourcePin)
				{
					bOperationSuccess = Operation.TargetPin->LinkedTo.Contains(Operation.SourcePin);
					Operation.TargetPin->BreakLinkTo(Operation.SourcePin);
					if (Operation.SourcePin->LinkedTo.Contains(Operation.TargetPin))
						Operation.SourcePin->BreakLinkTo(
							Operation.TargetPin);
				}
				else
				{
					bOperationSuccess = Operation.TargetPin->LinkedTo.Num() > 0;
					Operation.TargetPin->BreakAllPinLinks();
				}
			}
			auto OperationResult = MakeShared<FJsonObject>();
			OperationResult->SetNumberField(TEXT("index"), Index);
			OperationResult->SetStringField(TEXT("op"), Operation.Name);
			OperationResult->SetBoolField(TEXT("success"), bOperationSuccess);
			if (!ProducedNodeId.IsEmpty())
			{
				OperationResult->SetStringField(TEXT("node_id"), ProducedNodeId);
				OperationResult->SetStringField(TEXT("source_node_id"), Operation.NodeId);
			}
			if (Operation.Name == TEXT("set_function_metadata") && bOperationSuccess)
			{
				OperationResult->SetBoolField(TEXT("metadataReadbackPending"), true);
			}
			OperationResults.Add(MakeShared<FJsonValueObject>(OperationResult));
			if (!bOperationSuccess)
			{
				bApplied = false;
				break;
			}
		}

		if (bApplied)
		{
			Graph->LinkMaterialExpressionsFromGraph();
			Graph->NotifyGraphChanged();
			UEAIIntegration::MaterialEditing::NotifyMaterialSourceEdited(Asset);
			if (bDeferCompile)
			{
				Asset->MarkPackageDirty();
			}
			else
			{
				Asset->PostEditChange();
			}
		}

		TSharedPtr<const FSnapshot> AfterSnapshot;
		FMCPToolResult AfterResult = Capture(Asset, SourceSnapshot->AssetPath, FString(), nullptr,
		                                     SourceSnapshot->bIncludeNamedReroutes, &AfterSnapshot, false);
		FString ExpectedAfterHash;
		const bool bHasExpectedAfter = Params->TryGetStringField(TEXT("expectedAfterProjectionHash"), ExpectedAfterHash)
			&& !ExpectedAfterHash.IsEmpty();
		bool bFunctionMetadataVerified = true;
		if (bApplied && bHasFunctionMetadataOperation && Function)
		{
			FString ExpectedDescription = OriginalFunctionDescription;
			bool bExpectedExposeToLibrary = bOriginalFunctionExposeToLibrary;
			TArray<FText> ExpectedLibraryCategories = OriginalFunctionLibraryCategories;
			for (const FPlanOperation& Operation : Plan)
			{
				if (Operation.Name != TEXT("set_function_metadata")) continue;
				if (Operation.bHasDescription) ExpectedDescription = Operation.Description;
				if (Operation.bHasExposeToLibrary) bExpectedExposeToLibrary = Operation.bExposeToLibrary;
				if (Operation.bHasLibraryCategories) ExpectedLibraryCategories = Operation.LibraryCategories;
			}
			bFunctionMetadataVerified = Function->Description == ExpectedDescription
				&& Function->bExposeToLibrary == bExpectedExposeToLibrary
				&& AreTextArraysEqual(Function->LibraryCategoriesText, ExpectedLibraryCategories);
		}
		const bool bPostconditionVerified = bApplied && bFunctionMetadataVerified && AfterResult.bSuccess &&
			AfterSnapshot
			&& (!bHasExpectedAfter || AfterSnapshot->ProjectionHash == ExpectedAfterHash);
		if (!bPostconditionVerified)
		{
			const bool bRollbackVerified = Restore();
			if (bRollbackVerified) Transaction.Cancel();
			auto Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("beforeProjectionHash"), SourceSnapshot->ProjectionHash);
			Error->SetStringField(
				TEXT("afterProjectionHash"), AfterSnapshot ? AfterSnapshot->ProjectionHash : FString());
			Error->SetArrayField(TEXT("operationResults"), OperationResults);
			Error->SetBoolField(TEXT("functionMetadataReadbackVerified"), bFunctionMetadataVerified);
			Error->SetBoolField(TEXT("rollbackAttempted"), true);
			Error->SetBoolField(TEXT("rollbackVerified"), bRollbackVerified);
			Error->SetStringField(
				TEXT("attempt_status"), bRollbackVerified ? TEXT("rolled_back") : TEXT("rollback_failed"));
			Error->SetStringField(TEXT("restore_status"), bRollbackVerified ? TEXT("restored") : TEXT("unverified"));
			FMCPToolResult Failure = FMCPToolResult::Error(
				bRollbackVerified
					? TEXT("Material graph plan failed and was rolled back.")
					: TEXT("Material graph plan failed and rollback could not be verified."),
				bRollbackVerified ? TEXT("material_graph_plan_failed") : TEXT("material_graph_rollback_failed"), 500);
			Failure.Data = Error;
			return Failure;
		}

		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("assetPath"), SourceSnapshot->AssetPath);
		Result->SetStringField(TEXT("snapshotId"), SnapshotId);
		Result->SetStringField(TEXT("beforeProjectionHash"), SourceSnapshot->ProjectionHash);
		Result->SetStringField(TEXT("afterProjectionHash"), AfterSnapshot->ProjectionHash);
		Result->SetArrayField(TEXT("operationResults"), OperationResults);
		Result->SetBoolField(TEXT("postconditionChecked"), true);
		Result->SetBoolField(TEXT("postconditionVerified"), true);
		Result->SetBoolField(TEXT("functionMetadataReadbackVerified"), bFunctionMetadataVerified);
		Result->SetBoolField(TEXT("rollbackAttempted"), false);
		Result->SetBoolField(TEXT("rollbackVerified"), false);
		Result->SetStringField(TEXT("attempt_status"), TEXT("committed"));
		Result->SetStringField(TEXT("restore_status"), TEXT("not_attempted"));
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compileDeferred"), bDeferCompile);
		return FMCPToolResult::Ok(Result);
	}

	FMCPToolResult Release(const FString& SnapshotId)
	{
		check(IsInGameThread());
		const int32 Removed = Snapshots.RemoveAll([&](const auto& Item) { return Item->Id == SnapshotId; });
		const int32 ReleasedBoundaries = BoundaryProofs.RemoveAll(
			[&SnapshotId](const TSharedPtr<const FBoundaryProof>& Item)
			{
				return Item && Item->SnapshotId == SnapshotId;
			});
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("released"), Removed != 0);
		Result->SetNumberField(TEXT("releasedBoundaryCount"), ReleasedBoundaries);
		return FMCPToolResult::Ok(Result);
	}
}

class FTool_IndexMaterialGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.index"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		bool bIncludeNamedReroutes = false;
		if (Params->HasField(TEXT("includeNamedReroutes")) && !Params->TryGetBoolField(
			TEXT("includeNamedReroutes"), bIncludeNamedReroutes))
			return FMCPToolResult::Error(
				TEXT("includeNamedReroutes must be boolean."), TEXT("invalid_graph_query"), 400);
		const FString Path = Params->GetStringField(TEXT("assetPath"));
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		if (!FPackageName::IsValidLongPackageName(Package) || Path.Len() > 1024)
			return FMCPToolResult::Error(
				TEXT("Use a full material or material-function asset path."), TEXT("invalid_asset_path"), 400);
		const FString ObjectPath = Path.Contains(TEXT("."))
			                           ? Path
			                           : Package + TEXT(".") + FPackageName::GetShortName(Package);
		if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
		{
			auto* Asset = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
			if (!Asset || (!Asset->IsA<UMaterial>() && !Asset->IsA<UMaterialFunction>()))
				return
					UEAIIntegration::MaterialQuery::Capture(Asset);
			auto P = MakeShared<FJsonObject>();
			P->Values = Params->Values;
			P->SetStringField(Asset->IsA<UMaterial>() ? TEXT("material") : TEXT("materialFunction"), ObjectPath);
			UEAIIntegration::MaterialEditing::FTarget Target;
			FString Error;
			if (!UEAIIntegration::MaterialEditing::Resolve(P, Target, Error) || !Target.Editor)
				return
					FMCPToolResult::Error(Error, TEXT("invalid_preview_query"), 400);
			return UEAIIntegration::MaterialQuery::Capture(Target.Asset, Target.OriginalAsset->GetPathName(),
			                                               UEAIIntegration::MaterialEditing::PreviewId(Target),
			                                               &Target.Expressions, bIncludeNamedReroutes);
		}
		return UEAIIntegration::MaterialQuery::Capture(LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn),
		                                               FString(), FString(), nullptr, bIncludeNamedReroutes);
	}
};

class FTool_ListMaterialGraphNodes : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.nodes.list"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::ListNodes(Params);
	}
};

class FTool_GetMaterialSubgraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.subgraph.get"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::Subgraph(Params);
	}
};

class FTool_GetMaterialGraphBoundary : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.boundary.get"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::Boundary(Params);
	}
};

class FTool_ResolveMaterialNodeSource : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.node.source.resolve"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::ResolveNodeSource(Params);
	}
};

class FTool_ListMaterialGraphDefinitions : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.definitions.list"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::ListDefinitions(Params);
	}
};

class FTool_DiffMaterialGraphSnapshots : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.snapshots.diff"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::Diff(Params);
	}
};

class FTool_ExecuteMaterialGraphPlan : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.execute_plan"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::ExecutePlan(Params);
	}
};

class FTool_ReleaseMaterialGraphSnapshot : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.graph.snapshot.release"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegration::MaterialQuery::Release(Params->GetStringField(TEXT("snapshotId")));
	}
};

namespace UEAIIntegrationTools
{
	void RegisterMaterialGraphQueryTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_IndexMaterialGraph>());
		Registry.Register(MakeShared<FTool_ListMaterialGraphNodes>());
		Registry.Register(MakeShared<FTool_GetMaterialSubgraph>());
		Registry.Register(MakeShared<FTool_GetMaterialGraphBoundary>());
		Registry.Register(MakeShared<FTool_ResolveMaterialNodeSource>());
		Registry.Register(MakeShared<FTool_ListMaterialGraphDefinitions>());
		Registry.Register(MakeShared<FTool_DiffMaterialGraphSnapshots>());
		Registry.Register(MakeShared<FTool_ExecuteMaterialGraphPlan>());
		Registry.Register(MakeShared<FTool_ReleaseMaterialGraphSnapshot>());
	}
}
