// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeSchema.h"

namespace BlueprintBridge
{
FSchemaBuilder::FSchemaBuilder()
	: Schema(MakeShared<FJsonObject>())
	, Properties(MakeShared<FJsonObject>())
{
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetObjectField(TEXT("properties"), Properties);
}

FSchemaBuilder FSchemaBuilder::Object()
{
	return FSchemaBuilder();
}

FSchemaBuilder& FSchemaBuilder::RequiredString(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("string"), Description, true);
}

FSchemaBuilder& FSchemaBuilder::OptionalString(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("string"), Description, false);
}

FSchemaBuilder& FSchemaBuilder::RequiredNumber(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("number"), Description, true);
}

FSchemaBuilder& FSchemaBuilder::OptionalNumber(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("number"), Description, false);
}

FSchemaBuilder& FSchemaBuilder::RequiredBoolean(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("boolean"), Description, true);
}

FSchemaBuilder& FSchemaBuilder::OptionalBoolean(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("boolean"), Description, false);
}

FSchemaBuilder& FSchemaBuilder::RequiredObject(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("object"), Description, true);
}

FSchemaBuilder& FSchemaBuilder::OptionalObject(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("object"), Description, false);
}

FSchemaBuilder& FSchemaBuilder::RequiredArray(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("array"), Description, true);
}

FSchemaBuilder& FSchemaBuilder::OptionalArray(const TCHAR* Name, const TCHAR* Description)
{
	return AddProperty(Name, TEXT("array"), Description, false);
}

TSharedPtr<FJsonObject> FSchemaBuilder::Build()
{
	if (!Required.IsEmpty())
	{
		Schema->SetArrayField(TEXT("required"), Required);
	}
	return Schema;
}

FSchemaBuilder& FSchemaBuilder::AddProperty(const TCHAR* Name, const TCHAR* Type, const TCHAR* Description, const bool bRequired)
{
	TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
	Property->SetStringField(TEXT("type"), Type);
	if (Description != nullptr && Description[0] != TEXT('\0'))
	{
		Property->SetStringField(TEXT("description"), Description);
	}

	Properties->SetObjectField(Name, Property);
	if (bRequired)
	{
		Required.Add(MakeShared<FJsonValueString>(Name));
	}
	return *this;
}
} // namespace BlueprintBridge
