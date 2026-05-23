// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace BlueprintBridge
{
// Reads `Params.fields` (optional array of dot-path strings). If empty/missing, returns Result unchanged.
// Otherwise returns a filtered copy of Result keeping only the keys named by the field paths.
//
// Path semantics:
//   - "nodes"           keeps the entire `nodes` subtree intact.
//   - "nodes.title"     descends into `nodes` (arrays are walked element-wise) and keeps only `title` on each.
//   - "nodes.pins.name" descends two levels.
//   - unknown paths     are silently dropped (empty objects/arrays may result), never an error.
TSharedRef<FJsonObject> ApplyFieldSelection(
	const TSharedPtr<FJsonObject>& Params,
	const TSharedRef<FJsonObject>& Result);
} // namespace BlueprintBridge
