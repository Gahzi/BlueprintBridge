// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "BlueprintBridgeSettings.generated.h"

UCLASS(Config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="Blueprint Bridge"))
class UBlueprintBridgeSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UBlueprintBridgeSettings();

	FName GetCategoryName() const override;
	FName GetSectionName() const override;

	UPROPERTY(Config, EditAnywhere, Category="Server")
	bool bEnableServer = true;

	UPROPERTY(Config, EditAnywhere, Category="Server")
	FString PipeName = TEXT("BlueprintBridge");

	UPROPERTY(Config, EditAnywhere, Category="Server")
	bool bStartInUnattended = false;

	UPROPERTY(Config, EditAnywhere, Category="Security")
	bool bRequireAuthToken = false;

	UPROPERTY(Config, EditAnywhere, Category="Security", meta=(EditCondition="bRequireAuthToken"))
	FString AuthToken;

	UPROPERTY(Config, EditAnywhere, Category="Validation")
	bool bValidateRequestsAgainstSchemas = false;

	UPROPERTY(Config, EditAnywhere, Category="Validation", meta=(ClampMin="0"))
	int32 MaxBatchSize = 0;
};

namespace BlueprintBridge
{
bool IsServerEnabled();
bool ShouldStartInUnattended();
bool ShouldValidateRequestsAgainstSchemas();
int32 GetMaxBatchSize();
FString GetConfiguredPipeName();
FString GetConfiguredAuthToken();
bool IsAuthRequired();
}
