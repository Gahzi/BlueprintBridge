// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;

namespace BlueprintBridge
{
// Lowers Semantic IR to an ApplyGraphPatch-shaped body. The resolutions map records what each
// implicit identifier resolved to, keyed by a dotted/bracketed path (e.g. "flow[0].if.args.Context"
// → "param:Context"). The path format is a readability-first convenience, not RFC 6901 JSON Pointer.
struct FSemanticLowerResult
{
	TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Resolutions = MakeShared<FJsonObject>();
};

bool LowerSemanticFunctionIR(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	FSemanticLowerResult& OutResult,
	FString& OutErrorPointer,
	FString& OutError);
} // namespace BlueprintBridge
