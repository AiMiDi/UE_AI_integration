#include "Infrastructure/MaterialCustomEditing.h"
#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialFunctionMutation.h"
#include "Infrastructure/MaterialFunctionDependencies.h"
#include "Infrastructure/MaterialSourceFingerprint.h"
#include "Infrastructure/MaterialDiagnosticSourceMap.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Workflow/UEWorkflowRuntime.h"
#include "Tools/MCPToolRegistry.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "ShaderCore.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "Engine/Texture.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "Internationalization/Regex.h"
#include "IMaterialEditor.h"
#include "MaterialEditorActions.h"
#include "Framework/Commands/UICommandList.h"

namespace UEAIIntegration::MaterialEditing
{
namespace
{
using namespace MCPMaterialInfrastructure;
constexpr int32 MaxCodeChars = 65536;
struct FCompileSource
{
	TWeakObjectPtr<UMaterial> Material;
	FMaterialSourceFingerprint Fingerprint;
	FMaterialResource* Resource = nullptr; // Identity only; never dereferenced from the record.
	FMaterialDiagnosticSourceMap DiagnosticSourceMap;
	bool bAwaitingMappedCompilation = false;
	bool bMappedCompilationFinished = false;
};
TArray<FCompileSource> CompileSources;

// A native recompile can reuse the same resource and unchanged authored nodes.
// Keep the captured map only for its observed completion, including async calls.
// The RAII observer unregisters before the module's compile records are destroyed.
struct FMaterialCompileObserver
{
	FDelegateHandle Handle;
	FMaterialCompileObserver()
	{
		Handle = UMaterial::OnMaterialCompilationFinished().AddLambda([](UMaterialInterface* Interface)
		{
			auto* Source = CompileSources.FindByPredicate([&](const FCompileSource& Item) { return Item.Material.Get() == Interface; });
			if (!Source || !Source->Material.IsValid()) return;
			auto* Resource = Source->Material->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel));
			if (Source->bAwaitingMappedCompilation && Resource == Source->Resource && Resource && Resource->IsCompilationFinished())
			{
				Source->bAwaitingMappedCompilation = false; Source->bMappedCompilationFinished = true;
			}
			else if (Source->bMappedCompilationFinished)
			{
				Source->bMappedCompilationFinished = false; Source->DiagnosticSourceMap.State = TEXT("native_recompile_since_capture");
			}
		});
	}
	~FMaterialCompileObserver() { UMaterial::OnMaterialCompilationFinished().Remove(Handle); }
};

FMCPToolResult Invalid(const FString& Message, const TCHAR* Code = TEXT("invalid_material_edit"))
{
	return FMCPToolResult::Error(Message, Code, 400);
}

FString TypeName(ECustomMaterialOutputType Type)
{
	switch (Type)
	{
	case CMOT_Float1: return TEXT("Float1");
	case CMOT_Float2: return TEXT("Float2");
	case CMOT_Float3: return TEXT("Float3");
	case CMOT_Float4: return TEXT("Float4");
	case CMOT_MaterialAttributes: return TEXT("MaterialAttributes");
	default: return TEXT("Invalid");
	}
}

bool ParseType(const FString& Name, TEnumAsByte<ECustomMaterialOutputType>& Type)
{
	for (int32 I = 0; I < CMOT_MAX; ++I)
		if (Name == TypeName(static_cast<ECustomMaterialOutputType>(I))) { Type = static_cast<ECustomMaterialOutputType>(I); return true; }
	return false;
}

bool Identifier(const FString& Name)
{
	if (Name.IsEmpty() || Name.Len() > 128 || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase)) return false;
	auto Letter = [](TCHAR C) { return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_'; };
	if (!Letter(Name[0])) return false;
	for (TCHAR C : Name) if (!Letter(C) && !(C >= '0' && C <= '9')) return false;
	// Parameters is supplied by UE; other names are HLSL declarations/control words.
	const FString Reserved = TEXT("|Parameters|return|in|out|inout|if|else|for|while|do|switch|case|break|continue|struct|void|bool|int|uint|half|float|float2|float3|float4|Texture2D|SamplerState|true|false|");
	return !Reserved.Contains(TEXT("|") + Name + TEXT("|"), ESearchCase::CaseSensitive);
}

FString JsonHash(const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
	FTCHARToUTF8 Bytes(*Json);
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Digest);
	return TEXT("sha1:") + BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
}

int32 JsonBytes(const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
	return FTCHARToUTF8(*Json).Length();
}

TSharedRef<FJsonObject> CustomState(UMaterialExpressionCustom* Custom, bool bIncludeCode = true)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Custom));
	Result->SetStringField(TEXT("description"), Custom->Description);
	Result->SetStringField(TEXT("outputType"), TypeName(Custom->OutputType));
	Result->SetNumberField(TEXT("codeCharacters"), Custom->Code.Len());
	if (bIncludeCode) Result->SetStringField(TEXT("code"), Custom->Code);
	TArray<TSharedPtr<FJsonValue>> Inputs, Outputs, Defines, Includes;
	for (int32 I = 0; I < Custom->Inputs.Num(); ++I)
	{
		const auto& Input = Custom->Inputs[I]; auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Input.InputName.ToString());
		Item->SetNumberField(TEXT("index"), I);
		Item->SetBoolField(TEXT("connected"), Input.Input.Expression != nullptr);
		if (Input.Input.Expression)
		{
			Item->SetStringField(TEXT("sourceNodeId"), ExpressionNodeId(Input.Input.Expression));
			Item->SetNumberField(TEXT("sourceOutputIndex"), Input.Input.OutputIndex);
		}
		Inputs.Add(MakeShared<FJsonValueObject>(Item));
	}
	int32 OutputPinIndex = 1;
	for (int32 I = 0; I < Custom->AdditionalOutputs.Num(); ++I)
	{
		const auto& Output = Custom->AdditionalOutputs[I]; auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Output.OutputName.IsNone() ? FString() : Output.OutputName.ToString());
		Item->SetStringField(TEXT("type"), TypeName(Output.OutputType));
		Item->SetNumberField(TEXT("configurationIndex"), I);
		Item->SetNumberField(TEXT("index"), Output.OutputName.IsNone() ? INDEX_NONE : OutputPinIndex++);
		Outputs.Add(MakeShared<FJsonValueObject>(Item));
	}
	for (const auto& Define : Custom->AdditionalDefines)
	{
		auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("name"), Define.DefineName); Item->SetStringField(TEXT("value"), Define.DefineValue);
		Defines.Add(MakeShared<FJsonValueObject>(Item));
	}
	for (const auto& Include : Custom->IncludeFilePaths) Includes.Add(MakeShared<FJsonValueString>(Include));
	Result->SetArrayField(TEXT("inputs"), Inputs); Result->SetArrayField(TEXT("additionalOutputs"), Outputs);
	Result->SetArrayField(TEXT("defines"), Defines); Result->SetArrayField(TEXT("includePaths"), Includes);
	return Result;
}

// Replacement arrays use previousName only for explicit renames. Matching by name
// keeps wires stable under reorder; omitted elements are removals, never index shifts.
bool ReadPinArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, const TArray<FString>& OldNames,
	TArray<FString>& Names, TArray<int32>& OldIndices, TArray<TEnumAsByte<ECustomMaterialOutputType>>& Types, bool bOutputs, FString& Error)
{
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (!Params->TryGetArrayField(Field, Array) || Array->Num() > (bOutputs ? 32 : 64)) { Error = TEXT("Invalid or oversized pin array."); return false; }
	TSet<FName> UsedNames; TSet<int32> UsedIndices;
	for (const auto& Value : *Array)
	{
		if (Value->Type != EJson::Object) { Error = TEXT("Each pin must be an object."); return false; }
		const auto Item = Value->AsObject(); FString Name, Previous, Type;
		Item->TryGetStringField(TEXT("name"), Name);
		if (!Identifier(Name) || UsedNames.Contains(FName(*Name))) { Error = TEXT("Pin names must be unique HLSL identifiers (UE names are case-insensitive)."); return false; }
		UsedNames.Add(FName(*Name));
		const bool bRename = Item->TryGetStringField(TEXT("previousName"), Previous);
		const int32 Old = OldNames.IndexOfByKey(bRename ? Previous : Name);
		if ((bRename && Old == INDEX_NONE) || (Old != INDEX_NONE && UsedIndices.Contains(Old))) { Error = TEXT("previousName must identify one existing pin exactly once."); return false; }
		if (Old != INDEX_NONE) UsedIndices.Add(Old);
		for (const FString& OldName : OldNames)
			if (OldName != Name && OldName.Equals(Name, ESearchCase::IgnoreCase)) { Error = TEXT("Case-only pin renames are unsupported by UE FName; use a distinct name."); return false; }
		TEnumAsByte<ECustomMaterialOutputType> Parsed = CMOT_Float1;
		if (bOutputs && (!Item->TryGetStringField(TEXT("type"), Type) || !ParseType(Type, Parsed))) { Error = TEXT("Each additional output requires a supported type."); return false; }
		Names.Add(Name); OldIndices.Add(Old); Types.Add(Parsed);
	}
	return true;
}

