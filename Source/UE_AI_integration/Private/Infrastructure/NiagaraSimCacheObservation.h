#pragma once

#include "Tools/MCPToolBase.h"

namespace UEAINiagaraSimCachePrivate
{
// UE 5.4 ReadAttribute returns component-major arrays, despite the header's AoS comment.
// Keep conversion independent of the recorder so mixed types and pagination can be tested.
FMCPToolResult MakeAttributePage(const FString& Type, int32 Instances, int32 FloatCount,
	int32 HalfCount, int32 IntCount, const TArray<float>& Floats, const TArray<FFloat16>& Halfs,
	const TArray<int32>& Ints, int32 Offset, int32 Limit);
}
