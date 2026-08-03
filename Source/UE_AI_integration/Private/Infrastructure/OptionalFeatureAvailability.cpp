#include "Infrastructure/OptionalFeatureAvailability.h"

#include "HAL/PlatformProperties.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif
#ifndef WITH_UEAI_WATER
#define WITH_UEAI_WATER 0
#endif
#ifndef WITH_UEAI_PCG
#define WITH_UEAI_PCG 0
#endif

namespace UEAIIntegration::Infrastructure
{
namespace
{
int32 CompareEngineVersionComponents(
	const FEngineVersion& Left,
	const FEngineVersion& Right)
{
	if (Left.GetMajor() != Right.GetMajor())
	{
		return Left.GetMajor() < Right.GetMajor() ? -1 : 1;
	}
	if (Left.GetMinor() != Right.GetMinor())
	{
		return Left.GetMinor() < Right.GetMinor() ? -1 : 1;
	}
	if (Left.GetPatch() != Right.GetPatch())
	{
		return Left.GetPatch() < Right.GetPatch() ? -1 : 1;
	}
	return 0;
}
}

bool IsOptionalFeatureCompiled(const FString& FeatureName)
{
	if (FeatureName.Equals(TEXT("Niagara"), ESearchCase::IgnoreCase))
	{
		return WITH_UEAI_NIAGARA != 0;
	}
	if (FeatureName.Equals(TEXT("Water"), ESearchCase::IgnoreCase))
	{
		return WITH_UEAI_WATER != 0;
	}
	if (FeatureName.Equals(TEXT("PCG"), ESearchCase::IgnoreCase))
	{
		return WITH_UEAI_PCG != 0;
	}
	return false;
}

TArray<FString> GetCapabilityUnavailableReasons(
	const TSharedPtr<FJsonObject>& Descriptor)
{
	TArray<FString> Reasons;
	const TSharedPtr<FJsonObject>* Requirements = nullptr;
	if (!Descriptor.IsValid()
		|| !Descriptor->TryGetObjectField(TEXT("requires"), Requirements)
		|| !Requirements
		|| !Requirements->IsValid())
	{
		return Reasons;
	}

	if ((*Requirements)->HasTypedField<EJson::Array>(TEXT("features")))
	{
		for (const TSharedPtr<FJsonValue>& Value :
			(*Requirements)->GetArrayField(TEXT("features")))
		{
			const FString FeatureName = Value->AsString();
			if (!IsOptionalFeatureCompiled(FeatureName))
			{
				Reasons.Add(FString::Printf(TEXT("feature:%s"), *FeatureName));
			}
		}
	}

	if ((*Requirements)->HasTypedField<EJson::Array>(TEXT("plugins")))
	{
		for (const TSharedPtr<FJsonValue>& Value :
			(*Requirements)->GetArrayField(TEXT("plugins")))
		{
			const FString PluginName = Value->AsString();
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
			if (!Plugin.IsValid() || !Plugin->IsEnabled())
			{
				Reasons.Add(FString::Printf(TEXT("plugin:%s"), *PluginName));
			}
		}
	}

	if ((*Requirements)->HasTypedField<EJson::Array>(TEXT("modules")))
	{
		for (const TSharedPtr<FJsonValue>& Value :
			(*Requirements)->GetArrayField(TEXT("modules")))
		{
			const FString ModuleName = Value->AsString();
			if (!FModuleManager::Get().ModuleExists(*ModuleName))
			{
				Reasons.Add(FString::Printf(TEXT("module:%s"), *ModuleName));
			}
		}
	}

	if ((*Requirements)->HasTypedField<EJson::Array>(TEXT("platforms")))
	{
		bool bPlatformMatched = false;
		const FString PlatformName = UTF8_TO_TCHAR(FPlatformProperties::PlatformName());
		const FString IniPlatformName =
			UTF8_TO_TCHAR(FPlatformProperties::IniPlatformName());
		for (const TSharedPtr<FJsonValue>& Value :
			(*Requirements)->GetArrayField(TEXT("platforms")))
		{
			const FString RequiredPlatform = Value->AsString();
			if (RequiredPlatform.Equals(PlatformName, ESearchCase::IgnoreCase)
				|| RequiredPlatform.Equals(IniPlatformName, ESearchCase::IgnoreCase))
			{
				bPlatformMatched = true;
				break;
			}
		}
		if (!bPlatformMatched)
		{
			Reasons.Add(FString::Printf(TEXT("platform:%s"), *PlatformName));
		}
	}

	const TSharedPtr<FJsonObject>* EngineRequirement = nullptr;
	if ((*Requirements)->TryGetObjectField(TEXT("engine"), EngineRequirement)
		&& EngineRequirement
		&& EngineRequirement->IsValid())
	{
		FString MinimumText;
		FEngineVersion Minimum;
		if ((*EngineRequirement)->TryGetStringField(TEXT("min"), MinimumText)
			&& (!FEngineVersion::Parse(MinimumText, Minimum)
				|| CompareEngineVersionComponents(
					FEngineVersion::Current(), Minimum) < 0))
		{
			Reasons.Add(FString::Printf(TEXT("engineMin:%s"), *MinimumText));
		}

		FString MaximumText;
		FEngineVersion Maximum;
		if ((*EngineRequirement)->TryGetStringField(
				TEXT("maxExclusive"), MaximumText)
			&& (!FEngineVersion::Parse(MaximumText, Maximum)
				|| CompareEngineVersionComponents(
					FEngineVersion::Current(), Maximum) >= 0))
		{
			Reasons.Add(FString::Printf(
				TEXT("engineMaxExclusive:%s"), *MaximumText));
		}
	}

	Reasons.Sort();
	return Reasons;
}
}
