// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
static UClass* ResolveReflectionClass(const FString& ClassPath)
{
	if (UClass* Class = LoadClassByPath(ClassPath))
	{
		return Class;
	}

	if (UBlueprint* Blueprint = LoadBlueprint(ClassPath))
	{
		if (Blueprint->SkeletonGeneratedClass)
		{
			return Blueprint->SkeletonGeneratedClass;
		}
		return Blueprint->GeneratedClass;
	}

	return nullptr;
}

static FString PropertyDirection(const FProperty* Property)
{
	if (!Property)
	{
		return TEXT("Unknown");
	}
	if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
	{
		return TEXT("Return");
	}
	if (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ConstParm))
	{
		return TEXT("Output");
	}
	return TEXT("Input");
}

static TSharedRef<FJsonObject> DescribePinType(const FEdGraphPinType& PinType)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
	Result->SetStringField(TEXT("subCategory"), PinType.PinSubCategory.ToString());
	Result->SetStringField(TEXT("subCategoryObject"), PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("containerType"), PinContainerTypeToString(PinType));
	Result->SetBoolField(TEXT("byRef"), PinType.bIsReference);
	Result->SetBoolField(TEXT("isConst"), PinType.bIsConst);
	return Result;
}

static TSharedRef<FJsonObject> MakeFieldMetadata(const FField* Field)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	static const TCHAR* MetadataKeys[] = {
		TEXT("DisplayName"),
		TEXT("CompactNodeTitle"),
		TEXT("Keywords"),
		TEXT("Category"),
		TEXT("ToolTip"),
		TEXT("ShortToolTip"),
		TEXT("WorldContext"),
		TEXT("DefaultToSelf"),
		TEXT("DeterminesOutputType"),
		TEXT("DynamicOutputParam"),
		TEXT("ExpandEnumAsExecs"),
		TEXT("Latent"),
		TEXT("LatentInfo"),
		TEXT("DeprecatedFunction"),
		TEXT("DeprecationMessage"),
		TEXT("BlueprintInternalUseOnly"),
	};

	if (!Field)
	{
		return Result;
	}

	for (const TCHAR* Key : MetadataKeys)
	{
		if (Field->HasMetaData(Key))
		{
			Result->SetStringField(Key, Field->GetMetaData(Key));
		}
	}
	return Result;
}

static TSharedRef<FJsonObject> MakeFieldMetadata(const UField* Field)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	static const TCHAR* MetadataKeys[] = {
		TEXT("DisplayName"),
		TEXT("CompactNodeTitle"),
		TEXT("Keywords"),
		TEXT("Category"),
		TEXT("ToolTip"),
		TEXT("ShortToolTip"),
		TEXT("WorldContext"),
		TEXT("DefaultToSelf"),
		TEXT("DeterminesOutputType"),
		TEXT("DynamicOutputParam"),
		TEXT("ExpandEnumAsExecs"),
		TEXT("Latent"),
		TEXT("LatentInfo"),
		TEXT("DeprecatedFunction"),
		TEXT("DeprecationMessage"),
		TEXT("BlueprintInternalUseOnly"),
	};

	if (!Field)
	{
		return Result;
	}

	for (const TCHAR* Key : MetadataKeys)
	{
		if (Field->HasMetaData(Key))
		{
			Result->SetStringField(Key, Field->GetMetaData(Key));
		}
	}
	return Result;
}

static TSharedRef<FJsonObject> DescribeReflectedProperty(const FProperty* Property)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Property)
	{
		return Result;
	}

	Result->SetStringField(TEXT("name"), Property->GetName());
	Result->SetStringField(TEXT("class"), Property->GetClass()->GetName());
	Result->SetStringField(TEXT("owner"), Property->GetOwnerStruct() ? Property->GetOwnerStruct()->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("cppType"), Property->GetCPPType());
	Result->SetStringField(TEXT("direction"), PropertyDirection(Property));
	Result->SetBoolField(TEXT("isParam"), Property->HasAnyPropertyFlags(CPF_Parm));
	Result->SetBoolField(TEXT("isReturn"), Property->HasAnyPropertyFlags(CPF_ReturnParm));
	Result->SetBoolField(TEXT("isOut"), Property->HasAnyPropertyFlags(CPF_OutParm));
	Result->SetBoolField(TEXT("byRef"), Property->HasAnyPropertyFlags(CPF_ReferenceParm));
	Result->SetBoolField(TEXT("isConst"), Property->HasAnyPropertyFlags(CPF_ConstParm));
	Result->SetBoolField(TEXT("blueprintVisible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
	Result->SetBoolField(TEXT("blueprintReadOnly"), Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
	Result->SetBoolField(TEXT("editAnywhere"), Property->HasAnyPropertyFlags(CPF_Edit));
	Result->SetBoolField(TEXT("disableEditOnInstance"), Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance));
	Result->SetBoolField(TEXT("exposeOnSpawn"), Property->HasMetaData(TEXT("ExposeOnSpawn")));
	Result->SetObjectField(TEXT("metadata"), MakeFieldMetadata(Property));

	FEdGraphPinType PinType;
	if (GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType))
	{
		Result->SetObjectField(TEXT("pinType"), DescribePinType(PinType));
	}

	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		Result->SetStringField(TEXT("propertyClass"), ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetPathName() : TEXT(""));
	}
	else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		Result->SetStringField(TEXT("struct"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : TEXT(""));
	}
	else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		Result->SetStringField(TEXT("enum"), EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetPathName() : TEXT(""));
	}
	else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
	{
		if (ByteProperty->Enum)
		{
			Result->SetStringField(TEXT("enum"), ByteProperty->Enum->GetPathName());
		}
	}

	return Result;
}

