#include "Infrastructure/MaterialGraphIdentity.h"
// Material Read Tools — list, get, describe, search materials and material functions
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

using namespace MCPMaterialInfrastructure;

namespace
{
struct FMaterialQueryPage
{
	int32 Offset = 0;
	int32 Limit = 50;
};

bool ReadMaterialQueryPage(
	const TSharedPtr<FJsonObject>& Params,
	FMaterialQueryPage& OutPage,
	FString& OutError,
	bool bAllowLegacyMaxResults = false)
{
	const TCHAR* LimitField = bAllowLegacyMaxResults
		&& !Params->HasField(TEXT("limit")) && Params->HasField(TEXT("maxResults"))
		? TEXT("maxResults") : TEXT("limit");
	auto ReadInteger = [&](const TCHAR* Field, int32 Minimum, int32 Maximum, int32& OutValue)
	{
		if (!Params->HasField(Field))
		{
			return true;
		}
		double Value = 0;
		if (!Params->TryGetNumberField(Field, Value) || !FMath::IsFinite(Value)
			|| Value != FMath::FloorToDouble(Value) || Value < Minimum || Value > Maximum)
		{
			OutError = FString::Printf(TEXT("%s must be an integer in [%d, %d]."), Field, Minimum, Maximum);
			return false;
		}
		OutValue = static_cast<int32>(Value);
		return true;
	};
	return ReadInteger(LimitField, 1, 200, OutPage.Limit)
		&& ReadInteger(TEXT("offset"), 0, MAX_int32, OutPage.Offset);
}

void SortMaterialQueryAssets(TArray<FAssetData>& Assets)
{
	Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
	{
		return Left.GetObjectPathString() < Right.GetObjectPathString();
	});
}

bool IsMaterialQueryPageEntry(const FMaterialQueryPage& Page, int64 MatchIndex)
{
	return MatchIndex >= Page.Offset && MatchIndex - Page.Offset < Page.Limit;
}

void AddMaterialQueryPageFields(
	const TSharedRef<FJsonObject>& Result,
	const FMaterialQueryPage& Page,
	int64 Total,
	int32 Count)
{
	const bool bHasMore = static_cast<int64>(Page.Offset) + Count < Total;
	Result->SetNumberField(TEXT("offset"), Page.Offset);
	Result->SetNumberField(TEXT("limit"), Page.Limit);
	Result->SetNumberField(TEXT("total"), static_cast<double>(Total));
	Result->SetNumberField(TEXT("count"), Count);
	Result->SetBoolField(TEXT("hasMore"), bHasMore);
	Result->SetBoolField(TEXT("truncated"), bHasMore);
}
}

// ============================================================
// list_materials
// ============================================================
class FTool_ListMaterials : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.list");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Filter;
		Params->TryGetStringField(TEXT("filter"), Filter);
		FString TypeFilter;
		Params->TryGetStringField(TEXT("type"), TypeFilter);

		const bool bIncludeMaterials = TypeFilter.IsEmpty() || TypeFilter == TEXT("all") || TypeFilter == TEXT("material");
		const bool bIncludeInstances = TypeFilter.IsEmpty() || TypeFilter == TEXT("all") || TypeFilter == TEXT("instance");
		if (!bIncludeMaterials && !bIncludeInstances)
		{
			return FMCPToolResult::Error(TEXT("type must be material, instance, or all."), TEXT("invalid_request"), 400);
		}
		FMaterialQueryPage Page;
		FString PageError;
		if (!ReadMaterialQueryPage(Params, Page, PageError))
		{
			return FMCPToolResult::Error(PageError, TEXT("invalid_request"), 400);
		}

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> Assets;
		if (bIncludeMaterials)
		{
			Registry.GetAssetsByClass(UMaterial::StaticClass()->GetClassPathName(), Assets, false);
		}
		if (bIncludeInstances)
		{
			TArray<FAssetData> MIAssets;
			Registry.GetAssetsByClass(UMaterialInstanceConstant::StaticClass()->GetClassPathName(), MIAssets, false);
			Assets.Append(MoveTemp(MIAssets));
		}
		SortMaterialQueryAssets(Assets);

		TArray<TSharedPtr<FJsonValue>> Entries;
		int64 Total = 0;
		for (const FAssetData& Asset : Assets)
		{
			const FString Name = Asset.AssetName.ToString();
			const FString Path = Asset.PackageName.ToString();
			if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase) && !Path.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (IsMaterialQueryPageEntry(Page, Total++))
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Name);
				Entry->SetStringField(TEXT("path"), Path);
				Entry->SetStringField(TEXT("type"), Asset.AssetClassPath == UMaterial::StaticClass()->GetClassPathName()
					? TEXT("Material") : TEXT("MaterialInstance"));
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		AddMaterialQueryPageFields(Result, Page, Total, Entries.Num());
		Result->SetArrayField(TEXT("materials"), Entries);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// get_material
