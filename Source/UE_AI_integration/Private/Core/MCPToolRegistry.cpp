#include "Tools/MCPToolRegistry.h"

#include "Core/MCPCapabilityCatalog.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAIIntegrationRegistry, Log, All);

void FMCPToolRegistry::BeginDomainRegistration(const FString& Domain)
{
	static const TSet<FString> ValidDomains = {
		TEXT("blueprint"),
		TEXT("scene"),
		TEXT("content"),
		TEXT("animation"),
		TEXT("ai"),
		TEXT("production"),
	};

	if (!ActiveRegistrationDomain.IsEmpty())
	{
		RegistrationErrors.AddUnique(
			FString::Printf(
				TEXT("Cannot begin domain '%s' while domain '%s' is still active."),
				*Domain,
				*ActiveRegistrationDomain));
		return;
	}
	if (!ValidDomains.Contains(Domain))
	{
		RegistrationErrors.AddUnique(
			FString::Printf(TEXT("Unknown registration domain '%s'."), *Domain));
		return;
	}
	ActiveRegistrationDomain = Domain;
}

void FMCPToolRegistry::EndDomainRegistration()
{
	if (ActiveRegistrationDomain.IsEmpty())
	{
		RegistrationErrors.AddUnique(
			TEXT("Attempted to end a domain registration when no domain was active."));
		return;
	}
	ActiveRegistrationDomain.Reset();
}

void FMCPToolRegistry::Register(TSharedPtr<FMCPToolBase> Tool)
{
	if (!Tool.IsValid())
	{
		RegistrationErrors.AddUnique(TEXT("Attempted to register an invalid tool instance."));
		return;
	}

	const FString CapabilityId = Tool->GetCapabilityId();
	if (ActiveRegistrationDomain.IsEmpty())
	{
		RegistrationErrors.AddUnique(
			FString::Printf(
				TEXT("Implementation '%s' was registered outside a domain registrar scope."),
				*CapabilityId));
		return;
	}
	if (!UEAIIntegration::Core::IsDottedCapabilityId(CapabilityId))
	{
		RegistrationErrors.AddUnique(
			FString::Printf(TEXT("Registered implementation has invalid dotted id '%s'."), *CapabilityId));
		return;
	}
	if (!CapabilityId.StartsWith(ActiveRegistrationDomain + TEXT(".")))
	{
		RegistrationErrors.AddUnique(
			FString::Printf(
				TEXT("Implementation '%s' was bound through domain '%s'."),
				*CapabilityId,
				*ActiveRegistrationDomain));
		return;
	}
	if (Tools.Contains(CapabilityId))
	{
		RegistrationErrors.AddUnique(
			FString::Printf(TEXT("Duplicate registered implementation id '%s'."), *CapabilityId));
		return;
	}

	UE_LOG(LogUEAIIntegrationRegistry, Verbose, TEXT("Registered capability implementation: %s"), *CapabilityId);
	Tools.Add(CapabilityId, MoveTemp(Tool));
}

FMCPToolBase* FMCPToolRegistry::FindTool(const FString& Name) const
{
	const TSharedPtr<FMCPToolBase>* Found = Tools.Find(Name);
	return Found ? Found->Get() : nullptr;
}

const TSharedPtr<FJsonObject>* FMCPToolRegistry::FindCapabilityDescriptor(
	const FString& CapabilityId) const
{
	return CapabilityDescriptors.FindByPredicate(
		[&CapabilityId](const TSharedPtr<FJsonObject>& Descriptor)
		{
			FString Id;
			return Descriptor.IsValid()
				&& Descriptor->TryGetStringField(TEXT("id"), Id)
				&& Id == CapabilityId;
		});
}

FMCPToolResult FMCPToolRegistry::ExecuteTool(
	const FString& Name,
	const TSharedPtr<FJsonObject>& Params)
{
	FMCPToolBase* Tool = FindTool(Name);
	if (!Tool)
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Capability '%s' was not found."), *Name),
			TEXT("capability_not_found"),
			404);
	}
	return Tool->Execute(Params);
}