TSharedRef<FJsonObject> FunctionCallState(UMaterialExpressionMaterialFunctionCall* Call)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Call));
	Result->SetStringField(TEXT("function"), Call->MaterialFunction ? Call->MaterialFunction->GetPathName() : FString());
	TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
	for (int32 I = 0; I < Call->FunctionInputs.Num(); ++I)
	{
		const auto& Input = Call->FunctionInputs[I]; auto Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), I);
		Item->SetStringField(TEXT("name"), Input.Input.InputName.ToString());
		Item->SetStringField(TEXT("interfaceId"), Input.ExpressionInputId.ToString());
		Item->SetBoolField(TEXT("connected"), Input.Input.Expression != nullptr);
		if (Input.ExpressionInput)
		{
			Item->SetStringField(TEXT("type"), StaticEnum<EFunctionInputType>()->GetNameStringByValue(Input.ExpressionInput->InputType));
			Item->SetBoolField(TEXT("usesDefault"), Input.ExpressionInput->bUsePreviewValueAsDefault);
		}
		if (Input.Input.Expression)
		{
			Item->SetStringField(TEXT("sourceNodeId"), ExpressionNodeId(Input.Input.Expression));
			Item->SetNumberField(TEXT("sourceOutputIndex"), Input.Input.OutputIndex);
		}
		Inputs.Add(MakeShared<FJsonValueObject>(Item));
	}
	for (int32 I = 0; I < Call->FunctionOutputs.Num(); ++I)
	{
		const auto& Output = Call->FunctionOutputs[I]; auto Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), I);
		Item->SetStringField(TEXT("name"), Output.Output.OutputName.ToString());
		Item->SetStringField(TEXT("interfaceId"), Output.ExpressionOutputId.ToString());
		Outputs.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("inputs"), Inputs); Result->SetArrayField(TEXT("outputs"), Outputs);
	return Result;
}

