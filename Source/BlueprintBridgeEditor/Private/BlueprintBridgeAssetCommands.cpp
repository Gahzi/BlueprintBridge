// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
TSharedRef<FJsonObject> DuplicateAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString SourceAssetPath;
	FString DestAssetPath;
	if (!TryGetRequiredString(Params, TEXT("sourceAsset"), SourceAssetPath) || !TryGetRequiredString(Params, TEXT("destAsset"), DestAssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DuplicateAsset requires params.sourceAsset and params.destAsset."));
	}

	UObject* SourceAsset = LoadAssetObject(SourceAssetPath);
	if (!SourceAsset)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load source asset '%s'."), *SourceAssetPath));
	}

	FString DestPackagePath;
	FString DestAssetName;
	if (!SplitAssetPath(DestAssetPath, DestPackagePath, DestAssetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidAssetPath"), FString::Printf(TEXT("'%s' is not a valid destination asset path."), *DestAssetPath));
	}

	if (DoesAssetExistQuiet(DestAssetPath))
	{
		return MakeBridgeError(Id, TEXT("AssetAlreadyExists"), FString::Printf(TEXT("Asset '%s' already exists."), *DestAssetPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "DuplicateAsset", "Blueprint Bridge: Duplicate Asset"));
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.DuplicateAsset(DestAssetName, DestPackagePath, SourceAsset);
	if (!NewAsset)
	{
		return MakeBridgeError(Id, TEXT("DuplicateAssetFailed"), FString::Printf(TEXT("Could not duplicate '%s' to '%s'."), *SourceAssetPath, *DestAssetPath));
	}

	NewAsset->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), DestAssetPath);
	Result->SetStringField(TEXT("name"), NewAsset->GetName());
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(NewAsset))
	{
		Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	}
	Result->SetStringField(TEXT("sourceAsset"), SourceAssetPath);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> CheckoutAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CheckoutAsset requires params.asset."));
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath));
	if (!Asset)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load asset '%s'."), *AssetPath));
	}

	UPackage* Package = Asset->GetOutermost();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	if (!SourceControlHelpers::CheckOutFile(PackageFilename))
	{
		return MakeBridgeError(Id, TEXT("CheckoutFailed"), FString::Printf(TEXT("Could not check out '%s'."), *PackageFilename));
	}

	return MakeSuccessMessage(Id, TEXT("CheckedOut"));
}

FString GetBlueprintStatusString(const EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Unknown:
		return TEXT("Unknown");
	case BS_Dirty:
		return TEXT("Dirty");
	case BS_Error:
		return TEXT("Error");
	case BS_UpToDate:
		return TEXT("UpToDate");
	case BS_BeingCreated:
		return TEXT("BeingCreated");
	case BS_UpToDateWithWarnings:
		return TEXT("UpToDateWithWarnings");
	default:
		return TEXT("Invalid");
	}
}

TSharedRef<FJsonObject> CompileBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CompileBlueprint requires params.asset."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), GetBlueprintStatusString(Blueprint->Status));
	Result->SetBoolField(TEXT("success"), Blueprint->Status != BS_Error);
	Result->SetArrayField(TEXT("messages"), TArray<TSharedPtr<FJsonValue>>());
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> SaveAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SaveAsset requires params.asset."));
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath));
	if (!Asset)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load asset '%s'."), *AssetPath));
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Asset->GetOutermost());
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
	if (!bSaved)
	{
		return MakeBridgeError(Id, TEXT("SaveFailed"), FString::Printf(TEXT("Could not save '%s'."), *AssetPath));
	}

	return MakeSuccessMessage(Id, TEXT("Saved"));
}
} // namespace BlueprintBridge
