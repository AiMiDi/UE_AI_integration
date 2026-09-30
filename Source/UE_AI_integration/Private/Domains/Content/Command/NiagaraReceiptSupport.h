#pragma once

#include "CoreMinimal.h"
#include "UObject/Package.h"

namespace UEAINiagaraReceiptSupport
{
constexpr int32 MaxActiveReceiptCount = 256;
constexpr int32 MaxTerminalRequestHistory = 4096;

struct FReleasedRequest
{
	FString ReceiptId;
	FString RequestDigest;
};

class FReleasedRequestHistory
{
public:
	const FReleasedRequest* Find(const FString& RequestId) const
	{
		return Entries.Find(RequestId);
	}

	void Remember(const FString& RequestId, const FString& ReceiptId, const FString& RequestDigest)
	{
		if (Entries.Contains(RequestId))
		{
			TerminalOrder.RemoveSingle(RequestId);
		}

		FReleasedRequest Released;
		Released.ReceiptId = ReceiptId;
		Released.RequestDigest = RequestDigest;
		Entries.Add(RequestId, MoveTemp(Released));
		TerminalOrder.Add(RequestId);
		while (TerminalOrder.Num() > MaxTerminalRequestHistory)
		{
			const FString OldestRequestId = TerminalOrder[0];
			TerminalOrder.RemoveAt(0, 1, EAllowShrinking::No);
			Entries.Remove(OldestRequestId);
		}
	}

private:
	TMap<FString, FReleasedRequest> Entries;
	TArray<FString> TerminalOrder;
};

template <typename ReceiptType>
void RestoreDirtyState(UPackage* Package, bool bBeforeDirty, bool bMayClearCleanDirty, ReceiptType& Receipt)
{
	Receipt.bDirtyRestored = false;
	Receipt.bDirtyPreserved = false;
	Receipt.bTrackedStateRestored = false;
	Receipt.bFullPackageStateRestored = false;
	if (!Package)
	{
		return;
	}

	if (bBeforeDirty)
	{
		if (!Package->IsDirty())
		{
			Package->SetDirtyFlag(true);
		}
		Receipt.bDirtyRestored = Package->IsDirty();
	}
	else if (!Package->IsDirty())
	{
		Receipt.bDirtyRestored = true;
	}
	else if (bMayClearCleanDirty)
	{
		// Only a same-call failure recovery can attribute this clean-to-dirty
		// transition to itself. A later rollback must preserve package dirtiness.
		Package->SetDirtyFlag(false);
		Receipt.bDirtyRestored = !Package->IsDirty();
	}
	else
	{
		Receipt.bDirtyPreserved = true;
	}
	Receipt.bTrackedStateRestored = Receipt.bSemanticRollbackVerified && Receipt.bDirtyRestored;
	// No package fingerprint is captured. Matching the owned semantic state and
	// dirty bit cannot prove that other objects in the package were restored.
	Receipt.bFullPackageStateRestored = false;
}
}
