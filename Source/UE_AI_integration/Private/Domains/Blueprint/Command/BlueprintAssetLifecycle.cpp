// Explicit Blueprint asset lifecycle commands for compile/save/reload.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Domains/Content/Command/WidgetEventBindingSupport.h"
#include "Infrastructure/MCPToolHelpers.h"

#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

namespace
{
FString BlueprintStatusName(const EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Dirty:
		return TEXT("Dirty");
	case BS_Error:
		return TEXT("Error");
	case BS_UpToDate:
		return TEXT("UpToDate");
	case BS_BeingCreated:
		return TEXT("BeingCreated");
	case BS_UpToDateWithWarnings:
		return TEXT("UpToDateWithWarnings");
	case BS_Unknown:
	default:
		return TEXT("Unknown");
	}
}

TSharedPtr<FJsonObject> MakeLifecycleMutation(
	const bool bChanged,
	const bool bCompiled,
	const bool bSaved,
	const TOptional<bool>& bReloaded,
	const bool bVerified)
{
	TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
	Mutation->SetBoolField(TEXT("changed"), bChanged);
	Mutation->SetBoolField(TEXT("compiled"), bCompiled);
	Mutation->SetBoolField(TEXT("saved"), bSaved);
	if (bReloaded.IsSet())
	{
		Mutation->SetBoolField(TEXT("reloaded"), bReloaded.GetValue());
	}
	else
	{
		Mutation->SetField(
			TEXT("reloaded"),
			MakeShared<FJsonValueNull>());
	}
	Mutation->SetBoolField(TEXT("verified"), bVerified);
	Mutation->SetArrayField(TEXT("warnings"), {});
	Mutation->SetArrayField(TEXT("errors"), {});
	return Mutation;
}

UBlueprint* LoadLifecycleBlueprint(
	const TSharedPtr<FJsonObject>& Params,
	FString& OutInput,
	FString& OutError)
{
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("blueprint"), OutInput)
		|| OutInput.IsEmpty())
	{
		OutError = TEXT("Missing required field: blueprint");
		return nullptr;
	}
	return MCPHelpers::LoadBlueprintByName(OutInput, OutError);
}

