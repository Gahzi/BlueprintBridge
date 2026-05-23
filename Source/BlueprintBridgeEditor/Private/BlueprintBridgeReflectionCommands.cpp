// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

#include "BlueprintBridgeFieldSelection.h"

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

TArray<TSharedPtr<FJsonValue>> CollectClassDelegates(UClass* Class)
{
	TArray<TSharedPtr<FJsonValue>> Delegates;
	if (!Class)
	{
		return Delegates;
	}
	for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::IncludeSuper); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		if (GetDelegateSignature(Property))
		{
			Delegates.Add(MakeShared<FJsonValueObject>(DescribeReflectedProperty(Property)));
		}
	}
	return Delegates;
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

	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
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

	return MakeSuccess(Id, ApplyFieldSelection(Params, DescribeFunctionFull(Function)));
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

struct FBridgeSignatureParam
{
	FName Name;
	FString Direction;
	FEdGraphPinType PinType;
	bool bByRef = false;
	bool bIsConst = false;
};

static TSharedRef<FJsonObject> DescribeSignatureParam(const FBridgeSignatureParam& Param)
{
	TSharedRef<FJsonObject> Result = DescribePinType(Param.PinType);
	Result->SetStringField(TEXT("name"), Param.Name.ToString());
	Result->SetStringField(TEXT("direction"), Param.Direction);
	Result->SetBoolField(TEXT("byRef"), Param.bByRef);
	Result->SetBoolField(TEXT("isConst"), Param.bIsConst);
	return Result;
}

static void BuildSignatureFromUFunction(UFunction* Function, TArray<FBridgeSignatureParam>& OutParams)
{
	if (!Function)
	{
		return;
	}

	for (TFieldIterator<FProperty> PropertyIt(Function); PropertyIt && PropertyIt->HasAnyPropertyFlags(CPF_Parm); ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			continue;
		}

		FEdGraphPinType PinType;
		if (!GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, PinType))
		{
			continue;
		}

		FBridgeSignatureParam Param;
		Param.Name = Property->GetFName();
		Param.Direction = PropertyDirection(Property);
		Param.PinType = PinType;
		Param.bByRef = Property->HasAnyPropertyFlags(CPF_ReferenceParm);
		Param.bIsConst = Property->HasAnyPropertyFlags(CPF_ConstParm);
		Param.PinType.bIsReference = Param.bByRef;
		Param.PinType.bIsConst = Param.bIsConst;
		OutParams.Add(Param);
	}
}

static void BuildSignatureFromBlueprintFunctionGraph(UEdGraph* Graph, TArray<FBridgeSignatureParam>& OutParams)
{
	if (!Graph)
	{
		return;
	}

	if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Graph))
	{
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->Direction != EGPD_Output)
			{
				continue;
			}

			FBridgeSignatureParam Param;
			Param.Name = Pin->PinName;
			Param.Direction = TEXT("Input");
			Param.PinType = Pin->PinType;
			Param.bByRef = Pin->PinType.bIsReference;
			Param.bIsConst = Pin->PinType.bIsConst;
			OutParams.Add(Param);
		}
	}

	if (UK2Node_FunctionResult* ResultNode = FindFunctionResultNode(Graph))
	{
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->Direction != EGPD_Input)
			{
				continue;
			}

			FBridgeSignatureParam Param;
			Param.Name = Pin->PinName;
			Param.Direction = TEXT("Output");
			Param.PinType = Pin->PinType;
			Param.bByRef = Pin->PinType.bIsReference;
			Param.bIsConst = Pin->PinType.bIsConst;
			OutParams.Add(Param);
		}
	}
}

static bool PinTypesCompatible(const FEdGraphPinType& Expected, const FEdGraphPinType& Actual)
{
	return Expected.PinCategory == Actual.PinCategory &&
		Expected.PinSubCategory == Actual.PinSubCategory &&
		Expected.PinSubCategoryObject == Actual.PinSubCategoryObject &&
		Expected.ContainerType == Actual.ContainerType;
}

