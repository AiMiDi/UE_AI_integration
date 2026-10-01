#include "Infrastructure/MaterialFunctionMutation.h"
#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Infrastructure/MaterialFunctionDependencies.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "Infrastructure/MaterialSharedWriteProtection.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Tools/MCPToolRegistry.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialFunctionInstanceFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "MaterialShared.h"
#include "Engine/Texture.h"
#include "Factories/MaterialFunctionMaterialLayerFactory.h"
#include "Factories/MaterialFunctionMaterialLayerBlendFactory.h"

namespace MCPMaterialInfrastructure
{
	namespace
	{
		FMCPToolResult BadFunctionEdit(const FString& Message)
		{
			return FMCPToolResult::Error(Message, TEXT("invalid_material_function_edit"), 400);
		}

		/**
		 * Validate a function connection with the same value-type rules used by
		 * UMaterialGraphSchema. Material functions store authored links directly
		 * in FExpressionInput, so there is no graph-schema call at the mutation
		 * site. Keep this check before the first Connect() to avoid persisting an
		 * invalid link that only fails during a later compile.
		 */
		FMCPToolResult ValidateFunctionConnectionTypes(
			UMaterialExpression* Source,
			int32 OutputIndex,
			UMaterialExpression* Target,
			int32 InputIndex)
		{
			if (!Source || !Target)
			{
				return FMCPToolResult::Error(
					TEXT("Source or target expression is unavailable."),
					TEXT("material_pin_type_unavailable"),
					409);
			}

			const uint32 InputType = Target->GetInputType(InputIndex);
			const uint32 OutputType = Source->GetOutputType(OutputIndex);
			if (CanConnectMaterialValueTypes(InputType, OutputType))
			{
				return FMCPToolResult::Ok(MakeShared<FJsonObject>());
			}

			TArray<FText> InputDescriptions;
			TArray<FText> OutputDescriptions;
			GetMaterialValueTypeDescriptions(InputType, InputDescriptions);
			GetMaterialValueTypeDescriptions(OutputType, OutputDescriptions);
			FString InputDescription;
			for (const FText& Description : InputDescriptions)
			{
				if (!InputDescription.IsEmpty())
				{
					InputDescription += TEXT(", ");
				}
				InputDescription += Description.ToString();
			}
			FString OutputDescription;
			for (const FText& Description : OutputDescriptions)
			{
				if (!OutputDescription.IsEmpty())
				{
					OutputDescription += TEXT(", ");
				}
				OutputDescription += Description.ToString();
			}
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Material function pin types are incompatible (output=%s, input=%s)."),
					*OutputDescription,
					*InputDescription),
				TEXT("material_pin_type_incompatible"),
				422);
		}

		FMCPToolResult RequireFunctionMutationBoundary(
			UMaterialFunction* Function,
			const TSharedPtr<FJsonObject>& Params,
			UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& OutValidation)
		{
			if (!Params->HasField(TEXT("boundaryId")))
			{
				return FMCPToolResult::Error(
					TEXT("Authored material function graph writes require boundaryId, snapshotId and expectedProjectionHash."),
					TEXT("material_boundary_required_for_mutation"),
					409);
			}
			return UEAIIntegration::MaterialQuery::ValidateBoundaryWrite(Function, Params, OutValidation);
		}

		FMCPToolResult RequireFunctionBoundaryNode(
			const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
			const UMaterialExpression* Expression,
			const TCHAR* Role)
		{
			if (!Validation.Boundary.IsValid())
			{
				return FMCPToolResult::Ok(MakeShared<FJsonObject>());
			}
			const FString NodeId = Expression ? ExpressionNodeId(const_cast<UMaterialExpression*>(Expression)) : FString();
			if (NodeId.IsEmpty())
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("The %s function node has no stable material identity."), Role),
					TEXT("material_boundary_node_identity_missing"),
					409);
			}
			if (!Validation.Boundary->SelectedNodeIds.Contains(NodeId))
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("The %s function node '%s' is outside the selected material boundary."), Role, *NodeId),
					TEXT("material_boundary_node_outside_selection"),
					409);
			}
			if (!Validation.Boundary->WritableNodeIds.Contains(NodeId))
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("The %s function node '%s' is not writable in the selected material boundary."), Role, *NodeId),
					TEXT("material_boundary_node_not_writable"),
					409);
			}
			return FMCPToolResult::Ok(MakeShared<FJsonObject>());
		}

		FMCPToolResult RequireFunctionSharedWrites(
			UMaterialFunction* Function,
			const TArray<UMaterialExpression*>& Expressions,
			const TSharedPtr<FJsonObject>& Params,
			const TSharedRef<FJsonObject>& Result)
		{
			TSet<UMaterialExpression*> Checked;
			TArray<TSharedPtr<FJsonValue>> ProtectedNodes;
			bool bSharedImpactDetected = false;
			bool bSharedImpactConfirmed = false;
			for (UMaterialExpression* Expression : Expressions)
			{
				if (!Expression || Checked.Contains(Expression)) continue;
				Checked.Add(Expression);
				UEAIIntegration::MaterialEditing::FMaterialSharedWriteProof Proof;
				const FMCPToolResult Validation = UEAIIntegration::MaterialEditing::ValidateMaterialExpressionSharedWrite(
					Function, Expression, Params, Proof);
				if (!Validation.bSuccess) return Validation;
				auto NodeProof = MakeShared<FJsonObject>();
				NodeProof->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
				UEAIIntegration::MaterialEditing::DescribeMaterialExpressionSharedWrite(Proof, NodeProof);
				bSharedImpactDetected |= NodeProof->GetBoolField(TEXT("sharedNodeImpactDetected"));
				bSharedImpactConfirmed |= NodeProof->GetBoolField(TEXT("sharedNodeImpactConfirmed"));
				ProtectedNodes.Add(MakeShared<FJsonValueObject>(NodeProof));
			}
			Result->SetArrayField(TEXT("sharedWriteProtectedNodes"), ProtectedNodes);
			Result->SetBoolField(TEXT("sharedNodeImpactDetected"), bSharedImpactDetected);
			Result->SetBoolField(TEXT("sharedNodeImpactConfirmed"), bSharedImpactConfirmed);
			Result->SetBoolField(TEXT("writeBoundaryVerified"), true);
			Result->SetBoolField(TEXT("freshLiveProjectionVerified"), true);
			return FMCPToolResult::Ok(MakeShared<FJsonObject>());
		}

		struct FFunctionConnectionSnapshot
		{
			UMaterialExpression* Owner = nullptr;
			int32 InputIndex = INDEX_NONE;
			FExpressionInput Value;
		};

		void CaptureFunctionConnections(
			UMaterialFunction* Function,
			TArray<FFunctionConnectionSnapshot>& OutConnections)
		{
			OutConnections.Reset();
			if (!Function)
			{
				return;
			}
			for (UMaterialExpression* Expression : Function->GetExpressions())
			{
				if (!Expression)
				{
					continue;
				}
				const TArrayView<FExpressionInput*> Inputs = Expression->GetInputsView();
				for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
				{
					if (!Inputs[InputIndex])
					{
						continue;
					}
					FFunctionConnectionSnapshot& Snapshot = OutConnections.AddDefaulted_GetRef();
					Snapshot.Owner = Expression;
					Snapshot.InputIndex = InputIndex;
					Snapshot.Value = *Inputs[InputIndex];
				}
			}
		}

		bool RestoreFunctionConnections(
			UMaterialFunction* Function,
			const TArray<FFunctionConnectionSnapshot>& Connections)
		{
			if (!Function)
			{
				return false;
			}
			for (UMaterialExpression* Expression : Function->GetExpressions())
			{
				if (!Expression)
				{
					continue;
				}
				for (FExpressionInput* Input : Expression->GetInputsView())
				{
					if (Input)
					{
						*Input = FExpressionInput();
					}
				}
			}
			for (const FFunctionConnectionSnapshot& Snapshot : Connections)
			{
				if (!Snapshot.Owner)
				{
					return false;
				}
				FExpressionInput* Input = Snapshot.Owner->GetInput(Snapshot.InputIndex);
				if (!Input)
				{
					return false;
				}
				*Input = Snapshot.Value;
			}
			return true;
		}

		struct FFunctionExpressionDeleteSnapshot
		{
			UMaterialExpression* Expression = nullptr;
			TArray<UMaterialExpression*> ExpressionOrder;
			TArray<FFunctionConnectionSnapshot> Connections;
		};

		FFunctionExpressionDeleteSnapshot CaptureFunctionExpressionDeleteSnapshot(
			UMaterialFunction* Function,
			UMaterialExpression* Expression)
		{
			FFunctionExpressionDeleteSnapshot Snapshot;
			Snapshot.Expression = Expression;
			if (Function)
			{
				const auto Expressions = Function->GetExpressions();
				Snapshot.ExpressionOrder.Reserve(Expressions.Num());
				for (UMaterialExpression* Current : Expressions)
				{
					if (Current)
					{
						Snapshot.ExpressionOrder.Add(Current);
					}
				}
			}
			CaptureFunctionConnections(Function, Snapshot.Connections);
			return Snapshot;
		}

		bool RestoreFunctionExpressionDeleteSnapshot(
			UMaterialFunction* Function,
			const FFunctionExpressionDeleteSnapshot& Snapshot)
		{
			if (!Function || !Snapshot.Expression || !Snapshot.ExpressionOrder.Contains(Snapshot.Expression))
			{
				return false;
			}
			if (!Function->GetExpressions().Contains(Snapshot.Expression))
			{
				Function->GetExpressionCollection().AddExpression(Snapshot.Expression);
			}
			return RestoreFunctionConnections(Function, Snapshot.Connections);
		}

		FMCPToolResult VerifyFunctionMutationPostcondition(
			UMaterialFunction* Function,
			const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
			const TSharedPtr<FJsonObject>& Params)
		{
			FString Expected;
			if (!Params->TryGetStringField(TEXT("expectedAfterProjectionHash"), Expected) || Expected.IsEmpty())
			{
				return FMCPToolResult::Ok(MakeShared<FJsonObject>());
			}
			TSharedPtr<const UEAIIntegration::MaterialQuery::FSnapshot> Fresh;
			const FMCPToolResult CaptureResult = UEAIIntegration::MaterialQuery::Capture(
				Function,
				Function->GetPathName(),
				FString(),
				nullptr,
				Validation.SourceSnapshot.IsValid() ? Validation.SourceSnapshot->bIncludeNamedReroutes : false,
				&Fresh,
				false);
			if (!CaptureResult.bSuccess || !Fresh)
			{
				return FMCPToolResult::Error(
					TEXT("The material function mutation completed, but its postcondition projection could not be captured."),
					TEXT("material_boundary_postcondition_capture_failed"),
					500);
			}
			if (Fresh->ProjectionHash != Expected)
			{
				return FMCPToolResult::Error(
					TEXT("The material function mutation did not produce the expected projection."),
					TEXT("material_boundary_postcondition_failed"),
					409);
			}
			return FMCPToolResult::Ok(MakeShared<FJsonObject>());
		}

		void FinalizeFunctionRollback(UMaterialFunction* Function, const TSharedPtr<FJsonObject>& Params)
		{
			if (!Function)
			{
				return;
			}
			Function->MarkPackageDirty();
			UEAIIntegration::MaterialEditing::NotifyMaterialSourceEdited(Function);
			if (!UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				Function->PostEditChange();
				UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
				SaveMaterialPackage(Function);
			}
		}

		FMCPToolResult RollbackFunctionMutation(
			UMaterialFunction* Function,
			const TArray<FFunctionConnectionSnapshot>& Connections,
			const TSharedPtr<FJsonObject>& Params,
			const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
			FMCPToolResult Failure)
		{
			const bool bRestored = RestoreFunctionConnections(Function, Connections);
			FinalizeFunctionRollback(Function, Params);
			bool bVerified = bRestored;
			if (bVerified && Validation.SourceSnapshot.IsValid())
			{
				TSharedPtr<const UEAIIntegration::MaterialQuery::FSnapshot> Restored;
				const FMCPToolResult CaptureResult = UEAIIntegration::MaterialQuery::Capture(
					Function,
					Function->GetPathName(),
					FString(),
					nullptr,
					Validation.SourceSnapshot->bIncludeNamedReroutes,
					&Restored,
					false);
				bVerified = CaptureResult.bSuccess && Restored.IsValid()
					&& Restored->ProjectionHash == Validation.SourceSnapshot->ProjectionHash;
			}
			if (!Failure.Data.IsValid())
			{
				Failure.Data = MakeShared<FJsonObject>();
			}
			Failure.Data->SetStringField(TEXT("attempt_status"), TEXT("rolled_back"));
			Failure.Data->SetStringField(TEXT("restore_status"), bVerified ? TEXT("restored") : TEXT("restore_failed"));
			Failure.Data->SetBoolField(TEXT("rollbackVerified"), bVerified);
			return Failure;
		}

		FMCPToolResult RollbackFunctionExpressionDelete(
			UMaterialFunction* Function,
			const FFunctionExpressionDeleteSnapshot& Snapshot,
			const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
			const TSharedPtr<FJsonObject>& Params,
			FMCPToolResult Failure)
		{
			const bool bRestored = RestoreFunctionExpressionDeleteSnapshot(Function, Snapshot);
			FinalizeFunctionRollback(Function, Params);
			bool bVerified = bRestored;
			if (bVerified && Validation.SourceSnapshot.IsValid())
			{
				TSharedPtr<const UEAIIntegration::MaterialQuery::FSnapshot> Restored;
				const FMCPToolResult CaptureResult = UEAIIntegration::MaterialQuery::Capture(
					Function,
					Function->GetPathName(),
					FString(),
					nullptr,
					Validation.SourceSnapshot->bIncludeNamedReroutes,
					&Restored,
					false);
				bVerified = CaptureResult.bSuccess && Restored.IsValid()
					&& Restored->ProjectionHash == Validation.SourceSnapshot->ProjectionHash;
			}
			if (!Failure.Data.IsValid())
			{
				Failure.Data = MakeShared<FJsonObject>();
			}
			Failure.Data->SetStringField(TEXT("attempt_status"), TEXT("rolled_back"));
			Failure.Data->SetStringField(TEXT("restore_status"), bVerified ? TEXT("restored") : TEXT("restore_failed"));
			Failure.Data->SetBoolField(TEXT("rollbackVerified"), bVerified);
			return Failure;
		}

		UMaterialFunctionInterface* LoadMaterialFunctionInterfaceByName(const FString& Name, FString& OutError)
		{
			if (!Name.StartsWith(TEXT("/")))
			{
				OutError = TEXT("parentFunction must be an exact asset path.");
				return nullptr;
			}
			const FString PackageName = FPackageName::ObjectPathToPackageName(Name);
			const FString ObjectPath = Name.Contains(TEXT("."))
				                           ? Name
				                           : PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
			if (UMaterialFunctionInterface* Asset = LoadObject<UMaterialFunctionInterface>(
				nullptr, *ObjectPath, nullptr, LOAD_NoWarn))
			{
				return Asset;
			}
			OutError = FString::Printf(TEXT("MaterialFunctionInterface '%s' not found."), *Name);
			return nullptr;
		}

		UMaterialFunctionInstance* LoadMaterialFunctionInstanceByName(const FString& Name, FString& OutError)
		{
			if (!Name.StartsWith(TEXT("/")))
			{
				OutError = TEXT("functionInstance must be an exact asset path.");
				return nullptr;
			}
			const FString PackageName = FPackageName::ObjectPathToPackageName(Name);
			const FString ObjectPath = Name.Contains(TEXT("."))
				                           ? Name
				                           : PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
			if (UMaterialFunctionInstance* Asset = LoadObject<UMaterialFunctionInstance>(
				nullptr, *ObjectPath, nullptr, LOAD_NoWarn))
			{
				return Asset;
			}
			OutError = FString::Printf(TEXT("MaterialFunctionInstance '%s' not found."), *Name);
			return nullptr;
		}

		bool ReadColorOverride(const TSharedPtr<FJsonValue>& Value, FLinearColor& Out)
		{
			if (Value->Type != EJson::Object) return false;
			const auto O = Value->AsObject();
			double C[4] = {0, 0, 0, 1};
			const TCHAR* Names[4] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
			for (const auto& Pair : O->Values)
			{
				if (Pair.Key != TEXT("r") && Pair.Key != TEXT("g") && Pair.Key != TEXT("b") && Pair.Key != TEXT("a"))
					return false;
			}
			for (int32 I = 0; I < 4; ++I)
			{
				if (O->HasField(Names[I]) && (!O->TryGetNumberField(Names[I], C[I]) || !FMath::IsFinite(C[I]) ||
					FMath::Abs(C[I]) > TNumericLimits<float>::Max()))
					return false;
			}
			Out = FLinearColor(static_cast<float>(C[0]), static_cast<float>(C[1]), static_cast<float>(C[2]),
			                   static_cast<float>(C[3]));
			return true;
		}

		// Reuses the same override-type inference + UpdateParameterSet pattern as
		// content.material.function.instance.create. Existing entries are updated in
		// place (preserving their ExpressionGUID); new entries resolve the GUID from
		// the base function via the base-class template, qualified because
		// UMaterialFunctionInstance::UpdateParameterSet() (non-template) shadows it.
		bool SetFunctionInstanceOverride(UMaterialFunctionInstance* Instance, const FName& ParamName,
		                                 const TSharedPtr<FJsonValue>& Value, FString& OutType, FString& OutError)
		{
			if (Value->Type == EJson::Number)
			{
				double N;
				if (!Value->TryGetNumber(N) || !FMath::IsFinite(N) || FMath::Abs(N) > TNumericLimits<float>::Max())
				{
					OutError = TEXT("Scalar override must be a finite float.");
					return false;
				}
				const float Scalar = static_cast<float>(N);
				for (FScalarParameterValue& Entry : Instance->ScalarParameterValues)
				{
					if (Entry.ParameterInfo.Name == ParamName)
					{
						Entry.ParameterValue = Scalar;
						OutType = TEXT("scalar");
						return true;
					}
				}
				FScalarParameterValue NewEntry;
				NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
				NewEntry.ParameterValue = Scalar;
				if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
					FScalarParameterValue, UMaterialExpressionScalarParameter>(NewEntry))
				{
					OutError = FString::Printf(
						TEXT("Scalar parameter '%s' not found in parent."), *ParamName.ToString());
					return false;
				}
				Instance->ScalarParameterValues.Add(NewEntry);
				OutType = TEXT("scalar");
				return true;
			}
			if (Value->Type == EJson::Object)
			{
				FLinearColor Color;
				if (!ReadColorOverride(Value, Color))
				{
					OutError = TEXT("Vector override must be an object with numeric r/g/b/a.");
					return false;
				}
				for (FVectorParameterValue& Entry : Instance->VectorParameterValues)
				{
					if (Entry.ParameterInfo.Name == ParamName)
					{
						Entry.ParameterValue = Color;
						OutType = TEXT("vector");
						return true;
					}
				}
				FVectorParameterValue NewEntry;
				NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
				NewEntry.ParameterValue = Color;
				if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
					FVectorParameterValue, UMaterialExpressionVectorParameter>(NewEntry))
				{
					OutError = FString::Printf(
						TEXT("Vector parameter '%s' not found in parent."), *ParamName.ToString());
					return false;
				}
				Instance->VectorParameterValues.Add(NewEntry);
				OutType = TEXT("vector");
				return true;
			}
			if (Value->Type == EJson::String)
			{
				FString TexPath;
				if (!Value->TryGetString(TexPath) || !TexPath.StartsWith(TEXT("/")) || TexPath.Len() > 1024)
				{
					OutError = TEXT("Texture override must be an exact asset path.");
					return false;
				}
				auto* Texture = LoadObject<UTexture>(nullptr, *TexPath, nullptr, LOAD_NoWarn);
				if (!Texture)
				{
					OutError = FString::Printf(TEXT("Texture '%s' not found."), *TexPath);
					return false;
				}
				for (FTextureParameterValue& Entry : Instance->TextureParameterValues)
				{
					if (Entry.ParameterInfo.Name == ParamName)
					{
						Entry.ParameterValue = Texture;
						OutType = TEXT("texture");
						return true;
					}
				}
				FTextureParameterValue NewEntry;
				NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
				NewEntry.ParameterValue = Texture;
				if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
					FTextureParameterValue, UMaterialExpressionTextureSampleParameter>(NewEntry))
				{
					OutError = FString::Printf(
						TEXT("Texture parameter '%s' not found in parent."), *ParamName.ToString());
					return false;
				}
				Instance->TextureParameterValues.Add(NewEntry);
				OutType = TEXT("texture");
				return true;
			}
			if (Value->Type == EJson::Boolean)
			{
				bool B;
				Value->TryGetBool(B);
				for (FStaticSwitchParameter& Entry : Instance->StaticSwitchParameterValues)
				{
					if (Entry.ParameterInfo.Name == ParamName)
					{
						Entry.bOverride = true;
						Entry.Value = B;
						OutType = TEXT("staticSwitch");
						return true;
					}
				}
				FStaticSwitchParameter NewEntry;
				NewEntry.ParameterInfo = FMaterialParameterInfo(ParamName);
				NewEntry.Value = B;
				NewEntry.bOverride = true;
				if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
					FStaticSwitchParameter, UMaterialExpressionStaticSwitchParameter>(NewEntry))
				{
					OutError = FString::Printf(
						TEXT("Static switch parameter '%s' not found in parent."), *ParamName.ToString());
					return false;
				}
				Instance->StaticSwitchParameterValues.Add(NewEntry);
				OutType = TEXT("staticSwitch");
				return true;
			}
			OutError = TEXT("Override value must be a number, object, string, or boolean.");
			return false;
		}

		int32 ClearFunctionInstanceOverride(UMaterialFunctionInstance* Instance, const FName& ParamName)
		{
			int32 Removed = 0;
			Removed += Instance->ScalarParameterValues.RemoveAll([&](const FScalarParameterValue& Entry)
			{
				return Entry.ParameterInfo.Name == ParamName;
			});
			Removed += Instance->VectorParameterValues.RemoveAll([&](const FVectorParameterValue& Entry)
			{
				return Entry.ParameterInfo.Name == ParamName;
			});
			Removed += Instance->TextureParameterValues.RemoveAll([&](const FTextureParameterValue& Entry)
			{
				return Entry.ParameterInfo.Name == ParamName;
			});
			Removed += Instance->StaticSwitchParameterValues.RemoveAll([&](const FStaticSwitchParameter& Entry)
			{
				return Entry.ParameterInfo.Name == ParamName;
			});
			return Removed;
		}

		FMCPToolResult FinishFunctionEdit(UMaterialFunction* Function, const TSharedPtr<FJsonObject>& Params,
		                                  const TSharedPtr<FJsonObject>& Result)
		{
			Function->MarkPackageDirty();
			UEAIIntegration::MaterialEditing::NotifyMaterialSourceEdited(Function);
			const bool bDeferred = UEAIIntegration::Workflow::ShouldDeferCompile(Params);
			if (!bDeferred)
			{
				Function->PostEditChange();
				UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
			}
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("materialFunction"), Function->GetPathName());
			Result->SetBoolField(TEXT("saved"), !bDeferred && SaveMaterialPackage(Function));
			return FMCPToolResult::Ok(Result);
		}

		int32 FindInput(UMaterialExpression* Expression, const FString& PinName)
		{
			const auto Names = UMaterialEditingLibrary::GetMaterialExpressionInputNames(Expression);
			for (int32 Index = 0; Index < Names.Num(); ++Index)
			{
				if (Names[Index] == PinName || PinName == FString::Printf(TEXT("index:%d"), Index)
					|| (Names.Num() == 1 && PinName == TEXT("Input")))
					return Index;
			}
			return INDEX_NONE;
		}

		int32 FindOutput(UMaterialExpression* Expression, const FString& PinName)
		{
			const auto& Outputs = Expression->GetOutputs();
			for (int32 Index = 0; Index < Outputs.Num(); ++Index)
			{
				if (PinName == FString::Printf(TEXT("index:%d"), Index)
					|| (!Outputs[Index].OutputName.IsNone() && Outputs[Index].OutputName.ToString() == PinName)
					|| (Index == 0 && (PinName.IsEmpty() || PinName == TEXT("Output"))))
					return Index;
			}
			return INDEX_NONE;
		}

		bool ReadPosition(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32& Out)
		{
			double Value = 0;
			if (Params->HasField(Field) && !Params->TryGetNumberField(Field, Value)) return false;
			if (!FMath::IsFinite(Value) || Value < MIN_int32 || Value > MAX_int32) return false;
			Out = static_cast<int32>(Value);
			return true;
		}

		// Keep function-asset type selection in one place so the handler and its
		// response use the same accepted values and Unreal factory path. The layer
		// factories set EMaterialFunctionUsage as part of construction; selecting only
		// the UClass would leave a layer asset with the regular-function usage.
		bool ResolveMaterialFunctionAssetType(
			const TSharedPtr<FJsonObject>& Params,
			UClass*& OutClass,
			UFactory*& OutFactory,
			FString& OutType,
			FString& OutError)
		{
			OutType = TEXT("MaterialFunction");
			FString RequestedAssetType;
			FString LegacyType;
			const bool bHasAssetType = Params->TryGetStringField(TEXT("assetType"), RequestedAssetType);
			const bool bHasLegacyType = Params->TryGetStringField(TEXT("type"), LegacyType);
			if (bHasAssetType && bHasLegacyType && RequestedAssetType != LegacyType)
			{
				OutError = TEXT("assetType and type must match when both are supplied.");
				return false;
			}
			if (bHasAssetType)
			{
				OutType = RequestedAssetType;
			}
			else if (bHasLegacyType)
			{
				// Monolith calls this field `type`; assetType is the canonical UE_AI
				// spelling, while accepting the alias keeps direct Monolith plans usable.
				OutType = LegacyType;
			}
			if (OutType == TEXT("MaterialFunction"))
			{
				OutClass = UMaterialFunction::StaticClass();
				OutFactory = NewObject<UMaterialFunctionFactoryNew>();
				return true;
			}
			if (OutType == TEXT("MaterialLayer"))
			{
				OutClass = UMaterialFunctionMaterialLayer::StaticClass();
				OutFactory = NewObject<UMaterialFunctionMaterialLayerFactory>();
				return true;
			}
			if (OutType == TEXT("MaterialLayerBlend"))
			{
				OutClass = UMaterialFunctionMaterialLayerBlend::StaticClass();
				OutFactory = NewObject<UMaterialFunctionMaterialLayerBlendFactory>();
				return true;
			}

			OutError = FString::Printf(
				TEXT(
					"Unknown function type '%s'; valid values are MaterialFunction, MaterialLayer, and MaterialLayerBlend."),
				*OutType);
			return false;
		}

		FString DescribeMaterialFunctionAssetType(const UMaterialFunction* Function)
		{
			if (Function && Function->IsA<UMaterialFunctionMaterialLayer>())
			{
				return TEXT("MaterialLayer");
			}
			if (Function && Function->IsA<UMaterialFunctionMaterialLayerBlend>())
			{
				return TEXT("MaterialLayerBlend");
			}
			return TEXT("MaterialFunction");
		}

		bool ReadOptionalBoolAlias(
			const TSharedPtr<FJsonObject>& Params,
			const TCHAR* CanonicalField,
			const TCHAR* LegacyField,
			bool& OutValue,
			bool& bOutPresent,
			FString& OutError)
		{
			const bool bHasCanonical = Params->HasField(CanonicalField);
			const bool bHasLegacy = Params->HasField(LegacyField);
			bool CanonicalValue = false;
			bool LegacyValue = false;
			if (bHasCanonical && (!Params->TryGetBoolField(CanonicalField, CanonicalValue)
				|| Params->Values.FindChecked(CanonicalField)->Type != EJson::Boolean))
			{
				OutError = FString::Printf(TEXT("%s must be a boolean."), CanonicalField);
				return false;
			}
			if (bHasLegacy && (!Params->TryGetBoolField(LegacyField, LegacyValue)
				|| Params->Values.FindChecked(LegacyField)->Type != EJson::Boolean))
			{
				OutError = FString::Printf(TEXT("%s must be a boolean."), LegacyField);
				return false;
			}
			if (bHasCanonical && bHasLegacy && CanonicalValue != LegacyValue)
			{
				OutError = FString::Printf(
					TEXT("%s and %s must match when both are supplied."), CanonicalField, LegacyField);
				return false;
			}
			bOutPresent = bHasCanonical || bHasLegacy;
			OutValue = bHasCanonical ? CanonicalValue : LegacyValue;
			return true;
		}

		bool ReadFunctionLibraryCategories(
			const TSharedPtr<FJsonObject>& Params,
			TArray<FText>& OutCategories,
			FString& OutError)
		{
			const TCHAR* Fields[] = {TEXT("libraryCategories"), TEXT("library_categories")};
			const TArray<TSharedPtr<FJsonValue>>* Arrays[2] = {nullptr, nullptr};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
			{
				if (Params->HasField(Fields[Index])
					&& (!Params->TryGetArrayField(Fields[Index], Arrays[Index]) || !Arrays[Index]
						|| Arrays[Index]->Num() > 64))
				{
					OutError = FString::Printf(TEXT("%s must be an array of at most 64 strings."), Fields[Index]);
					return false;
				}
			}
			if (Arrays[0] && Arrays[1])
			{
				if (Arrays[0]->Num() != Arrays[1]->Num())
				{
					OutError = TEXT("libraryCategories and library_categories must match when both are supplied.");
					return false;
				}
				for (int32 Index = 0; Index < Arrays[0]->Num(); ++Index)
				{
					FString Left;
					FString Right;
					if (!(*Arrays[0])[Index].IsValid() || !(*Arrays[0])[Index]->TryGetString(Left)
						|| !(*Arrays[1])[Index].IsValid() || !(*Arrays[1])[Index]->TryGetString(Right)
						|| Left.Len() > 256 || Right.Len() > 256 || Left != Right)
					{
						OutError = TEXT("libraryCategories and library_categories must contain the same strings.");
						return false;
					}
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Selected = Arrays[0] ? Arrays[0] : Arrays[1];
			if (!Selected)
			{
				return true;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Selected)
			{
				FString Category;
				if (!Value.IsValid() || !Value->TryGetString(Category) || Category.Len() > 256)
				{
					OutError = TEXT("library categories must contain strings of at most 256 characters.");
					return false;
				}
				OutCategories.Add(FText::FromString(Category));
			}
			return true;
		}

		bool ReadFunctionMetadata(
			const TSharedPtr<FJsonObject>& Params,
			FString& OutDescription,
			bool& bOutHasDescription,
			bool& bOutExposeToLibrary,
			bool& bOutHasExposeToLibrary,
			TArray<FText>& OutCategories,
			bool& bOutHasCategories,
			FString& OutError)
		{
			bOutHasDescription = Params->HasField(TEXT("description"));
			if (bOutHasDescription
				&& (!Params->TryGetStringField(TEXT("description"), OutDescription)
					|| OutDescription.Len() > 4096))
			{
				OutError = TEXT("description must be a string of at most 4096 characters.");
				return false;
			}
			if (!ReadOptionalBoolAlias(Params, TEXT("exposeToLibrary"), TEXT("expose_to_library"), bOutExposeToLibrary,
			                           bOutHasExposeToLibrary, OutError))
			{
				return false;
			}
			bOutHasCategories = Params->HasField(TEXT("libraryCategories")) || Params->HasField(
				TEXT("library_categories"));
			if (!ReadFunctionLibraryCategories(Params, OutCategories, OutError))
			{
				return false;
			}
			return true;
		}

		void ApplyFunctionMetadata(
			UMaterialFunction* Function,
			const FString& Description,
			bool bHasDescription,
			bool bExposeToLibrary,
			bool bHasExposeToLibrary,
			const TArray<FText>& Categories,
			bool bHasCategories)
		{
			if (!Function)
			{
				return;
			}
			Function->Modify();
			if (bHasDescription)
			{
				Function->Description = Description;
			}
			if (bHasExposeToLibrary)
			{
				Function->bExposeToLibrary = bExposeToLibrary;
			}
			if (bHasCategories)
			{
				Function->LibraryCategoriesText = Categories;
			}
		}
	}

	FMCPToolResult MutateMaterialFunction(const FString& Capability, const TSharedPtr<FJsonObject>& Params)
	{
		FString FunctionPath, MaterialPath;
		Params->TryGetStringField(TEXT("materialFunction"), FunctionPath);
		Params->TryGetStringField(TEXT("material"), MaterialPath);
		if (FunctionPath.IsEmpty() || !MaterialPath.IsEmpty())
			return BadFunctionEdit(
				TEXT("Specify exactly one materialFunction target."));
		FString Error;
		UMaterialFunction* Function = LoadMaterialFunctionByName(FunctionPath, Error);
		if (!Function) return BadFunctionEdit(Error);
		auto Result = MakeShared<FJsonObject>();
		if (Capability == TEXT("content.material.expression.add"))
		{
			FString ClassName = Params->GetStringField(TEXT("expressionClass"));
			if (ClassName == TEXT("Lerp")) ClassName = TEXT("LinearInterpolate");
			UClass* Class = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
				if (It->GetName() == TEXT("MaterialExpression") + ClassName && It->IsChildOf(
					UMaterialExpression::StaticClass()))
				{
					Class = *It;
					break;
				}
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				|| Class->IsChildOf(UMaterialExpressionComposite::StaticClass()) || Class->IsChildOf(
					UMaterialExpressionPinBase::StaticClass()))
				return BadFunctionEdit(TEXT("Unsupported expression class for direct function editing."));
			int32 X, Y;
			if (!ReadPosition(Params, TEXT("posX"), X) || !ReadPosition(Params, TEXT("posY"), Y))
				return
					BadFunctionEdit(TEXT("Invalid expression coordinates."));
			Function->Modify();
			// Native helper initializes expression/parameter/interface GUIDs and defaults,
			// but does not construct an editor graph or compile the function.
			UMaterialExpression* Expression = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
				Function, Class, X, Y);
			if (!Expression) return BadFunctionEdit(TEXT("Could not create expression."));
			Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
			Result->SetStringField(TEXT("expressionClass"), ClassName);
			return FinishFunctionEdit(Function, Params, Result);
		}
		if (Capability == TEXT("content.material.pin.connect"))
		{
			auto* Source = FindFunctionExpression(Function, Params->GetStringField(TEXT("sourceNodeId")));
			auto* Target = FindFunctionExpression(Function, Params->GetStringField(TEXT("targetNodeId")));
			if (!Source || !Target)
				return BadFunctionEdit(
					TEXT("Source or target expression is not in the scoped function."));
			const int32 OutputIndex = FindOutput(Source, Params->GetStringField(TEXT("sourcePinName")));
			const int32 InputIndex = FindInput(Target, Params->GetStringField(TEXT("targetPinName")));
			if (OutputIndex == INDEX_NONE || InputIndex == INDEX_NONE || !Target->GetInput(InputIndex))
				return
					BadFunctionEdit(TEXT("Source output or target input was not found; use a pin name or index:N."));
			UEAIIntegration::MaterialQuery::FBoundaryWriteValidation BoundaryValidation;
			FMCPToolResult BoundaryResult = RequireFunctionMutationBoundary(Function, Params, BoundaryValidation);
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			BoundaryResult = RequireFunctionBoundaryNode(BoundaryValidation, Source, TEXT("source"));
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			BoundaryResult = RequireFunctionBoundaryNode(BoundaryValidation, Target, TEXT("target"));
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			BoundaryResult = ValidateFunctionConnectionTypes(Source, OutputIndex, Target, InputIndex);
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			// Replacing a target input also changes the old source's fan-out.
			BoundaryResult = RequireFunctionSharedWrites(Function,
				{Source, Target, Target->GetInput(InputIndex)->Expression}, Params, Result);
			if (!BoundaryResult.bSuccess) return BoundaryResult;
			TArray<FFunctionConnectionSnapshot> ConnectionSnapshot;
			CaptureFunctionConnections(Function, ConnectionSnapshot);
			Function->Modify();
			Target->Modify();
			Target->GetInput(InputIndex)->Connect(OutputIndex, Source);
			Result->SetStringField(TEXT("sourceNodeId"), ExpressionNodeId(Source));
			Result->SetStringField(TEXT("targetNodeId"), ExpressionNodeId(Target));
			if (BoundaryValidation.Boundary.IsValid())
			{
				Result->SetStringField(TEXT("boundaryId"), BoundaryValidation.Boundary->BoundaryId);
				Result->SetStringField(TEXT("snapshotId"), BoundaryValidation.SourceSnapshot->Id);
				Result->SetStringField(TEXT("sourceProjectionHash"), BoundaryValidation.SourceSnapshot->ProjectionHash);
				Result->SetStringField(TEXT("freshProjectionHash"), BoundaryValidation.FreshSnapshot->ProjectionHash);
			}
			const FMCPToolResult Postcondition = VerifyFunctionMutationPostcondition(Function, BoundaryValidation, Params);
			if (!Postcondition.bSuccess)
			{
				return RollbackFunctionMutation(Function, ConnectionSnapshot, Params, BoundaryValidation, Postcondition);
			}
			return FinishFunctionEdit(Function, Params, Result);
		}

		auto* Expression = FindFunctionExpression(Function, Params->GetStringField(TEXT("nodeId")));
		if (!Expression) return BadFunctionEdit(TEXT("Expression was not found in the scoped function."));
		Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
		if (Capability == TEXT("content.material.expression.delete"))
		{
			if (Expression->IsA<UMaterialExpressionComposite>() || Expression->IsA<UMaterialExpressionPinBase>())
				return
					BadFunctionEdit(TEXT("Composite interfaces require a dedicated editing operation."));
			UEAIIntegration::MaterialQuery::FBoundaryWriteValidation BoundaryValidation;
			FMCPToolResult BoundaryResult = RequireFunctionMutationBoundary(Function, Params, BoundaryValidation);
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			BoundaryResult = RequireFunctionBoundaryNode(BoundaryValidation, Expression, TEXT("deleted"));
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			BoundaryResult = RequireFunctionSharedWrites(Function, {Expression}, Params, Result);
			if (!BoundaryResult.bSuccess) return BoundaryResult;
			const FFunctionExpressionDeleteSnapshot DeleteSnapshot =
				CaptureFunctionExpressionDeleteSnapshot(Function, Expression);
			if (BoundaryValidation.Boundary.IsValid())
			{
				Result->SetStringField(TEXT("boundaryId"), BoundaryValidation.Boundary->BoundaryId);
				Result->SetStringField(TEXT("snapshotId"), BoundaryValidation.SourceSnapshot->Id);
				Result->SetStringField(TEXT("sourceProjectionHash"), BoundaryValidation.SourceSnapshot->ProjectionHash);
				Result->SetStringField(TEXT("freshProjectionHash"), BoundaryValidation.FreshSnapshot->ProjectionHash);
			}
			Function->Modify();
			Expression->Modify();
			for (UMaterialExpression* Other : Function->GetExpressions())
			{
				if (!Other || Other == Expression) continue;
				for (FExpressionInput* Input : Other->GetInputsView())
					if (Input && Input->Expression == Expression)
					{
						Other->Modify();
						*Input = FExpressionInput();
					}
			}
			Function->GetExpressionCollection().RemoveExpression(Expression);
			// Workflow's transaction/checkpoint owns detached objects until completion.
			// Never garbage-mark a deleted expression before rollback can restore it.
			const FMCPToolResult Postcondition = VerifyFunctionMutationPostcondition(Function, BoundaryValidation, Params);
			if (!Postcondition.bSuccess)
			{
				return RollbackFunctionExpressionDelete(
					Function,
					DeleteSnapshot,
					BoundaryValidation,
					Params,
					Postcondition);
			}
			return FinishFunctionEdit(Function, Params, Result);
		}
		if (Capability == TEXT("content.material.expression.move"))
		{
			int32 X, Y;
			if (!Params->HasField(TEXT("posX")) || !Params->HasField(TEXT("posY")) || !
				ReadPosition(Params, TEXT("posX"), X) || !ReadPosition(Params, TEXT("posY"), Y))
				return BadFunctionEdit(
					TEXT("Missing or invalid coordinates."));
			Function->Modify();
			Expression->Modify();
			Expression->MaterialExpressionEditorX = X;
			Expression->MaterialExpressionEditorY = Y;
			if (Expression->GraphNode)
			{
				Expression->GraphNode->Modify();
				Expression->GraphNode->NodePosX = X;
				Expression->GraphNode->NodePosY = Y;
			}
			return FinishFunctionEdit(Function, Params, Result);
		}
		if (Capability == TEXT("content.material.pin.disconnect"))
		{
			const FString PinName = Params->GetStringField(TEXT("pinName"));
			const int32 InputIndex = FindInput(Expression, PinName);
			const int32 OutputIndex = FindOutput(Expression, PinName);
			if (InputIndex == INDEX_NONE && OutputIndex == INDEX_NONE) return BadFunctionEdit(TEXT("Pin not found."));
			if (InputIndex != INDEX_NONE && OutputIndex != INDEX_NONE)
				return BadFunctionEdit(
					TEXT("Ambiguous input/output pin name; use a unique pin name."));
			UEAIIntegration::MaterialQuery::FBoundaryWriteValidation BoundaryValidation;
			FMCPToolResult BoundaryResult = RequireFunctionMutationBoundary(Function, Params, BoundaryValidation);
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			BoundaryResult = RequireFunctionBoundaryNode(BoundaryValidation, Expression, TEXT("target"));
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			const FExpressionInput* DisconnectedInput = InputIndex != INDEX_NONE ? Expression->GetInput(InputIndex) : nullptr;
			BoundaryResult = RequireFunctionSharedWrites(Function,
				{Expression, DisconnectedInput ? DisconnectedInput->Expression : nullptr}, Params, Result);
			if (!BoundaryResult.bSuccess) return BoundaryResult;
			TArray<FFunctionConnectionSnapshot> ConnectionSnapshot;
			CaptureFunctionConnections(Function, ConnectionSnapshot);
			Function->Modify();
			int32 Removed = 0;
			if (InputIndex != INDEX_NONE)
			{
				FExpressionInput* Input = Expression->GetInput(InputIndex);
				if (Input && Input->Expression)
				{
					Expression->Modify();
					*Input = FExpressionInput();
					++Removed;
				}
			}
			else
				for (UMaterialExpression* Other : Function->GetExpressions())
				{
					if (!Other) continue;
					for (FExpressionInput* Input : Other->GetInputsView())
						if (Input && Input->Expression == Expression && Input->OutputIndex == OutputIndex)
						{
							Other->Modify();
							*Input = FExpressionInput();
							++Removed;
						}
				}
			Result->SetNumberField(TEXT("disconnectedCount"), Removed);
			if (BoundaryValidation.Boundary.IsValid())
			{
				Result->SetStringField(TEXT("boundaryId"), BoundaryValidation.Boundary->BoundaryId);
				Result->SetStringField(TEXT("snapshotId"), BoundaryValidation.SourceSnapshot->Id);
				Result->SetStringField(TEXT("sourceProjectionHash"), BoundaryValidation.SourceSnapshot->ProjectionHash);
				Result->SetStringField(TEXT("freshProjectionHash"), BoundaryValidation.FreshSnapshot->ProjectionHash);
			}
			const FMCPToolResult Postcondition = VerifyFunctionMutationPostcondition(Function, BoundaryValidation, Params);
			if (!Postcondition.bSuccess)
			{
				return RollbackFunctionMutation(Function, ConnectionSnapshot, Params, BoundaryValidation, Postcondition);
			}
			return FinishFunctionEdit(Function, Params, Result);
		}
		return BadFunctionEdit(TEXT("Unsupported material function operation."));
	}
}

