// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeSettings.h"

#include "Misc/ConfigCacheIni.h"

static constexpr TCHAR LegacySettingsSection[] = TEXT("/Script/BlueprintBridgeEditor.BlueprintBridge");

UBlueprintBridgeSettings::UBlueprintBridgeSettings()
{
}

FName UBlueprintBridgeSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}

FName UBlueprintBridgeSettings::GetSectionName() const
{
	return TEXT("BlueprintBridge");
}

namespace BlueprintBridge
{
static bool TryGetLegacyBool(const TCHAR* Key, bool& OutValue)
{
	return GConfig && GConfig->GetBool(LegacySettingsSection, Key, OutValue, GEditorPerProjectIni);
}

static bool TryGetLegacyString(const TCHAR* Key, FString& OutValue)
{
	return GConfig && GConfig->GetString(LegacySettingsSection, Key, OutValue, GEditorPerProjectIni);
}

static bool TryGetLegacyInt(const TCHAR* Key, int32& OutValue)
{
	return GConfig && GConfig->GetInt(LegacySettingsSection, Key, OutValue, GEditorPerProjectIni);
}

bool IsServerEnabled()
{
	bool bEnabled = GetDefault<UBlueprintBridgeSettings>()->bEnableServer;
	TryGetLegacyBool(TEXT("bEnableServer"), bEnabled);
	return bEnabled;
}

bool ShouldStartInUnattended()
{
	bool bStartInUnattended = GetDefault<UBlueprintBridgeSettings>()->bStartInUnattended;
	TryGetLegacyBool(TEXT("bStartInUnattended"), bStartInUnattended);
	return bStartInUnattended;
}

bool ShouldValidateRequestsAgainstSchemas()
{
	bool bValidateRequestsAgainstSchemas = GetDefault<UBlueprintBridgeSettings>()->bValidateRequestsAgainstSchemas;
	TryGetLegacyBool(TEXT("bValidateRequestsAgainstSchemas"), bValidateRequestsAgainstSchemas);
	return bValidateRequestsAgainstSchemas;
}

int32 GetMaxBatchSize()
{
	int32 MaxBatchSize = GetDefault<UBlueprintBridgeSettings>()->MaxBatchSize;
	TryGetLegacyInt(TEXT("MaxBatchSize"), MaxBatchSize);
	return FMath::Max(0, MaxBatchSize);
}

FString GetConfiguredPipeName()
{
	FString PipeName = GetDefault<UBlueprintBridgeSettings>()->PipeName;
	TryGetLegacyString(TEXT("PipeName"), PipeName);
	PipeName = PipeName.TrimStartAndEnd();
	return PipeName.IsEmpty() ? TEXT("BlueprintBridge") : PipeName;
}

FString GetConfiguredAuthToken()
{
	FString AuthToken = GetDefault<UBlueprintBridgeSettings>()->AuthToken;
	TryGetLegacyString(TEXT("AuthToken"), AuthToken);
	return AuthToken;
}

bool IsAuthRequired()
{
	bool bRequireAuthToken = GetDefault<UBlueprintBridgeSettings>()->bRequireAuthToken;
	TryGetLegacyBool(TEXT("bRequireAuthToken"), bRequireAuthToken);
	return bRequireAuthToken;
}
} // namespace BlueprintBridge