static void AddSignatureMismatch(TArray<TSharedPtr<FJsonValue>>& Mismatches, const FString& Kind, const FBridgeSignatureParam* Expected, const FBridgeSignatureParam* Actual)
{
	TSharedRef<FJsonObject> Mismatch = MakeShared<FJsonObject>();
	Mismatch->SetStringField(TEXT("kind"), Kind);
	if (Expected)
	{
		Mismatch->SetStringField(TEXT("param"), Expected->Name.ToString());
		Mismatch->SetObjectField(TEXT("expected"), DescribeSignatureParam(*Expected));
	}
	if (Actual)
	{
		if (!Expected)
		{
			Mismatch->SetStringField(TEXT("param"), Actual->Name.ToString());
		}
		Mismatch->SetObjectField(TEXT("actual"), DescribeSignatureParam(*Actual));
	}
	if (Expected && Actual && (Expected->bByRef != Actual->bByRef || Expected->bIsConst != Actual->bIsConst))
	{
		TSharedRef<FJsonObject> SuggestedFix = MakeShared<FJsonObject>();
		SuggestedFix->SetStringField(TEXT("command"), TEXT("SetUserDefinedPinFlags"));
		TSharedRef<FJsonObject> SuggestedParams = MakeShared<FJsonObject>();
		SuggestedParams->SetStringField(TEXT("pin"), Actual->Name.ToString());
		SuggestedParams->SetBoolField(TEXT("byRef"), Expected->bByRef);
		SuggestedParams->SetBoolField(TEXT("isConst"), Expected->bIsConst);
		SuggestedFix->SetObjectField(TEXT("params"), SuggestedParams);
		Mismatch->SetObjectField(TEXT("suggestedFix"), SuggestedFix);
	}
	Mismatches.Add(MakeShared<FJsonValueObject>(Mismatch));
}

