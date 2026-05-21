// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
FString GetPipeNamePath()
{
	return FString::Printf(TEXT("\\\\.\\pipe\\%s"), *GetConfiguredPipeName());
}

bool ValidateAuthToken(const TSharedPtr<FJsonObject>& Request)
{
	if (!IsAuthRequired())
	{
		return true;
	}

	if (!Request.IsValid() || !Request->HasTypedField<EJson::String>(TEXT("authToken")))
	{
		return false;
	}

	return Request->GetStringField(TEXT("authToken")) == GetConfiguredAuthToken();
}

FString JsonToString(const TSharedRef<FJsonObject>& JsonObject)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonObject, Writer);
	return Output;
}

TSharedRef<FJsonObject> MakeBridgeError(const FString& Id, const FString& Code, const FString& Message)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), false);

	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	Response->SetObjectField(TEXT("error"), Error);
	return Response;
}

FString NormalizeBlueprintObjectPath(const FString& AssetPath)
{
	if (AssetPath.Contains(TEXT(".")))
	{
		return AssetPath;
	}

	FString AssetName;
	if (!AssetPath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return AssetPath;
	}

	return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

TSharedRef<FJsonObject> MakeSuccess(const FString& Id, const TSharedPtr<FJsonObject>& Result)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), true);
	if (Result.IsValid())
	{
		Response->SetObjectField(TEXT("result"), Result.ToSharedRef());
	}
	return Response;
}

TSharedRef<FJsonObject> MakeSuccessMessage(const FString& Id, const FString& Message)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("result"), Message);
	return Response;
}

UBlueprint* LoadBlueprint(const FString& AssetPath)
{
	return Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath)));
}

UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath)
{
	return Cast<UWidgetBlueprint>(StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath)));
}

FString PinContainerTypeToString(const FEdGraphPinType& PinType)
{
	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array:
		return TEXT("Array");
	case EPinContainerType::Set:
		return TEXT("Set");
	case EPinContainerType::Map:
		return TEXT("Map");
	case EPinContainerType::None:
	default:
		return TEXT("None");
	}
}

bool ApplyPinContainerType(const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& PinType, FString& OutError)
{
	FString ContainerType;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("containerType"), ContainerType))
	{
		bool bIsArray = false;
		if (Params.IsValid() && Params->TryGetBoolField(TEXT("isArray"), bIsArray) && bIsArray)
		{
			PinType.ContainerType = EPinContainerType::Array;
		}
		return true;
	}

	if (ContainerType.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		PinType.ContainerType = EPinContainerType::None;
		return true;
	}
	if (ContainerType.Equals(TEXT("Array"), ESearchCase::IgnoreCase))
	{
		PinType.ContainerType = EPinContainerType::Array;
		return true;
	}
	if (ContainerType.Equals(TEXT("Set"), ESearchCase::IgnoreCase))
	{
		PinType.ContainerType = EPinContainerType::Set;
		return true;
	}

	OutError = FString::Printf(TEXT("Unsupported containerType '%s'. Supported values are None, Array, and Set."), *ContainerType);
	return false;
}

UObject* LoadAssetObject(const FString& AssetPath)
{
	return StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath));
}

bool DoesAssetExistQuiet(const FString& AssetPath)
{
	const FString ObjectPath = NormalizeBlueprintObjectPath(AssetPath);
	if (FindObject<UObject>(nullptr, *ObjectPath))
	{
		return true;
	}

	FString PackagePath = AssetPath;
	if (PackagePath.Contains(TEXT(".")))
	{
		PackagePath.Split(TEXT("."), &PackagePath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	return FPackageName::DoesPackageExist(PackagePath);
}

bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
	FString NormalizedAssetPath = AssetPath;
	if (NormalizedAssetPath.Contains(TEXT(".")))
	{
		NormalizedAssetPath.Split(TEXT("."), &NormalizedAssetPath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath, false))
	{
		return false;
	}

	if (!NormalizedAssetPath.Split(TEXT("/"), &OutPackagePath, &OutAssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || OutPackagePath.IsEmpty() || OutAssetName.IsEmpty())
	{
		return false;
	}

	return true;
}

UClass* LoadClassByPath(const FString& ClassPath)
{
	if (UClass* Class = LoadObject<UClass>(nullptr, *ClassPath))
	{
		return Class;
	}

	return StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
}

bool TryGetRequiredString(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FString& OutValue)
{
	if (!Params.IsValid() || !Params->HasTypedField<EJson::String>(FieldName))
	{
		return false;
	}

	OutValue = Params->GetStringField(FieldName);
	return true;
}

FString GetBlueprintStatusString(const EBlueprintStatus Status);
FName NormalizePinCategory(const FString& Category);
UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph);
bool TryMakePinTypeFromParams(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& OutPinType, FString& OutError);
} // namespace BlueprintBridge
