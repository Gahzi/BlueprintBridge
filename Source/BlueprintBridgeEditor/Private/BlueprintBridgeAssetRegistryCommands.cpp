// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace BlueprintBridge
{
static IAssetRegistry* GetAssetRegistry()
{
	FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	return &Module.Get();
}

static FName PackageNameFromAssetPath(const FString& AssetPath)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	return *(PackageName.IsEmpty() ? AssetPath : PackageName);
}

static bool MatchesAssetClassFilter(IAssetRegistry* Registry, FName PackageName, const FString& ClassFilter)
{
	if (ClassFilter.IsEmpty()) { return true; }
	TArray<FAssetData> AssetsInPackage;
	Registry->GetAssetsByPackageName(PackageName, AssetsInPackage);
	for (const FAssetData& Asset : AssetsInPackage)
	{
		const FString AssetClassPath = Asset.AssetClassPath.ToString();
		if (AssetClassPath.Equals(ClassFilter, ESearchCase::IgnoreCase)) { return true; }
	}
	return false;
}

static TSharedRef<FJsonObject> BuildAssetPathListResult(const TArray<FName>& PackageNames, IAssetRegistry* Registry, const FString& ClassFilter)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(PackageNames.Num());
	for (FName PackageName : PackageNames)
	{
		if (!MatchesAssetClassFilter(Registry, PackageName, ClassFilter)) { continue; }
		Values.Add(MakeShared<FJsonValueString>(PackageName.ToString()));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("assets"), Values);
	Result->SetNumberField(TEXT("count"), Values.Num());
	return Result;
}

TSharedRef<FJsonObject> FindAssetReferences(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindAssetReferences requires params.asset."));
	}

	bool bIncludeSoft = true;
	FString ClassFilter;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("includeSoft"), bIncludeSoft);
		Params->TryGetStringField(TEXT("assetClassFilter"), ClassFilter);
	}

	IAssetRegistry* Registry = GetAssetRegistry();
	const FName PackageName = PackageNameFromAssetPath(AssetPath);
	TArray<FName> Referencers;
	UE::AssetRegistry::FDependencyQuery Query;
	if (!bIncludeSoft)
	{
		Query.Required = UE::AssetRegistry::EDependencyProperty::Hard;
	}
	Registry->GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package, Query);
	return MakeSuccess(Id, BuildAssetPathListResult(Referencers, Registry, ClassFilter));
}

TSharedRef<FJsonObject> FindAssetDependencies(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindAssetDependencies requires params.asset."));
	}

	bool bIncludeSoft = true;
	FString ClassFilter;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("includeSoft"), bIncludeSoft);
		Params->TryGetStringField(TEXT("assetClassFilter"), ClassFilter);
	}

	IAssetRegistry* Registry = GetAssetRegistry();
	const FName PackageName = PackageNameFromAssetPath(AssetPath);
	TArray<FName> Dependencies;
	UE::AssetRegistry::FDependencyQuery Query;
	if (!bIncludeSoft)
	{
		Query.Required = UE::AssetRegistry::EDependencyProperty::Hard;
	}
	Registry->GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, Query);
	return MakeSuccess(Id, BuildAssetPathListResult(Dependencies, Registry, ClassFilter));
}

TSharedRef<FJsonObject> FindInterfaceImplementations(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString InterfaceClass;
	if (!TryGetRequiredString(Params, TEXT("interfaceClass"), InterfaceClass))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindInterfaceImplementations requires params.interfaceClass."));
	}

	bool bIncludeChildClasses = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("includeChildClasses"), bIncludeChildClasses);
	}

	UClass* InterfaceTarget = LoadClassByPath(InterfaceClass);
	if (!InterfaceTarget)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load interface class '%s'."), *InterfaceClass));
	}

	// Build the set of class path strings to look for inside each Blueprint's serialized
	// ImplementedInterfaces tag. Reading the tag avoids loading every Blueprint in the project.
	TArray<FString> MatchPaths;
	MatchPaths.Add(InterfaceTarget->GetPathName());
	if (bIncludeChildClasses)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Candidate = *ClassIt;
			if (Candidate && Candidate != InterfaceTarget && Candidate->IsChildOf(InterfaceTarget))
			{
				MatchPaths.Add(Candidate->GetPathName());
			}
		}
	}

	IAssetRegistry* Registry = GetAssetRegistry();
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	TArray<FAssetData> BlueprintAssets;
	Registry->GetAssets(Filter, BlueprintAssets);

	static const FName ImplementedInterfacesTag(TEXT("ImplementedInterfaces"));
	TArray<TSharedPtr<FJsonValue>> Matches;
	for (const FAssetData& Asset : BlueprintAssets)
	{
		FString Serialized;
		if (!Asset.GetTagValue<FString>(ImplementedInterfacesTag, Serialized) || Serialized.IsEmpty())
		{
			continue;
		}
		bool bMatched = false;
		for (const FString& Path : MatchPaths)
		{
			if (Serialized.Contains(Path, ESearchCase::CaseSensitive))
			{
				bMatched = true;
				break;
			}
		}
		if (bMatched)
		{
			Matches.Add(MakeShared<FJsonValueString>(Asset.GetSoftObjectPath().ToString()));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("assets"), Matches);
	Result->SetNumberField(TEXT("count"), Matches.Num());
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