class FTool_CreateMaterialFunction : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.create"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString Name = Params->GetStringField(TEXT("name"));
		const FString PackagePath = Params->GetStringField(TEXT("packagePath"));
		if (Name.IsEmpty() || Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")) || !PackagePath.StartsWith(
				TEXT("/Game/"))
			|| !FPackageName::IsValidLongPackageName(PackagePath / Name))
			return MCPMaterialInfrastructure::BadFunctionEdit(
				TEXT("Use a valid asset name and /Game/ package directory."));
		if (FPackageName::DoesPackageExist(PackagePath / Name) || FindPackage(nullptr, *(PackagePath / Name)))
			return
				MCPMaterialInfrastructure::BadFunctionEdit(TEXT("Target package already exists."));
		FString Description;
		bool bHasDescription = false;
		bool bExposeToLibrary = true;
		bool bHasExposeToLibrary = false;
		TArray<FText> LibraryCategories;
		bool bHasLibraryCategories = false;
		FString MetadataError;
		if (!MCPMaterialInfrastructure::ReadFunctionMetadata(
			Params,
			Description,
			bHasDescription,
			bExposeToLibrary,
			bHasExposeToLibrary,
			LibraryCategories,
			bHasLibraryCategories,
			MetadataError))
		{
			return MCPMaterialInfrastructure::BadFunctionEdit(MetadataError);
		}
		UClass* FunctionClass = nullptr;
		UFactory* Factory = nullptr;
		FString FunctionType;
		FString TypeError;
		if (!MCPMaterialInfrastructure::ResolveMaterialFunctionAssetType(
			Params, FunctionClass, Factory, FunctionType, TypeError))
		{
			return MCPMaterialInfrastructure::BadFunctionEdit(TypeError);
		}
		auto& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		auto* Function = Cast<UMaterialFunction>(AssetTools.CreateAsset(Name, PackagePath, FunctionClass, Factory));
		if (!Function) return MCPMaterialInfrastructure::BadFunctionEdit(TEXT("Could not create MaterialFunction."));
		MCPMaterialInfrastructure::ApplyFunctionMetadata(
			Function,
			Description,
			bHasDescription,
			bExposeToLibrary,
			bHasExposeToLibrary,
			LibraryCategories,
			bHasLibraryCategories);
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("path"), Function->GetPathName());
		const FString CreatedType = MCPMaterialInfrastructure::DescribeMaterialFunctionAssetType(Function);
		Result->SetStringField(TEXT("assetType"), CreatedType);
		Result->SetStringField(TEXT("type"), CreatedType);
		Result->SetStringField(TEXT("description"), Function->Description);
		Result->SetBoolField(TEXT("exposeToLibrary"), Function->bExposeToLibrary);
		Result->SetBoolField(TEXT("expose_to_library"), Function->bExposeToLibrary);
		TArray<TSharedPtr<FJsonValue>> CategoryValues;
		for (const FText& Category : Function->LibraryCategoriesText)
		{
			CategoryValues.Add(MakeShared<FJsonValueString>(Category.ToString()));
		}
		Result->SetArrayField(TEXT("libraryCategories"), CategoryValues);
		Result->SetArrayField(TEXT("library_categories"), CategoryValues);
		return MCPMaterialInfrastructure::FinishFunctionEdit(Function, Params, Result);
	}
};

