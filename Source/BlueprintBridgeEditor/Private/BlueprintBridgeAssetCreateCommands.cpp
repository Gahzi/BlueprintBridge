// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
UBlueprint* CreateBlueprintAssetWorker(const FString& AssetPath, const FString& ParentClassPath, FString& OutErrorCode, FString& OutErrorMessage)
{
	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		OutErrorCode = TEXT("InvalidAssetPath");
		OutErrorMessage = FString::Printf(TEXT("'%s' is not a valid asset path."), *AssetPath);
		return nullptr;
	}

	if (DoesAssetExistQuiet(AssetPath))
	{
		OutErrorCode = TEXT("AssetAlreadyExists");
		OutErrorMessage = FString::Printf(TEXT("Asset '%s' already exists."), *AssetPath);
		return nullptr;
	}

	UClass* ParentClass = LoadClassByPath(ParentClassPath);
	if (!ParentClass)
	{
		OutErrorCode = TEXT("ClassNotFound");
		OutErrorMessage = FString::Printf(TEXT("Could not load parent class '%s'."), *ParentClassPath);
		return nullptr;
	}

	UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName));
	if (!Package)
	{
		OutErrorCode = TEXT("CreatePackageFailed");
		OutErrorMessage = FString::Printf(TEXT("Could not create package for '%s'."), *AssetPath);
		return nullptr;
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("BlueprintBridge"));

	if (!Blueprint)
	{
		OutErrorCode = TEXT("CreateBlueprintFailed");
		OutErrorMessage = FString::Printf(TEXT("Could not create Blueprint '%s'."), *AssetPath);
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	return Blueprint;
}

TSharedRef<FJsonObject> CreateBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ParentClassPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("parentClass"), ParentClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CreateBlueprintAsset requires params.asset and params.parentClass."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CreateBlueprintAsset", "Blueprint Bridge: Create Blueprint Asset"));
	FString ErrorCode;
	FString ErrorMessage;
	UBlueprint* Blueprint = CreateBlueprintAssetWorker(AssetPath, ParentClassPath, ErrorCode, ErrorMessage);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, ErrorCode, ErrorMessage);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> CreateWidgetBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CreateWidgetBlueprintAsset requires params.asset."));
	}

	FString ParentClassPath = TEXT("/Script/UMG.UserWidget");
	Params->TryGetStringField(TEXT("parentClass"), ParentClassPath);

	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidAssetPath"), FString::Printf(TEXT("'%s' is not a valid asset path."), *AssetPath));
	}

	if (DoesAssetExistQuiet(AssetPath))
	{
		return MakeBridgeError(Id, TEXT("AssetAlreadyExists"), FString::Printf(TEXT("Asset '%s' already exists."), *AssetPath));
	}

	UClass* ParentClass = LoadClassByPath(ParentClassPath);
	if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load UserWidget parent class '%s'."), *ParentClassPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CreateWidgetBlueprintAsset", "Blueprint Bridge: Create Widget Blueprint Asset"));
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName));
	if (!Package)
	{
		return MakeBridgeError(Id, TEXT("CreatePackageFailed"), FString::Printf(TEXT("Could not create package for '%s'."), *AssetPath));
	}

	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass(),
		TEXT("BlueprintBridge")));

	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("CreateBlueprintFailed"), FString::Printf(TEXT("Could not create Widget Blueprint '%s'."), *AssetPath));
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	AddWidgetTreeDescription(Result, Blueprint);
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