// ============================================================
class FTool_GetMaterial : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: name"));

		// Try as UMaterial
		FString LoadError;
		UMaterial* Material = LoadMaterialByName(Name, LoadError);
		if (Material)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Material->GetName());
			Result->SetStringField(TEXT("path"), Material->GetPathName());
			Result->SetStringField(TEXT("type"), TEXT("Material"));

			if (const UEnum* DomainEnum = StaticEnum<EMaterialDomain>())
				Result->SetStringField(TEXT("domain"), DomainEnum->GetNameStringByValue((int64)Material->MaterialDomain));
			if (const UEnum* BlendEnum = StaticEnum<EBlendMode>())
				Result->SetStringField(TEXT("blendMode"), BlendEnum->GetNameStringByValue((int64)Material->BlendMode));

			TArray<TSharedPtr<FJsonValue>> ShadingModels;
			FMaterialShadingModelField SMField = Material->GetShadingModels();
			if (const UEnum* SMEnum = StaticEnum<EMaterialShadingModel>())
			{
				for (int32 i = 0; i < SMEnum->NumEnums() - 1; ++i)
				{
					EMaterialShadingModel SM = (EMaterialShadingModel)SMEnum->GetValueByIndex(i);
					if (SMField.HasShadingModel(SM))
						ShadingModels.Add(MakeShared<FJsonValueString>(SMEnum->GetNameStringByIndex(i)));
				}
			}
			Result->SetArrayField(TEXT("shadingModels"), ShadingModels);
			Result->SetBoolField(TEXT("twoSided"), Material->IsTwoSided());

			auto Expressions = Material->GetExpressions();
			Result->SetNumberField(TEXT("expressionCount"), Expressions.Num());

			// Parameters
			TArray<TSharedPtr<FJsonValue>> Parameters;
			for (UMaterialExpression* Expr : Expressions)
			{
				if (!Expr) continue;
				TSharedRef<FJsonObject> ParamObj = MakeShared<FJsonObject>();
				bool bIsParam = false;

				if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
				{
					bIsParam = true;
					ParamObj->SetStringField(TEXT("name"), SP->ParameterName.ToString());
					ParamObj->SetStringField(TEXT("type"), TEXT("Scalar"));
					ParamObj->SetStringField(TEXT("group"), SP->Group.ToString());
					ParamObj->SetNumberField(TEXT("defaultValue"), SP->DefaultValue);
				}
				else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
				{
					bIsParam = true;
					ParamObj->SetStringField(TEXT("name"), VP->ParameterName.ToString());
					ParamObj->SetStringField(TEXT("type"), TEXT("Vector"));
					ParamObj->SetStringField(TEXT("group"), VP->Group.ToString());
					TSharedRef<FJsonObject> DefVal = MakeShared<FJsonObject>();
					DefVal->SetNumberField(TEXT("r"), VP->DefaultValue.R);
					DefVal->SetNumberField(TEXT("g"), VP->DefaultValue.G);
					DefVal->SetNumberField(TEXT("b"), VP->DefaultValue.B);
					DefVal->SetNumberField(TEXT("a"), VP->DefaultValue.A);
					ParamObj->SetObjectField(TEXT("defaultValue"), DefVal);
				}
				else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
				{
					bIsParam = true;
					ParamObj->SetStringField(TEXT("name"), TP->ParameterName.ToString());
					ParamObj->SetStringField(TEXT("type"), TEXT("Texture"));
					ParamObj->SetStringField(TEXT("group"), TP->Group.ToString());
					if (TP->Texture) ParamObj->SetStringField(TEXT("defaultValue"), TP->Texture->GetPathName());
				}
				else if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				{
					bIsParam = true;
					ParamObj->SetStringField(TEXT("name"), SSP->ParameterName.ToString());
					ParamObj->SetStringField(TEXT("type"), TEXT("StaticSwitch"));
					ParamObj->SetStringField(TEXT("group"), SSP->Group.ToString());
					ParamObj->SetBoolField(TEXT("defaultValue"), SSP->DefaultValue);
				}

				if (bIsParam)
					Parameters.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
			Result->SetArrayField(TEXT("parameters"), Parameters);

			// Referenced textures
			TArray<TSharedPtr<FJsonValue>> ReferencedTextures;
			for (const TObjectPtr<UObject>& TexObj : Material->GetReferencedTextures())
			{
				if (TexObj)
					ReferencedTextures.Add(MakeShared<FJsonValueString>(TexObj->GetPathName()));
			}
			Result->SetArrayField(TEXT("referencedTextures"), ReferencedTextures);

			int32 GraphNodeCount = Material->MaterialGraph ? Material->MaterialGraph->Nodes.Num() : 0;
			Result->SetNumberField(TEXT("graphNodeCount"), GraphNodeCount);

			// Usage flags
			TSharedRef<FJsonObject> UsageFlags = MakeShared<FJsonObject>();
			UsageFlags->SetBoolField(TEXT("bUsedWithSkeletalMesh"), Material->bUsedWithSkeletalMesh != 0);
			UsageFlags->SetBoolField(TEXT("bUsedWithMorphTargets"), Material->bUsedWithMorphTargets != 0);
			UsageFlags->SetBoolField(TEXT("bUsedWithNiagaraSprites"), Material->bUsedWithNiagaraSprites != 0);
			UsageFlags->SetBoolField(TEXT("bUsedWithParticleSprites"), Material->bUsedWithParticleSprites != 0);
			UsageFlags->SetBoolField(TEXT("bUsedWithStaticLighting"), Material->bUsedWithStaticLighting != 0);
			Result->SetObjectField(TEXT("usageFlags"), UsageFlags);

			Result->SetNumberField(TEXT("opacityMaskClipValue"), Material->OpacityMaskClipValue);
			Result->SetBoolField(TEXT("ditheredLODTransition"), Material->DitheredLODTransition != 0);

			int32 TextureSampleCount = 0;
			for (UMaterialExpression* Expr : Expressions)
			{
				if (Expr && Expr->IsA<UMaterialExpressionTextureSample>()) TextureSampleCount++;
			}
			Result->SetNumberField(TEXT("textureSampleCount"), TextureSampleCount);

			return FMCPToolResult::Ok(Result);
		}

		// Try as MaterialInstance
		FString MILoadError;
		UMaterialInstanceConstant* MI = LoadMaterialInstanceByName(Name, MILoadError);
		if (MI)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), MI->GetName());
			Result->SetStringField(TEXT("path"), MI->GetPathName());
			Result->SetStringField(TEXT("type"), TEXT("MaterialInstance"));

			if (MI->Parent)
			{
				Result->SetStringField(TEXT("parent"), MI->Parent->GetName());
				Result->SetStringField(TEXT("parentPath"), MI->Parent->GetPathName());
			}

			TArray<TSharedPtr<FJsonValue>> OverriddenParams;
			for (const FScalarParameterValue& Param : MI->ScalarParameterValues)
			{
				TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
				PObj->SetStringField(TEXT("type"), TEXT("Scalar"));
				PObj->SetNumberField(TEXT("value"), Param.ParameterValue);
				OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
			}
			for (const FVectorParameterValue& Param : MI->VectorParameterValues)
			{
				TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
				PObj->SetStringField(TEXT("type"), TEXT("Vector"));
				TSharedRef<FJsonObject> Val = MakeShared<FJsonObject>();
				Val->SetNumberField(TEXT("r"), Param.ParameterValue.R);
				Val->SetNumberField(TEXT("g"), Param.ParameterValue.G);
				Val->SetNumberField(TEXT("b"), Param.ParameterValue.B);
				Val->SetNumberField(TEXT("a"), Param.ParameterValue.A);
				PObj->SetObjectField(TEXT("value"), Val);
				OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
			}
			for (const FTextureParameterValue& Param : MI->TextureParameterValues)
			{
				TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
				PObj->SetStringField(TEXT("type"), TEXT("Texture"));
				PObj->SetStringField(TEXT("value"), Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
				OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
			}
			const FStaticParameterSet StaticParameters = MI->GetStaticParameters();
			for (const FStaticSwitchParameter& Param : StaticParameters.StaticSwitchParameters)
			{
				TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
				PObj->SetStringField(TEXT("type"), TEXT("StaticSwitch"));
				PObj->SetBoolField(TEXT("value"), Param.Value);
				PObj->SetBoolField(TEXT("overridden"), Param.bOverride);
				OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
			}
			Result->SetArrayField(TEXT("overriddenParameters"), OverriddenParams);
			return FMCPToolResult::Ok(Result);
		}

		return FMCPToolResult::Error(FString::Printf(TEXT("Material or MaterialInstance '%s' not found. Use list_materials to see available assets."), *Name));
	}
};

