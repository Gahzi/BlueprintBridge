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

FSchemaBuilder& FSchemaBuilder::RequiredStringEnum(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values)
{
	return AddStringEnumProperty(Name, Description, Values, true);
}

FSchemaBuilder& FSchemaBuilder::OptionalStringEnum(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values)
{
	return AddStringEnumProperty(Name, Description, Values, false);
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

FSchemaBuilder& FSchemaBuilder::RequiredObject(const TCHAR* Name, const TCHAR* Description, const TSharedPtr<FJsonObject>& ObjectSchema)
{
	return AddObjectProperty(Name, Description, ObjectSchema, true);
}

FSchemaBuilder& FSchemaBuilder::OptionalObject(const TCHAR* Name, const TCHAR* Description, const TSharedPtr<FJsonObject>& ObjectSchema)
{
	return AddObjectProperty(Name, Description, ObjectSchema, false);
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

FSchemaBuilder& FSchemaBuilder::AddStringEnumProperty(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values, const bool bRequired)
{
	AddProperty(Name, TEXT("string"), Description, bRequired);

	const TSharedPtr<FJsonObject>* Property = nullptr;
	if (Properties->TryGetObjectField(Name, Property) && Property != nullptr && Property->IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		for (const TCHAR* Value : Values)
		{
			if (Value != nullptr)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(Value));
			}
		}
		(*Property)->SetArrayField(TEXT("enum"), EnumValues);
	}

	return *this;
}

FSchemaBuilder& FSchemaBuilder::AddObjectProperty(const TCHAR* Name, const TCHAR* Description, const TSharedPtr<FJsonObject>& ObjectSchema, const bool bRequired)
{
	AddProperty(Name, TEXT("object"), Description, bRequired);

	const TSharedPtr<FJsonObject>* Property = nullptr;
	if (ObjectSchema.IsValid() && Properties->TryGetObjectField(Name, Property) && Property != nullptr && Property->IsValid())
	{
		const TSharedPtr<FJsonObject>* NestedProperties = nullptr;
		if (ObjectSchema->TryGetObjectField(TEXT("properties"), NestedProperties) && NestedProperties != nullptr && NestedProperties->IsValid())
		{
			(*Property)->SetObjectField(TEXT("properties"), NestedProperties->ToSharedRef());
		}

		const TArray<TSharedPtr<FJsonValue>>* NestedRequired = nullptr;
		if (ObjectSchema->TryGetArrayField(TEXT("required"), NestedRequired) && NestedRequired != nullptr)
		{
			(*Property)->SetArrayField(TEXT("required"), *NestedRequired);
		}
	}

	return *this;
}
} // namespace BlueprintBridge