class FTool_CompileBlueprintAsset final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.compile");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		FString LoadError;
		UBlueprint* Blueprint = LoadLifecycleBlueprint(
			Params,
			BlueprintInput,
			LoadError);
		if (!Blueprint)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("target_not_found"),
				404);
		}

		UPackage* Package = Blueprint->GetOutermost();
		const bool bDirtyBefore = Package && Package->IsDirty();
		int32 FinalizedBindingCount = 0;
		int32 VerifiedBindingCount = 0;
		UWidgetBlueprint* WidgetBlueprint =
			Cast<UWidgetBlueprint>(Blueprint);
		if (WidgetBlueprint)
		{
			FString FinalizeError;
			if (!UEAIIntegration::WidgetEventBindings::FinalizeDeferred(
					WidgetBlueprint,
					FinalizedBindingCount,
					FinalizeError))
			{
				return FMCPToolResult::Error(
					FinalizeError,
					TEXT("verification_failed"),
					500);
			}
		}

		FCompilerResultsLog CompileLog;
		CompileLog.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(
			Blueprint,
			EBlueprintCompileOptions::SkipSave,
			&CompileLog);

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		for (const TSharedRef<FTokenizedMessage>& Message :
			CompileLog.Messages)
		{
			TSharedPtr<FJsonObject> Diagnostic =
				MakeShared<FJsonObject>();
			const EMessageSeverity::Type Severity =
				Message->GetSeverity();
			Diagnostic->SetStringField(
				TEXT("severity"),
				Severity == EMessageSeverity::Error
					? TEXT("error")
					: Severity == EMessageSeverity::Warning
						|| Severity ==
							EMessageSeverity::PerformanceWarning
					? TEXT("warning")
					: TEXT("info"));
			Diagnostic->SetStringField(
				TEXT("message"),
				Message->ToText().ToString());
			Diagnostics.Add(
				MakeShared<FJsonValueObject>(Diagnostic));
		}

		bool bVerified =
			CompileLog.NumErrors == 0
			&& Blueprint->Status != BS_Error;
		if (!bVerified)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetArrayField(TEXT("diagnostics"), Diagnostics);
			FMCPToolResult Failure = FMCPToolResult::Error(
				FString::Printf(
					TEXT("Blueprint '%s' failed to compile."),
					*Blueprint->GetPathName()),
				TEXT("asset_compile_failed"),
				500);
			Failure.Data = Details;
			return Failure;
		}

		if (WidgetBlueprint)
		{
			TArray<FString> BindingErrors;
			bVerified =
				UEAIIntegration::WidgetEventBindings::ValidateCompiled(
					WidgetBlueprint,
					VerifiedBindingCount,
					BindingErrors);
			if (!bVerified)
			{
				TArray<TSharedPtr<FJsonValue>> Errors;
				for (const FString& BindingError : BindingErrors)
				{
					Errors.Add(
						MakeShared<FJsonValueString>(
							BindingError));
				}
				TSharedPtr<FJsonObject> Details =
					MakeShared<FJsonObject>();
				Details->SetArrayField(TEXT("errors"), Errors);
				FMCPToolResult Failure = FMCPToolResult::Error(
					TEXT("Widget Blueprint compiled but generated delegate bindings failed verification."),
					TEXT("verification_failed"),
					500);
				Failure.Data = Details;
				return Failure;
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("blueprint"),
			Blueprint->GetPathName());
		Result->SetStringField(
			TEXT("status"),
			BlueprintStatusName(Blueprint->Status));
		Result->SetNumberField(
			TEXT("errorCount"),
			CompileLog.NumErrors);
		Result->SetNumberField(
			TEXT("warningCount"),
			CompileLog.NumWarnings);
		Result->SetNumberField(
			TEXT("finalizedBindingCount"),
			FinalizedBindingCount);
		Result->SetNumberField(
			TEXT("verifiedBindingCount"),
			VerifiedBindingCount);
		Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
		Result->SetBoolField(TEXT("compiled"), true);
		Result->SetBoolField(TEXT("verified"), bVerified);
		Result->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
		Result->SetBoolField(
			TEXT("dirtyAfter"),
			Package && Package->IsDirty());
		Result->SetObjectField(
			TEXT("mutation"),
			MakeLifecycleMutation(
				false,
				true,
				false,
				TOptional<bool>(),
				bVerified));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SaveBlueprintAsset final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.save");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		FString LoadError;
		UBlueprint* Blueprint = LoadLifecycleBlueprint(
			Params,
			BlueprintInput,
			LoadError);
		if (!Blueprint)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("target_not_found"),
				404);
		}

		UPackage* Package = Blueprint->GetOutermost();
		if (!Package)
		{
			return FMCPToolResult::Error(
				TEXT("Blueprint has no package."),
				TEXT("asset_package_missing"),
				500);
		}
		bool bOnlyIfDirty = false;
		Params->TryGetBoolField(
			TEXT("onlyIfDirty"),
			bOnlyIfDirty);
		const bool bDirtyBefore = Package->IsDirty();
		if (bOnlyIfDirty && !bDirtyBefore)
		{
			TSharedPtr<FJsonObject> Result =
				MakeShared<FJsonObject>();
			Result->SetStringField(
				TEXT("blueprint"),
				Blueprint->GetPathName());
			Result->SetStringField(
				TEXT("package"),
				Package->GetName());
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("skipped"), true);
			Result->SetBoolField(TEXT("dirtyBefore"), false);
			Result->SetBoolField(TEXT("dirtyAfter"), false);
			Result->SetBoolField(TEXT("verified"), true);
			Result->SetObjectField(
				TEXT("mutation"),
				MakeLifecycleMutation(
					false,
					false,
					false,
					TOptional<bool>(),
					true));
			return FMCPToolResult::Ok(Result);
		}

		UEAIIntegration::Infrastructure::FBlueprintPersistenceTarget
			PersistenceTarget;
		UEAIIntegration::Infrastructure::FBlueprintPersistenceError
			PersistenceError;
		const bool bSaved =
			UEAIIntegration::Infrastructure::SaveBlueprintPackage(
				Blueprint,
				&PersistenceTarget,
				PersistenceError);
		const bool bVerified = bSaved && !Package->IsDirty();
		if (!bSaved)
		{
			return FMCPToolResult::Error(
				PersistenceError.Message,
				PersistenceError.Code,
				PersistenceError.HttpStatus);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("blueprint"),
			Blueprint->GetPathName());
		Result->SetStringField(TEXT("package"), Package->GetName());
		Result->SetStringField(
			TEXT("filename"),
			PersistenceTarget.Filename);
		Result->SetStringField(
			TEXT("packageKind"),
			UEAIIntegration::Infrastructure::BlueprintPackageKindName(
				PersistenceTarget.Kind));
		Result->SetBoolField(TEXT("saved"), true);
		Result->SetBoolField(TEXT("skipped"), false);
		Result->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
		Result->SetBoolField(
			TEXT("dirtyAfter"),
			Package->IsDirty());
		Result->SetBoolField(TEXT("verified"), bVerified);
		Result->SetObjectField(
			TEXT("mutation"),
			MakeLifecycleMutation(
				false,
				false,
				true,
				TOptional<bool>(),
				bVerified));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_ReloadBlueprintAsset final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.reload");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		FString LoadError;
		UBlueprint* Blueprint = LoadLifecycleBlueprint(
			Params,
			BlueprintInput,
			LoadError);
		if (!Blueprint)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("target_not_found"),
				404);
		}

		UPackage* Package = Blueprint->GetOutermost();
		if (!Package)
		{
			return FMCPToolResult::Error(
				TEXT("Blueprint has no package."),
				TEXT("asset_package_missing"),
				500);
		}
		bool bForce = false;
		Params->TryGetBoolField(TEXT("force"), bForce);
		const bool bDirtyBefore = Package->IsDirty();
		if (bDirtyBefore && !bForce)
		{
			return FMCPToolResult::Error(
				TEXT("Blueprint package is dirty; pass force=true to discard unsaved changes."),
				TEXT("asset_dirty"),
				409);
		}

		const FString PackageName = Package->GetName();
		TArray<UPackage*> PackagesToReload = { Package };
		FText ReloadError;
		const bool bReloaded = UPackageTools::ReloadPackages(
			PackagesToReload,
			ReloadError,
			EReloadPackagesInteractionMode::AssumePositive);
		if (!bReloaded)
		{
			return FMCPToolResult::Error(
				ReloadError.IsEmpty()
					? FString::Printf(
						TEXT("Failed to reload Blueprint package '%s'."),
						*PackageName)
					: ReloadError.ToString(),
				TEXT("asset_reload_failed"),
				500);
		}

		FString ReloadLoadError;
		UBlueprint* ReloadedBlueprint =
			MCPHelpers::LoadBlueprintByName(
				BlueprintInput,
				ReloadLoadError);
		if (!ReloadedBlueprint)
		{
			return FMCPToolResult::Error(
				ReloadLoadError,
				TEXT("asset_reload_verification_failed"),
				500);
		}
		UPackage* ReloadedPackage =
			ReloadedBlueprint->GetOutermost();
		const bool bVerified =
			ReloadedPackage
			&& ReloadedPackage->GetName() == PackageName
			&& !ReloadedPackage->IsDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("blueprint"),
			ReloadedBlueprint->GetPathName());
		Result->SetStringField(TEXT("package"), PackageName);
		Result->SetStringField(
			TEXT("status"),
			BlueprintStatusName(ReloadedBlueprint->Status));
		Result->SetBoolField(TEXT("reloaded"), true);
		Result->SetBoolField(TEXT("forced"), bForce);
		Result->SetBoolField(TEXT("dirtyBefore"), bDirtyBefore);
		Result->SetBoolField(
			TEXT("dirtyAfter"),
			ReloadedPackage && ReloadedPackage->IsDirty());
		Result->SetBoolField(TEXT("verified"), bVerified);
		Result->SetObjectField(
			TEXT("mutation"),
			MakeLifecycleMutation(
				bDirtyBefore && bForce,
				false,
				false,
				TOptional<bool>(true),
				bVerified));
		return FMCPToolResult::Ok(Result);
	}
};
} // namespace

namespace UEAIIntegrationTools
{
void RegisterBlueprintAssetLifecycleTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_CompileBlueprintAsset>());
	Registry.Register(MakeShared<FTool_SaveBlueprintAsset>());
	Registry.Register(MakeShared<FTool_ReloadBlueprintAsset>());
}
} // namespace UEAIIntegrationTools