static TSharedRef<FJsonObject> DescribeFunctionSummary(UFunction* Function)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Function)
	{
		return Result;
	}

	const bool bBlueprintPure = Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
	Result->SetStringField(TEXT("name"), Function->GetName());
	Result->SetStringField(TEXT("path"), Function->GetPathName());
	Result->SetStringField(TEXT("ownerClass"), Function->GetOuterUClass() ? Function->GetOuterUClass()->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("blueprintCallable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	Result->SetBoolField(TEXT("blueprintPure"), bBlueprintPure);
	Result->SetBoolField(TEXT("const"), Function->HasAnyFunctionFlags(FUNC_Const));
	Result->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
	Result->SetBoolField(TEXT("event"), Function->HasAnyFunctionFlags(FUNC_Event));
	Result->SetBoolField(TEXT("net"), Function->HasAnyFunctionFlags(FUNC_Net));
	Result->SetBoolField(TEXT("isPureNode"), bBlueprintPure);
	Result->SetBoolField(TEXT("hasExecPins"), !bBlueprintPure);
	Result->SetObjectField(TEXT("metadata"), MakeFieldMetadata(Function));
	return Result;
}

static TSharedRef<FJsonObject> DescribeFunctionFull(UFunction* Function)
{
	TSharedRef<FJsonObject> Result = DescribeFunctionSummary(Function);
	TArray<TSharedPtr<FJsonValue>> Params;
	for (TFieldIterator<FProperty> PropertyIt(Function); PropertyIt && PropertyIt->HasAnyPropertyFlags(CPF_Parm); ++PropertyIt)
	{
		Params.Add(MakeShared<FJsonValueObject>(DescribeReflectedProperty(*PropertyIt)));
	}
	Result->SetArrayField(TEXT("params"), Params);
	return Result;
}

static FProperty* FindPropertyByName(UClass* Class, const FString& PropertyName)
{
	if (!Class)
	{
		return nullptr;
	}

	for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
	{
		if (PropertyIt->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
		{
			return *PropertyIt;
		}
	}
	return nullptr;
}

static UFunction* FindFunctionByName(UClass* Class, const FString& FunctionName)
{
	if (!Class)
	{
		return nullptr;
	}

	for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::IncludeSuper); FunctionIt; ++FunctionIt)
	{
		if (FunctionIt->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			return *FunctionIt;
		}
	}
	return nullptr;
}

static UFunction* GetDelegateSignature(FProperty* Property)
{
	if (FMulticastDelegateProperty* MulticastDelegate = CastField<FMulticastDelegateProperty>(Property))
	{
		return MulticastDelegate->SignatureFunction;
	}
	if (FDelegateProperty* Delegate = CastField<FDelegateProperty>(Property))
	{
		return Delegate->SignatureFunction;
	}
	return nullptr;
}

