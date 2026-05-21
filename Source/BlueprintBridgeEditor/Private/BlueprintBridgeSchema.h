// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#include <initializer_list>

namespace BlueprintBridge
{
class FSchemaBuilder
{
public:
	static FSchemaBuilder Object();

	FSchemaBuilder& RequiredString(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalString(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredStringEnum(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values);
	FSchemaBuilder& OptionalStringEnum(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values);
	FSchemaBuilder& RequiredNumber(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalNumber(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredBoolean(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalBoolean(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredObject(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalObject(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& RequiredObject(const TCHAR* Name, const TCHAR* Description, const TSharedPtr<FJsonObject>& ObjectSchema);
	FSchemaBuilder& OptionalObject(const TCHAR* Name, const TCHAR* Description, const TSharedPtr<FJsonObject>& ObjectSchema);
	FSchemaBuilder& RequiredArray(const TCHAR* Name, const TCHAR* Description);
	FSchemaBuilder& OptionalArray(const TCHAR* Name, const TCHAR* Description);

	TSharedPtr<FJsonObject> Build();

private:
	FSchemaBuilder();

	FSchemaBuilder& AddProperty(const TCHAR* Name, const TCHAR* Type, const TCHAR* Description, bool bRequired);
	FSchemaBuilder& AddStringEnumProperty(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values, bool bRequired);
	FSchemaBuilder& AddObjectProperty(const TCHAR* Name, const TCHAR* Description, const TSharedPtr<FJsonObject>& ObjectSchema, bool bRequired);

	TSharedRef<FJsonObject> Schema;
	TSharedRef<FJsonObject> Properties;
	TArray<TSharedPtr<FJsonValue>> Required;
};
} // namespace BlueprintBridge