FMCPToolResult ConfigureFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
	FTarget Target; FString Error, FunctionPath;
	if (!Resolve(Params, Target, Error, true)) return Invalid(Error);
	auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Target.Find(Params->GetStringField(TEXT("nodeId"))));
	if (!Call || !Params->TryGetStringField(TEXT("function"), FunctionPath)) return Invalid(TEXT("A function call node and function asset path (empty to clear) are required."));
	auto Before = FunctionCallState(Call); const FString BeforeHash = JsonHash(Before);
	FString Expected;
	if (Params->TryGetStringField(TEXT("expectedStateHash"), Expected) && Expected != BeforeHash)
		return Invalid(TEXT("Function call changed; read it again."), TEXT("material_edit_conflict"));
	UMaterialFunction* Function = nullptr;
	TArray<FFunctionExpressionInput> Inputs; TArray<FFunctionExpressionOutput> Outputs;
	if (!FunctionPath.IsEmpty())
	{
		Function = LoadMaterialFunctionByName(FunctionPath, Error);
		if (!Function) return Invalid(Error);
		TArray<UMaterialFunctionInterface*> Dependencies;
		if (!CollectFunctionDependencies(Function, Dependencies, Error)) return Invalid(Error);
		if (Target.Function && (Dependencies.Contains(Target.Function) || (Target.Editor && Dependencies.Contains(Cast<UMaterialFunction>(Target.OriginalAsset))))) return Invalid(TEXT("Function call would create a recursive dependency, including after native Apply."), TEXT("function_call_cycle"));
		if (Function->GetMaterialFunctionUsage() != EMaterialFunctionUsage::Default) return Invalid(TEXT("Function call requires a regular MaterialFunction; material layers use a different interface."));
		Function->GetInputsAndOutputs(Inputs, Outputs);
		if (Inputs.Num() > 128 || Outputs.Num() > 128 || Outputs.IsEmpty()) return Invalid(TEXT("Function must expose 1..128 outputs and at most 128 inputs."));
		TSet<FName> Names; TSet<FGuid> Ids;
		for (const auto& Input : Inputs)
		{
			if (Input.Input.InputName.IsNone() || Names.Contains(Input.Input.InputName) || !Input.ExpressionInputId.IsValid() || Ids.Contains(Input.ExpressionInputId)) return Invalid(TEXT("Function input names and GUIDs must be valid and unique."));
			Names.Add(Input.Input.InputName); Ids.Add(Input.ExpressionInputId);
		}
		Names.Reset(); Ids.Reset();
		for (const auto& Output : Outputs)
		{
			if (Output.Output.OutputName.IsNone() || Names.Contains(Output.Output.OutputName) || !Output.ExpressionOutputId.IsValid() || Ids.Contains(Output.ExpressionOutputId)) return Invalid(TEXT("Function output names and GUIDs must be valid and unique."));
			Names.Add(Output.Output.OutputName); Ids.Add(Output.ExpressionOutputId);
		}
	}
	const bool bSameFunction = Call->MaterialFunction == Function;
	TArray<int32> OldInputs, OldOutputs;
	for (auto& Input : Inputs)
	{
		const int32 Old = Call->FunctionInputs.IndexOfByPredicate([&](const auto& Item) { return bSameFunction ? Item.ExpressionInputId == Input.ExpressionInputId : Item.Input.InputName == Input.Input.InputName; });
		OldInputs.Add(Old);
		if (Old != INDEX_NONE)
		{
			const FName Name = Input.Input.InputName;
			Input.Input = Call->FunctionInputs[Old].Input; Input.Input.InputName = Name;
		}
	}
	for (const auto& Output : Outputs)
		OldOutputs.Add(Call->FunctionOutputs.IndexOfByPredicate([&](const auto& Item) { return bSameFunction ? Item.ExpressionOutputId == Output.ExpressionOutputId : Item.Output.OutputName == Output.Output.OutputName; }));
	int32 Removed = 0;
	for (int32 I = 0; I < Call->FunctionInputs.Num(); ++I)
	{
		bool bConnected = Call->FunctionInputs[I].Input.Expression != nullptr;
		if (auto* Node = Cast<UMaterialGraphNode>(Call->GraphNode); Target.Graph() && Node)
			if (auto* Pin = Node->GetInputPin(I)) bConnected = !Pin->LinkedTo.IsEmpty();
		if (!OldInputs.Contains(I) && bConnected) ++Removed;
	}
	if (auto* Node = Cast<UMaterialGraphNode>(Call->GraphNode); Target.Graph() && Node)
	{
		for (int32 I = 0; I < Call->FunctionOutputs.Num(); ++I)
			if (!OldOutputs.Contains(I)) if (auto* Pin = Node->GetOutputPin(I)) Removed += Pin->LinkedTo.Num();
	}
	else
	{
		for (auto* E : Target.Expressions) if (E) for (auto* Input : E->GetInputsView())
			if (Input && Input->Expression == Call && !OldOutputs.Contains(Input->OutputIndex)) ++Removed;
		if (Target.Material) for (int32 I = 0; I < MP_MAX; ++I)
			if (auto* Input = Target.Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(I)); Input && Input->Expression == Call && !OldOutputs.Contains(Input->OutputIndex)) ++Removed;
	}
	bool bDisconnect = false, bDryRun = false;
	Params->TryGetBoolField(TEXT("disconnectRemoved"), bDisconnect); Params->TryGetBoolField(TEXT("dryRun"), bDryRun);
	if (Removed && !bDisconnect) return Invalid(TEXT("Changing this function removes connected pins; disconnectRemoved:true is required."), TEXT("connected_pin_removal"));
	TStrongObjectPtr<UMaterialExpressionMaterialFunctionCall> Desired(NewObject<UMaterialExpressionMaterialFunctionCall>());
	Desired->MaterialFunction = Function; Desired->FunctionInputs = Inputs; Desired->FunctionOutputs = Outputs;
	auto After = FunctionCallState(Desired.Get()); After->SetStringField(TEXT("nodeId"), ExpressionNodeId(Call));
	if (JsonBytes(After) > 240 * 1024) return Invalid(TEXT("Function interface exceeds 240 KiB."));
	const bool bChanged = JsonHash(After) != BeforeHash;
	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("changed"), bChanged); Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("saved"), false);
	Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Call)); Result->SetNumberField(TEXT("removedConnections"), Removed);
	if (bChanged && !bDryRun)
	{
		BeginEdit(Target); Call->Modify();
		for (auto* E : Target.Expressions) if (E) E->Modify();
		if (Target.Graph()) { Target.Graph()->Modify(); Target.Graph()->LinkMaterialExpressionsFromGraph(); }
		for (int32 I = 0; I < Inputs.Num(); ++I) if (OldInputs[I] != INDEX_NONE)
		{
			const FName Name = Inputs[I].Input.InputName;
			Inputs[I].Input = Call->FunctionInputs[OldInputs[I]].Input; Inputs[I].Input.InputName = Name;
		}
		Call->MaterialFunction = Function; Call->FunctionInputs = Inputs; Call->FunctionOutputs = Outputs;
		auto& Pins = Call->GetOutputs(); Pins.Reset(); for (const auto& Output : Outputs) Pins.Add(Output.Output);
		auto Remap = [&](FExpressionInput* Input)
		{
			if (!Input || Input->Expression != Call) return;
			const int32 Index = OldOutputs.IndexOfByKey(Input->OutputIndex);
			if (Index == INDEX_NONE) *Input = FExpressionInput(); else Input->OutputIndex = Index;
		};
		for (auto* E : Target.Expressions) if (E) for (auto* Input : E->GetInputsView()) Remap(Input);
		if (Target.Material) for (int32 I = 0; I < MP_MAX; ++I) Remap(Target.Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(I)));
		if (Target.Graph() && Call->GraphNode) { Call->GraphNode->Modify(); Call->GraphNode->ReconstructNode(); Target.Graph()->LinkGraphNodesFromMaterial(); }
		FinishEdit(Target, Call, Params, Result);
	}
	Result->SetStringField(TEXT("function"), Call->MaterialFunction ? Call->MaterialFunction->GetPathName() : FString());
	Result->SetStringField(TEXT("stateHash"), JsonHash(FunctionCallState(Call)));
	DescribeTarget(Target, Result);
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult ConfigureCustom(const TSharedPtr<FJsonObject>& Params)
{
	FTarget Target; FString Error;
	if (!Resolve(Params, Target, Error, true)) return Invalid(Error);
	auto* Custom = Cast<UMaterialExpressionCustom>(Target.Find(Params->GetStringField(TEXT("nodeId"))));
	if (!Custom) return Invalid(TEXT("nodeId must identify a Custom expression in the scoped asset."));
	const auto Before = CustomState(Custom);
	const FString BeforeHash = JsonHash(Before);
	FString Expected;
	if (Params->TryGetStringField(TEXT("expectedStateHash"), Expected) && Expected != BeforeHash) return Invalid(TEXT("Custom configuration changed; read it again before editing."), TEXT("material_edit_conflict"));
	FString Code = Custom->Code, Description = Custom->Description, Type;
	Params->TryGetStringField(TEXT("code"), Code); Params->TryGetStringField(TEXT("description"), Description);
	if (Code.Len() > MaxCodeChars || Code.IsEmpty() || Description.Len() > 1024) return Invalid(TEXT("Code must contain 1..65536 characters; description is limited to 1024."));
	TEnumAsByte<ECustomMaterialOutputType> OutputType = Custom->OutputType;
	if (Params->TryGetStringField(TEXT("outputType"), Type) && !ParseType(Type, OutputType)) return Invalid(TEXT("Unsupported Custom output type."));
	TArray<FString> InputNames, OutputNames, OldInputNames, OldOutputNames;
	TArray<int32> InputOld, OutputOld; TArray<TEnumAsByte<ECustomMaterialOutputType>> InputTypes, OutputTypes;
	for (const auto& Input : Custom->Inputs) OldInputNames.Add(Input.InputName.IsNone() ? FString() : Input.InputName.ToString());
	// Native RebuildOutputs omits unnamed configuration rows. Wires use the
	// compact pin indices, not the indices in AdditionalOutputs.
	for (const auto& Output : Custom->AdditionalOutputs) if (!Output.OutputName.IsNone()) OldOutputNames.Add(Output.OutputName.ToString());
	const bool bInputs = Params->HasField(TEXT("inputs")), bOutputs = Params->HasField(TEXT("additionalOutputs"));
	if (bInputs && !ReadPinArray(Params, TEXT("inputs"), OldInputNames, InputNames, InputOld, InputTypes, false, Error)) return Invalid(Error);
	if (bOutputs && !ReadPinArray(Params, TEXT("additionalOutputs"), OldOutputNames, OutputNames, OutputOld, OutputTypes, true, Error)) return Invalid(Error);
	// A Custom function's inputs, outputs and generated texture samplers share a namespace.
	TSet<FName> Symbols;
	for (const auto& Name : bInputs ? InputNames : OldInputNames)
	{
		if (Name.IsEmpty()) continue;
		if (Symbols.Contains(FName(*Name)) || Symbols.Contains(FName(*(Name + TEXT("Sampler"))))) return Invalid(TEXT("Input names collide with another input or generated texture sampler."));
		Symbols.Add(FName(*Name));
		Symbols.Add(FName(*(Name + TEXT("Sampler"))));
	}
	for (const auto& Name : bOutputs ? OutputNames : OldOutputNames)
		if (Symbols.Contains(FName(*Name))) return Invalid(TEXT("Input, generated sampler and output names must not collide."));
	TArray<FCustomDefine> Defines = Custom->AdditionalDefines;
	TArray<FString> Includes = Custom->IncludeFilePaths;
	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Params->HasField(TEXT("defines")))
	{
		if (!Params->TryGetArrayField(TEXT("defines"), Array) || Array->Num() > 64) return Invalid(TEXT("defines must contain at most 64 entries."));
		Defines.Reset(); TSet<FString> Names;
		for (const auto& Value : *Array)
		{
			if (Value->Type != EJson::Object) return Invalid(TEXT("Each define requires name and value."));
			FCustomDefine Define; Value->AsObject()->TryGetStringField(TEXT("name"), Define.DefineName);
			if (!Value->AsObject()->TryGetStringField(TEXT("value"), Define.DefineValue) || !Identifier(Define.DefineName) || Names.Contains(Define.DefineName)
				|| Define.DefineValue.Len() > 1024 || Define.DefineValue.Contains(TEXT("\n")) || Define.DefineValue.Contains(TEXT("\r"))) return Invalid(TEXT("Defines need unique HLSL identifiers and single-line values up to 1024 characters."));
			Names.Add(Define.DefineName); Defines.Add(Define);
		}
	}
	if (Params->HasField(TEXT("includePaths")))
	{
		if (!Params->TryGetArrayField(TEXT("includePaths"), Array) || Array->Num() > 32) return Invalid(TEXT("includePaths must contain at most 32 paths."));
		Includes.Reset();
		for (const auto& Value : *Array)
		{
			FString Path;
			if (!Value->TryGetString(Path) || !Path.StartsWith(TEXT("/")) || Path.Len() > 512 || Path.Contains(TEXT("..")) || Path.Contains(TEXT("\\")) || Path.Contains(TEXT("\""))
				|| Path.Contains(TEXT("\n")) || Path.Contains(TEXT("\r")) || !(Path.EndsWith(TEXT(".ush")) || Path.EndsWith(TEXT(".usf")))) return Invalid(TEXT("Include paths must be virtual /Engine/... or registered /Plugin/... shader paths ending in .ush/.usf, without traversal."));
			Includes.AddUnique(Path);
		}
	}
	bool bDisconnect = false, bDryRun = false;
	Params->TryGetBoolField(TEXT("disconnectRemoved"), bDisconnect); Params->TryGetBoolField(TEXT("dryRun"), bDryRun);
	int32 RemovedConnections = 0;
	if (bInputs) for (int32 I = 0; I < Custom->Inputs.Num(); ++I)
	{
		bool bConnected = Custom->Inputs[I].Input.Expression != nullptr;
		if (auto* Node = Cast<UMaterialGraphNode>(Custom->GraphNode)) if (auto* Pin = Node->GetInputPin(I)) bConnected = Pin->LinkedTo.Num() > 0;
		if (!InputOld.Contains(I) && bConnected) ++RemovedConnections;
	}
	if (bOutputs)
	{
		if (auto* Node = Cast<UMaterialGraphNode>(Custom->GraphNode); Target.Graph() && Node)
		{
			for (int32 I = 0; I < OldOutputNames.Num(); ++I)
				if (!OutputOld.Contains(I)) if (auto* Pin = Node->GetOutputPin(I + 1)) RemovedConnections += Pin->LinkedTo.Num();
		}
		else
		{
			for (auto* E : Target.Expressions) if (E) for (auto* Input : E->GetInputsView())
				if (Input && Input->Expression == Custom && Input->OutputIndex > 0 && !OutputOld.Contains(Input->OutputIndex - 1)) ++RemovedConnections;
			if (Target.Material) for (int32 I = 0; I < MP_MAX; ++I) if (auto* Input = Target.Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(I)))
				if (Input->Expression == Custom && Input->OutputIndex > 0 && !OutputOld.Contains(Input->OutputIndex - 1)) ++RemovedConnections;
		}
	}
	if (RemovedConnections && !bDisconnect) return Invalid(TEXT("Removing connected pins requires disconnectRemoved:true; rename with previousName to retain wires."), TEXT("connected_pin_removal"));
	// Build desired metadata without touching the asset. This also detects a no-op.
	TStrongObjectPtr<UMaterialExpressionCustom> Desired(NewObject<UMaterialExpressionCustom>());
	Desired->Code = Code; Desired->Description = Description; Desired->OutputType = OutputType;
	Desired->Inputs = Custom->Inputs; Desired->AdditionalOutputs = Custom->AdditionalOutputs;
	Desired->AdditionalDefines = Defines; Desired->IncludeFilePaths = Includes;
	if (bInputs)
	{
		Desired->Inputs.Reset();
		for (int32 I = 0; I < InputNames.Num(); ++I) { FCustomInput Input; Input.InputName = FName(*InputNames[I]); if (InputOld[I] != INDEX_NONE) Input.Input = Custom->Inputs[InputOld[I]].Input; Desired->Inputs.Add(Input); }
	}
	if (bOutputs)
	{
		Desired->AdditionalOutputs.Reset();
		for (int32 I = 0; I < OutputNames.Num(); ++I) { FCustomOutput Output; Output.OutputName = FName(*OutputNames[I]); Output.OutputType = OutputTypes[I]; Desired->AdditionalOutputs.Add(Output); }
	}
	auto After = CustomState(Desired.Get()); After->SetStringField(TEXT("nodeId"), ExpressionNodeId(Custom));
	if (JsonBytes(After) > 240 * 1024) return Invalid(TEXT("Combined Custom configuration exceeds 240 KiB UTF-8 JSON."), TEXT("custom_size_limit"));
	const bool bChanged = JsonHash(After) != BeforeHash;
	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true); Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("saved"), false);
	Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Custom));
	Result->SetNumberField(TEXT("removedConnections"), RemovedConnections);
	if (bChanged && !bDryRun)
	{
		BeginEdit(Target); Custom->Modify();
		if (Target.Graph())
		{
			Target.Graph()->Modify();
			for (auto* E : Target.Expressions) if (E) E->Modify();
			// Preserve any earlier deferred graph-pin edits before changing pin layout.
			Target.Graph()->LinkMaterialExpressionsFromGraph();
		}
		if (bInputs) for (int32 I = 0; I < InputOld.Num(); ++I)
			if (InputOld[I] != INDEX_NONE) Desired->Inputs[I].Input = Custom->Inputs[InputOld[I]].Input;
		Custom->Code = Code; Custom->Description = Description; Custom->OutputType = OutputType;
		if (bInputs) Custom->Inputs = Desired->Inputs;
		Custom->AdditionalOutputs = Desired->AdditionalOutputs; Custom->AdditionalDefines = Defines; Custom->IncludeFilePaths = Includes;
		RebuildCustomOutputs(Custom);
		if (bOutputs)
		{
			auto Remap = [&](FExpressionInput* Input, UObject* Owner)
			{
				if (!Input || Input->Expression != Custom || Input->OutputIndex == 0) return;
				Owner->Modify(); const int32 NewIndex = OutputOld.IndexOfByKey(Input->OutputIndex - 1);
				if (NewIndex == INDEX_NONE) *Input = FExpressionInput(); else Input->OutputIndex = NewIndex + 1;
			};
			for (auto* E : Target.Expressions) if (E) for (auto* Input : E->GetInputsView()) Remap(Input, E);
			if (Target.Material) for (int32 I = 0; I < MP_MAX; ++I) Remap(Target.Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(I)), Target.Material);
		}
		if (Target.Graph() && Custom->GraphNode)
		{
			Custom->GraphNode->Modify(); Custom->GraphNode->ReconstructNode();
			Target.Graph()->LinkGraphNodesFromMaterial();
		}
		FinishEdit(Target, Custom, Params, Result);
	}
	Result->SetStringField(TEXT("stateHash"), JsonHash(CustomState(Custom)));
	DescribeTarget(Target, Result);
	Result->SetBoolField(TEXT("hlslValidated"), false);
	return FMCPToolResult::Ok(Result);
}