class FTool_CreateMaterialFunctionInstance : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.instance.create"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace MCPMaterialInfrastructure;
		const FString ParentPath = Params->GetStringField(TEXT("parentFunction"));
		const FString Name = Params->GetStringField(TEXT("name"));
		const FString PackagePath = Params->GetStringField(TEXT("packagePath"));
		if (Name.IsEmpty() || Name.Contains(TEXT("/")) || Name.Contains(TEXT(".")) || !PackagePath.StartsWith(
				TEXT("/Game/"))
			|| !FPackageName::IsValidLongPackageName(PackagePath / Name))
			return BadFunctionEdit(TEXT("Use a valid asset name and /Game/ package directory."));
		if (FPackageName::DoesPackageExist(PackagePath / Name) || FindPackage(nullptr, *(PackagePath / Name)))
			return
				BadFunctionEdit(TEXT("Target package already exists."));
		FString Error;
		UMaterialFunctionInterface* Parent = LoadMaterialFunctionInterfaceByName(ParentPath, Error);
		if (!Parent) return BadFunctionEdit(Error);
		// Reuse the AssetTools path from content.material.function.create; the factory
		// sets Parent and caches Base before we add parameter overrides.
		auto* Factory = NewObject<UMaterialFunctionInstanceFactory>();
		Factory->InitialParent = Parent;
		auto& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		auto* Instance = Cast<UMaterialFunctionInstance>(
			AssetTools.CreateAsset(Name, PackagePath, UMaterialFunctionInstance::StaticClass(), Factory));
		if (!Instance) return BadFunctionEdit(TEXT("Could not create MaterialFunctionInstance."));
		int32 Applied = 0;
		TArray<TSharedPtr<FJsonValue>> Errors;
		const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
		if (Params->TryGetArrayField(TEXT("overrides"), Overrides))
		{
			if (Overrides->Num() > 128) return BadFunctionEdit(TEXT("overrides must contain at most 128 entries."));
			for (const auto& Item : *Overrides)
			{
				const auto O = Item->Type == EJson::Object ? Item->AsObject() : nullptr;
				if (!O) return BadFunctionEdit(TEXT("Every override must be an object with parameter and value."));
				FString Parameter;
				O->TryGetStringField(TEXT("parameter"), Parameter);
				const TSharedPtr<FJsonValue>* Value = O->Values.Find(TEXT("value"));
				if (Parameter.IsEmpty() || Parameter.Len() > 256 || !Value)
					return BadFunctionEdit(
						TEXT("Each override requires a nonempty parameter (<=256) and a value."));
				const FName ParamName(*Parameter);
				if ((*Value)->Type == EJson::Number)
				{
					double N;
					if (!(*Value)->TryGetNumber(N) || !FMath::IsFinite(N) || FMath::Abs(N) > TNumericLimits<
						float>::Max())
						return BadFunctionEdit(TEXT("Scalar override must be a finite float."));
					FScalarParameterValue Entry;
					Entry.ParameterInfo = FMaterialParameterInfo(ParamName);
					Entry.ParameterValue = static_cast<float>(N);
					if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
						FScalarParameterValue, UMaterialExpressionScalarParameter>(Entry))
					{
						Errors.Add(MakeShared<FJsonValueString>(
							TEXT("Scalar parameter '") + Parameter + TEXT("' not found in parent.")));
						continue;
					}
					Instance->ScalarParameterValues.Add(Entry);
					++Applied;
				}
				else if ((*Value)->Type == EJson::Object)
				{
					FLinearColor Color;
					if (!ReadColorOverride(*Value, Color))
						return BadFunctionEdit(
							TEXT("Vector override must be an object with numeric r/g/b/a."));
					FVectorParameterValue Entry;
					Entry.ParameterInfo = FMaterialParameterInfo(ParamName);
					Entry.ParameterValue = Color;
					if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
						FVectorParameterValue, UMaterialExpressionVectorParameter>(Entry))
					{
						Errors.Add(MakeShared<FJsonValueString>(
							TEXT("Vector parameter '") + Parameter + TEXT("' not found in parent.")));
						continue;
					}
					Instance->VectorParameterValues.Add(Entry);
					++Applied;
				}
				else if ((*Value)->Type == EJson::String)
				{
					FString TexPath;
					if (!(*Value)->TryGetString(TexPath) || !TexPath.StartsWith(TEXT("/")) || TexPath.Len() > 1024)
						return BadFunctionEdit(TEXT("Texture override must be an exact asset path."));
					auto* Texture = LoadObject<UTexture>(nullptr, *TexPath, nullptr, LOAD_NoWarn);
					if (!Texture)
					{
						Errors.Add(MakeShared<FJsonValueString>(TEXT("Texture '") + TexPath + TEXT("' not found.")));
						continue;
					}
					FTextureParameterValue Entry;
					Entry.ParameterInfo = FMaterialParameterInfo(ParamName);
					Entry.ParameterValue = Texture;
					if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
						FTextureParameterValue, UMaterialExpressionTextureSampleParameter>(Entry))
					{
						Errors.Add(MakeShared<FJsonValueString>(
							TEXT("Texture parameter '") + Parameter + TEXT("' not found in parent.")));
						continue;
					}
					Instance->TextureParameterValues.Add(Entry);
					++Applied;
				}
				else if ((*Value)->Type == EJson::Boolean)
				{
					bool B;
					(*Value)->TryGetBool(B);
					FStaticSwitchParameter Entry;
					Entry.ParameterInfo = FMaterialParameterInfo(ParamName);
					Entry.Value = B;
					Entry.bOverride = true;
					if (!Instance->UMaterialFunctionInterface::UpdateParameterSet<
						FStaticSwitchParameter, UMaterialExpressionStaticSwitchParameter>(Entry))
					{
						Errors.Add(MakeShared<FJsonValueString>(
							TEXT("Static switch parameter '") + Parameter + TEXT("' not found in parent.")));
						continue;
					}
					Instance->StaticSwitchParameterValues.Add(Entry);
					++Applied;
				}
				else return BadFunctionEdit(TEXT("Override value must be a number, object, string, or boolean."));
			}
		}
		Instance->MarkPackageDirty();
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("path"), Instance->GetPathName());
		Result->SetStringField(TEXT("parent"), Parent->GetPathName());
		Result->SetNumberField(TEXT("overridesApplied"), Applied);
		Result->SetBoolField(TEXT("saved"), false);
		if (Errors.Num() > 0) Result->SetArrayField(TEXT("errors"), Errors);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_GetMaterialFunctionInstance : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.instance.get"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace MCPMaterialInfrastructure;
		FString Error;
		auto* Instance = LoadMaterialFunctionInstanceByName(Params->GetStringField(TEXT("functionInstance")), Error);
		if (!Instance) return BadFunctionEdit(Error);
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("functionInstance"), Instance->GetPathName());
		if (Instance->Parent) Result->SetStringField(TEXT("parent"), Instance->Parent->GetPathName());
		else Result->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
		// GetBaseFunction() resolves the concrete base MaterialFunction through any
		// nested MaterialFunctionInstance parents, matching Monolith's get_function_instance_info.
		UMaterialFunction* Base = Instance->GetBaseFunction();
		if (Base) Result->SetStringField(TEXT("base"), Base->GetPathName());
		else Result->SetField(TEXT("base"), MakeShared<FJsonValueNull>());

		TArray<TSharedPtr<FJsonValue>> Scalars, Vectors, Textures, Switches;
		for (const FScalarParameterValue& Entry : Instance->ScalarParameterValues)
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
			O->SetNumberField(TEXT("value"), Entry.ParameterValue);
			O->SetBoolField(TEXT("hasExpressionGuid"), Entry.ExpressionGUID.IsValid());
			Scalars.Add(MakeShared<FJsonValueObject>(O));
		}
		for (const FVectorParameterValue& Entry : Instance->VectorParameterValues)
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
			auto V = MakeShared<FJsonObject>();
			V->SetNumberField(TEXT("r"), Entry.ParameterValue.R);
			V->SetNumberField(TEXT("g"), Entry.ParameterValue.G);
			V->SetNumberField(TEXT("b"), Entry.ParameterValue.B);
			V->SetNumberField(TEXT("a"), Entry.ParameterValue.A);
			O->SetObjectField(TEXT("value"), V);
			O->SetBoolField(TEXT("hasExpressionGuid"), Entry.ExpressionGUID.IsValid());
			Vectors.Add(MakeShared<FJsonValueObject>(O));
		}
		for (const FTextureParameterValue& Entry : Instance->TextureParameterValues)
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
			if (Entry.ParameterValue) O->SetStringField(TEXT("value"), Entry.ParameterValue->GetPathName());
			else O->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
			O->SetBoolField(TEXT("hasExpressionGuid"), Entry.ExpressionGUID.IsValid());
			Textures.Add(MakeShared<FJsonValueObject>(O));
		}
		for (const FStaticSwitchParameter& Entry : Instance->StaticSwitchParameterValues)
		{
			auto O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("name"), Entry.ParameterInfo.Name.ToString());
			O->SetBoolField(TEXT("value"), Entry.Value);
			O->SetBoolField(TEXT("isOverridden"), Entry.bOverride);
			O->SetBoolField(TEXT("hasExpressionGuid"), Entry.ExpressionGUID.IsValid());
			Switches.Add(MakeShared<FJsonValueObject>(O));
		}
		Result->SetArrayField(TEXT("scalarOverrides"), Scalars);
		Result->SetArrayField(TEXT("vectorOverrides"), Vectors);
		Result->SetArrayField(TEXT("textureOverrides"), Textures);
		Result->SetArrayField(TEXT("staticSwitchOverrides"), Switches);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SetMaterialFunctionInstanceParameter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.instance.set_parameter"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace MCPMaterialInfrastructure;
		FString Error;
		auto* Instance = LoadMaterialFunctionInstanceByName(Params->GetStringField(TEXT("functionInstance")), Error);
		if (!Instance) return BadFunctionEdit(Error);
		FString Parameter;
		Params->TryGetStringField(TEXT("parameter"), Parameter);
		if (Parameter.IsEmpty() || Parameter.Len() > 256)
			return BadFunctionEdit(
				TEXT("parameter must contain 1..256 characters."));
		const FName ParamName(*Parameter);
		// Absent value or explicit null clears the override; a present non-null
		// value sets/updates it (type inferred from the JSON value shape).
		const TSharedPtr<FJsonValue>* Value = Params->Values.Find(TEXT("value"));
		const bool bClear = !Value || (*Value)->IsNull();
		Instance->Modify();
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("functionInstance"), Instance->GetPathName());
		Result->SetStringField(TEXT("parameter"), Parameter);
		if (bClear)
		{
			Result->SetBoolField(TEXT("cleared"), true);
			Result->SetNumberField(TEXT("removedCount"), ClearFunctionInstanceOverride(Instance, ParamName));
			Instance->MarkPackageDirty();
			Result->SetBoolField(TEXT("saved"), false);
			return FMCPToolResult::Ok(Result);
		}
		FString Type;
		if (!SetFunctionInstanceOverride(Instance, ParamName, *Value, Type, Error)) return BadFunctionEdit(Error);
		Instance->MarkPackageDirty();
		Result->SetStringField(TEXT("type"), Type);
		Result->SetBoolField(TEXT("cleared"), false);
		Result->SetBoolField(TEXT("saved"), false);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SetMaterialFunctionInterface : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.interface.set"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
			return UEAIIntegration::MaterialEditing::MutatePreviewGraph(GetCapabilityId(), Params);
		using namespace MCPMaterialInfrastructure;
		FString Error;
		auto* Function = LoadMaterialFunctionByName(Params->GetStringField(TEXT("materialFunction")), Error);
		if (!Function) return BadFunctionEdit(Error);
		auto* Expression = FindFunctionExpression(Function, Params->GetStringField(TEXT("nodeId")));
		auto* Input = Cast<UMaterialExpressionFunctionInput>(Expression);
		auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression);
		if (!Input && !Output) return BadFunctionEdit(TEXT("nodeId must identify a FunctionInput or FunctionOutput."));
		FString Name;
		Params->TryGetStringField(TEXT("name"), Name);
		if (Params->HasField(TEXT("name")) && (Name.TrimStartAndEnd().IsEmpty() || Name.Len() > 128))
			return
				BadFunctionEdit(TEXT("Interface name must contain 1..128 characters."));
		const FName NewName = Name.IsEmpty() ? (Input ? Input->InputName : Output->OutputName) : FName(*Name);
		for (UMaterialExpression* Other : Function->GetExpressions())
		{
			if (Other == Expression) continue;
			if ((Input && Cast<UMaterialExpressionFunctionInput>(Other) && CastChecked<
					UMaterialExpressionFunctionInput>(Other)->InputName == NewName)
				|| (Output && Cast<UMaterialExpressionFunctionOutput>(Other) && CastChecked<
					UMaterialExpressionFunctionOutput>(Other)->OutputName == NewName))
				return BadFunctionEdit(TEXT("Duplicate function interface name."));
		}
		FString Type;
		Params->TryGetStringField(TEXT("inputType"), Type);
		int64 TypeValue = Input ? static_cast<int64>(Input->InputType) : 0;
		if (!Type.IsEmpty())
			TypeValue = StaticEnum<EFunctionInputType>()->GetValueByNameString(
				TEXT("FunctionInput_") + Type);
		if (TypeValue < 0 || TypeValue >= FunctionInput_MAX)
			return BadFunctionEdit(
				TEXT("Unsupported function input type."));
		if (Output && (Params->HasField(TEXT("inputType")) || Params->HasField(TEXT("usePreviewValueAsDefault"))))
			return BadFunctionEdit(TEXT("Input-only settings cannot target FunctionOutput."));
		int32 SortPriority;
		if (!ReadPosition(Params, TEXT("sortPriority"), SortPriority))
			return BadFunctionEdit(
				TEXT("Invalid sortPriority."));
		Function->Modify();
		Expression->Modify();
		if (Input)
		{
			Input->InputName = NewName;
			Input->InputType = static_cast<EFunctionInputType>(TypeValue);
			bool bDefault = Input->bUsePreviewValueAsDefault;
			Params->TryGetBoolField(TEXT("usePreviewValueAsDefault"), bDefault);
			Input->bUsePreviewValueAsDefault = bDefault;
			if (Params->HasField(TEXT("sortPriority"))) Input->SortPriority = SortPriority;
		}
		else
		{
			Output->OutputName = NewName;
			if (Params->HasField(TEXT("sortPriority"))) Output->SortPriority = SortPriority;
		}
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
		Result->SetStringField(TEXT("name"), NewName.ToString());
		return FinishFunctionEdit(Function, Params, Result);
	}
};

