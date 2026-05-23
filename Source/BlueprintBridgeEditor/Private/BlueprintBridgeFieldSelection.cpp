// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeFieldSelection.h"

namespace BlueprintBridge
{
namespace
{
static TSharedPtr<FJsonValue> FilterValue(const TSharedPtr<FJsonValue>& Value, const TArray<FString>& Paths)
{
	if (!Value.IsValid())
	{
		return Value;
	}

	const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
	if (Value->TryGetObject(ObjectPtr) && ObjectPtr != nullptr && ObjectPtr->IsValid())
	{
		TSharedRef<FJsonObject> Filtered = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ObjectPtr)->Values)
		{
			bool bKeepFull = false;
			TArray<FString> Suffixes;
			for (const FString& Path : Paths)
			{
				if (Path.Equals(Pair.Key, ESearchCase::CaseSensitive))
				{
					bKeepFull = true;
					break;
				}
				const FString Prefix = Pair.Key + TEXT(".");
				if (Path.StartsWith(Prefix, ESearchCase::CaseSensitive))
				{
					Suffixes.Add(Path.Mid(Prefix.Len()));
				}
			}
			if (bKeepFull)
			{
				Filtered->SetField(Pair.Key, Pair.Value);
			}
			else if (Suffixes.Num() > 0)
			{
				Filtered->SetField(Pair.Key, FilterValue(Pair.Value, Suffixes));
			}
		}
		return MakeShared<FJsonValueObject>(Filtered);
	}

	const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
	if (Value->TryGetArray(ArrayPtr) && ArrayPtr != nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> FilteredArr;
		FilteredArr.Reserve(ArrayPtr->Num());
		for (const TSharedPtr<FJsonValue>& Item : *ArrayPtr)
		{
			FilteredArr.Add(FilterValue(Item, Paths));
		}
		return MakeShared<FJsonValueArray>(FilteredArr);
	}

	// Primitives: a path landed on a leaf — keep it.
	return Value;
}
} // anonymous

TSharedRef<FJsonObject> ApplyFieldSelection(
	const TSharedPtr<FJsonObject>& Params,
	const TSharedRef<FJsonObject>& Result)
{
	if (!Params.IsValid())
	{
		return Result;
	}
	const TArray<TSharedPtr<FJsonValue>>* FieldArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("fields"), FieldArray) || FieldArray == nullptr || FieldArray->Num() == 0)
	{
		return Result;
	}

	TArray<FString> Paths;
	Paths.Reserve(FieldArray->Num());
	for (const TSharedPtr<FJsonValue>& FieldValue : *FieldArray)
	{
		FString Path;
		if (FieldValue.IsValid() && FieldValue->TryGetString(Path) && !Path.IsEmpty())
		{
			Paths.Add(Path);
		}
	}
	if (Paths.Num() == 0)
	{
		return Result;
	}

	TSharedPtr<FJsonValue> Filtered = FilterValue(MakeShared<FJsonValueObject>(Result), Paths);
	const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
	if (Filtered.IsValid() && Filtered->TryGetObject(ObjectPtr) && ObjectPtr != nullptr && ObjectPtr->IsValid())
	{
		return (*ObjectPtr).ToSharedRef();
	}
	return Result;
}

} // namespace BlueprintBridge