bool FMCPToolRegistry::LoadCapabilityManifests()
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	if (!Plugin.IsValid())
	{
		CapabilityDescriptors.Reset();
		CapabilitySchemas.Reset();
		DomainCounts.Reset();
		ValidationErrors = RegistrationErrors;
		ValidationErrors.AddUnique(
			TEXT("IPluginManager could not resolve the UE_AI_integration plugin base directory."));
		bCatalogLoaded = false;
		return false;
	}

	const FString Directory =
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Capabilities"));
	return LoadCapabilityManifestsFromDirectory(Directory);
}

bool FMCPToolRegistry::LoadCapabilityManifestsFromDirectory(const FString& Directory)
{
	UEAIIntegration::Core::FCapabilityCatalogData Catalog;
	TArray<FString> CatalogErrors;
	bCatalogLoaded =
		UEAIIntegration::Core::LoadCapabilityCatalog(Directory, Catalog, CatalogErrors);

	CapabilityDescriptors.Reset(Catalog.Descriptors.Num());
	for (FMCPCapabilityDescriptor& Descriptor : Catalog.Descriptors)
	{
		CapabilityDescriptors.Add(MoveTemp(Descriptor.Json));
	}
	CapabilitySchemas = MoveTemp(Catalog.InputSchemas);
	DomainCounts = MoveTemp(Catalog.DomainCounts);

	ValidationErrors = RegistrationErrors;
	for (const FString& Error : CatalogErrors)
	{
		ValidationErrors.AddUnique(Error);
	}
	ValidateExactBindings();

	if (!ValidationErrors.IsEmpty())
	{
		UE_LOG(
			LogUEAIIntegrationRegistry,
			Error,
			TEXT("Capability catalog is degraded with %d validation error(s)."),
			ValidationErrors.Num());
		for (const FString& Error : ValidationErrors)
		{
			// The aggregate error is actionable at normal verbosity. Keep the
			// complete machine-readable list in ValidationErrors and emit each
			// entry only for verbose diagnostics so catalog growth does not
			// flood logs or make tests depend on a fixed capability count.
			UE_LOG(LogUEAIIntegrationRegistry, Verbose, TEXT("  %s"), *Error);
		}
	}
	else
	{
		UE_LOG(
			LogUEAIIntegrationRegistry,
			Log,
			TEXT("Validated %d exact capability bindings across %d domains."),
			CapabilityDescriptors.Num(),
			DomainCounts.Num());
	}

	return IsReady();
}

bool FMCPToolRegistry::ValidateParams(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutErrors) const
{
	const TSharedPtr<FJsonObject>* Schema = CapabilitySchemas.Find(CapabilityId);
	if (!Schema)
	{
		OutErrors.Reset();
		OutErrors.Add(
			FString::Printf(TEXT("No input schema is available for '%s'."), *CapabilityId));
		return false;
	}
	return UEAIIntegration::Core::ValidateCapabilityParams(*Schema, Params, OutErrors);
}

void FMCPToolRegistry::ValidateExactBindings()
{
	TSet<FString> ManifestIds;
	for (const TSharedPtr<FJsonObject>& Descriptor : CapabilityDescriptors)
	{
		if (Descriptor.IsValid())
		{
			ManifestIds.Add(Descriptor->GetStringField(TEXT("id")));
		}
	}

	TArray<FString> RegisteredIds;
	Tools.GetKeys(RegisteredIds);
	RegisteredIds.Sort();
	for (const FString& RegisteredId : RegisteredIds)
	{
		if (!ManifestIds.Contains(RegisteredId))
		{
			ValidationErrors.AddUnique(
				FString::Printf(
					TEXT("Registered implementation '%s' has no manifest capability."),
					*RegisteredId));
		}
	}

	TArray<FString> SortedManifestIds = ManifestIds.Array();
	SortedManifestIds.Sort();
	for (const FString& ManifestId : SortedManifestIds)
	{
		if (!Tools.Contains(ManifestId))
		{
			ValidationErrors.AddUnique(
				FString::Printf(
					TEXT("Manifest capability '%s' has no registered implementation."),
					*ManifestId));
		}
	}
}