class FTool_ValidateMaterialFunction : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.validate"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace MCPMaterialInfrastructure;
		FString Error;
		auto* Function = LoadMaterialFunctionByName(Params->GetStringField(TEXT("materialFunction")), Error);
		if (!Function) return BadFunctionEdit(Error);
		UMaterial* ValidationMaterial = nullptr;
		FString ValidationPath;
		if (Params->HasField(TEXT("waitForCompilation")) && !Params->HasField(TEXT("validationMaterial")))
			return BadFunctionEdit(TEXT("waitForCompilation requires validationMaterial."));
		TArray<UMaterialFunctionInterface*> Dependencies;
		if (!UEAIIntegration::MaterialEditing::CollectFunctionDependencies(Function, Dependencies, Error))
			return
				BadFunctionEdit(Error);
		if (Params->TryGetStringField(TEXT("validationMaterial"), ValidationPath))
		{
			ValidationMaterial = LoadMaterialByName(ValidationPath, Error);
			if (!ValidationMaterial) return BadFunctionEdit(Error);
			if (!UEAIIntegration::MaterialEditing::CollectFunctionDependencies(ValidationMaterial, Dependencies, Error))
				return BadFunctionEdit(Error);
			if (!UEAIIntegration::MaterialEditing::ReferencesFunctionFromOutputs(ValidationMaterial, Function, &Error))
				return BadFunctionEdit(
					TEXT("validationMaterial must reference this function from a supported material output path. ") +
					Error);
		}
		TSet<UMaterialExpression*> Members;
		TSet<FName> InputNames, OutputNames;
		TArray<TSharedPtr<FJsonValue>> Errors;
		for (UMaterialExpression* Expression : Function->GetExpressions()) if (Expression) Members.Add(Expression);
		TMap<UMaterialExpression*, int32> Indegree;
		TMap<UMaterialExpression*, TArray<UMaterialExpression*>> Consumers;
		int32 OutputCount = 0;
		for (UMaterialExpression* Expression : Members)
		{
			Indegree.FindOrAdd(Expression);
			if (auto* Input = Cast<UMaterialExpressionFunctionInput>(Expression))
			{
				if (Input->InputName.IsNone() || InputNames.Contains(Input->InputName))
					Errors.Add(
						MakeShared<FJsonValueString>(TEXT("FunctionInput names must be nonempty and unique.")));
				InputNames.Add(Input->InputName);
			}
			if (auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
			{
				++OutputCount;
				if (Output->OutputName.IsNone() || OutputNames.Contains(Output->OutputName) || !Output->A.Expression)
					Errors.Add(MakeShared<FJsonValueString>(
						TEXT("FunctionOutput names must be nonempty/unique and outputs connected.")));
				OutputNames.Add(Output->OutputName);
			}
			for (FExpressionInput* Input : Expression->GetInputsView())
			{
				if (!Input || !Input->Expression) continue;
				if (!Members.Contains(Input->Expression) || !Input->Expression->GetOutputs().
				                                                    IsValidIndex(Input->OutputIndex))
					Errors.Add(
						MakeShared<FJsonValueString>(TEXT("Invalid or external expression connection.")));
				else
				{
					++Indegree.FindOrAdd(Expression);
					Consumers.FindOrAdd(Input->Expression).Add(Expression);
				}
			}
		}
		if (OutputCount == 0) Errors.Add(MakeShared<FJsonValueString>(TEXT("Function needs at least one output.")));
		TArray<UMaterialExpression*> Queue;
		for (const auto& Pair : Indegree) if (Pair.Value == 0) Queue.Add(Pair.Key);
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
			if (const auto* Targets = Consumers.Find(Queue[Index]))
				for (auto* Target : *Targets)
					if (--Indegree.
						FindChecked(Target) == 0)
						Queue.Add(Target);
		if (Queue.Num() != Members.Num()) Errors.Add(MakeShared<FJsonValueString>(TEXT("Function contains a cycle.")));
		const bool bValid = Errors.IsEmpty();
		const bool bShaderCacheRefreshed = bValid && ValidationMaterial &&
			UEAIIntegration::MaterialEditing::PrepareMaterialSourceValidation(ValidationMaterial);
		if (bValid)
		{
#if WITH_DEV_AUTOMATION_TESTS
			UEAIIntegration::Workflow::NotifyMaterialCompileFinalizerForTests();
#endif
			Function->PostEditChange();
			UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		}
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("valid"), bValid);
		Result->SetArrayField(TEXT("errors"), Errors);
		Result->SetNumberField(TEXT("errorCount"), Errors.Num());
		Result->SetStringField(TEXT("validation"), TEXT("structuralAndNativeFunctionUpdate"));
		Result->SetBoolField(TEXT("shaderValidationPerformed"), false);
		Result->SetBoolField(TEXT("dependentMaterialsUpdated"), bValid);
		if (bValid && ValidationMaterial)
		{
			// Use the caller's real graph/input types and material settings. No
			// guessed temporary preview material and no implicit asset save.
			// UpdateMaterialFunction already invalidated every authored dependent,
			// including this reachability-checked host. Reuse that resource setup;
			// another PostEditChange would discard it and trigger duplicate work.
			bool bWait = false;
			Params->TryGetBoolField(TEXT("waitForCompilation"), bWait);
			auto Diagnostics = UEAIIntegration::MaterialEditing::CompleteMaterialValidation(ValidationMaterial, bWait);
			Diagnostics->SetBoolField(TEXT("shaderFileCacheRefreshed"), bShaderCacheRefreshed);
			Result->SetObjectField(TEXT("materialDiagnostics"), Diagnostics);
			Result->SetField(TEXT("valid"), Diagnostics->Values.FindChecked(TEXT("valid")));
			Result->SetBoolField(TEXT("structuralValid"), true);
			Result->SetBoolField(
				TEXT("shaderValidationPerformed"), Diagnostics->GetBoolField(TEXT("shaderValidationPerformed")));
			Result->SetStringField(TEXT("validation"), TEXT("structuralAndExplicitHostMaterial"));
			Result->SetStringField(TEXT("compileState"), Diagnostics->GetStringField(TEXT("compileState")));
			for (const auto& Item : Diagnostics->GetArrayField(TEXT("diagnostics")))
				Errors.Add(
					MakeShared<FJsonValueString>(Item->AsObject()->GetStringField(TEXT("message"))));
			Result->SetArrayField(TEXT("errors"), Errors);
			Result->SetNumberField(TEXT("errorCount"), Errors.Num());
		}
		return FMCPToolResult::Ok(Result);
	}
};

namespace UEAIIntegrationTools
{
	void RegisterMaterialFunctionMutationTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_CreateMaterialFunction>());
		Registry.Register(MakeShared<FTool_CreateMaterialFunctionInstance>());
		Registry.Register(MakeShared<FTool_GetMaterialFunctionInstance>());
		Registry.Register(MakeShared<FTool_SetMaterialFunctionInstanceParameter>());
		Registry.Register(MakeShared<FTool_SetMaterialFunctionInterface>());
		Registry.Register(MakeShared<FTool_ValidateMaterialFunction>());
	}
}