TSharedRef<FJsonObject> ParameterState(UMaterialExpression* Expression)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
	Result->SetStringField(TEXT("className"), Expression->GetClass()->GetName());
	Result->SetStringField(TEXT("name"), Expression->GetParameterName().ToString());
	Result->SetStringField(TEXT("parameterGuid"), Expression->GetParameterExpressionId().ToString());
	Result->SetStringField(TEXT("description"), Expression->Desc);
	if (auto* Parameter = Cast<UMaterialExpressionParameter>(Expression))
	{
		Result->SetStringField(TEXT("group"), Parameter->Group.ToString()); Result->SetNumberField(TEXT("sortPriority"), Parameter->SortPriority);
	}
	if (auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
	{
		Result->SetNumberField(TEXT("defaultValue"), Scalar->DefaultValue);
		Result->SetNumberField(TEXT("sliderMin"), Scalar->SliderMin); Result->SetNumberField(TEXT("sliderMax"), Scalar->SliderMax);
	}
	else if (auto* Vector = Cast<UMaterialExpressionVectorParameter>(Expression))
	{
		auto Value = MakeShared<FJsonObject>();
		Value->SetNumberField(TEXT("r"), Vector->DefaultValue.R); Value->SetNumberField(TEXT("g"), Vector->DefaultValue.G);
		Value->SetNumberField(TEXT("b"), Vector->DefaultValue.B); Value->SetNumberField(TEXT("a"), Vector->DefaultValue.A);
		Result->SetObjectField(TEXT("defaultValue"), Value);
	}
	else if (auto* Bool = Cast<UMaterialExpressionStaticBoolParameter>(Expression)) Result->SetBoolField(TEXT("defaultValue"), Bool->DefaultValue);
	else if (auto* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Expression))
	{
		Result->SetStringField(TEXT("defaultValue"), Texture->Texture ? Texture->Texture->GetPathName() : FString());
		Result->SetStringField(TEXT("group"), Texture->Group.ToString()); Result->SetNumberField(TEXT("sortPriority"), Texture->SortPriority);
		Result->SetStringField(TEXT("samplerType"), StaticEnum<EMaterialSamplerType>()->GetNameStringByValue(Texture->SamplerType).RightChop(12));
	}
	Result->SetBoolField(TEXT("editable"), Expression->IsA<UMaterialExpressionScalarParameter>() || Expression->IsA<UMaterialExpressionVectorParameter>()
		|| Expression->IsA<UMaterialExpressionStaticBoolParameter>() || Expression->IsA<UMaterialExpressionTextureSampleParameter>());
	return Result;
}

