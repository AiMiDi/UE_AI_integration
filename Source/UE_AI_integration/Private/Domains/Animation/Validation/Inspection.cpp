#include "Domains/Animation/AnimationInspection.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

namespace UEAIAnimationInspectionValidationPrivate
{
using namespace UEAIIntegration::AnimationInspection;

FMCPToolResult ValidationAssetNotFound(const FString& Path)
{
	return FMCPToolResult::Error(
		FString::Printf(TEXT("Animation asset '%s' was not found."), *Path),
		TEXT("animation_asset_not_found"),
		404);
}

TSharedRef<FJsonObject> MakeDiff(
	const FString& Kind,
	const FString& LeftPath,
	const TSharedRef<FJsonObject>& Left,
	const FString& RightPath,
	const TSharedRef<FJsonObject>& Right)
{
	const FString LeftHash = Left->GetStringField(TEXT("snapshotHash"));
	const FString RightHash = Right->GetStringField(TEXT("snapshotHash"));
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.snapshot-diff.v1"));
	Result->SetStringField(TEXT("kind"), Kind);
	Result->SetStringField(TEXT("left"), LeftPath);
	Result->SetStringField(TEXT("right"), RightPath);
	Result->SetStringField(TEXT("leftHash"), LeftHash);
	Result->SetStringField(TEXT("rightHash"), RightHash);
	Result->SetBoolField(TEXT("identical"), LeftHash == RightHash);
	TArray<TSharedPtr<FJsonValue>> ChangedFields;
	TArray<FString> Keys;
	Left->Values.GetKeys(Keys);
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Right->Values)
	{
		Keys.AddUnique(Pair.Key);
	}
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		if (Key == TEXT("snapshotHash"))
		{
			continue;
		}
		const TSharedPtr<FJsonValue> LeftValue = Left->Values.FindRef(Key);
		const TSharedPtr<FJsonValue> RightValue = Right->Values.FindRef(Key);
		TSharedRef<FJsonObject> LeftWrapper = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> RightWrapper = MakeShared<FJsonObject>();
		LeftWrapper->SetField(
			TEXT("value"),
			LeftValue.IsValid() ? LeftValue : MakeShared<FJsonValueNull>());
		RightWrapper->SetField(
			TEXT("value"),
			RightValue.IsValid() ? RightValue : MakeShared<FJsonValueNull>());
		if (UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
				MakeShared<FJsonValueObject>(LeftWrapper))
			!= UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
				MakeShared<FJsonValueObject>(RightWrapper)))
		{
			ChangedFields.Add(MakeShared<FJsonValueString>(Key));
		}
	}
	Result->SetArrayField(TEXT("changedFields"), ChangedFields);
	Result->SetNumberField(TEXT("changedFieldCount"), ChangedFields.Num());
	return Result;
}

class FTool_AnimationBlueprintValidate final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.blueprint.validate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(LoadAsset(Path));
		if (!Blueprint)
		{
			return ValidationAssetNotFound(Path);
		}
		TArray<TSharedPtr<FJsonValue>> Findings;
		if (!Blueprint->TargetSkeleton)
		{
			AddFinding(
				Findings,
				TEXT("animation.blueprint.missing_skeleton"),
				TEXT("error"),
				TEXT("Animation Blueprint has no target Skeleton."),
				Blueprint->GetPathName());
		}

		int32 StateMachineCount = 0;
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			UAnimationStateMachineGraph* StateMachine =
				Cast<UAnimationStateMachineGraph>(Graph);
			if (!StateMachine)
			{
				continue;
			}
			++StateMachineCount;
			int32 StateCount = 0;
			for (UEdGraphNode* Node : StateMachine->Nodes)
			{
				if (UAnimStateNode* State = Cast<UAnimStateNode>(Node))
				{
					++StateCount;
					if (!State->GetBoundGraph())
					{
						AddFinding(
							Findings,
							TEXT("animation.state.missing_graph"),
							TEXT("error"),
							FString::Printf(
								TEXT("State '%s' has no bound graph."),
								*State->GetStateName()),
							Blueprint->GetPathName(),
							StateMachine->GetName(),
							Node->NodeGuid.ToString());
					}
				}
				else if (UAnimStateTransitionNode* Transition =
					Cast<UAnimStateTransitionNode>(Node))
				{
					if (!Transition->GetPreviousState()
						|| !Transition->GetNextState())
					{
						AddFinding(
							Findings,
							TEXT("animation.transition.invalid_endpoint"),
							TEXT("error"),
							TEXT("Transition is missing a source or destination state."),
							Blueprint->GetPathName(),
							StateMachine->GetName(),
							Node->NodeGuid.ToString());
					}
					if (!Transition->GetBoundGraph())
					{
						AddFinding(
							Findings,
							TEXT("animation.transition.missing_rule_graph"),
							TEXT("error"),
							TEXT("Transition has no rule graph."),
							Blueprint->GetPathName(),
							StateMachine->GetName(),
							Node->NodeGuid.ToString());
					}
				}
			}
			if (StateCount == 0)
			{
				AddFinding(
					Findings,
					TEXT("animation.state_machine.empty"),
					TEXT("warning"),
					FString::Printf(
						TEXT("State machine '%s' has no states."),
						*StateMachine->GetName()),
					Blueprint->GetPathName(),
					StateMachine->GetName());
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), Blueprint->GetPathName());
		Result->SetBoolField(TEXT("valid"), Findings.IsEmpty());
		Result->SetNumberField(TEXT("stateMachineCount"), StateMachineCount);
		Result->SetNumberField(TEXT("findingCount"), Findings.Num());
		Result->SetArrayField(TEXT("findings"), Findings);
		Result->SetObjectField(TEXT("snapshot"), DescribeAnimBlueprint(Blueprint));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_AnimationBlueprintDiff final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.blueprint.diff"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString LeftPath;
		FString RightPath;
		Params->TryGetStringField(TEXT("left"), LeftPath);
		Params->TryGetStringField(TEXT("right"), RightPath);
		UAnimBlueprint* Left =
			Cast<UAnimBlueprint>(LoadAsset(LeftPath));
		UAnimBlueprint* Right =
			Cast<UAnimBlueprint>(LoadAsset(RightPath));
		if (!Left)
		{
			return ValidationAssetNotFound(LeftPath);
		}
		if (!Right)
		{
			return ValidationAssetNotFound(RightPath);
		}
		return FMCPToolResult::Ok(MakeDiff(
			TEXT("animationBlueprint"),
			LeftPath,
			DescribeAnimBlueprint(Left),
			RightPath,
			DescribeAnimBlueprint(Right)));
	}
};

