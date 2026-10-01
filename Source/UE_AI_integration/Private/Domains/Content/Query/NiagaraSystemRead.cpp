// Bounded authored Niagara System inspection. This intentionally reports
// configuration and compile-request state, not runtime particle execution.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "NiagaraEmitter.h"
#include "NiagaraEffectType.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace UEAINiagaraSystemRead
{
	using UEAIIntegration::Infrastructure::TryDigestJson;

	constexpr int32 MaxPathCharacters = 2048;
	constexpr int32 MaxOffset = 65536;
	constexpr int32 MaxPageSize = 128;
	constexpr int32 MaxRenderersPerEmitter = 128;
	constexpr int32 MaxExecutionContextsPerEmitter = 128;

	FMCPToolResult Error(const FString& Message, const TCHAR* Code = TEXT("invalid_niagara_system_request"),
	                     int32 Status = 422)
	{
		return FMCPToolResult::Error(Message, Code, Status);
	}

	bool ReadIndex(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Name,
		int32 DefaultValue,
		int32 Maximum,
		int32& OutValue)
	{
		double Number = DefaultValue;
		if (Params->HasField(Name) && !Params->TryGetNumberField(Name, Number))
		{
			return false;
		}
		if (!FMath::IsFinite(Number) || Number < 0.0 || Number > Maximum || Number != FMath::FloorToDouble(Number))
		{
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	FMCPToolResult LoadSystem(const TSharedPtr<FJsonObject>& Params, UNiagaraSystem*& OutSystem)
	{
		FString ObjectPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system"), ObjectPath)
			|| ObjectPath.IsEmpty() || ObjectPath.Len() > MaxPathCharacters)
		{
			return Error(TEXT("system must be an exact Niagara System package or object path."));
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return Error(TEXT("system must be a valid long package or object path."));
		}
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		}
		OutSystem = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		return OutSystem
			       ? FMCPToolResult::Ok(nullptr)
			       : Error(TEXT("The Niagara System was not found."), TEXT("system_not_found"), 404);
	}

	TSharedPtr<FJsonValue> FiniteNumber(double Value)
	{
		if (FMath::IsFinite(Value))
		{
			return MakeShared<FJsonValueNumber>(Value);
		}
		return MakeShared<FJsonValueNull>();
	}

	TSharedPtr<FJsonValue> ParameterValue(
		const FNiagaraVariable& Variable,
		const FNiagaraParameterStore& Store,
		bool& bOutSupported)
	{
		bOutSupported = true;
		const FNiagaraTypeDefinition& Type = Variable.GetType();
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return FiniteNumber(Store.GetParameterValue<float>(Variable));
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return MakeShared<FJsonValueNumber>(Store.GetParameterValue<int32>(Variable));
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			const FNiagaraBool Value = Store.GetParameterValue<FNiagaraBool>(Variable);
			if (Value.IsValid())
			{
				return MakeShared<FJsonValueBoolean>(Value.GetValue());
			}
			return MakeShared<FJsonValueNull>();
		}
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			const FVector2f Value = Store.GetParameterValue<FVector2f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			const FVector3f Value = Store.GetParameterValue<FVector3f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), FiniteNumber(Value.Z));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			const FVector4f Value = Store.GetParameterValue<FVector4f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), FiniteNumber(Value.Z));
			Json->SetField(TEXT("w"), FiniteNumber(Value.W));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetQuatDef())
		{
			const FQuat4f Value = Store.GetParameterValue<FQuat4f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), FiniteNumber(Value.Z));
			Json->SetField(TEXT("w"), FiniteNumber(Value.W));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const FLinearColor Value = Store.GetParameterValue<FLinearColor>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("r"), FiniteNumber(Value.R));
			Json->SetField(TEXT("g"), FiniteNumber(Value.G));
			Json->SetField(TEXT("b"), FiniteNumber(Value.B));
			Json->SetField(TEXT("a"), FiniteNumber(Value.A));
			return MakeShared<FJsonValueObject>(Json);
		}
		bOutSupported = false;
		return MakeShared<FJsonValueNull>();
	}

	TSharedRef<FJsonObject> BoundsJson(const FBox& Bounds)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("valid"), Bounds.IsValid != 0);
		if (Bounds.IsValid)
		{
			auto Minimum = MakeShared<FJsonObject>();
			Minimum->SetField(TEXT("x"), FiniteNumber(Bounds.Min.X));
			Minimum->SetField(TEXT("y"), FiniteNumber(Bounds.Min.Y));
			Minimum->SetField(TEXT("z"), FiniteNumber(Bounds.Min.Z));
			auto Maximum = MakeShared<FJsonObject>();
			Maximum->SetField(TEXT("x"), FiniteNumber(Bounds.Max.X));
			Maximum->SetField(TEXT("y"), FiniteNumber(Bounds.Max.Y));
			Maximum->SetField(TEXT("z"), FiniteNumber(Bounds.Max.Z));
			Json->SetObjectField(TEXT("min"), Minimum);
			Json->SetObjectField(TEXT("max"), Maximum);
		}
		return Json;
	}

	class FSystemInspect final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.system.inspect");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			UNiagaraSystem* System = nullptr;
			FMCPToolResult Result = LoadSystem(Params, System);
			if (!Result.bSuccess)
			{
				return Result;
			}

			int32 EmitterOffset = 0;
			int32 EmitterLimit = 32;
			int32 ParameterOffset = 0;
			int32 ParameterLimit = 64;
			int32 RendererLimit = 32;
			int32 EventHandlerOffset = 0;
			int32 EventHandlerLimit = 32;
			int32 SimulationStageOffset = 0;
			int32 SimulationStageLimit = 32;
			if (!ReadIndex(Params, TEXT("emitterOffset"), 0, MaxOffset, EmitterOffset)
				|| !ReadIndex(Params, TEXT("emitterLimit"), 32, MaxPageSize, EmitterLimit) || EmitterLimit == 0
				|| !ReadIndex(Params, TEXT("parameterOffset"), 0, MaxOffset, ParameterOffset)
				|| !ReadIndex(Params, TEXT("parameterLimit"), 64, MaxPageSize, ParameterLimit) || ParameterLimit == 0
				|| !ReadIndex(Params, TEXT("rendererLimit"), 32, MaxRenderersPerEmitter, RendererLimit) || RendererLimit == 0
				|| !ReadIndex(Params, TEXT("eventHandlerOffset"), 0, MaxOffset, EventHandlerOffset)
				|| !ReadIndex(Params, TEXT("eventHandlerLimit"), 32, MaxExecutionContextsPerEmitter, EventHandlerLimit)
				|| EventHandlerLimit == 0
				|| !ReadIndex(Params, TEXT("simulationStageOffset"), 0, MaxOffset, SimulationStageOffset)
				|| !ReadIndex(Params, TEXT("simulationStageLimit"), 32, MaxExecutionContextsPerEmitter, SimulationStageLimit)
				|| SimulationStageLimit == 0)
			{
				return Error(TEXT(
					"Offsets must be 0..65536; emitterLimit, parameterLimit, rendererLimit, eventHandlerLimit and simulationStageLimit must be 1..128."));
			}

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-inspection.v1"));
			Json->SetStringField(TEXT("system"), System->GetPathName());
			Json->SetStringField(TEXT("package"), System->GetOutermost()->GetName());
			Json->SetBoolField(TEXT("dirty"), System->GetOutermost()->IsDirty());
			Json->SetBoolField(TEXT("readyToRun"), System->IsReadyToRun());
			Json->SetBoolField(TEXT("compileOutstanding"), System->HasOutstandingCompilationRequests(true));
			Json->SetNumberField(TEXT("warmupTime"), System->GetWarmupTime());
			Json->SetNumberField(TEXT("warmupTickCount"), System->GetWarmupTickCount());
			Json->SetNumberField(TEXT("warmupTickDelta"), System->GetWarmupTickDelta());
			Json->SetStringField(
				TEXT("effectType"), System->GetEffectType() ? System->GetEffectType()->GetPathName() : FString());
			Json->SetObjectField(TEXT("fixedBounds"), BoundsJson(System->GetFixedBounds()));

			const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
			TArray<TSharedPtr<FJsonValue>> Emitters;
			const int32 EmitterEnd = FMath::Min(Handles.Num(), EmitterOffset + EmitterLimit);
			for (int32 EmitterIndex = EmitterOffset; EmitterIndex < EmitterEnd; ++EmitterIndex)
			{
				const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
				auto Emitter = MakeShared<FJsonObject>();
				Emitter->SetNumberField(TEXT("index"), EmitterIndex);
				Emitter->SetStringField(TEXT("name"), Handle.GetName().ToString());
				Emitter->SetStringField(
					TEXT("handleId"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
				Emitter->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
				Emitter->SetStringField(
					TEXT("version"), Handle.GetInstance().Version.ToString(EGuidFormats::DigitsWithHyphensLower));
				Emitter->SetStringField(TEXT("emitterObject"), Handle.GetInstance().Emitter
					                                               ? Handle.GetInstance().Emitter->GetPathName()
					                                               : FString());
				Emitter->SetBoolField(TEXT("ownedBySystemPackage"), Handle.GetInstance().Emitter
				                      && Handle.GetInstance().Emitter->GetOutermost() == System->GetOutermost());

				const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
				Emitter->SetBoolField(TEXT("dataAvailable"), Data != nullptr);
				if (Data)
				{
					Emitter->SetStringField(
						TEXT("simTarget"),
						StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue(static_cast<int64>(Data->SimTarget)));
					const TArray<UNiagaraRendererProperties*>& Renderers = Data->GetRenderers();
					Emitter->SetNumberField(TEXT("rendererTotal"), Renderers.Num());
					TArray<TSharedPtr<FJsonValue>> RendererRows;
					const int32 RendererEnd = FMath::Min(Renderers.Num(), RendererLimit);
					for (int32 RendererIndex = 0; RendererIndex < RendererEnd; ++RendererIndex)
					{
						UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
						auto RendererJson = MakeShared<FJsonObject>();
						RendererJson->SetNumberField(TEXT("index"), RendererIndex);
						RendererJson->SetStringField(TEXT("path"), Renderer ? Renderer->GetPathName() : FString());
						RendererJson->SetStringField(
							TEXT("class"), Renderer ? Renderer->GetClass()->GetPathName() : FString());
						RendererJson->SetBoolField(TEXT("valid"), IsValid(Renderer));
						RendererJson->SetBoolField(TEXT("enabled"), IsValid(Renderer) && Renderer->GetIsEnabled());
						RendererRows.Add(MakeShared<FJsonValueObject>(RendererJson));
					}
					Emitter->SetArrayField(TEXT("renderers"), RendererRows);
					Emitter->SetBoolField(TEXT("renderersTruncated"), RendererEnd < Renderers.Num());

					// Event handlers and simulation stages are authored execution
					// contexts. Keep this inventory bounded and report truncation so
					// large emitters cannot produce an unbounded response.
					const TArray<FNiagaraEventScriptProperties>& EventHandlers = Data->GetEventHandlers();
					TArray<TSharedPtr<FJsonValue>> EventHandlerRows;
					const int32 EventHandlerEnd = FMath::Min(
						EventHandlers.Num(), EventHandlerOffset + EventHandlerLimit);
					for (int32 EventIndex = EventHandlerOffset; EventIndex < EventHandlerEnd; ++EventIndex)
					{
						const FNiagaraEventScriptProperties& EventHandler = EventHandlers[EventIndex];
						auto EventJson = MakeShared<FJsonObject>();
						EventJson->SetNumberField(TEXT("index"), EventIndex);
						EventJson->SetStringField(
							TEXT("script"),
							EventHandler.Script ? EventHandler.Script->GetPathName() : FString());
						EventJson->SetStringField(
							TEXT("scriptUsageId"),
							EventHandler.Script
								? EventHandler.Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower)
								: FString());
						EventJson->SetStringField(
							TEXT("executionMode"),
							StaticEnum<EScriptExecutionMode>()->GetNameStringByValue(
								static_cast<int64>(EventHandler.ExecutionMode)));
						EventJson->SetNumberField(TEXT("spawnNumber"), EventHandler.SpawnNumber);
						EventJson->SetNumberField(TEXT("minSpawnNumber"), EventHandler.MinSpawnNumber);
						EventJson->SetNumberField(TEXT("maxEventsPerFrame"), EventHandler.MaxEventsPerFrame);
						EventJson->SetBoolField(TEXT("randomSpawnNumber"), EventHandler.bRandomSpawnNumber);
						EventJson->SetBoolField(
							TEXT("updateAttributeInitialValues"),
							EventHandler.UpdateAttributeInitialValues);
						EventJson->SetStringField(
							TEXT("sourceEmitterId"),
							EventHandler.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower));
						EventJson->SetStringField(
							TEXT("sourceEventName"),
							EventHandler.SourceEventName.ToString());
						EventJson->SetBoolField(TEXT("scriptAvailable"), IsValid(EventHandler.Script));
						EventHandlerRows.Add(MakeShared<FJsonValueObject>(EventJson));
					}
					Emitter->SetArrayField(TEXT("eventHandlers"), EventHandlerRows);
					Emitter->SetNumberField(TEXT("eventHandlerTotal"), EventHandlers.Num());
					Emitter->SetNumberField(TEXT("eventHandlerOffset"), EventHandlerOffset);
					Emitter->SetNumberField(TEXT("eventHandlerLimit"), EventHandlerLimit);
					Emitter->SetBoolField(
						TEXT("eventHandlersHasMore"), EventHandlerEnd < EventHandlers.Num());
					Emitter->SetNumberField(TEXT("eventHandlersNextOffset"), EventHandlerEnd);
					Emitter->SetBoolField(
						TEXT("eventHandlersTruncated"),
						EventHandlerEnd < EventHandlers.Num());

					const TArray<UNiagaraSimulationStageBase*>& SimulationStages = Data->GetSimulationStages();
					TArray<TSharedPtr<FJsonValue>> SimulationStageRows;
					const int32 SimulationStageEnd = FMath::Min(
						SimulationStages.Num(), SimulationStageOffset + SimulationStageLimit);
					for (int32 StageIndex = SimulationStageOffset; StageIndex < SimulationStageEnd; ++StageIndex)
					{
						const UNiagaraSimulationStageBase* Stage = SimulationStages[StageIndex];
						auto StageJson = MakeShared<FJsonObject>();
						StageJson->SetNumberField(TEXT("index"), StageIndex);
						StageJson->SetStringField(TEXT("path"), Stage ? Stage->GetPathName() : FString());
						StageJson->SetStringField(
							TEXT("class"),
							Stage ? Stage->GetClass()->GetPathName() : FString());
						StageJson->SetStringField(
							TEXT("name"),
							Stage ? Stage->SimulationStageName.ToString() : FString());
						StageJson->SetBoolField(TEXT("enabled"), Stage && Stage->bEnabled != 0);
						StageJson->SetStringField(
							TEXT("script"),
							Stage && Stage->Script ? Stage->Script->GetPathName() : FString());
						StageJson->SetStringField(
							TEXT("scriptUsageId"),
							Stage && Stage->Script
								? Stage->Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower)
								: FString());
						StageJson->SetBoolField(
							TEXT("scriptAvailable"),
							Stage && IsValid(Stage->Script));
						SimulationStageRows.Add(MakeShared<FJsonValueObject>(StageJson));
					}
					Emitter->SetArrayField(TEXT("simulationStages"), SimulationStageRows);
					Emitter->SetNumberField(TEXT("simulationStageTotal"), SimulationStages.Num());
					Emitter->SetNumberField(TEXT("simulationStageOffset"), SimulationStageOffset);
					Emitter->SetNumberField(TEXT("simulationStageLimit"), SimulationStageLimit);
					Emitter->SetBoolField(
						TEXT("simulationStagesHasMore"), SimulationStageEnd < SimulationStages.Num());
					Emitter->SetNumberField(TEXT("simulationStagesNextOffset"), SimulationStageEnd);
					Emitter->SetBoolField(
						TEXT("simulationStagesTruncated"),
						SimulationStageEnd < SimulationStages.Num());
				}
				Emitters.Add(MakeShared<FJsonValueObject>(Emitter));
			}
			Json->SetArrayField(TEXT("emitters"), Emitters);
			Json->SetNumberField(TEXT("emitterTotal"), Handles.Num());
			Json->SetNumberField(TEXT("emitterOffset"), EmitterOffset);
			Json->SetBoolField(TEXT("emittersHasMore"), EmitterEnd < Handles.Num());
			Json->SetNumberField(TEXT("emittersNextOffset"), EmitterEnd);

			const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			TArray<FNiagaraVariable> Variables;
			Variables.Reserve(Store.ReadParameterVariables().Num());
			for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
			{
				Variables.Add(Variable);
			}
			Variables.Sort([](const FNiagaraVariable& Left, const FNiagaraVariable& Right)
			{
				const FString LeftName = Left.GetName().ToString();
				const FString RightName = Right.GetName().ToString();
				if (LeftName != RightName)
				{
					return LeftName < RightName;
				}
				return Left.GetType().GetName() < Right.GetType().GetName();
			});
			TArray<TSharedPtr<FJsonValue>> Parameters;
			const int32 ParameterEnd = FMath::Min(Variables.Num(), ParameterOffset + ParameterLimit);
			for (int32 ParameterIndex = ParameterOffset; ParameterIndex < ParameterEnd; ++ParameterIndex)
			{
				const FNiagaraVariable& Variable = Variables[ParameterIndex];
				auto Parameter = MakeShared<FJsonObject>();
				Parameter->SetNumberField(TEXT("index"), ParameterIndex);
				Parameter->SetStringField(TEXT("name"), Variable.GetName().ToString());
				Parameter->SetStringField(TEXT("type"), Variable.GetType().GetName());
				Parameter->SetNumberField(TEXT("size"), Variable.GetType().GetSize());
				Parameter->SetBoolField(TEXT("dataInterface"), Variable.GetType().IsDataInterface());
				Parameter->SetBoolField(TEXT("object"), Variable.GetType().IsUObject());
				bool bValueSupported = false;
				Parameter->SetField(TEXT("value"), ParameterValue(Variable, Store, bValueSupported));
				Parameter->SetBoolField(TEXT("valueSupported"), bValueSupported);
				Parameters.Add(MakeShared<FJsonValueObject>(Parameter));
			}
			Json->SetArrayField(TEXT("userParameters"), Parameters);
			Json->SetNumberField(TEXT("userParameterTotal"), Variables.Num());
			Json->SetNumberField(TEXT("parameterOffset"), ParameterOffset);
			Json->SetBoolField(TEXT("parametersHasMore"), ParameterEnd < Variables.Num());
			Json->SetNumberField(TEXT("parametersNextOffset"), ParameterEnd);
			Json->SetStringField(
				TEXT("scope"),
				TEXT(
					"authored System configuration; loaded component overrides and runtime execution are not inspected"));

			FString StateDigest;
			if (!TryDigestJson(Json, StateDigest))
			{
				return Error(TEXT("Could not hash the bounded System inspection."), TEXT("digest_unavailable"), 500);
			}
			Json->SetStringField(TEXT("stateDigest"), StateDigest);
			return FMCPToolResult::Ok(Json);
		}
	};
}
#endif

namespace UEAIIntegrationTools
{
	void RegisterNiagaraSystemReadTools(FMCPToolRegistry& Registry)
	{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
		Registry.Register(MakeShared<UEAINiagaraSystemRead::FSystemInspect>());
#else
		class FUnavailableNiagaraSystemRead final : public FMCPToolBase
		{
		public:
			explicit FUnavailableNiagaraSystemRead(const TCHAR* InId) : Id(InId)
			{
			}

			FString GetCapabilityId() const override { return Id; }

			FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
			{
				return FMCPToolResult::Error(
					TEXT("Niagara editor inspection support is unavailable in this build."),
					TEXT("capability_unavailable"),
					409);
			}

		private:
			FString Id;
		};
		Registry.Register(MakeShared<FUnavailableNiagaraSystemRead>(TEXT("content.niagara.system.inspect")));
#endif
	}
}
