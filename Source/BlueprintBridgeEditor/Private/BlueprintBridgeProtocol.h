// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace BlueprintBridge
{
struct ICommand;

FString GetPipeNamePath();
bool ValidateAuthToken(const TSharedPtr<FJsonObject>& Request);
FString JsonToString(const TSharedRef<FJsonObject>& JsonObject);
TSharedRef<FJsonObject> MakeBridgeError(const FString& Id, const FString& Code, const FString& Message);
TSharedRef<FJsonObject> MakeSuccess(const FString& Id, const TSharedPtr<FJsonObject>& Result);
TSharedRef<FJsonObject> MakeSuccessMessage(const FString& Id, const FString& Message);
TSharedRef<FJsonObject> MakeCommandDescription(const ICommand& Command, bool bIncludeSchemas);
TSharedRef<FJsonObject> ExecuteRequestOnGameThread(const FString& RequestText);
} // namespace BlueprintBridge