TSharedRef<FJsonObject> CheckDelegateCompatibility(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString FunctionName;
	FString DelegateOwnerClassPath;
	FString DelegateName;
	if (!TryGetRequiredString(Params, TEXT("function"), FunctionName) ||
		!TryGetRequiredString(Params, TEXT("delegateOwnerClass"), DelegateOwnerClassPath) ||
		!TryGetRequiredString(Params, TEXT("delegate"), DelegateName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CheckDelegateCompatibility requires params.function, params.delegateOwnerClass, and params.delegate."));
	}

	UClass* DelegateOwnerClass = ResolveReflectionClass(DelegateOwnerClassPath);
	if (!DelegateOwnerClass)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load delegate owner class '%s'."), *DelegateOwnerClassPath));
	}

	FProperty* DelegateProperty = FindPropertyByName(DelegateOwnerClass, DelegateName);
	UFunction* DelegateSignature = GetDelegateSignature(DelegateProperty);
	if (!DelegateSignature)
	{
		return MakeBridgeError(Id, TEXT("DelegateNotFound"), FString::Printf(TEXT("Could not find delegate '%s' on '%s'."), *DelegateName, *DelegateOwnerClassPath));
	}

	TArray<FBridgeSignatureParam> DelegateParams;
	BuildSignatureFromUFunction(DelegateSignature, DelegateParams);

	TArray<FBridgeSignatureParam> FunctionParams;
	FString FunctionOwner;
	FString AssetPath;
	FString FunctionClassPath;
	if (Params->TryGetStringField(TEXT("asset"), AssetPath) && !AssetPath.IsEmpty())
	{
		UBlueprint* Blueprint = LoadBlueprint(AssetPath);
		if (!Blueprint)
		{
			return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
		}

		UEdGraph* FunctionGraph = FindBlueprintGraph(Blueprint, FunctionName);
		if (!FunctionGraph || !Blueprint->FunctionGraphs.Contains(FunctionGraph))
		{
			return MakeBridgeError(Id, TEXT("FunctionNotFound"), FString::Printf(TEXT("Could not find function graph '%s' on '%s'."), *FunctionName, *AssetPath));
		}
		BuildSignatureFromBlueprintFunctionGraph(FunctionGraph, FunctionParams);
		FunctionOwner = AssetPath;
	}
	else if (Params->TryGetStringField(TEXT("functionClass"), FunctionClassPath) && !FunctionClassPath.IsEmpty())
	{
		UClass* FunctionClass = ResolveReflectionClass(FunctionClassPath);
		if (!FunctionClass)
		{
			return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load function class '%s'."), *FunctionClassPath));
		}

		UFunction* Function = FindFunctionByName(FunctionClass, FunctionName);
		if (!Function)
		{
			return MakeBridgeError(Id, TEXT("FunctionNotFound"), FString::Printf(TEXT("Could not find function '%s' on '%s'."), *FunctionName, *FunctionClassPath));
		}
		BuildSignatureFromUFunction(Function, FunctionParams);
		FunctionOwner = FunctionClass->GetPathName();
	}
	else
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CheckDelegateCompatibility requires params.asset or params.functionClass."));
	}

	TArray<TSharedPtr<FJsonValue>> Mismatches;
	if (DelegateParams.Num() != FunctionParams.Num())
	{
		TSharedRef<FJsonObject> Mismatch = MakeShared<FJsonObject>();
		Mismatch->SetStringField(TEXT("kind"), TEXT("ParamCountMismatch"));
		Mismatch->SetNumberField(TEXT("expectedCount"), DelegateParams.Num());
		Mismatch->SetNumberField(TEXT("actualCount"), FunctionParams.Num());
		Mismatches.Add(MakeShared<FJsonValueObject>(Mismatch));
	}

	const int32 CompareCount = FMath::Min(DelegateParams.Num(), FunctionParams.Num());
	for (int32 Index = 0; Index < CompareCount; ++Index)
	{
		const FBridgeSignatureParam& Expected = DelegateParams[Index];
		const FBridgeSignatureParam& Actual = FunctionParams[Index];
		if (!Expected.Name.ToString().Equals(Actual.Name.ToString(), ESearchCase::IgnoreCase))
		{
			AddSignatureMismatch(Mismatches, TEXT("ParamNameMismatch"), &Expected, &Actual);
			continue;
		}
		if (Expected.Direction != Actual.Direction)
		{
			AddSignatureMismatch(Mismatches, TEXT("ParamDirectionMismatch"), &Expected, &Actual);
			continue;
		}
		if (!PinTypesCompatible(Expected.PinType, Actual.PinType))
		{
			AddSignatureMismatch(Mismatches, TEXT("ParamTypeMismatch"), &Expected, &Actual);
			continue;
		}
		if (Expected.bByRef != Actual.bByRef || Expected.bIsConst != Actual.bIsConst)
		{
			AddSignatureMismatch(Mismatches, TEXT("ParamFlagsMismatch"), &Expected, &Actual);
		}
	}

	TArray<TSharedPtr<FJsonValue>> ExpectedParamsJson;
	for (const FBridgeSignatureParam& Param : DelegateParams)
	{
		ExpectedParamsJson.Add(MakeShared<FJsonValueObject>(DescribeSignatureParam(Param)));
	}
	TArray<TSharedPtr<FJsonValue>> ActualParamsJson;
	for (const FBridgeSignatureParam& Param : FunctionParams)
	{
		ActualParamsJson.Add(MakeShared<FJsonValueObject>(DescribeSignatureParam(Param)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("compatible"), Mismatches.IsEmpty());
	Result->SetArrayField(TEXT("mismatches"), Mismatches);
	TSharedRef<FJsonObject> DelegateJson = MakeShared<FJsonObject>();
	DelegateJson->SetStringField(TEXT("ownerClass"), DelegateOwnerClass->GetPathName());
	DelegateJson->SetStringField(TEXT("name"), DelegateName);
	DelegateJson->SetStringField(TEXT("signature"), DelegateSignature->GetName());
	DelegateJson->SetArrayField(TEXT("params"), ExpectedParamsJson);
	Result->SetObjectField(TEXT("delegate"), DelegateJson);
	TSharedRef<FJsonObject> FunctionJson = MakeShared<FJsonObject>();
	FunctionJson->SetStringField(TEXT("ownerClass"), FunctionOwner);
	FunctionJson->SetStringField(TEXT("name"), FunctionName);
	FunctionJson->SetArrayField(TEXT("params"), ActualParamsJson);
	Result->SetObjectField(TEXT("function"), FunctionJson);
	return MakeSuccess(Id, Result);
}

struct FReflectionSymbolResult
{
	int32 Score = 0;
	FString SortKey;
	TSharedPtr<FJsonObject> Json;
};

static bool ShouldIncludeKind(const TSet<FString>& Kinds, const FString& Kind)
{
	return Kinds.IsEmpty() || Kinds.Contains(Kind);
}

static TSet<FString> GetRequestedSymbolKinds(const TSharedPtr<FJsonObject>& Params)
{
	TSet<FString> Kinds;
	const TArray<TSharedPtr<FJsonValue>>* KindValues = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("kinds"), KindValues) || KindValues == nullptr)
	{
		return Kinds;
	}

	for (const TSharedPtr<FJsonValue>& Value : *KindValues)
	{
		FString Kind;
		if (Value.IsValid() && Value->TryGetString(Kind))
		{
			Kinds.Add(Kind.ToLower());
		}
	}
	return Kinds;
}