bool FloatField(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, float& Value)
{
	if (!Params->HasField(Name)) return true;
	double Number;
	if (!Params->TryGetNumberField(Name, Number) || !FMath::IsFinite(Number) || FMath::Abs(Number) > MAX_flt) return false;
	Value = static_cast<float>(Number); return true;
}

FMCPToolResult SetParameter(const TSharedPtr<FJsonObject>& Params)
{
	FTarget Target; FString Error;
	if (!Resolve(Params, Target, Error, true)) return Invalid(Error);
	auto* Expression = Target.Find(Params->GetStringField(TEXT("nodeId")));
	if (!Expression || !Expression->HasAParameterName()) return Invalid(TEXT("nodeId must identify a parameter expression in this asset."));
	auto* Parameter = Cast<UMaterialExpressionParameter>(Expression);
	auto* Texture = Cast<UMaterialExpressionTextureSampleParameter>(Expression);
	auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression);
	auto* Vector = Cast<UMaterialExpressionVectorParameter>(Expression);
	auto* Bool = Cast<UMaterialExpressionStaticBoolParameter>(Expression);
	if (!Scalar && !Vector && !Bool && !Texture) return Invalid(TEXT("Editing supports Scalar, Vector, StaticBool/StaticSwitch and Texture parameters."));
	auto Before = ParameterState(Expression); const FString BeforeHash = JsonHash(Before); FString Expected;
	if (Params->TryGetStringField(TEXT("expectedStateHash"), Expected) && Expected != BeforeHash) return Invalid(TEXT("Parameter changed; read it again before editing."), TEXT("material_edit_conflict"));
	FString Name = Expression->GetParameterName().ToString(), Group = Parameter ? Parameter->Group.ToString() : Texture->Group.ToString(), Description = Expression->Desc;
	Params->TryGetStringField(TEXT("name"), Name); Params->TryGetStringField(TEXT("group"), Group); Params->TryGetStringField(TEXT("description"), Description);
	if (Name.TrimStartAndEnd().IsEmpty() || Name.Len() > 128 || FName(*Name).IsNone() || Group.Len() > 128 || Description.Len() > 1024) return Invalid(TEXT("Invalid parameter name or oversized metadata."));
	if (Name != Expression->GetParameterName().ToString() && FName(*Name) == Expression->GetParameterName()) return Invalid(TEXT("Use a distinct name for parameter renames; case-only changes are unsupported."));
	if (FName(*Name) != Expression->GetParameterName()) for (auto* Other : Target.Expressions)
		if (Other && Other != Expression && Other->HasAParameterName() && Other->GetParameterName() == FName(*Name)) return Invalid(TEXT("A parameter with that name already exists in this asset."));
	double Sort = Parameter ? Parameter->SortPriority : Texture->SortPriority;
	if (Params->HasField(TEXT("sortPriority")) && (!Params->TryGetNumberField(TEXT("sortPriority"), Sort) || !FMath::IsFinite(Sort) || Sort < MIN_int32 || Sort > MAX_int32 || Sort != FMath::FloorToDouble(Sort))) return Invalid(TEXT("sortPriority must be an int32."));
	float ScalarValue = Scalar ? Scalar->DefaultValue : 0, Min = Scalar ? Scalar->SliderMin : 0, Max = Scalar ? Scalar->SliderMax : 0;
	FLinearColor VectorValue = Vector ? Vector->DefaultValue : FLinearColor::Black;
	bool BoolValue = Bool && Bool->DefaultValue;
	UTexture* TextureValue = Texture ? Texture->Texture.Get() : nullptr;
	TEnumAsByte<EMaterialSamplerType> Sampler = Texture ? Texture->SamplerType : TEnumAsByte<EMaterialSamplerType>(SAMPLERTYPE_Color);
	if (!Scalar && (Params->HasField(TEXT("sliderMin")) || Params->HasField(TEXT("sliderMax")))) return Invalid(TEXT("Slider settings require a scalar parameter."));
	if (Scalar && (!FloatField(Params, TEXT("defaultValue"), ScalarValue) || !FloatField(Params, TEXT("sliderMin"), Min) || !FloatField(Params, TEXT("sliderMax"), Max) || Min > Max)) return Invalid(TEXT("Scalar values must be finite, and sliderMin <= sliderMax."));
	if (Vector && Params->HasField(TEXT("defaultValue")))
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (!Params->TryGetObjectField(TEXT("defaultValue"), Value) || !FloatField(*Value, TEXT("r"), VectorValue.R) || !FloatField(*Value, TEXT("g"), VectorValue.G)
			|| !FloatField(*Value, TEXT("b"), VectorValue.B) || !FloatField(*Value, TEXT("a"), VectorValue.A)) return Invalid(TEXT("Vector defaultValue requires finite {r,g,b,a}; omitted channels retain their values."));
	}
	if (Bool && Params->HasField(TEXT("defaultValue")) && !Params->TryGetBoolField(TEXT("defaultValue"), BoolValue)) return Invalid(TEXT("Static parameters require a boolean defaultValue."));
	if (Texture && Params->HasField(TEXT("defaultValue")))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("defaultValue"), Path)) return Invalid(TEXT("Texture defaultValue must be an asset path, or empty to clear."));
		TextureValue = Path.IsEmpty() ? nullptr : LoadObject<UTexture>(nullptr, *Path);
		if (!Path.IsEmpty() && (!TextureValue || !Texture->TextureIsValid(TextureValue, Error))) return Invalid(TEXT("Invalid texture for this parameter: ") + Error);
	}
	if (Params->HasField(TEXT("samplerType")))
	{
		FString NameValue;
		if (!Texture || !Params->TryGetStringField(TEXT("samplerType"), NameValue)) return Invalid(TEXT("samplerType requires a texture parameter."));
		const int64 Value = StaticEnum<EMaterialSamplerType>()->GetValueByNameString(TEXT("SAMPLERTYPE_") + NameValue);
		if (Value < 0 || Value >= SAMPLERTYPE_MAX) return Invalid(TEXT("Unsupported samplerType; use a native suffix such as Color, Normal or LinearColor."));
		Sampler = static_cast<EMaterialSamplerType>(Value);
	}
	bool bDryRun = false; Params->TryGetBoolField(TEXT("dryRun"), bDryRun);
	bool bChanged = Name != Expression->GetParameterName().ToString() || FName(*Group) != (Parameter ? Parameter->Group : Texture->Group)
		|| Description != Expression->Desc || Sort != (Parameter ? Parameter->SortPriority : Texture->SortPriority)
		|| (Scalar && (ScalarValue != Scalar->DefaultValue || Min != Scalar->SliderMin || Max != Scalar->SliderMax))
		|| (Vector && VectorValue != Vector->DefaultValue) || (Bool && BoolValue != Bool->DefaultValue)
		|| (Texture && (TextureValue != Texture->Texture || Sampler != Texture->SamplerType));
	auto Result = MakeShared<FJsonObject>(); Result->SetBoolField(TEXT("success"), true); Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("dryRun"), bDryRun); Result->SetBoolField(TEXT("saved"), false); Result->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
	if (bChanged && !bDryRun)
	{
		BeginEdit(Target); Expression->Modify();
		Expression->SetParameterName(FName(*Name)); Expression->Desc = Description;
		if (Parameter) { Parameter->Group = FName(*Group); Parameter->SortPriority = static_cast<int32>(Sort); }
		if (Scalar) { Scalar->DefaultValue = ScalarValue; Scalar->SliderMin = Min; Scalar->SliderMax = Max; }
		if (Vector) Vector->DefaultValue = VectorValue;
		if (Bool) Bool->DefaultValue = BoolValue;
		if (Texture) { Texture->Texture = TextureValue; Texture->SamplerType = Sampler; Texture->Group = FName(*Group); Texture->SortPriority = static_cast<int32>(Sort); }
		FinishEdit(Target, Expression, Params, Result);
	}
	Result->SetStringField(TEXT("stateHash"), JsonHash(ParameterState(Expression)));
	Result->SetStringField(TEXT("parameterGuid"), Expression->GetParameterExpressionId().ToString());
	DescribeTarget(Target, Result);
	return FMCPToolResult::Ok(Result);
}
}

