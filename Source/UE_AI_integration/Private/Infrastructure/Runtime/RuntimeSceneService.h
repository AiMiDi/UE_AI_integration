#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;
class UWorld;

namespace UEAIIntegration::Infrastructure
{
struct FRuntimeServiceResult
{
	bool bSuccess = true;
	TSharedPtr<FJsonObject> Data;
	FString ErrorCode;
	FString ErrorMessage;
	int32 HttpStatus = 200;

	static FRuntimeServiceResult Ok(const TSharedPtr<FJsonObject>& InData);
	static FRuntimeServiceResult Error(
		const FString& InCode,
		const FString& InMessage,
		int32 InHttpStatus);
};

/**
 * Session-scoped UObject registry and reflection facade used by Scene Runtime
 * handlers and by higher-level Scenario runners.
 *
 * Handles never expose raw object paths as identity. Each object is retained
 * only through TWeakObjectPtr and can be resolved only in the PIE generation
 * in which it was created.
 */
class FRuntimeSceneService
{
public:
	FRuntimeSceneService();
	~FRuntimeSceneService();

	void PrepareNextSession();
	void CancelPreparedSession();
	void BeginSession();
	void EndSession();
	void SetPaused(bool bInPaused);

	const FString& GetSessionId() const;
	uint64 GetGeneration() const;
	bool IsSessionActive() const;
	bool IsPaused() const;

	FRuntimeServiceResult ListWorldContexts(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult FindObjects(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult GetObject(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult SetObject(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult CallObject(const TSharedPtr<FJsonObject>& Params);

	FRuntimeServiceResult GetWidgetTree(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult GetWidgetState(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult HitTestWidget(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult FocusWidget(const TSharedPtr<FJsonObject>& Params);

	FRuntimeServiceResult ListDelegates(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult BindDelegate(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult UnbindDelegate(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult IsDelegateBound(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult BroadcastDelegate(const TSharedPtr<FJsonObject>& Params);

	FRuntimeServiceResult PointerInput(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult KeyInput(const TSharedPtr<FJsonObject>& Params);
	FRuntimeServiceResult SetInputMode(const TSharedPtr<FJsonObject>& Params);

	/** Resolve a public objectRef. Intended for Scenario composition. */
	FRuntimeServiceResult ResolveObjectRef(
		const TSharedPtr<FJsonObject>& ObjectRef,
		UObject*& OutObject) const;

	/** Create or reuse a stable handle for an object in the active session. */
	FRuntimeServiceResult MakeObjectRef(
		UObject* Object,
		TSharedPtr<FJsonObject>& OutObjectRef);

private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
}
