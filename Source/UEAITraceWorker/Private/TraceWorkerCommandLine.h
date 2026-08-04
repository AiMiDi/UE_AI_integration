#pragma once

#include "CoreMinimal.h"
#include "Misc/Parse.h"

namespace UEAI::TraceWorker
{
/** Accept UE's traditional -switch and the public CLI's --long-switch form. */
inline bool HasCommandLineSwitch(
	const TCHAR* CommandLine,
	const TCHAR* Name)
{
	const FString LongName = TEXT("-") + FString(Name);
	return FParse::Param(CommandLine, Name)
		|| FParse::Param(CommandLine, *LongName);
}

template <typename ValueType>
bool ReadCommandLineValue(
	const TCHAR* CommandLine,
	const TCHAR* Match,
	ValueType& OutValue)
{
	const FString LongMatch = TEXT("-") + FString(Match);
	return FParse::Value(CommandLine, Match, OutValue)
		|| FParse::Value(CommandLine, *LongMatch, OutValue);
}
}