void NotifyMaterialSourceEdited(UObject* Asset)
{
	// Track edits not yet copied from deferred graph pins to authored expressions.
	RecordMaterialSourceEdit(Asset);
}

bool PrepareMaterialSourceValidation(UMaterial* Material)
{
	const auto Source = CaptureMaterialSourceFingerprint(Material, true, false);
	if (!Source.bShaderCacheMismatch) return false;
	FlushShaderFileCache(); return true;
}

void RecordMaterialCompileRequest(UMaterial* Material)
{
	static FMaterialCompileObserver Observer;
	CompileSources.RemoveAll([&](const FCompileSource& Item) { return !Item.Material.IsValid() || Item.Material == Material; });
	if (CompileSources.Num() >= 64) CompileSources.RemoveAt(0);
	CompileSources.Add({Material, CaptureMaterialSourceFingerprint(Material, true), Material->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel))});
}

void RebuildCustomOutputs(UMaterialExpressionCustom* Custom)
{
	auto& Outputs = Custom->GetOutputs(); Outputs.Reset(Custom->AdditionalOutputs.Num() + 1);
	Custom->bShowOutputNameOnPin = !Custom->AdditionalOutputs.IsEmpty();
	Outputs.Add(FExpressionOutput(Custom->AdditionalOutputs.IsEmpty() ? TEXT("") : TEXT("return")));
	for (const auto& Output : Custom->AdditionalOutputs) if (!Output.OutputName.IsNone()) Outputs.Add(FExpressionOutput(Output.OutputName));
}

TSharedRef<FJsonObject> CompleteMaterialValidation(UMaterial* Material, bool bWait)
{
	FMaterialResource* Resource = Material->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel));
	RecordMaterialCompileRequest(Material);
	if (Resource)
	{
		Resource->FinishCacheShaders();
		Resource->SubmitCompileJobs_GameThread(EShaderCompileJobPriority::High);
		if (auto* Source = CompileSources.FindByPredicate([&](const FCompileSource& Item) { return Item.Material == Material; }))
		{
			Source->DiagnosticSourceMap = CaptureMaterialDiagnosticSourceMap(Resource, Source->Fingerprint);
			Source->bAwaitingMappedCompilation = Source->DiagnosticSourceMap.State == TEXT("captured");
		}
		if (bWait) Resource->FinishCompilation();
	}
	auto Result = ReadDiagnostics(Material);
	Result->SetBoolField(TEXT("compileTriggered"), Resource != nullptr);
	Result->SetBoolField(TEXT("waitedForCompilation"), bWait && Resource != nullptr);
	return Result;
}

TSharedRef<FJsonObject> ReadDiagnostics(UMaterial* Material)
{
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("materialPath"), Material->GetPathName());
	Result->SetStringField(TEXT("featureLevel"), GMaxRHIFeatureLevel == ERHIFeatureLevel::SM5 ? TEXT("SM5") : GMaxRHIFeatureLevel == ERHIFeatureLevel::SM6 ? TEXT("SM6") : TEXT("ES3_1"));
	Result->SetStringField(TEXT("coverage"), TEXT("currentMaterialResource"));
	Result->SetBoolField(TEXT("allPlatformsValidated"), false);
	Result->SetBoolField(TEXT("compileTriggered"), false);
	TArray<TSharedPtr<FJsonValue>> Errors, Nodes, ExpressionLocations;
	FMaterialResource* Resource = Material->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel));
	Result->SetBoolField(TEXT("resourceAvailable"), Resource != nullptr);
	const bool bFinished = Resource && Resource->IsCompilationFinished();
	const bool bComplete = bFinished && Resource->IsGameThreadShaderMapComplete();
	const auto* Source = CompileSources.FindByPredicate([&](const FCompileSource& Item) { return Item.Material == Material; });
	const auto CurrentSource = CaptureMaterialSourceFingerprint(Material);
	const bool bSourceVerified = CurrentSource.bComplete && Source && Source->Fingerprint.bComplete && Source->Resource == Resource;
	const bool bSourceMatches = bSourceVerified && Source->Fingerprint.Hash == CurrentSource.Hash;
	const bool bMappingCurrent = bSourceMatches && bFinished && Resource && !Resource->GetCompileErrors().IsEmpty()
		&& Source->bMappedCompilationFinished && Source->DiagnosticSourceMap.State == TEXT("captured");
	Result->SetStringField(TEXT("sourceState"), !Source ? TEXT("untracked") : !bSourceVerified ? TEXT("unverified") : bSourceMatches ? TEXT("matched") : TEXT("stale"));
	Result->SetStringField(TEXT("sourceCheckCoverage"), TEXT("authoredMaterial; transitiveFunctionsInstanceParentsAndLayers; literalCustomIncludeClosure; conditionalBranchesConservative"));
	Result->SetBoolField(TEXT("sourceCheckComplete"), bSourceVerified);
	Result->SetBoolField(TEXT("implicitEngineShaderSourcesChecked"), false);
	Result->SetStringField(TEXT("sourceFingerprint"), CurrentSource.Hash);
	Result->SetStringField(TEXT("sourceMapState"), Source ? Source->DiagnosticSourceMap.State : TEXT("untracked"));
	Result->SetBoolField(TEXT("diagnosticSourceMapCurrent"), bMappingCurrent);
	Result->SetNumberField(TEXT("sourceFunctionCount"), CurrentSource.FunctionCount); Result->SetNumberField(TEXT("sourceIncludeCount"), CurrentSource.IncludeCount);
	TArray<FString> Issues = CurrentSource.Issues;
	if (Source && Source->Resource != Resource) Issues.Add(TEXT("resource_changed_since_validation"));
	if (Source) for (const auto& Issue : Source->Fingerprint.Issues) if (Issues.Num() < 32) Issues.AddUnique(TEXT("atCompile:") + Issue);
	TArray<TSharedPtr<FJsonValue>> SourceIssues; for (const auto& Issue : Issues) SourceIssues.Add(MakeShared<FJsonValueString>(Issue));
	Result->SetArrayField(TEXT("sourceCheckIssues"), SourceIssues);
	if (Resource)
	{
		for (auto* Expression : Resource->GetErrorExpressions()) if (Expression && Nodes.Num() < 128)
		{
			Nodes.Add(MakeShared<FJsonValueString>(MCPMaterialInfrastructure::ExpressionNodeId(Expression)));
			auto Location = MakeShared<FJsonObject>();
			UObject* Owner = Expression->Function ? static_cast<UObject*>(Expression->Function.Get()) : static_cast<UObject*>(Expression->Material.Get());
			Location->SetStringField(TEXT("assetPath"), Owner ? Owner->GetPathName() : Expression->GetOuter()->GetPathName());
			Location->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Expression));
			Location->SetStringField(TEXT("expressionPath"), Expression->GetPathName());
			Location->SetStringField(TEXT("className"), Expression->GetClass()->GetName());
			ExpressionLocations.Add(MakeShared<FJsonValueObject>(Location));
		}
		const auto& CompileErrors = Resource->GetCompileErrors();
		for (int32 I = 0; I < FMath::Min(CompileErrors.Num(), 64); ++I)
		{
			auto Entry = MakeShared<FJsonObject>(); const FString Message = CompileErrors[I].Left(2048);
			Entry->SetStringField(TEXT("severity"), TEXT("error")); Entry->SetStringField(TEXT("message"), Message);
			Entry->SetStringField(TEXT("code"), TEXT("material_compile_error"));
			// Preserve the compiler location even when no authored mapping is proven.
			Entry->SetBoolField(TEXT("sourceLineMapped"), false);
			Entry->SetStringField(TEXT("sourceMappingState"), TEXT("no_compiler_location"));
			FString CompilerFile; int32 CompilerLine = 0, CompilerColumn = 0;
			if (ParseMaterialCompilerLocation(Message, CompilerFile, CompilerLine, CompilerColumn))
			{
				auto Where = MakeShared<FJsonObject>(); Where->SetStringField(TEXT("file"), CompilerFile);
				Where->SetNumberField(TEXT("line"), CompilerLine); Where->SetNumberField(TEXT("column"), CompilerColumn);
				Where->SetStringField(TEXT("space"), TEXT("compilerReported")); Entry->SetObjectField(TEXT("location"), Where);
				if (Source) MapMaterialDiagnosticLocation(Source->DiagnosticSourceMap, CompilerFile, CompilerLine, CompilerColumn, bMappingCurrent, Entry);
			}
			if (Message.Contains(TEXT("undeclared identifier"))) Entry->SetStringField(TEXT("hint"), TEXT("Read Custom inputs/outputs/defines and check exact HLSL spelling; update code and renamed pins in one custom.set."));
			else if (Message.Contains(TEXT("missing input"))) Entry->SetStringField(TEXT("hint"), TEXT("Read Custom inputs and connect each named input, or remove it together with its code reference."));
			else if (Message.Contains(TEXT("include"))) Entry->SetStringField(TEXT("hint"), TEXT("Check registered virtual shader paths and include dependencies; filesystem paths are not shader virtual paths."));
			Errors.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetNumberField(TEXT("errorCount"), CompileErrors.Num());
		Result->SetBoolField(TEXT("truncated"), CompileErrors.Num() > 64 || Resource->GetErrorExpressions().Num() > 128 || CompileErrors.ContainsByPredicate([](const FString& E) { return E.Len() > 2048; }));
	}
	else { Result->SetNumberField(TEXT("errorCount"), 0); Result->SetBoolField(TEXT("truncated"), false); }
	const FString State = Source && !bSourceVerified ? TEXT("unverified") : Source && !bSourceMatches ? TEXT("stale") : !Resource ? TEXT("unavailable") : !bFinished ? TEXT("pending") : !Errors.IsEmpty() ? TEXT("failed") : bComplete ? TEXT("succeeded") : TEXT("unavailable");
	Result->SetStringField(TEXT("compileState"), State);
	Result->SetBoolField(TEXT("diagnosticsMayBeStale"), State == TEXT("pending") || !bSourceMatches);
	Result->SetBoolField(TEXT("shaderValidationPerformed"), bSourceMatches && (State == TEXT("succeeded") || State == TEXT("failed")));
	if (bSourceMatches && (State == TEXT("succeeded") || State == TEXT("failed"))) Result->SetBoolField(TEXT("valid"), State == TEXT("succeeded"));
	else Result->SetField(TEXT("valid"), MakeShared<FJsonValueNull>());
	Result->SetArrayField(TEXT("diagnostics"), Errors); Result->SetArrayField(TEXT("errorExpressionCandidates"), Nodes);
	Result->SetArrayField(TEXT("errorExpressions"), ExpressionLocations);
	Result->SetStringField(TEXT("nextAction"), State == TEXT("unverified") ? TEXT("Inspect sourceCheckIssues; the bounded dependency check could not establish a current verdict. Missing, dynamic or generated includes and unsupported dependencies require resolution or native inspection.") : State == TEXT("stale") || !Source ? TEXT("Run material.validate, function.validate with its actual host, or editor.refresh for a preview. Validation refreshes changed Custom shader-source caches before compilation.") : State == TEXT("pending") ? TEXT("Poll diagnostics.get; do not re-trigger compilation.")
		: State == TEXT("failed") ? TEXT("Read custom.get/parameter.list, apply a scoped Workflow correction, then validate and read diagnostics again.")
		: State == TEXT("unavailable") ? TEXT("Compile in an Editor with a rendering resource; NullRHI or an uncompiled resource cannot prove HLSL validity.") : TEXT("Compilation succeeded for the reported resource."));
	return Result;
}
}

