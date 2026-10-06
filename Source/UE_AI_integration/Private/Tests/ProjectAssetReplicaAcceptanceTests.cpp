// Opt-in real project assets, duplicated into an independent replica.
// The runner supplies a module-proof gate and uses a fresh Editor per phase.
#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialAuthoredCheckpoint.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "MaterialShared.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationServer.h"
#include "UEAIIntegrationSubsystem.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Workflow/UEWorkflowRuntime.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace UEAIProjectReplicaAcceptance
{
constexpr TCHAR MaterialSeed[] = TEXT("/Game/LogicRes/FeatureTest/AudioTest/M_AudioDebugColor");
constexpr TCHAR BlueprintSeed[] = TEXT("/Game/ArtRes/SeqRes/Templete_BP_Res/Prop/BP_Int_Gen_Taskline_Photo_A_Paper_01");
constexpr TCHAR CounterName[] = TEXT("UEAIReplicaCounter");
constexpr TCHAR EventName[] = TEXT("UEAIReplicaSetCounter");
constexpr TCHAR PreparePidKey[] = TEXT("UEAI.Replica.PreparePid");
constexpr TCHAR RenamePidKey[] = TEXT("UEAI.Replica.RenamePid");
constexpr TCHAR MaterialDigestKey[] = TEXT("UEAI.Replica.MaterialDigest");

FString AbsoluteFilename(FString Filename)
{
	Filename = FPaths::ConvertRelativePathToFull(Filename);
	FPaths::NormalizeFilename(Filename);
	FPaths::CollapseRelativeDirectories(Filename);
	while (Filename.Len() > 3 && Filename.EndsWith(TEXT("/"))) Filename.LeftChopInline(1);
	return Filename;
}

FString PhysicalExistingPath(const FString& Filename)
{
#if PLATFORM_WINDOWS
	const HANDLE Handle = ::CreateFileW(*AbsoluteFilename(Filename), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (Handle == INVALID_HANDLE_VALUE) return FString();
	ON_SCOPE_EXIT { ::CloseHandle(Handle); };
	TArray<WCHAR> Buffer;
	Buffer.SetNumUninitialized(32768);
	const DWORD Length = ::GetFinalPathNameByHandleW(Handle, Buffer.GetData(), Buffer.Num(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (Length == 0 || Length >= static_cast<DWORD>(Buffer.Num())) return FString();
	FString Resolved(static_cast<int32>(Length), Buffer.GetData());
	Resolved.RemoveFromStart(TEXT("\\\\?\\"));
	return AbsoluteFilename(Resolved);
#else
	// This particular replica runner is Windows-only. Refuse to weaken its
	// physical-path protection on an unimplemented platform.
	return FString();
#endif
}

FString PhysicalSaveFilename(const FString& Filename)
{
	if (IFileManager::Get().FileExists(*Filename)) return PhysicalExistingPath(Filename);
	const FString Parent = PhysicalExistingPath(FPaths::GetPath(AbsoluteFilename(Filename)));
	return Parent.IsEmpty() ? FString() : AbsoluteFilename(Parent / FPaths::GetCleanFilename(Filename));
}

bool PublishEvidenceAtomically(const FString& Filename, const FString& Json, FString& Error)
{
	// A same-directory rename exposes the final path only after the writer has
	// closed a complete JSON file. Never replace evidence from another phase.
	const FString TemporaryFilename = Filename + TEXT(".")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	ON_SCOPE_EXIT { IFileManager::Get().Delete(*TemporaryFilename, false, false, true); };
	if (!FFileHelper::SaveStringToFile(Json, *TemporaryFilename,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Error = TEXT("Could not write the complete native evidence temporary file.");
		return false;
	}
	if (!IFileManager::Get().Move(*Filename, *TemporaryFilename, false, false, false, true))
	{
		Error = TEXT("Could not atomically rename native evidence to its final path.");
		return false;
	}
	return true;
}

// Junctions do not impose read-only access. Chain the Editor's existing veto
// and reject all package saves except this run's independent output directory.
class FScopedReplicaSaveVeto
{
public:
	explicit FScopedReplicaSaveVeto(const FString& InPackageRoot)
		: Previous(FCoreUObjectDelegates::IsPackageOKToSaveDelegate)
		, PackagePrefix(InPackageRoot + TEXT("/"))
		, PhysicalRoot(PhysicalExistingPath(FPackageName::LongPackageNameToFilename(InPackageRoot)))
	{
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda(
			[this](UPackage* Package, const FString& Filename, FOutputDevice* Error)
			{
				const FString PhysicalFilename = PhysicalSaveFilename(Filename);
				if (!Package || PhysicalRoot.IsEmpty() || !Package->GetName().StartsWith(PackagePrefix)
					|| PhysicalFilename.IsEmpty()
					|| !PhysicalFilename.StartsWith(PhysicalRoot + TEXT("/"), ESearchCase::IgnoreCase))
				{
					++BlockedSaves;
					return false;
				}
				++AllowedSaves;
				return !Previous.IsBound() || Previous.Execute(Package, Filename, Error);
			});
	}

	~FScopedReplicaSaveVeto()
	{
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate = Previous;
	}

	int32 AllowedSaves = 0;
	int32 BlockedSaves = 0;

private:
	FCoreUObjectDelegates::FIsPackageOKToSaveDelegate Previous;
	FString PackagePrefix;
	FString PhysicalRoot;
};

bool SaveBlueprint(UBlueprint* Blueprint, FString& Error)
{
	if (!Blueprint) return false;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
	if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
	{
		Error = TEXT("Replica Blueprint failed to compile.");
		return false;
	}
	UEAIIntegration::Infrastructure::FBlueprintPersistenceError SaveError;
	const bool bSaved = UEAIIntegration::Infrastructure::SaveBlueprintPackage(Blueprint, nullptr, SaveError);
	Error = SaveError.Code + TEXT(": ") + SaveError.Message;
	return bSaved;
}

bool AddRuntimeMarker(UBlueprint* Blueprint)
{
	if (!Blueprint || Blueprint->UbergraphPages.IsEmpty()) return false;
	FEdGraphPinType Type;
	Type.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(CounterName), Type, TEXT("0"))) return false;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph, NAME_None, RF_Transactional);
	Event->CustomFunctionName = FName(EventName);
	Event->CreateNewGuid();
	Graph->AddNode(Event, false, false);
	Event->AllocateDefaultPins();
	UK2Node_VariableSet* Setter = NewObject<UK2Node_VariableSet>(Graph, NAME_None, RF_Transactional);
	Setter->VariableReference.SetSelfMember(FName(CounterName));
	Setter->CreateNewGuid();
	Graph->AddNode(Setter, false, false);
	Setter->AllocateDefaultPins();
	UEdGraphPin* Value = Setter->FindPin(FName(CounterName));
	UEdGraphPin* Execute = Setter->FindPin(UEdGraphSchema_K2::PN_Execute);
	UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Value || !Execute || !Then) return false;
	Schema->TrySetDefaultValue(*Value, TEXT("73"));
	return Value->DefaultValue == TEXT("73") && Schema->TryCreateConnection(Then, Execute);
}

class FReplicaAcceptanceCommand final : public IAutomationLatentCommand
{
public:
	FReplicaAcceptanceCommand(FAutomationTestBase& InTest, FString InRunId, FString InPhase)
		: Test(InTest), RunId(MoveTemp(InRunId)), Phase(MoveTemp(InPhase))
		, Root(TEXT("/Game/UEAIValidation/") + RunId)
		, GateFile(FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_PROJECT_REPLICA_GATE")))
		, ResultFile(FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_PROJECT_REPLICA_RESULT")))
		, AckFile(FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_PROJECT_REPLICA_ACK")))
		, Started(FPlatformTime::Seconds())
	{
	}

	bool Update() override
	{
		if (!bStarted)
		{
			FString GateText;
			if (!FFileHelper::LoadFileToString(GateText, *GateFile))
			{
				if (FPlatformTime::Seconds() - Started < 120.0) return false;
				Test.AddError(TEXT("Exact module identity gate was not supplied within 120 seconds."));
				return true;
			}
			TSharedPtr<FJsonObject> Gate;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(GateText), Gate) || !Gate
				|| Gate->GetIntegerField(TEXT("processId")) != FPlatformProcess::GetCurrentProcessId()
				|| Gate->GetStringField(TEXT("runId")) != RunId
				|| Gate->GetStringField(TEXT("phase")) != Phase
				|| !Gate->GetBoolField(TEXT("moduleVerified"))
				|| !Gate->GetBoolField(TEXT("independentWriteRoot")))
			{
				Test.AddError(TEXT("Replica module proof gate is invalid or belongs to another process/run."));
				return true;
			}
			bStarted = true;
			UUEAIIntegrationSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
			Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
			FUEAIIntegrationServer* Server = Subsystem ? Subsystem->GetServer() : nullptr;
			Runtime = Server ? Server->GetWorkflowRuntimeForTesting() : nullptr;
			if (!Registry || !Runtime || GUsingNullRHI)
			{
				Test.AddError(TEXT("Project replica acceptance requires the loaded registry/Workflow runtime and NonNullRHI."));
				return true;
			}
			RunPhase();
			Started = FPlatformTime::Seconds();
		}
		if (!bResultWritten && Material.IsValid() && Material->IsCompiling())
		{
			if (FPlatformTime::Seconds() - Started < 180.0) return false;
			Test.AddError(TEXT("Replica material shader compilation did not finish within 180 seconds."));
		}
		if (!bResultWritten)
		{
			if (Veto) Test.TestEqual(TEXT("The entire native phase kept package writes within its physical output root"), Veto->BlockedSaves, 1);
			if (Material.IsValid())
			{
				auto Params = MakeShared<FJsonObject>();
				Params->SetStringField(TEXT("material"), Material->GetPathName());
				const FMCPToolResult Diagnostics = Call(TEXT("content.material.diagnostics.get"), Params);
				if (Result(TEXT("Fresh restored material compiler verdict is readable"), Diagnostics) && Diagnostics.Data)
				{
					bMaterialCompileVerified = Test.TestTrue(TEXT("Current restored material completed actual shader validation"),
						bFreshMaterialCompileRequested && Diagnostics.Data->GetStringField(TEXT("compileState")) == TEXT("succeeded"));
				}
				const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel);
				Test.TestNotNull(TEXT("Restored real project material has an RHI shader resource"), Resource);
				if (Resource)
				{
					Test.TestNotNull(TEXT("Fresh restored material has its own completed shader map"), Resource->GetGameThreadShaderMap());
					for (const FString& Error : Resource->GetCompileErrors()) Test.AddError(Error);
				}
				if (Phase == TEXT("prepare") && !Test.HasAnyErrors())
				{
					// Compilation can refresh derived resource identities. Capture the
					// final authored readback after it finishes, then persist that state.
					FString PersistedDigest;
					if (Result(TEXT("Freshly compiled restored state is captured for disk verification"),
						UEAIIntegration::MaterialCheckpoint::Capture(Material.Get(), TEXT("replica-persisted-") + RunId, PersistedDigest)))
					{
						Material->GetOutermost()->GetMetaData()->SetValue(Material.Get(), MaterialDigestKey, *PersistedDigest);
						Test.TestTrue(TEXT("Restored real material saves after fresh shader validation"), UEditorAssetLibrary::SaveAsset(Material->GetPathName(), false));
					}
				}
			}
			auto Evidence = MakeShared<FJsonObject>();
			Evidence->SetStringField(TEXT("schema"), TEXT("ue.project-replica-native-acceptance.v1"));
			Evidence->SetStringField(TEXT("runId"), RunId);
			Evidence->SetStringField(TEXT("phase"), Phase);
			Evidence->SetNumberField(TEXT("processId"), FPlatformProcess::GetCurrentProcessId());
			Evidence->SetStringField(TEXT("projectFile"), AbsoluteFilename(FPaths::GetProjectFilePath()));
			Evidence->SetStringField(TEXT("writePackageRoot"), Root);
			Evidence->SetStringField(TEXT("materialSeed"), MaterialSeed);
			Evidence->SetStringField(TEXT("blueprintSeed"), BlueprintSeed);
			Evidence->SetBoolField(TEXT("passed"), !Test.HasAnyErrors());
			Evidence->SetBoolField(TEXT("materialWriteRestoreVerified"), bMaterialVerified);
			Evidence->SetBoolField(TEXT("materialCompileVerified"), bMaterialCompileVerified);
			Evidence->SetBoolField(TEXT("blueprintReferencesVerified"), bBlueprintReferencesVerified);
			Evidence->SetBoolField(TEXT("runtimeVerified"), bRuntimeVerified);
			Evidence->SetBoolField(TEXT("originalSaveVetoVerified"), bSaveVetoVerified);
			Evidence->SetBoolField(TEXT("visualVerified"), false);
			FString Json;
			FJsonSerializer::Serialize(Evidence, TJsonWriterFactory<>::Create(&Json));
			FString PublicationError;
			if (!PublishEvidenceAtomically(ResultFile, Json, PublicationError))
			{
				// Automation retains this native failure even if the result path is
				// unwritable. Never acknowledge or claim a published success.
				Test.AddError(TEXT("Native replica evidence publication failed: ") + PublicationError);
				return true;
			}
			bResultWritten = true;
			Started = FPlatformTime::Seconds();
		}
		// Keep the DLL resident until the runner has verified module identity again.
		if (IFileManager::Get().FileExists(*AckFile)) return true;
		if (FPlatformTime::Seconds() - Started < 90.0) return false;
		Test.AddError(TEXT("Runner did not acknowledge the post-test module identity proof."));
		return true;
	}

private:
	FMCPToolResult Call(const TCHAR* Capability, const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Errors;
		if (!Registry->ValidateParams(Capability, Params, Errors))
		{
			return FMCPToolResult::Error(FString(Capability) + TEXT(": ") + FString::Join(Errors, TEXT("; ")),
				TEXT("replica_schema_invalid"), 400);
		}
		return Registry->ExecuteTool(Capability, Params);
	}

	bool Result(const TCHAR* Message, const FMCPToolResult& Value)
	{
		if (Test.TestTrue(Message, Value.bSuccess)) return true;
		Test.AddError(Value.ErrorCode + TEXT(": ") + Value.ErrorMessage);
		return false;
	}

	bool PrepareMaterial()
	{
		UMaterial* Copy = Cast<UMaterial>(UEditorAssetLibrary::DuplicateAsset(MaterialSeed, Root + TEXT("/M_ProjectSeed")));
		if (!Test.TestNotNull(TEXT("Real project material duplicates into the replica"), Copy)) return false;
		Material.Reset(Copy);
		UMaterialExpressionVectorParameter* Source = nullptr;
		for (UMaterialExpression* Expression : Copy->GetExpressions())
		{
			if (auto* Parameter = Cast<UMaterialExpressionVectorParameter>(Expression)) { Source = Parameter; break; }
		}
		if (!Test.TestNotNull(TEXT("Real material retains its authored vector parameter"), Source)) return false;
		// Add two known consumers only to the duplicate so protection has a concrete
		// shared edge without depending on an undocumented shape of the source seed.
		UMaterialExpressionAdd* Target = NewObject<UMaterialExpressionAdd>(Copy, TEXT("UEAIReplicaSelectedConsumer"), RF_Transactional);
		UMaterialExpressionAdd* External = NewObject<UMaterialExpressionAdd>(Copy, TEXT("UEAIReplicaExternalConsumer"), RF_Transactional);
		for (UMaterialExpressionAdd* Expression : {Target, External})
		{
			Expression->Material = Copy;
			Expression->UpdateMaterialExpressionGuid(true, false);
			Copy->GetExpressionCollection().AddExpression(Expression);
			Expression->A.Connect(0, Source);
		}
		Copy->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Target);
		MCPMaterialInfrastructure::EnsureMaterialGraph(Copy);
		if (Copy->MaterialGraph) Copy->MaterialGraph->RebuildGraph();
		Copy->PostEditChange();
		if (!Test.TestTrue(TEXT("Replica material baseline saves"), UEditorAssetLibrary::SaveAsset(Copy->GetPathName(), false))) return false;

		auto CaptureParams = MakeShared<FJsonObject>();
		CaptureParams->SetStringField(TEXT("material"), Copy->GetPathName());
		CaptureParams->SetStringField(TEXT("captureMode"), TEXT("authoredGraph"));
		const FMCPToolResult Checkpoint = Call(TEXT("content.material.graph.snapshot"), CaptureParams);
		if (!Result(TEXT("Real material authored checkpoint captures"), Checkpoint) || !Checkpoint.Data) return false;
		const FString CheckpointId = Checkpoint.Data->GetStringField(TEXT("snapshotId"));
		const FString BaselineDigest = Checkpoint.Data->GetStringField(TEXT("stateDigest"));
		auto GraphParams = MakeShared<FJsonObject>();
		GraphParams->SetStringField(TEXT("assetPath"), Copy->GetPathName());
		GraphParams->SetStringField(TEXT("targetContext"), TEXT("asset"));
		GraphParams->SetBoolField(TEXT("includeNamedReroutes"), true);
		const FMCPToolResult Graph = Call(TEXT("content.material.graph.index"), GraphParams);
		if (!Result(TEXT("Real material GraphIR captures"), Graph) || !Graph.Data) return false;
		const FString GraphId = Graph.Data->GetStringField(TEXT("snapshotId"));
		ON_SCOPE_EXIT { UEAIIntegration::MaterialQuery::Release(GraphId); };
		auto BoundaryParams = MakeShared<FJsonObject>();
		BoundaryParams->SetStringField(TEXT("snapshotId"), GraphId);
		BoundaryParams->SetArrayField(TEXT("nodeIds"), {MakeShared<FJsonValueString>(MCPMaterialInfrastructure::ExpressionNodeId(Target))});
		BoundaryParams->SetStringField(TEXT("direction"), TEXT("upstream"));
		BoundaryParams->SetNumberField(TEXT("depth"), 8);
		const FMCPToolResult Boundary = Call(TEXT("content.material.graph.boundary.get"), BoundaryParams);
		if (!Result(TEXT("Real material shared boundary captures"), Boundary) || !Boundary.Data) return false;
		Test.TestTrue(TEXT("Actual duplicated parameter has an external consumer"), Boundary.Data->GetBoolField(TEXT("requiresSharedNodeConfirmation")));
		auto Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("op"), TEXT("disconnect"));
		Operation->SetStringField(TEXT("sourceNodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Source));
		Operation->SetStringField(TEXT("targetNodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Target));
		Operation->SetStringField(TEXT("targetPinName"), TEXT("A"));
		auto Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("assetPath"), Copy->GetPathName());
		Plan->SetStringField(TEXT("snapshotId"), GraphId);
		Plan->SetStringField(TEXT("boundaryId"), Boundary.Data->GetStringField(TEXT("boundaryId")));
		Plan->SetStringField(TEXT("expectedProjectionHash"), Graph.Data->GetStringField(TEXT("projectionHash")));
		Plan->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Operation)});
		const FMCPToolResult Denied = Call(TEXT("content.material.graph.execute_plan"), Plan);
		Test.TestFalse(TEXT("Real material unconfirmed shared write is refused"), Denied.bSuccess);
		Test.TestEqual(TEXT("Real material shared refusal has an exact code"), Denied.ErrorCode, FString(TEXT("material_boundary_shared_node_confirmation_required")));
		Test.TestTrue(TEXT("Denied request preserves both actual consumers"), Target->A.Expression == Source && External->A.Expression == Source);
		FString SavedDigest, CurrentDigest;
		if (!Test.TestTrue(TEXT("Checkpoint digest verifies refused write"), UEAIIntegration::MaterialCheckpoint::GetDigests(Copy, CheckpointId, SavedDigest, CurrentDigest)
			&& CurrentDigest == BaselineDigest)) return false;
		Plan->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		if (!Result(TEXT("Confirmed real material graph write commits"), Call(TEXT("content.material.graph.execute_plan"), Plan))) return false;
		Test.TestTrue(TEXT("Confirmed request preserves independent consumer"), Target->A.Expression == nullptr && External->A.Expression == Source);
		Source->DefaultValue = FLinearColor(0.17f, 0.31f, 0.59f, 1.0f);
		Source->MaterialExpressionEditorX += 61;
		const bool BaselineTwoSided = Copy->TwoSided;
		Copy->TwoSided = !Copy->TwoSided;
		if (!Test.TestTrue(TEXT("Mutated real material digest is readable"), UEAIIntegration::MaterialCheckpoint::GetDigests(Copy, CheckpointId, SavedDigest, CurrentDigest))) return false;
		auto Restore = MakeShared<FJsonObject>();
		Restore->SetStringField(TEXT("material"), Copy->GetPathName());
		Restore->SetStringField(TEXT("snapshotId"), CheckpointId);
		Restore->SetStringField(TEXT("restoreMode"), TEXT("authoredGraph"));
		Restore->SetStringField(TEXT("expectedCurrentDigest"), CurrentDigest);
		Restore->SetBoolField(TEXT("confirmFullGraphRestore"), true);
		const FMCPToolResult RestoreDenied = Call(TEXT("content.material.graph.restore"), Restore);
		Test.TestFalse(TEXT("Real material full restore also protects shared nodes"), RestoreDenied.bSuccess);
		Test.TestEqual(TEXT("Shared restore refusal is explicit"), RestoreDenied.ErrorCode, FString(TEXT("shared_node_impact_confirmation_required")));
		Restore->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		const FMCPToolResult Restored = Call(TEXT("content.material.graph.restore"), Restore);
		if (!Result(TEXT("Real material full authored restore succeeds"), Restored) || !Restored.Data) return false;
		Test.TestEqual(TEXT("Restore equals the full pre-mutation authored digest"), Restored.Data->GetStringField(TEXT("afterStateDigest")), BaselineDigest);
		Test.TestEqual(TEXT("Restore reads back the authored TwoSided value"), Copy->TwoSided, BaselineTwoSided);
		Test.TestTrue(TEXT("Restore reconnects both original consumer identities"), Target->A.Expression == Source && External->A.Expression == Source);
		bMaterialVerified = !Test.HasAnyErrors();
		return bMaterialVerified;
	}

	bool PrepareBlueprint()
	{
		const FString TargetPath = Root + TEXT("/BP_ProjectSeed");
		UBlueprint* Target = Cast<UBlueprint>(UEditorAssetLibrary::DuplicateAsset(BlueprintSeed, TargetPath));
		if (!Test.TestNotNull(TEXT("Real project Blueprint duplicates into independent storage"), Target)) return false;
		if (!Test.TestEqual(TEXT("Selected seed has a plain native Actor parent"), Target->ParentClass.Get(), AActor::StaticClass())) return false;
		// Editor-prepared workflow plans require an existing, clean asset baseline.
		// DuplicateAsset creates the new package dirty, so persist the duplicate
		// before asking the planning capability to inspect it. The later approved
		// execution performs the authored mutation and save under the same replica
		// root.
		FString InitialSaveError;
		if (!Test.TestTrue(TEXT("Real project Blueprint baseline saves before planning"), SaveBlueprint(Target, InitialSaveError)))
		{
			Test.AddError(InitialSaveError);
			return false;
		}
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("templateName"), TEXT("health_system"));
		Params->SetStringField(TEXT("blueprint"), TargetPath);
		auto Values = MakeShared<FJsonObject>();
		Values->SetNumberField(TEXT("maxHealth"), 125.5);
		Params->SetObjectField(TEXT("parameters"), Values);
		const FString Before = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Target);
		const FMCPToolResult Planned = Call(TEXT("blueprint.template.apply"), Params);
		if (!Result(TEXT("Named template plans on a real project Blueprint"), Planned) || !Planned.Data) return false;
		Test.TestEqual(TEXT("Template planning leaves real Blueprint unchanged"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Target), Before);
		auto Execute = MakeShared<FJsonObject>();
		Execute->SetStringField(TEXT("action"), TEXT("execute"));
		Execute->SetObjectField(TEXT("workflow"), Planned.Data->GetObjectField(TEXT("workflow")));
		Execute->SetStringField(TEXT("approvePlanDigest"), Planned.Data->GetStringField(TEXT("planDigest")));
		Execute->SetBoolField(TEXT("confirmWrite"), true);
		Execute->SetBoolField(TEXT("saveOnSuccess"), true);
		Execute->SetStringField(TEXT("detailLevel"), TEXT("summary"));
		const FMCPResult Applied = Runtime->HandleRequest(Execute);
		if (!Test.TestTrue(TEXT("Approved template writes and saves real duplicate"), Applied.bOk))
		{
			Test.AddError(Applied.Error.Code + TEXT(": ") + Applied.Error.Message);
			return false;
		}
		Target = LoadObject<UBlueprint>(nullptr, *TargetPath);
		if (!Test.TestTrue(TEXT("Runtime marker is authored on the real duplicate"), AddRuntimeMarker(Target))) return false;
		Target->GetOutermost()->GetMetaData()->SetValue(Target, PreparePidKey, *FString::FromInt(FPlatformProcess::GetCurrentProcessId()));
		FString Error;
		if (!Test.TestTrue(TEXT("Real duplicate compiles and saves"), SaveBlueprint(Target, Error))) { Test.AddError(Error); return false; }
		const FString ChildPath = Root + TEXT("/BP_ProjectReferencer");
		UPackage* Package = CreatePackage(*ChildPath);
		UBlueprint* Child = FKismetEditorUtilities::CreateBlueprint(Target->GeneratedClass, Package,
			FName(*FPackageName::GetLongPackageAssetName(ChildPath)), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("UEAI.ProjectReplicaAcceptance")));
		if (!Test.TestNotNull(TEXT("Independent referencer derives from real duplicate"), Child)) return false;
		FAssetRegistryModule::AssetCreated(Child);
		Child->GetOutermost()->GetMetaData()->SetValue(Child, PreparePidKey, *FString::FromInt(FPlatformProcess::GetCurrentProcessId()));
		if (!Test.TestTrue(TEXT("Real duplicate referencer compiles and persists"), SaveBlueprint(Child, Error))) { Test.AddError(Error); return false; }
		return true;
	}

	bool ReadBlueprintPhase()
	{
		const FString TargetPath = Root + TEXT("/BP_ProjectSeed");
		const FString RenamedPath = Root + TEXT("/BP_ProjectSeedRenamed");
		const FString ChildPath = Root + TEXT("/BP_ProjectReferencer");
		UBlueprint* Child = LoadObject<UBlueprint>(nullptr, *ChildPath);
		UBlueprint* Target = LoadObject<UBlueprint>(nullptr, *(Phase == TEXT("verify") ? RenamedPath : TargetPath));
		if (!Test.TestNotNull(TEXT("Saved real duplicate reloads in another process"), Target)
			|| !Test.TestNotNull(TEXT("Saved real-asset referencer reloads"), Child)) return false;
		for (UBlueprint* Blueprint : {Target, Child})
		{
			const FString PreparePid = Blueprint->GetOutermost()->GetMetaData()->GetValue(Blueprint, PreparePidKey);
			Test.TestFalse(TEXT("Prepare PID survived real package serialization"), PreparePid.IsEmpty());
			Test.TestNotEqual(TEXT("Package reload uses a different PID"), PreparePid, FString::FromInt(FPlatformProcess::GetCurrentProcessId()));
			Test.TestFalse(TEXT("Saved duplicate reloads clean"), Blueprint->GetOutermost()->IsDirty());
		}
		bBlueprintReferencesVerified = Test.TestEqual(TEXT("Cross-asset parent resolves exact real duplicate class"), Child->ParentClass.Get(), Target->GeneratedClass.Get());
		auto References = MakeShared<FJsonObject>();
		References->SetStringField(TEXT("assetPath"), Target->GetPathName());
		FAssetRegistryModule::GetRegistry().ScanPathsSynchronous({Root}, true);
		const FMCPToolResult Referencers = Call(TEXT("blueprint.asset.references"), References);
		if (!Result(TEXT("Real duplicate reference capability reads persisted identity"), Referencers) || !Referencers.Data) return false;
		Test.TestTrue(TEXT("Saved referencer is present in native AssetRegistry"), Referencers.Data->GetIntegerField(TEXT("blueprintReferencerCount")) >= 1);
		if (Phase == TEXT("rename"))
		{
			auto Rename = MakeShared<FJsonObject>();
			Rename->SetStringField(TEXT("assetPath"), TargetPath);
			Rename->SetStringField(TEXT("newPath"), RenamedPath);
			const FMCPToolResult Renamed = Call(TEXT("blueprint.asset.rename"), Rename);
			if (!Result(TEXT("Real duplicate renames through native capability"), Renamed) || !Renamed.Data) return false;
			Test.TestTrue(TEXT("Real duplicate rename persists its redirector"), Renamed.Data->GetBoolField(TEXT("redirectorCreated")));
			Child->GetOutermost()->GetMetaData()->SetValue(Child, RenamePidKey, *FString::FromInt(FPlatformProcess::GetCurrentProcessId()));
			FString Error;
			if (!Test.TestTrue(TEXT("Renamed real-asset reference saves"), SaveBlueprint(Child, Error))) { Test.AddError(Error); return false; }
			return true;
		}
		const FString RenamePid = Child->GetOutermost()->GetMetaData()->GetValue(Child, RenamePidKey);
		Test.TestFalse(TEXT("Rename PID survived disk persistence"), RenamePid.IsEmpty());
		Test.TestNotEqual(TEXT("Runtime verification is a third independent process"), RenamePid, FString::FromInt(FPlatformProcess::GetCurrentProcessId()));
		Test.TestEqual(TEXT("Old path redirects to exact renamed real duplicate"), LoadObject<UBlueprint>(nullptr, *TargetPath), Target);
		UMaterial* SavedMaterial = LoadObject<UMaterial>(nullptr, *(Root + TEXT("/M_ProjectSeed")));
		if (!Test.TestNotNull(TEXT("Restored real material reloads from disk"), SavedMaterial)) return false;
		Material.Reset(SavedMaterial);
		FString Digest;
		const FString CheckpointId = TEXT("replica-readback-") + RunId;
		if (!Result(TEXT("Reloaded real material authored state reads back"), UEAIIntegration::MaterialCheckpoint::Capture(SavedMaterial, CheckpointId, Digest))) return false;
		bMaterialVerified = Test.TestEqual(TEXT("Restored material authored digest survives a new process"), Digest,
			FString(SavedMaterial->GetOutermost()->GetMetaData()->GetValue(SavedMaterial, MaterialDigestKey)));
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!Test.TestNotNull(TEXT("Replica runtime world creates"), World) || !GEngine) return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
		FURL URL;
		World->InitializeActorsForPlay(URL);
		World->BeginPlay();
		AActor* Actor = World->SpawnActor<AActor>(Child->GeneratedClass, FTransform::Identity);
		if (!Test.TestNotNull(TEXT("Persisted real-asset referencer spawns in an actual Game world"), Actor)) return false;
		FIntProperty* Counter = FindFProperty<FIntProperty>(Actor->GetClass(), FName(CounterName));
		FNumericProperty* Health = FindFProperty<FNumericProperty>(Actor->GetClass(), TEXT("Health"));
		UFunction* Event = Actor->FindFunction(FName(EventName));
		if (!Test.TestNotNull(TEXT("Runtime class retains inherited marker property"), Counter)
			|| !Test.TestNotNull(TEXT("Runtime class retains named-template property"), Health)
			|| !Test.TestNotNull(TEXT("Runtime class retains inherited event bytecode"), Event)) return false;
		Test.TestEqual(TEXT("Actual runtime actor uses the saved template default"), Health->GetFloatingPointPropertyValue(Health->ContainerPtrToValuePtr<void>(Actor)), 125.5);
		Test.TestEqual(TEXT("Runtime marker starts at its saved baseline"), Counter->GetPropertyValue_InContainer(Actor), 0);
		Actor->ProcessEvent(Event, nullptr);
		Test.TestEqual(TEXT("Real duplicate's inherited bytecode performs the runtime write"), Counter->GetPropertyValue_InContainer(Actor), 73);
		auto RuntimeParams = MakeShared<FJsonObject>();
		RuntimeParams->SetStringField(TEXT("blueprint"), ChildPath);
		const FMCPToolResult Observed = Call(TEXT("blueprint.asset.runtime.verify"), RuntimeParams);
		bRuntimeVerified = Test.TestTrue(TEXT("Runtime acceptance identifies the exact actual Game-world actor"), Observed.bSuccess && Observed.Data
			&& Observed.Data->GetBoolField(TEXT("runtimeVerified"))
			&& Observed.Data->GetStringField(TEXT("instance")) == Actor->GetPathName()
			&& Observed.Data->GetStringField(TEXT("instanceClass")) == Actor->GetClass()->GetPathName());
		Actor->Destroy();
		return bRuntimeVerified;
	}

	void RunPhase()
	{
		Veto = MakeUnique<FScopedReplicaSaveVeto>(Root);
		UObject* Original = UEditorAssetLibrary::LoadAsset(MaterialSeed);
		if (!Test.TestNotNull(TEXT("Original project material exists in the replica's read-only mounts"), Original)) return;
		const FString OriginalFilename = FPackageName::LongPackageNameToFilename(Original->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
		bSaveVetoVerified = Test.TestFalse(TEXT("Replica veto refuses saving the original package before any write"),
			FCoreUObjectDelegates::IsPackageOKToSaveDelegate.Execute(Original->GetOutermost(), OriginalFilename, nullptr));
		if (Phase == TEXT("prepare"))
		{
			if (PrepareMaterial()) PrepareBlueprint();
		}
		else ReadBlueprintPhase();
		if (Material.IsValid())
		{
			Material->PostEditChange();
			auto Compile = MakeShared<FJsonObject>();
			Compile->SetStringField(TEXT("material"), Material->GetPathName());
			Compile->SetBoolField(TEXT("waitForCompilation"), false);
			bFreshMaterialCompileRequested = Result(TEXT("Restored real material explicitly requests fresh native validation"),
				Call(TEXT("content.material.validate"), Compile));
		}
		Test.TestEqual(TEXT("No additional package save escaped the validation scope"), Veto->BlockedSaves, 1);
		if (Phase != TEXT("verify")) Test.TestTrue(TEXT("Replica actually saved independent package files"), Veto->AllowedSaves > 0);
	}

	FAutomationTestBase& Test;
	FString RunId, Phase, Root, GateFile, ResultFile, AckFile;
	double Started;
	FMCPToolRegistry* Registry = nullptr;
	UEAIIntegration::Workflow::FWorkflowRuntime* Runtime = nullptr;
	TStrongObjectPtr<UMaterial> Material;
	TUniquePtr<FScopedReplicaSaveVeto> Veto;
	bool bStarted = false;
	bool bResultWritten = false;
	bool bMaterialVerified = false;
	bool bFreshMaterialCompileRequested = false;
	bool bMaterialCompileVerified = false;
	bool bBlueprintReferencesVerified = false;
	bool bRuntimeVerified = false;
	bool bSaveVetoVerified = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAIProjectAssetReplicaAcceptanceTest,
	"UE_AI_integration.ProjectReplica.RealAssetsWriteRestoreReferencesAndRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FUEAIProjectAssetReplicaAcceptanceTest::RunTest(const FString&)
{
	using namespace UEAIProjectReplicaAcceptance;
	const FString RunId = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_PROJECT_REPLICA_RUN"));
	const FString Phase = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_PROJECT_REPLICA_PHASE"));
	if (RunId.IsEmpty() && Phase.IsEmpty())
	{
		AddInfo(TEXT("Real-project replica acceptance is opt-in; run run-project-replica-acceptance.ps1. No real-project evidence was produced."));
		return true;
	}
	FGuid Guid;
	if (!TestTrue(TEXT("Replica run ID is an exact GUID"), FGuid::ParseExact(RunId, EGuidFormats::Digits, Guid))
		|| !TestTrue(TEXT("Replica phase is explicit"), Phase == TEXT("prepare") || Phase == TEXT("rename") || Phase == TEXT("verify"))) return false;
	const FString Replica = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_PROJECT_REPLICA_ROOT"));
	const FString ProjectDir = AbsoluteFilename(FPaths::ProjectDir());
	if (!TestTrue(TEXT("Replica owns its independent project directory under S:/tmp"),
		!Replica.IsEmpty() && ProjectDir.Equals(AbsoluteFilename(Replica), ESearchCase::IgnoreCase)
		&& ProjectDir.StartsWith(TEXT("S:/tmp/"), ESearchCase::IgnoreCase))) return false;
	const FString EvidencePrefix = ProjectDir + TEXT("/Saved/UEAIReplica/");
	for (const TCHAR* Key : {TEXT("UEAI_PROJECT_REPLICA_GATE"), TEXT("UEAI_PROJECT_REPLICA_RESULT"), TEXT("UEAI_PROJECT_REPLICA_ACK")})
	{
		if (!TestTrue(TEXT("Replica evidence paths remain in independent Saved storage"),
			AbsoluteFilename(FPlatformMisc::GetEnvironmentVariable(Key)).StartsWith(EvidencePrefix, ESearchCase::IgnoreCase))) return false;
	}
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FReplicaAcceptanceCommand>(*this, RunId, Phase));
	return true;
}
#endif
