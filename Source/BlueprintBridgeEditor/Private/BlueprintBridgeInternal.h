// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace BlueprintBridge
{
FString GetPipeNamePath();
TSharedRef<FJsonObject> ExecuteRequest(const FString& RequestText);
}