class FTool_GetMaterialCustom : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.custom.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::MaterialEditing;
		FTarget Target; FString Error; if (!Resolve(Params, Target, Error)) return Invalid(Error);
		auto* Custom = Cast<UMaterialExpressionCustom>(Target.Find(Params->GetStringField(TEXT("nodeId"))));
		if (!Custom) return Invalid(TEXT("nodeId must identify a Custom expression."));
		if (Custom->Code.Len() > MaxCodeChars || Custom->Inputs.Num() > 64 || Custom->AdditionalOutputs.Num() > 32 || Custom->AdditionalDefines.Num() > 64 || Custom->IncludeFilePaths.Num() > 32)
			return Invalid(TEXT("Custom exceeds the bounded editor contract; use graph query metadata to inspect its size."), TEXT("custom_size_limit"));
		auto Result = CustomState(Custom); Result->SetStringField(TEXT("stateHash"), JsonHash(Result));
		if (JsonBytes(Result) > 240 * 1024) return Invalid(TEXT("Custom configuration exceeds 240 KiB UTF-8 JSON."), TEXT("custom_size_limit"));
		DescribeTarget(Target, Result); Result->SetStringField(TEXT("source"), Target.Editor ? TEXT("editorPreviewExpression") : TEXT("authoredExpression"));
		TArray<TSharedPtr<FJsonValue>> Issues;
		for (const auto& Input : Custom->Inputs) if (!Input.InputName.IsNone() && !Input.Input.Expression)
		{
			auto Issue = MakeShared<FJsonObject>(); Issue->SetStringField(TEXT("code"), TEXT("missing_custom_input")); Issue->SetStringField(TEXT("input"), Input.InputName.ToString());
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
		Result->SetArrayField(TEXT("structuralDiagnostics"), Issues); Result->SetBoolField(TEXT("hlslValidated"), false);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SetMaterialCustom : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.custom.set"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override { return UEAIIntegration::MaterialEditing::ConfigureCustom(Params); }
};

class FTool_ListMaterialParameters : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.parameter.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::MaterialEditing;
		FTarget Target; FString Error; if (!Resolve(Params, Target, Error)) return Invalid(Error);
		int32 Offset = 0, Limit = 50; Params->TryGetNumberField(TEXT("offset"), Offset); Params->TryGetNumberField(TEXT("limit"), Limit);
		if (Offset < 0 || Limit < 1 || Limit > 200) return Invalid(TEXT("offset >= 0; limit is 1..200."));
		FString Name, Id; Params->TryGetStringField(TEXT("name"), Name); Params->TryGetStringField(TEXT("nodeId"), Id);
		UMaterialExpression* Requested = Id.IsEmpty() ? nullptr : Target.Find(Id);
		TArray<UMaterialExpression*> Matches;
		for (auto* E : Target.Expressions) if (E && E->HasAParameterName() && (Name.IsEmpty() || E->GetParameterName() == FName(*Name)) && (Id.IsEmpty() || Requested == E)) Matches.Add(E);
		Matches.Sort([](const UMaterialExpression& A, const UMaterialExpression& B) { return A.GetName() < B.GetName(); });
		TArray<TSharedPtr<FJsonValue>> Items; int32 End = FMath::Min(Offset, Matches.Num()), Bytes = 0;
		while (End < Matches.Num() && Items.Num() < Limit)
		{
			auto Item = ParameterState(Matches[End]); Item->SetStringField(TEXT("stateHash"), JsonHash(Item)); const int32 Size = JsonBytes(Item);
			if (Size > 240 * 1024) return Invalid(TEXT("One parameter exceeds the response budget."), TEXT("parameter_size_limit"));
			if (Bytes + Size > 240 * 1024) break;
			Items.Add(MakeShared<FJsonValueObject>(Item)); Bytes += Size; ++End;
		}
		auto Result = MakeShared<FJsonObject>(); Result->SetArrayField(TEXT("parameters"), Items); Result->SetNumberField(TEXT("total"), Matches.Num());
		Result->SetBoolField(TEXT("hasMore"), End < Matches.Num()); if (End < Matches.Num()) Result->SetNumberField(TEXT("nextOffset"), End);
		Result->SetStringField(TEXT("coverage"), TEXT("assetLocalExpressions")); Result->SetBoolField(TEXT("referencesExpanded"), false);
		DescribeTarget(Target, Result);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SetMaterialParameter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.parameter.set"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override { return UEAIIntegration::MaterialEditing::SetParameter(Params); }
};

class FTool_GetMaterialFunctionCall : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.call.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::MaterialEditing;
		FTarget Target; FString Error;
		if (!Resolve(Params, Target, Error)) return Invalid(Error);
		auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Target.Find(Params->GetStringField(TEXT("nodeId"))));
		if (!Call) return Invalid(TEXT("nodeId must identify a function call in the scoped asset."));
		if (Call->FunctionInputs.Num() > 128 || Call->FunctionOutputs.Num() > 128) return Invalid(TEXT("Function call exceeds 128 inputs or outputs."));
		auto Result = FunctionCallState(Call);
		if (JsonBytes(Result) > 240 * 1024) return Invalid(TEXT("Function call exceeds the response budget."));
		Result->SetStringField(TEXT("stateHash"), JsonHash(Result));
		DescribeTarget(Target, Result);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SetMaterialFunctionCall : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.function.call.set"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override { return UEAIIntegration::MaterialEditing::ConfigureFunctionCall(Params); }
};