// ============================================================
// get_material_graph
// ============================================================
class FTool_GetMaterialGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.graph.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: name"));

		FString LoadError;
		UMaterial* Material = LoadMaterialByName(Name, LoadError);
		if (!Material) return FMCPToolResult::Error(LoadError);

		EnsureMaterialGraph(Material);
		if (!Material->MaterialGraph)
			return FMCPToolResult::Error(TEXT("Could not build MaterialGraph for this material"));

		TSharedPtr<FJsonObject> GraphJson = MCPHelpers::SerializeGraph(Material->MaterialGraph);
		if (!GraphJson.IsValid())
			return FMCPToolResult::Error(TEXT("Failed to serialize material graph"));

		GraphJson->SetStringField(TEXT("material"), Material->GetName());
		GraphJson->SetStringField(TEXT("materialPath"), Material->GetPathName());
		return FMCPToolResult::Ok(GraphJson.ToSharedRef());
	}
};

// ============================================================
// describe_material
// ============================================================
class FTool_DescribeMaterial : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.describe");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString MaterialName = Params->GetStringField(TEXT("material"));
		if (MaterialName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: material"));

		FString LoadError;
		UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
		if (!Material) return FMCPToolResult::Error(LoadError);

		EnsureMaterialGraph(Material);
		if (!Material->MaterialGraph)
			return FMCPToolResult::Error(TEXT("Could not build MaterialGraph for this material"));

		// Recursive pin tracer
		TFunction<FString(UEdGraphPin*, int32)> TracePin = [&TracePin](UEdGraphPin* Pin, int32 Depth) -> FString
		{
			if (!Pin || Depth > 10) return TEXT("(unknown)");
			if (Pin->LinkedTo.Num() == 0)
			{
				if (!Pin->DefaultValue.IsEmpty())
					return FString::Printf(TEXT("(default: %s)"), *Pin->DefaultValue);
				return TEXT("(unconnected)");
			}

			TArray<FString> Sources;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
				UEdGraphNode* SourceNode = LinkedPin->GetOwningNode();
				FString NodeDesc;

				if (UMaterialGraphNode* MatNode = Cast<UMaterialGraphNode>(SourceNode))
				{
					UMaterialExpression* Expr = MatNode->MaterialExpression;
					if (!Expr)
						NodeDesc = TEXT("(null expression)");
					else if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
						NodeDesc = FString::Printf(TEXT("ScalarParam \"%s\" (default: %.4f)"), *SP->ParameterName.ToString(), SP->DefaultValue);
					else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
						NodeDesc = FString::Printf(TEXT("VectorParam \"%s\" (default: R=%.2f G=%.2f B=%.2f A=%.2f)"),
							*VP->ParameterName.ToString(), VP->DefaultValue.R, VP->DefaultValue.G, VP->DefaultValue.B, VP->DefaultValue.A);
					else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
						NodeDesc = FString::Printf(TEXT("TextureParam \"%s\" (%s)"), *TP->ParameterName.ToString(), TP->Texture ? *TP->Texture->GetName() : TEXT("None"));
					else if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
						NodeDesc = FString::Printf(TEXT("StaticSwitchParam \"%s\" (default: %s)"), *SSP->ParameterName.ToString(), SSP->DefaultValue ? TEXT("true") : TEXT("false"));
					else if (auto* SC = Cast<UMaterialExpressionConstant>(Expr))
						NodeDesc = FString::Printf(TEXT("Constant(%.4f)"), SC->R);
					else if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
						NodeDesc = FString::Printf(TEXT("Constant3(R=%.2f G=%.2f B=%.2f)"), C3->Constant.R, C3->Constant.G, C3->Constant.B);
					else if (auto* C4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
						NodeDesc = FString::Printf(TEXT("Constant4(R=%.2f G=%.2f B=%.2f A=%.2f)"), C4->Constant.R, C4->Constant.G, C4->Constant.B, C4->Constant.A);
					else if (auto* TS = Cast<UMaterialExpressionTextureSample>(Expr))
						NodeDesc = FString::Printf(TEXT("TextureSample(%s)"), TS->Texture ? *TS->Texture->GetName() : TEXT("None"));
					else if (auto* MFC = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
						NodeDesc = FString::Printf(TEXT("FunctionCall(%s)"), MFC->MaterialFunction ? *MFC->MaterialFunction->GetName() : TEXT("None"));
					else
						NodeDesc = Expr->GetClass()->GetName();

					TArray<FString> InputDescs;
					for (UEdGraphPin* InputPin : SourceNode->Pins)
					{
						if (!InputPin || InputPin->Direction != EGPD_Input || InputPin->LinkedTo.Num() == 0) continue;
						InputDescs.Add(TracePin(InputPin, Depth + 1));
					}
					if (InputDescs.Num() > 0)
						NodeDesc += TEXT(" <- (") + FString::Join(InputDescs, TEXT(", ")) + TEXT(")");
				}
				else
				{
					NodeDesc = SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				}
				Sources.Add(NodeDesc);
			}
			return Sources.Num() == 1 ? Sources[0] : TEXT("(") + FString::Join(Sources, TEXT(", ")) + TEXT(")");
		};

		// Find root node
		UMaterialGraphNode_Root* RootNode = nullptr;
		for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
		{
			RootNode = Cast<UMaterialGraphNode_Root>(Node);
			if (RootNode) break;
		}
		if (!RootNode)
			return FMCPToolResult::Error(TEXT("Could not find root node in material graph"));

		TArray<TSharedPtr<FJsonValue>> InputDescriptions;
		FString TextDesc;

		for (UEdGraphPin* Pin : RootNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			FString PinName = Pin->PinName.ToString();
			FString Description = Pin->LinkedTo.Num() == 0 ? TEXT("(unconnected)") : TracePin(Pin, 0);

			TSharedRef<FJsonObject> InputObj = MakeShared<FJsonObject>();
			InputObj->SetStringField(TEXT("input"), PinName);
			InputObj->SetStringField(TEXT("chain"), Description);
			InputObj->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);
			InputDescriptions.Add(MakeShared<FJsonValueObject>(InputObj));

			if (Pin->LinkedTo.Num() > 0)
				TextDesc += FString::Printf(TEXT("%s <- %s\n"), *PinName, *Description);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("materialPath"), Material->GetPathName());
		Result->SetArrayField(TEXT("inputs"), InputDescriptions);
		if (!TextDesc.IsEmpty())
			Result->SetStringField(TEXT("description"), TextDesc);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// search_materials
// ============================================================
class FTool_SearchMaterials : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.search");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Query = Params->GetStringField(TEXT("query"));
		if (Query.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: query"));

		FMaterialQueryPage Page;
		FString PageError;
		if (!ReadMaterialQueryPage(Params, Page, PageError, true))
		{
			return FMCPToolResult::Error(PageError, TEXT("invalid_request"), 400);
		}

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> MatAssets;
		Registry.GetAssetsByClass(UMaterial::StaticClass()->GetClassPathName(), MatAssets, false);
		SortMaterialQueryAssets(MatAssets);

		TArray<TSharedPtr<FJsonValue>> Results;
		int64 Total = 0;
		int32 UnavailableAssets = 0;

		for (const FAssetData& Asset : MatAssets)
		{
			const FString MatName = Asset.AssetName.ToString();
			const bool bNameMatch = MatName.Contains(Query, ESearchCase::IgnoreCase);

			if (bNameMatch && IsMaterialQueryPageEntry(Page, Total++))
			{
				TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("material"), MatName);
				R->SetStringField(TEXT("materialPath"), Asset.PackageName.ToString());
				R->SetStringField(TEXT("matchType"), TEXT("materialName"));
				Results.Add(MakeShared<FJsonValueObject>(R));
			}

			UMaterial* Material = Cast<UMaterial>(Asset.GetAsset());
			if (!Material)
			{
				++UnavailableAssets;
				continue;
			}

			// Scan authored expressions directly. Querying does not build an editor
			// graph or request compilation, and only returned rows allocate JSON.
			TArray<UMaterialExpression*> Expressions;
			for (UMaterialExpression* Expression : Material->GetExpressions())
			{
				if (Expression)
				{
					Expressions.Add(Expression);
				}
			}
			Expressions.Sort([](const UMaterialExpression& Left, const UMaterialExpression& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
			for (UMaterialExpression* Expr : Expressions)
			{
				const FString ExprDesc = Expr->GetDescription();
				const FString ExprClass = Expr->GetClass()->GetName();
				const FString ParamName = Expr->HasAParameterName() ? Expr->GetParameterName().ToString() : FString();

				bool bMatch = ExprDesc.Contains(Query, ESearchCase::IgnoreCase) ||
					ExprClass.Contains(Query, ESearchCase::IgnoreCase) ||
					(!ParamName.IsEmpty() && ParamName.Contains(Query, ESearchCase::IgnoreCase));

				if (bMatch && IsMaterialQueryPageEntry(Page, Total++))
				{
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetStringField(TEXT("material"), MatName);
					R->SetStringField(TEXT("materialPath"), Asset.PackageName.ToString());
					R->SetStringField(TEXT("matchType"), TEXT("expression"));
					R->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expr));
					R->SetStringField(TEXT("expressionClass"), ExprClass);
					if (!ExprDesc.IsEmpty()) R->SetStringField(TEXT("description"), ExprDesc);
					if (!ParamName.IsEmpty()) R->SetStringField(TEXT("parameterName"), ParamName);
					Results.Add(MakeShared<FJsonValueObject>(R));
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("query"), Query);
		AddMaterialQueryPageFields(Result, Page, Total, Results.Num());
		Result->SetNumberField(TEXT("resultCount"), Results.Num());
		Result->SetNumberField(TEXT("unavailableAssetCount"), UnavailableAssets);
		Result->SetBoolField(TEXT("complete"), UnavailableAssets == 0);
		Result->SetArrayField(TEXT("results"), Results);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// find_material_references
// ============================================================
class FTool_FindMaterialReferences : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.references");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString MaterialName = Params->GetStringField(TEXT("material"));
		if (MaterialName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: material"));

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		// Find package path
		FString PackagePath;
		{
			TArray<FAssetData> Assets;
			Registry.GetAssetsByClass(UMaterial::StaticClass()->GetClassPathName(), Assets, false);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString() == MaterialName || Asset.PackageName.ToString() == MaterialName)
				{
					PackagePath = Asset.PackageName.ToString();
					break;
				}
			}
		}
		if (PackagePath.IsEmpty())
		{
			TArray<FAssetData> Assets;
			Registry.GetAssetsByClass(UMaterialInstanceConstant::StaticClass()->GetClassPathName(), Assets, false);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString() == MaterialName || Asset.PackageName.ToString() == MaterialName)
				{
					PackagePath = Asset.PackageName.ToString();
					break;
				}
			}
		}
		if (PackagePath.IsEmpty())
			return FMCPToolResult::Error(FString::Printf(TEXT("Material '%s' not found."), *MaterialName));

		TArray<FName> Referencers;
		Registry.GetReferencers(FName(*PackagePath), Referencers);

		TArray<TSharedPtr<FJsonValue>> RefArray;
		for (const FName& Ref : Referencers)
		{
			FString RefStr = Ref.ToString();
			if (RefStr == PackagePath) continue;
			RefArray.Add(MakeShared<FJsonValueString>(RefStr));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), MaterialName);
		Result->SetStringField(TEXT("packagePath"), PackagePath);
		Result->SetNumberField(TEXT("totalReferencers"), RefArray.Num());
		Result->SetArrayField(TEXT("referencers"), RefArray);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// list_material_functions