static bool IsEngineReflectionClass(const UClass* Class)
{
	if (!Class)
	{
		return true;
	}

	const FString Path = Class->GetPathName();
	static const TCHAR* EnginePrefixes[] = {
		TEXT("/Script/Core"),
		TEXT("/Script/CoreUObject"),
		TEXT("/Script/Engine"),
		TEXT("/Script/Slate"),
		TEXT("/Script/SlateCore"),
		TEXT("/Script/UMG"),
		TEXT("/Script/UnrealEd"),
		TEXT("/Script/BlueprintGraph"),
		TEXT("/Script/Editor"),
	};
	for (const TCHAR* Prefix : EnginePrefixes)
	{
		if (Path.StartsWith(Prefix))
		{
			return true;
		}
	}
	return false;
}

static int32 ScoreSymbol(const FString& Name, const FString& Query, const FString& DisplayName = FString())
{
	if (Name.Equals(Query, ESearchCase::CaseSensitive))
	{
		return 100;
	}
	if (Name.Equals(Query, ESearchCase::IgnoreCase))
	{
		return 95;
	}
	if (Name.StartsWith(Query, ESearchCase::IgnoreCase))
	{
		return 85;
	}
	if (Name.Contains(Query, ESearchCase::IgnoreCase))
	{
		return 70;
	}
	if (!DisplayName.IsEmpty() && DisplayName.Equals(Query, ESearchCase::IgnoreCase))
	{
		return 90;
	}
	if (!DisplayName.IsEmpty() && DisplayName.Contains(Query, ESearchCase::IgnoreCase))
	{
		return 65;
	}
	return 0;
}

static void AddSymbolResult(TArray<FReflectionSymbolResult>& Results, TSharedRef<FJsonObject> Json, const int32 Score)
{
	if (Score <= 0)
	{
		return;
	}

	FString Kind;
	FString Name;
	FString Owner;
	Json->TryGetStringField(TEXT("kind"), Kind);
	Json->TryGetStringField(TEXT("name"), Name);
	Json->TryGetStringField(TEXT("ownerClass"), Owner);
	Json->SetNumberField(TEXT("score"), Score);

	FReflectionSymbolResult Result;
	Result.Score = Score;
	Result.SortKey = FString::Printf(TEXT("%s:%s:%s"), *Kind, *Owner, *Name);
	Result.Json = Json;
	Results.Add(Result);
}