class FTool_AnimationBlendSpaceValidate final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.blend_space.validate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UBlendSpace* BlendSpace = Cast<UBlendSpace>(LoadAsset(Path));
		if (!BlendSpace)
		{
			return ValidationAssetNotFound(Path);
		}
		TArray<TSharedPtr<FJsonValue>> Findings;
		for (int32 AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
		{
			const FBlendParameter& Axis =
				BlendSpace->GetBlendParameter(AxisIndex);
			if (Axis.Min >= Axis.Max)
			{
				AddFinding(
					Findings,
					TEXT("animation.blend_space.invalid_axis_range"),
					TEXT("error"),
					FString::Printf(
						TEXT("Axis %d minimum must be less than maximum."),
						AxisIndex),
					BlendSpace->GetPathName());
			}
		}
		const FBlendParameter& XAxis = BlendSpace->GetBlendParameter(0);
		const FBlendParameter& YAxis = BlendSpace->GetBlendParameter(1);
		int32 SampleIndex = 0;
		for (const FBlendSample& Sample : BlendSpace->GetBlendSamples())
		{
			const FString SampleLocation =
				FString::Printf(TEXT("sample/%d"), SampleIndex++);
			if (!Sample.Animation)
			{
				AddFinding(
					Findings,
					TEXT("animation.blend_space.missing_animation"),
					TEXT("error"),
					TEXT("Blend Space sample has no animation."),
					BlendSpace->GetPathName(),
					TEXT("samples"),
					SampleLocation);
				continue;
			}
			if (Sample.SampleValue.X < XAxis.Min
				|| Sample.SampleValue.X > XAxis.Max
				|| Sample.SampleValue.Y < YAxis.Min
				|| Sample.SampleValue.Y > YAxis.Max)
			{
				AddFinding(
					Findings,
					TEXT("animation.blend_space.sample_out_of_range"),
					TEXT("error"),
					TEXT("Blend Space sample lies outside the configured axes."),
					BlendSpace->GetPathName(),
					TEXT("samples"),
					SampleLocation);
			}
			if (BlendSpace->GetSkeleton()
				&& Sample.Animation->GetSkeleton()
				&& BlendSpace->GetSkeleton()
					!= Sample.Animation->GetSkeleton())
			{
				AddFinding(
					Findings,
					TEXT("animation.blend_space.skeleton_mismatch"),
					TEXT("error"),
					TEXT("Sample animation uses a different Skeleton."),
					BlendSpace->GetPathName(),
					TEXT("samples"),
					SampleLocation);
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), BlendSpace->GetPathName());
		Result->SetBoolField(TEXT("valid"), Findings.IsEmpty());
		Result->SetNumberField(TEXT("findingCount"), Findings.Num());
		Result->SetArrayField(TEXT("findings"), Findings);
		Result->SetObjectField(TEXT("snapshot"), DescribeBlendSpace(BlendSpace));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_AnimationBlendSpaceDiff final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.blend_space.diff"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString LeftPath;
		FString RightPath;
		Params->TryGetStringField(TEXT("left"), LeftPath);
		Params->TryGetStringField(TEXT("right"), RightPath);
		UBlendSpace* Left = Cast<UBlendSpace>(LoadAsset(LeftPath));
		UBlendSpace* Right = Cast<UBlendSpace>(LoadAsset(RightPath));
		if (!Left)
		{
			return ValidationAssetNotFound(LeftPath);
		}
		if (!Right)
		{
			return ValidationAssetNotFound(RightPath);
		}
		return FMCPToolResult::Ok(MakeDiff(
			TEXT("blendSpace"),
			LeftPath,
			DescribeBlendSpace(Left),
			RightPath,
			DescribeBlendSpace(Right)));
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterAnimationInspectionValidationTools(FMCPToolRegistry& Registry)
{
	using namespace UEAIAnimationInspectionValidationPrivate;
	Registry.Register(MakeShared<FTool_AnimationBlueprintValidate>());
	Registry.Register(MakeShared<FTool_AnimationBlueprintDiff>());
	Registry.Register(MakeShared<FTool_AnimationBlendSpaceValidate>());
	Registry.Register(MakeShared<FTool_AnimationBlendSpaceDiff>());
}
}