// ============================================================
class FTool_ListMaterialFunctions : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.function.list");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Filter;
		Params->TryGetStringField(TEXT("filter"), Filter);
		FMaterialQueryPage Page;
		FString PageError;
		if (!ReadMaterialQueryPage(Params, Page, PageError))
		{
			return FMCPToolResult::Error(PageError, TEXT("invalid_request"), 400);
		}

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> MFAssets;
		Registry.GetAssetsByClass(UMaterialFunction::StaticClass()->GetClassPathName(), MFAssets, false);
		SortMaterialQueryAssets(MFAssets);

		TArray<TSharedPtr<FJsonValue>> Entries;
		int64 Total = 0;
		for (const FAssetData& Asset : MFAssets)
		{
			FString Name = Asset.AssetName.ToString();
			FString Path = Asset.PackageName.ToString();
			if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase) && !Path.Contains(Filter, ESearchCase::IgnoreCase))
				continue;
			if (!IsMaterialQueryPageEntry(Page, Total++))
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Name);
			Entry->SetStringField(TEXT("path"), Path);
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		AddMaterialQueryPageFields(Result, Page, Total, Entries.Num());
		Result->SetArrayField(TEXT("functions"), Entries);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// get_material_function
// ============================================================
class FTool_GetMaterialFunction : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.function.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: name"));

		FString LoadError;
		UMaterialFunction* MF = LoadMaterialFunctionByName(Name, LoadError);
		if (!MF) return FMCPToolResult::Error(LoadError);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), MF->GetName());
		Result->SetStringField(TEXT("path"), MF->GetPathName());
		Result->SetStringField(TEXT("description"), MF->GetDescription());
		Result->SetStringField(TEXT("userExposedCaption"), MF->UserExposedCaption);
		Result->SetBoolField(TEXT("exposeToLibrary"), MF->bExposeToLibrary);
		Result->SetBoolField(TEXT("prefixParameterNames"), MF->bPrefixParameterNames);
		Result->SetBoolField(TEXT("enableExecWire"), MF->bEnableExecWire);
		Result->SetBoolField(TEXT("enableNewHLSLGenerator"), MF->bEnableNewHLSLGenerator);
		if (Cast<UMaterialFunctionMaterialLayer>(MF))
		{
			Result->SetStringField(TEXT("type"), TEXT("MaterialLayer"));
		}
		else if (Cast<UMaterialFunctionMaterialLayerBlend>(MF))
		{
			Result->SetStringField(TEXT("type"), TEXT("MaterialLayerBlend"));
		}
		else
		{
			Result->SetStringField(TEXT("type"), TEXT("MaterialFunction"));
		}
		TArray<TSharedPtr<FJsonValue>> LibraryCategories;
		for (const FText& Category : MF->LibraryCategoriesText)
		{
			LibraryCategories.Add(MakeShared<FJsonValueString>(Category.ToString()));
		}
		Result->SetArrayField(TEXT("libraryCategories"), LibraryCategories);

		auto Expressions = MF->GetExpressions();
		Result->SetNumberField(TEXT("expressionCount"), Expressions.Num());

		TArray<TSharedPtr<FJsonValue>> Inputs, Outputs, ExpressionList;
		for (UMaterialExpression* Expr : Expressions)
		{
			if (!Expr) continue;
			if (auto* FI = Cast<UMaterialExpressionFunctionInput>(Expr))
			{
				TSharedRef<FJsonObject> InputObj = MakeShared<FJsonObject>();
				InputObj->SetStringField(TEXT("name"), FI->InputName.ToString());
				InputObj->SetStringField(TEXT("expressionName"), FI->GetName());
				InputObj->SetStringField(TEXT("id"), FI->Id.ToString());
				InputObj->SetStringField(TEXT("description"), FI->Description);
				InputObj->SetStringField(TEXT("type"), TEXT("FunctionInput"));
				const UEnum* InputTypeEnum = StaticEnum<EFunctionInputType>();
				if (InputTypeEnum)
				{
					FString InputType = InputTypeEnum->GetNameStringByIndex(static_cast<int32>(FI->InputType));
					InputType.RemoveFromStart(TEXT("FunctionInput_"));
					InputObj->SetStringField(TEXT("inputType"), InputType);
				}
				InputObj->SetNumberField(TEXT("sortPriority"), FI->SortPriority);
				InputObj->SetBoolField(TEXT("usePreviewValueAsDefault"), FI->bUsePreviewValueAsDefault);
				TArray<TSharedPtr<FJsonValue>> PreviewValue;
				PreviewValue.Add(MakeShared<FJsonValueNumber>(FI->PreviewValue.X));
				PreviewValue.Add(MakeShared<FJsonValueNumber>(FI->PreviewValue.Y));
				PreviewValue.Add(MakeShared<FJsonValueNumber>(FI->PreviewValue.Z));
				PreviewValue.Add(MakeShared<FJsonValueNumber>(FI->PreviewValue.W));
				InputObj->SetArrayField(TEXT("previewValue"), PreviewValue);
				InputObj->SetNumberField(TEXT("posX"), FI->MaterialExpressionEditorX);
				InputObj->SetNumberField(TEXT("posY"), FI->MaterialExpressionEditorY);
				Inputs.Add(MakeShared<FJsonValueObject>(InputObj));
			}
			else if (auto* FO = Cast<UMaterialExpressionFunctionOutput>(Expr))
			{
				TSharedRef<FJsonObject> OutputObj = MakeShared<FJsonObject>();
				OutputObj->SetStringField(TEXT("name"), FO->OutputName.ToString());
				OutputObj->SetStringField(TEXT("expressionName"), FO->GetName());
				OutputObj->SetStringField(TEXT("id"), FO->Id.ToString());
				OutputObj->SetStringField(TEXT("description"), FO->Description);
				OutputObj->SetStringField(TEXT("type"), TEXT("FunctionOutput"));
				OutputObj->SetNumberField(TEXT("sortPriority"), FO->SortPriority);
				OutputObj->SetNumberField(TEXT("posX"), FO->MaterialExpressionEditorX);
				OutputObj->SetNumberField(TEXT("posY"), FO->MaterialExpressionEditorY);
				Outputs.Add(MakeShared<FJsonValueObject>(OutputObj));
			}

			// Basic expression info
			TSharedRef<FJsonObject> ExprJson = MakeShared<FJsonObject>();
			ExprJson->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Expr));
			ExprJson->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
			ExprJson->SetStringField(TEXT("description"), Expr->GetDescription());
			ExprJson->SetNumberField(TEXT("posX"), Expr->MaterialExpressionEditorX);
			ExprJson->SetNumberField(TEXT("posY"), Expr->MaterialExpressionEditorY);
			ExpressionList.Add(MakeShared<FJsonValueObject>(ExprJson));
		}

		Result->SetArrayField(TEXT("inputs"), Inputs);
		Result->SetArrayField(TEXT("outputs"), Outputs);
		Result->SetArrayField(TEXT("expressions"), ExpressionList);

		if (MF->MaterialGraph)
		{
			TSharedPtr<FJsonObject> GraphJson = MCPHelpers::SerializeGraph(MF->MaterialGraph);
			if (GraphJson.IsValid())
				Result->SetObjectField(TEXT("graph"), GraphJson);
		}

		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// Registration
// ============================================================
namespace UEAIIntegrationTools
{
	void RegisterMaterialReadTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_ListMaterials>());
		Registry.Register(MakeShared<FTool_GetMaterial>());
		Registry.Register(MakeShared<FTool_GetMaterialGraph>());
		Registry.Register(MakeShared<FTool_DescribeMaterial>());
		Registry.Register(MakeShared<FTool_SearchMaterials>());
		Registry.Register(MakeShared<FTool_FindMaterialReferences>());
		Registry.Register(MakeShared<FTool_ListMaterialFunctions>());
		Registry.Register(MakeShared<FTool_GetMaterialFunction>());
	}
}