class FTool_GetMaterialDiagnostics : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.diagnostics.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::MaterialEditing;
		FTarget Target; FString Error; if (!Resolve(Params, Target, Error)) return Invalid(Error);
		if (!Target.Material) return Invalid(TEXT("Function shader diagnostics require a validationMaterial host or an explicit editorPreview context."));
		auto Result = ReadDiagnostics(Target.Material); DescribeTarget(Target, Result);
		if (Target.Editor && Target.Function)
		{
			Result->SetField(TEXT("valid"), MakeShared<FJsonValueNull>());
			Result->SetBoolField(TEXT("shaderValidationPerformed"), false);
			Result->SetStringField(TEXT("resourceCoverage"), TEXT("functionEditorBaseMaterialOnly"));
			Result->SetStringField(TEXT("nextAction"), TEXT("The native function editor may compile a separate selected-output preview. Apply in the editor, then validate the function with its actual validationMaterial host."));
		}
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_GetMaterialEditorContext : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.context.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::MaterialEditing;
		FTarget Target; FString Error; if (!Resolve(Params, Target, Error)) return Invalid(Error);
		auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("assetPath"), Target.OriginalAsset->GetPathName());
		if (!ResolvePreview(Target, Error))
		{
			Result->SetBoolField(TEXT("editorOpen"), false); Result->SetStringField(TEXT("reason"), Error);
			return FMCPToolResult::Ok(Result);
		}
		int32 Offset = 0, Limit = 50; Params->TryGetNumberField(TEXT("offset"), Offset); Params->TryGetNumberField(TEXT("limit"), Limit);
		if (Offset < 0 || Limit < 1 || Limit > 200) return Invalid(TEXT("offset >= 0; limit is 1..200."));
		DescribeTarget(Target, Result); Result->SetBoolField(TEXT("editorOpen"), true);
		Result->SetBoolField(TEXT("hasUnappliedChanges"), Target.Editor->GetToolkitCommands()->CanExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef()));
		Result->SetStringField(TEXT("previewPath"), Target.Asset->GetPathName());
		Result->SetStringField(TEXT("previewMaterialPath"), Target.Material->GetPathName());
		Result->SetBoolField(TEXT("originalPackageDirty"), Target.OriginalAsset->GetOutermost()->IsDirty());
		Result->SetStringField(TEXT("stateHash"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Target.Material));
		TArray<TSharedPtr<FJsonValue>> Nodes;
		const int32 Begin = FMath::Min(Offset, Target.Expressions.Num()), End = Begin + FMath::Min(Limit, Target.Expressions.Num() - Begin);
		for (int32 I = Begin; I < End; ++I) if (auto* E = Target.Expressions[I])
		{
			auto Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(E));
			Node->SetStringField(TEXT("className"), E->GetClass()->GetName()); Nodes.Add(MakeShared<FJsonValueObject>(Node));
		}
		Result->SetArrayField(TEXT("nodes"), Nodes); Result->SetNumberField(TEXT("total"), Target.Expressions.Num());
		Result->SetBoolField(TEXT("hasMore"), End < Target.Expressions.Num()); if (End < Target.Expressions.Num()) Result->SetNumberField(TEXT("nextOffset"), End);
		Result->SetStringField(TEXT("consistency"), TEXT("liveEditorPreview"));
		Result->SetStringField(TEXT("nextAction"), TEXT("Read/edit with targetContext=editorPreview and this previewId as expectedPreviewId; refresh once after the edits, then use native Apply. Refresh never applies or saves."));
		return FMCPToolResult::Ok(Result);
	}
};

namespace UEAIIntegration::MaterialEditing
{
bool ValidatePreviewDependencies(const FTarget& Target, FString& Error, const TCHAR*& ErrorCode)
{
	ErrorCode = TEXT("invalid_material_edit");
	TArray<UMaterialFunctionInterface*> Dependencies;
	if (!CollectFunctionDependencies(Target.Material, Dependencies, Error)) return false;
	if (Target.Function && Dependencies.Contains(Cast<UMaterialFunction>(Target.OriginalAsset)))
	{
		Error = TEXT("Preview would introduce a recursive function dependency after native Apply."); ErrorCode = TEXT("function_call_cycle"); return false;
	}
	return true;
}

TSharedRef<FJsonObject> CompletePreviewUpdate(const FTarget& Target, bool bWait)
{
	Target.Editor->UpdateDetailView(); Target.Graph()->NotifyGraphChanged();
	auto Result = CompleteMaterialValidation(Target.Material, bWait); DescribeTarget(Target, Result);
	Result->SetBoolField(TEXT("applied"), false); Result->SetBoolField(TEXT("saved"), false);
	Result->SetNumberField(TEXT("editorRefreshCount"), 1);
	if (Target.Function)
	{
		Result->SetField(TEXT("valid"), MakeShared<FJsonValueNull>());
		Result->SetBoolField(TEXT("shaderValidationPerformed"), false);
		Result->SetStringField(TEXT("resourceCoverage"), TEXT("functionEditorBaseMaterialOnly"));
		Result->SetStringField(TEXT("nextAction"), TEXT("Function output preview is updated through the native editor; validate the applied function with its actual validationMaterial host for a shader verdict."));
	}
	return Result;
}
}

class FTool_RefreshMaterialEditor : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.refresh"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::MaterialEditing;
		auto PreviewParams = MakeShared<FJsonObject>(); PreviewParams->Values = Params->Values;
		PreviewParams->SetStringField(TEXT("targetContext"), TEXT("editorPreview"));
		FTarget Target; FString Error; if (!Resolve(PreviewParams, Target, Error, true)) return Invalid(Error);
		const TCHAR* ErrorCode = nullptr;
		if (!ValidatePreviewDependencies(Target, Error, ErrorCode)) return Invalid(Error, ErrorCode);
		const bool bLivePreview = Target.Editor->GetToolkitCommands()->GetCheckState(FMaterialEditorCommands::Get().ToggleLivePreview.ToSharedRef()) == ECheckBoxState::Checked;
		const bool bShaderCacheRefreshed = PrepareMaterialSourceValidation(Target.Material);
		Target.Editor->UpdateMaterialAfterGraphChange();
		// The native refresh respects the user's Live Preview toggle. Explicit refresh
		// still compiles the base preview once, without changing that toggle.
		if (!bLivePreview) { Target.Material->PreEditChange(nullptr); Target.Material->PostEditChange(); }
		bool bWait = true; Params->TryGetBoolField(TEXT("waitForCompilation"), bWait);
		auto Result = CompletePreviewUpdate(Target, bWait); Result->SetBoolField(TEXT("shaderFileCacheRefreshed"), bShaderCacheRefreshed);
		return FMCPToolResult::Ok(Result);
	}
};

namespace UEAIIntegrationTools
{
void RegisterMaterialEditorGraphTools(FMCPToolRegistry& Registry);
void RegisterMaterialApplyReviewTools(FMCPToolRegistry& Registry);
void RegisterMaterialInstanceTools(FMCPToolRegistry& Registry);
void RegisterMaterialCustomTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_GetMaterialCustom>());
	Registry.Register(MakeShared<FTool_SetMaterialCustom>());
	Registry.Register(MakeShared<FTool_ListMaterialParameters>());
	Registry.Register(MakeShared<FTool_SetMaterialParameter>());
	Registry.Register(MakeShared<FTool_GetMaterialDiagnostics>());
	Registry.Register(MakeShared<FTool_GetMaterialFunctionCall>());
	Registry.Register(MakeShared<FTool_SetMaterialFunctionCall>());
	Registry.Register(MakeShared<FTool_GetMaterialEditorContext>());
	Registry.Register(MakeShared<FTool_RefreshMaterialEditor>());
	RegisterMaterialEditorGraphTools(Registry);
	RegisterMaterialApplyReviewTools(Registry);
	RegisterMaterialInstanceTools(Registry);
}
}
