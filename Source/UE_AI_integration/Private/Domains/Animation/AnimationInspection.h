#pragma once

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegration::AnimationInspection
{
inline UObject* LoadAsset(const FString& Path)
{
	return StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
}

inline TSharedRef<FJsonObject> DescribeStateMachine(
	UAnimationStateMachineGraph* Graph)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Graph->GetName());
	Result->SetStringField(TEXT("path"), Graph->GetPathName());

	TArray<TSharedPtr<FJsonValue>> States;
	TArray<TSharedPtr<FJsonValue>> Transitions;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UAnimStateNode* State = Cast<UAnimStateNode>(Node))
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), State->GetStateName());
			Item->SetStringField(TEXT("nodeGuid"), State->NodeGuid.ToString());
			Item->SetBoolField(TEXT("hasBoundGraph"), State->GetBoundGraph() != nullptr);
			Item->SetNumberField(
				TEXT("boundGraphNodeCount"),
				State->GetBoundGraph() ? State->GetBoundGraph()->Nodes.Num() : 0);
			States.Add(MakeShared<FJsonValueObject>(Item));
		}
		else if (UAnimStateTransitionNode* Transition =
			Cast<UAnimStateTransitionNode>(Node))
		{
			UAnimStateNode* Previous =
				Cast<UAnimStateNode>(Transition->GetPreviousState());
			UAnimStateNode* Next =
				Cast<UAnimStateNode>(Transition->GetNextState());
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(
				TEXT("from"),
				Previous ? Previous->GetStateName() : TEXT(""));
			Item->SetStringField(
				TEXT("to"),
				Next ? Next->GetStateName() : TEXT(""));
			Item->SetStringField(
				TEXT("nodeGuid"),
				Transition->NodeGuid.ToString());
			Item->SetBoolField(
				TEXT("hasRuleGraph"),
				Transition->GetBoundGraph() != nullptr);
			Transitions.Add(MakeShared<FJsonValueObject>(Item));
		}
	}
	States.Sort(
		[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("name"))
				< B->AsObject()->GetStringField(TEXT("name"));
		});
	Transitions.Sort(
		[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const FString AKey =
				A->AsObject()->GetStringField(TEXT("from"))
				+ TEXT("->")
				+ A->AsObject()->GetStringField(TEXT("to"));
			const FString BKey =
				B->AsObject()->GetStringField(TEXT("from"))
				+ TEXT("->")
				+ B->AsObject()->GetStringField(TEXT("to"));
			return AKey < BKey;
		});
	Result->SetArrayField(TEXT("states"), States);
	Result->SetArrayField(TEXT("transitions"), Transitions);
	Result->SetNumberField(TEXT("stateCount"), States.Num());
	Result->SetNumberField(TEXT("transitionCount"), Transitions.Num());
	return Result;
}

inline TSharedRef<FJsonObject> DescribeAnimBlueprint(UAnimBlueprint* Blueprint)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.animation-blueprint.snapshot.v1"));
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("path"), Blueprint->GetPathName());
	Result->SetStringField(
		TEXT("skeleton"),
		Blueprint->TargetSkeleton
			? Blueprint->TargetSkeleton->GetPathName()
			: TEXT(""));
	Result->SetStringField(
		TEXT("parentClass"),
		Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	TArray<TSharedPtr<FJsonValue>> GraphItems;
	TArray<TSharedPtr<FJsonValue>> StateMachines;
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Graph->GetName());
		Item->SetStringField(TEXT("class"), Graph->GetClass()->GetPathName());
		Item->SetNumberField(TEXT("nodeCount"), Graph->Nodes.Num());
		GraphItems.Add(MakeShared<FJsonValueObject>(Item));
		if (UAnimationStateMachineGraph* StateMachine =
			Cast<UAnimationStateMachineGraph>(Graph))
		{
			StateMachines.Add(MakeShared<FJsonValueObject>(
				DescribeStateMachine(StateMachine)));
		}
	}
	GraphItems.Sort(
		[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("name"))
				< B->AsObject()->GetStringField(TEXT("name"));
		});
	StateMachines.Sort(
		[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("name"))
				< B->AsObject()->GetStringField(TEXT("name"));
		});
	Result->SetArrayField(TEXT("graphs"), GraphItems);
	Result->SetArrayField(TEXT("stateMachines"), StateMachines);
	Result->SetNumberField(TEXT("graphCount"), GraphItems.Num());
	Result->SetNumberField(TEXT("stateMachineCount"), StateMachines.Num());

	const FString SnapshotHash = Infrastructure::DigestJson(Result);
	if (!SnapshotHash.IsEmpty())
	{
		Result->SetStringField(TEXT("snapshotHash"), SnapshotHash);
	}
	return Result;
}

