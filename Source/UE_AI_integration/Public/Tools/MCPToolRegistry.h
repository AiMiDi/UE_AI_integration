// Instance-owned tool registry and manifest-backed capability catalog.
#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

class FMCPToolRegistry
{
public:
	/** Open a registrar scope so implementation IDs can be checked against their owning domain. */
	void BeginDomainRegistration(const FString& Domain);

	/** Close the current registrar scope. */
	void EndDomainRegistration();

	/** Register a tool. Takes ownership. */
	void Register(TSharedPtr<FMCPToolBase> Tool);

	/** Find a tool by name. Returns nullptr if not found. */
	FMCPToolBase* FindTool(const FString& Name) const;

	/** Execute a tool by name. */
	FMCPToolResult ExecuteTool(const FString& Name, const TSharedPtr<FJsonObject>& Params);

	/** Begin a tool-owned bounded async execution when the tool supports it. */
	bool BeginExecuteToolAsync(
		const FString& Name,
		const TSharedPtr<FJsonObject>& Params,
		FMCPToolAsyncCompletion Completion);

	/** Cancel all active async tools synchronously (server stop/module unload). */
	void CancelAsyncTools(const FString& Reason);

	/** Load and validate the six manifests from this plugin's Resources directory. */
	bool LoadCapabilityManifests();

	/** Explicit-directory overload used by automation tests. */
	bool LoadCapabilityManifestsFromDirectory(const FString& Directory);

	/** Validate request params against the manifest input schema. */
	bool ValidateParams(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutErrors) const;

	/** Return the immutable manifest input schema for executor-level context injection. */
	const TSharedPtr<FJsonObject>* FindInputSchema(
		const FString& CapabilityId) const
	{
		return CapabilitySchemas.Find(CapabilityId);
	}

	/** Return the immutable manifest descriptor for availability evaluation. */
	const TSharedPtr<FJsonObject>* FindCapabilityDescriptor(
		const FString& CapabilityId) const;

	/** Manifest descriptors are authoritative for the public capability API. */
	const TArray<TSharedPtr<FJsonObject>>& GetCapabilityDescriptors() const
	{
		return CapabilityDescriptors;
	}

	const TMap<FString, int32>& GetDomainCounts() const { return DomainCounts; }
	const TArray<FString>& GetValidationErrors() const { return ValidationErrors; }

	/** The registry is ready only when catalog loading and exact implementation binding both pass. */
	bool IsReady() const { return bCatalogLoaded && ValidationErrors.IsEmpty(); }

	/** Number of registered implementations. */
	int32 Num() const { return Tools.Num(); }

	/** Number of public manifest capabilities. */
	int32 GetCapabilityCount() const { return CapabilityDescriptors.Num(); }

private:
	void ValidateExactBindings();

	TMap<FString, TSharedPtr<FMCPToolBase>> Tools;
	TArray<TSharedPtr<FJsonObject>> CapabilityDescriptors;
	TMap<FString, TSharedPtr<FJsonObject>> CapabilitySchemas;
	TMap<FString, int32> DomainCounts;
	TArray<FString> RegistrationErrors;
	TArray<FString> ValidationErrors;
	FString ActiveRegistrationDomain;
	bool bCatalogLoaded = false;
};
