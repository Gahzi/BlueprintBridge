// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace BlueprintBridge
{
class FSchemaBuilder
{
public:
	static FSchemaBuilder Object();

	FSchemaBuilder& RequiredString(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalString(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredNumber(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalNumber(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredBoolean(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalBoolean(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredObject(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalObject(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredArray(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalArray(const TCHAR* Name, const TCHAR* Description);

	TSharedPtr<FJsonObject> Build();

private:
	FSchemaBuilder();

	FSchemaBuilder& AddProperty(const TCHAR* Name, const TCHAR* Type, const TCHAR* Description, bool bRequired);

	TSharedRef<FJsonObject> Schema;
	TSharedRef<FJsonObject> Properties;
	TArray<TSharedPtr<FJsonValue>> Required;
};
} // namespace BlueprintBridge