TSharedRef<FJsonObject> DescribeClass(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	if (!TryGetRequiredString(Params, TEXT("class"), ClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeClass requires params.class."));
	}

	UClass* Class = ResolveReflectionClass(ClassPath);
	if (!Class)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *ClassPath));
	}

	bool bIncludeFunctions = false;
	bool bIncludeProperties = false;
	bool bIncludeDelegates = false;
	Params->TryGetBoolField(TEXT("includeFunctions"), bIncludeFunctions);
	Params->TryGetBoolField(TEXT("includeProperties"), bIncludeProperties);
	Params->TryGetBoolField(TEXT("includeDelegates"), bIncludeDelegates);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Class->GetName());
	Result->SetStringField(TEXT("path"), Class->GetPathName());
	Result->SetStringField(TEXT("parentClass"), Class->GetSuperClass() ? Class->GetSuperClass()->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("blueprintType"), Class->HasMetaData(TEXT("BlueprintType")));
	Result->SetBoolField(TEXT("notBlueprintable"), Class->HasMetaData(TEXT("NotBlueprintable")));
	Result->SetObjectField(TEXT("metadata"), MakeFieldMetadata(Class));

	if (bIncludeFunctions)
	{
		TArray<TSharedPtr<FJsonValue>> Functions;
		for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::IncludeSuper); FunctionIt; ++FunctionIt)
		{
			Functions.Add(MakeShared<FJsonValueObject>(DescribeFunctionSummary(*FunctionIt)));
		}
		Result->SetArrayField(TEXT("functions"), Functions);
	}

	if (bIncludeProperties || bIncludeDelegates)
	{
		TArray<TSharedPtr<FJsonValue>> Properties;
		TArray<TSharedPtr<FJsonValue>> Delegates;
		for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
		{
			FProperty* Property = *PropertyIt;
			if (GetDelegateSignature(Property))
			{
				if (bIncludeDelegates)
				{
					Delegates.Add(MakeShared<FJsonValueObject>(DescribeReflectedProperty(Property)));
				}
				continue;
			}

			if (bIncludeProperties)
			{
				Properties.Add(MakeShared<FJsonValueObject>(DescribeReflectedProperty(Property)));
			}
		}
		if (bIncludeProperties)
		{
			Result->SetArrayField(TEXT("properties"), Properties);
		}
		if (bIncludeDelegates)
		{
			Result->SetArrayField(TEXT("delegates"), Delegates);
		}
	}

	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> FindFunctions(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	if (!TryGetRequiredString(Params, TEXT("class"), ClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindFunctions requires params.class."));
	}

	UClass* Class = ResolveReflectionClass(ClassPath);
	if (!Class)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *ClassPath));
	}

	FString NameContains;
	bool bBlueprintCallableOnly = false;
	bool bIncludeInherited = true;
	Params->TryGetStringField(TEXT("nameContains"), NameContains);
	Params->TryGetBoolField(TEXT("blueprintCallableOnly"), bBlueprintCallableOnly);
	Params->TryGetBoolField(TEXT("includeInherited"), bIncludeInherited);

	TArray<TSharedPtr<FJsonValue>> Functions;
	const EFieldIteratorFlags::SuperClassFlags SuperClassFlags = bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
	for (TFieldIterator<UFunction> FunctionIt(Class, SuperClassFlags); FunctionIt; ++FunctionIt)
	{
		UFunction* Function = *FunctionIt;
		if (!NameContains.IsEmpty() && !Function->GetName().Contains(NameContains, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (bBlueprintCallableOnly && !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
		{
			continue;
		}
		Functions.Add(MakeShared<FJsonValueObject>(DescribeFunctionSummary(Function)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("class"), Class->GetPathName());
	Result->SetArrayField(TEXT("functions"), Functions);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> DescribeFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("class"), ClassPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeFunction requires params.class and params.function."));
	}

	UClass* Class = ResolveReflectionClass(ClassPath);
	if (!Class)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *ClassPath));
	}

	UFunction* Function = FindFunctionByName(Class, FunctionName);
	if (!Function)
	{
		return MakeBridgeError(Id, TEXT("FunctionNotFound"), FString::Printf(TEXT("Could not find function '%s' on '%s'."), *FunctionName, *ClassPath));
	}

	return MakeSuccess(Id, DescribeFunctionFull(Function));
}

TSharedRef<FJsonObject> DescribeProperty(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	FString PropertyName;
	if (!TryGetRequiredString(Params, TEXT("class"), ClassPath) || !TryGetRequiredString(Params, TEXT("property"), PropertyName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeProperty requires params.class and params.property."));
	}

	UClass* Class = ResolveReflectionClass(ClassPath);
	if (!Class)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *ClassPath));
	}

	FProperty* Property = FindPropertyByName(Class, PropertyName);
	if (!Property)
	{
		return MakeBridgeError(Id, TEXT("PropertyNotFound"), FString::Printf(TEXT("Could not find property '%s' on '%s'."), *PropertyName, *ClassPath));
	}

	return MakeSuccess(Id, DescribeReflectedProperty(Property));
}

TSharedRef<FJsonObject> DescribeDelegate(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString ClassPath;
	FString DelegateName;
	if (!TryGetRequiredString(Params, TEXT("class"), ClassPath) || !TryGetRequiredString(Params, TEXT("delegate"), DelegateName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeDelegate requires params.class and params.delegate."));
	}

	UClass* Class = ResolveReflectionClass(ClassPath);
	if (!Class)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *ClassPath));
	}

	FProperty* Property = FindPropertyByName(Class, DelegateName);
	if (!Property)
	{
		return MakeBridgeError(Id, TEXT("DelegateNotFound"), FString::Printf(TEXT("Could not find delegate '%s' on '%s'."), *DelegateName, *ClassPath));
	}

	UFunction* Signature = GetDelegateSignature(Property);
	if (!Signature)
	{
		return MakeBridgeError(Id, TEXT("DelegateNotFound"), FString::Printf(TEXT("Property '%s' is not a delegate."), *DelegateName));
	}

	TSharedRef<FJsonObject> Result = DescribeReflectedProperty(Property);
	Result->SetBoolField(TEXT("multicast"), Property->IsA<FMulticastDelegateProperty>());
	Result->SetObjectField(TEXT("signature"), DescribeFunctionFull(Signature));
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