inline TSharedRef<FJsonObject> DescribeBlendSpace(UBlendSpace* BlendSpace)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.blend-space.snapshot.v1"));
	Result->SetStringField(TEXT("name"), BlendSpace->GetName());
	Result->SetStringField(TEXT("path"), BlendSpace->GetPathName());
	Result->SetStringField(
		TEXT("skeleton"),
		BlendSpace->GetSkeleton()
			? BlendSpace->GetSkeleton()->GetPathName()
			: TEXT(""));

	TArray<TSharedPtr<FJsonValue>> Axes;
	for (int32 AxisIndex = 0; AxisIndex < 2; ++AxisIndex)
	{
		const FBlendParameter& Axis = BlendSpace->GetBlendParameter(AxisIndex);
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Axis.DisplayName);
		Item->SetNumberField(TEXT("min"), Axis.Min);
		Item->SetNumberField(TEXT("max"), Axis.Max);
		Item->SetNumberField(TEXT("gridDivisions"), Axis.GridNum);
		Item->SetBoolField(TEXT("wrapInput"), Axis.bWrapInput);
		Axes.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("axes"), Axes);

	TArray<TSharedPtr<FJsonValue>> Samples;
	for (const FBlendSample& Sample : BlendSpace->GetBlendSamples())
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(
			TEXT("animation"),
			Sample.Animation ? Sample.Animation->GetPathName() : TEXT(""));
		Item->SetNumberField(TEXT("x"), Sample.SampleValue.X);
		Item->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
		Item->SetNumberField(TEXT("rateScale"), Sample.RateScale);
		Samples.Add(MakeShared<FJsonValueObject>(Item));
	}
	Samples.Sort(
		[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			const TSharedPtr<FJsonObject> AO = A->AsObject();
			const TSharedPtr<FJsonObject> BO = B->AsObject();
			const FString AKey = FString::Printf(
				TEXT("%s|%.9g|%.9g"),
				*AO->GetStringField(TEXT("animation")),
				AO->GetNumberField(TEXT("x")),
				AO->GetNumberField(TEXT("y")));
			const FString BKey = FString::Printf(
				TEXT("%s|%.9g|%.9g"),
				*BO->GetStringField(TEXT("animation")),
				BO->GetNumberField(TEXT("x")),
				BO->GetNumberField(TEXT("y")));
			return AKey < BKey;
		});
	Result->SetArrayField(TEXT("samples"), Samples);
	Result->SetNumberField(TEXT("sampleCount"), Samples.Num());
	const FString SnapshotHash = Infrastructure::DigestJson(Result);
	if (!SnapshotHash.IsEmpty())
	{
		Result->SetStringField(TEXT("snapshotHash"), SnapshotHash);
	}
	return Result;
}

inline TSharedRef<FJsonObject> DescribeAnimationAsset(UAnimationAsset* Asset)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.animation-asset.snapshot.v1"));
	Result->SetStringField(TEXT("name"), Asset->GetName());
	Result->SetStringField(TEXT("path"), Asset->GetPathName());
	Result->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
	Result->SetStringField(
		TEXT("skeleton"),
		Asset->GetSkeleton() ? Asset->GetSkeleton()->GetPathName() : TEXT(""));
	const FString SnapshotHash = Infrastructure::DigestJson(Result);
	if (!SnapshotHash.IsEmpty())
	{
		Result->SetStringField(TEXT("snapshotHash"), SnapshotHash);
	}
	return Result;
}

inline void AddFinding(
	TArray<TSharedPtr<FJsonValue>>& Findings,
	const FString& RuleId,
	const FString& Severity,
	const FString& Message,
	const FString& AssetPath,
	const FString& GraphName = FString(),
	const FString& NodeGuid = FString())
{
	const FString ContractSeverity =
		Severity == TEXT("error")
			? TEXT("high")
			: (Severity == TEXT("warning") ? TEXT("medium") : Severity);
	Findings.Add(MakeShared<FJsonValueObject>(
		Infrastructure::MakeFinding(
			RuleId,
			ContractSeverity,
			0.95,
			AssetPath,
			GraphName,
			NodeGuid,
			Message)));
}
}
