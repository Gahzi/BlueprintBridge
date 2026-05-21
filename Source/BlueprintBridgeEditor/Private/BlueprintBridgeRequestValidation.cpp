// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
bool DoesJsonValueMatchType(const TSharedPtr<FJsonValue>& Value, const FString& ExpectedType)
{
	if (!Value.IsValid())
	{
		return false;
	}

	if (ExpectedType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
	{
		return Value->Type == EJson::String;
	}
	if (ExpectedType.Equals(TEXT("number"), ESearchCase::IgnoreCase))
	{
		return Value->Type == EJson::Number;
	}
	if (ExpectedType.Equals(TEXT("boolean"), ESearchCase::IgnoreCase))
	{
		return Value->Type == EJson::Boolean;
	}
	if (ExpectedType.Equals(TEXT("object"), ESearchCase::IgnoreCase))
	{
		return Value->Type == EJson::Object;
	}
	if (ExpectedType.Equals(TEXT("array"), ESearchCase::IgnoreCase))
	{
		return Value->Type == EJson::Array;
	}

	return true;
}

bool ValidateCommandParamsAgainstSchema(const FString& CommandName, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Schema, FString& OutError)
{
	if (!Schema.IsValid())
	{
		return true;
	}

	FString SchemaType;
	if (Schema->TryGetStringField(TEXT("type"), SchemaType) && !SchemaType.Equals(TEXT("object"), ESearchCase::IgnoreCase))
	{
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* RequiredFields = nullptr;
	if (Schema->TryGetArrayField(TEXT("required"), RequiredFields) && RequiredFields != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& RequiredField : *RequiredFields)
		{
			FString FieldName;
			if (RequiredField.IsValid() && RequiredField->TryGetString(FieldName) && (!Params.IsValid() || !Params->HasField(FieldName)))
			{
				OutError = FString::Printf(TEXT("%s requires params.%s."), *CommandName, *FieldName);
				return false;
			}
		}
	}

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	if (!Schema->TryGetObjectField(TEXT("properties"), Properties) || Properties == nullptr || !Properties->IsValid() || !Params.IsValid())
	{
		return true;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& PropertyPair : (*Properties)->Values)
	{
		const TSharedPtr<FJsonValue>* ParamValue = Params->Values.Find(PropertyPair.Key);
		if (!ParamValue || !ParamValue->IsValid())
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
		if (!PropertyPair.Value.IsValid() || !PropertyPair.Value->TryGetObject(PropertySchema) || PropertySchema == nullptr || !PropertySchema->IsValid())
		{
			continue;
		}

		FString ExpectedType;
		if ((*PropertySchema)->TryGetStringField(TEXT("type"), ExpectedType) && !DoesJsonValueMatchType(*ParamValue, ExpectedType))
		{
			OutError = FString::Printf(TEXT("%s params.%s must be a %s."), *CommandName, *PropertyPair.Key, *ExpectedType);
			return false;
		}
	}

	return true;
}
} // namespace BlueprintBridge