static void SearchClassSymbols(UClass* Class, const FString& Query, const TSet<FString>& Kinds, const bool bBlueprintCallableOnly, const bool bIncludeInherited, TArray<FReflectionSymbolResult>& Results)
{
	if (!Class)
	{
		return;
	}

	if (ShouldIncludeKind(Kinds, TEXT("class")))
	{
		TSharedRef<FJsonObject> ClassJson = MakeShared<FJsonObject>();
		ClassJson->SetStringField(TEXT("kind"), TEXT("class"));
		ClassJson->SetStringField(TEXT("name"), Class->GetName());
		ClassJson->SetStringField(TEXT("path"), Class->GetPathName());
		ClassJson->SetStringField(TEXT("ownerClass"), Class->GetPathName());
		AddSymbolResult(Results, ClassJson, ScoreSymbol(Class->GetName(), Query));
	}

	const EFieldIteratorFlags::SuperClassFlags SuperClassFlags = bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper;
	if (ShouldIncludeKind(Kinds, TEXT("function")))
	{
		for (TFieldIterator<UFunction> FunctionIt(Class, SuperClassFlags); FunctionIt; ++FunctionIt)
		{
			UFunction* Function = *FunctionIt;
			if (bBlueprintCallableOnly && !Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure))
			{
				continue;
			}
			const FString DisplayName = Function->GetMetaData(TEXT("DisplayName"));
			const int32 Score = ScoreSymbol(Function->GetName(), Query, DisplayName) + (Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure) ? 5 : 0);
			TSharedRef<FJsonObject> FunctionJson = DescribeFunctionSummary(Function);
			FunctionJson->SetStringField(TEXT("kind"), TEXT("function"));
			FunctionJson->SetStringField(TEXT("path"), Function->GetPathName());
			AddSymbolResult(Results, FunctionJson, Score);
		}
	}

	for (TFieldIterator<FProperty> PropertyIt(Class, SuperClassFlags); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		const bool bDelegate = GetDelegateSignature(Property) != nullptr;
		const FString Kind = bDelegate ? TEXT("delegate") : TEXT("property");
		if (!ShouldIncludeKind(Kinds, Kind))
		{
			continue;
		}
		if (bBlueprintCallableOnly && !Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			continue;
		}

		const FString DisplayName = Property->GetMetaData(TEXT("DisplayName"));
		const int32 Score = ScoreSymbol(Property->GetName(), Query, DisplayName) + (Property->HasAnyPropertyFlags(CPF_BlueprintVisible) ? 5 : 0);
		TSharedRef<FJsonObject> PropertyJson = DescribeReflectedProperty(Property);
		PropertyJson->SetStringField(TEXT("kind"), Kind);
		PropertyJson->SetStringField(TEXT("path"), FString::Printf(TEXT("%s:%s"), *Class->GetPathName(), *Property->GetName()));
		AddSymbolResult(Results, PropertyJson, Score);
	}
}

TSharedRef<FJsonObject> FindReflectionSymbols(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!TryGetRequiredString(Params, TEXT("query"), Query) || Query.IsEmpty())
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindReflectionSymbols requires non-empty params.query."));
	}

	bool bBlueprintCallableOnly = false;
	bool bIncludeEngine = false;
	bool bIncludeProject = true;
	bool bIncludeInherited = true;
	Params->TryGetBoolField(TEXT("blueprintCallableOnly"), bBlueprintCallableOnly);
	Params->TryGetBoolField(TEXT("includeEngine"), bIncludeEngine);
	Params->TryGetBoolField(TEXT("includeProject"), bIncludeProject);
	Params->TryGetBoolField(TEXT("includeInherited"), bIncludeInherited);

	int32 MaxResults = 50;
	Params->TryGetNumberField(TEXT("maxResults"), MaxResults);
	MaxResults = FMath::Clamp(MaxResults, 1, 500);

	const TSet<FString> Kinds = GetRequestedSymbolKinds(Params);
	TArray<FReflectionSymbolResult> Results;

	FString ClassPath;
	if (Params->TryGetStringField(TEXT("class"), ClassPath) && !ClassPath.IsEmpty())
	{
		UClass* Class = ResolveReflectionClass(ClassPath);
		if (!Class)
		{
			return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *ClassPath));
		}
		SearchClassSymbols(Class, Query, Kinds, bBlueprintCallableOnly, bIncludeInherited, Results);
	}
	else
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* Class = *ClassIt;
			const bool bEngineClass = IsEngineReflectionClass(Class);
			if ((bEngineClass && !bIncludeEngine) || (!bEngineClass && !bIncludeProject))
			{
				continue;
			}
			SearchClassSymbols(Class, Query, Kinds, bBlueprintCallableOnly, false, Results);
		}
	}

	Results.Sort([](const FReflectionSymbolResult& A, const FReflectionSymbolResult& B)
	{
		if (A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
		return A.SortKey < B.SortKey;
	});

	TArray<TSharedPtr<FJsonValue>> ResultValues;
	for (int32 Index = 0; Index < Results.Num() && Index < MaxResults; ++Index)
	{
		if (Results[Index].Json.IsValid())
		{
			ResultValues.Add(MakeShared<FJsonValueObject>(Results[Index].Json.ToSharedRef()));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), Query);
	Result->SetArrayField(TEXT("results"), ResultValues);
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
