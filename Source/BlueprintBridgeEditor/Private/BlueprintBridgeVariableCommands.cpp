// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
TSharedRef<FJsonObject> SetBlueprintDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString PropertyName;
	FString Value;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("property"), PropertyName) ||
		!TryGetRequiredString(Params, TEXT("value"), Value))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetBlueprintDefault requires params.asset, params.property, and params.value."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load compiled Blueprint '%s'."), *AssetPath));
	}

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	FProperty* Property = Blueprint->GeneratedClass->FindPropertyByName(*PropertyName);
	if (!CDO || !Property)
	{
		return MakeBridgeError(Id, TEXT("PropertyNotFound"), FString::Printf(TEXT("Could not find property '%s' on '%s'."), *PropertyName, *AssetPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetBlueprintDefault", "Blueprint Bridge: Set Blueprint Default"));
	Blueprint->Modify();
	CDO->Modify();

	void* PropertyValue = Property->ContainerPtrToValuePtr<void>(CDO);
	const TCHAR* ImportResult = Property->ImportText_Direct(*Value, PropertyValue, CDO, PPF_None);
	if (!ImportResult)
	{
		return MakeBridgeError(Id, TEXT("ImportFailed"), FString::Printf(TEXT("Could not import '%s' into property '%s'."), *Value, *PropertyName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("DefaultSet"));
}

TSharedRef<FJsonObject> DescribeSubobject(UObject* Subobject, const bool bIncludeProperties)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("name"), Subobject->GetName());
	Json->SetStringField(TEXT("class"), Subobject->GetClass()->GetPathName());
	Json->SetStringField(TEXT("path"), Subobject->GetPathName());
	Json->SetStringField(TEXT("outer"), GetPathNameSafe(Subobject->GetOuter()));

	if (bIncludeProperties)
	{
		TArray<TSharedPtr<FJsonValue>> Properties;
		for (TFieldIterator<FProperty> It(Subobject->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}

			FString Value;
			Property->ExportText_InContainer(0, Value, Subobject, Subobject, Subobject, PPF_None);

			TSharedRef<FJsonObject> PropertyJson = MakeShared<FJsonObject>();
			PropertyJson->SetStringField(TEXT("name"), Property->GetName());
			PropertyJson->SetStringField(TEXT("type"), Property->GetCPPType());
			PropertyJson->SetStringField(TEXT("value"), Value);
			Properties.Add(MakeShared<FJsonValueObject>(PropertyJson));
		}
		Json->SetArrayField(TEXT("properties"), Properties);
	}

	return Json;
}

void GetBlueprintCDOSubobjects(UBlueprint* Blueprint, TArray<UObject*>& OutSubobjects)
{
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return;
	}

	TSet<UObject*> Seen;
	auto AddIncludingNested = [&OutSubobjects, &Seen](UObject* Root)
	{
		if (!Root)
		{
			return;
		}
		if (!Seen.Contains(Root))
		{
			Seen.Add(Root);
			OutSubobjects.Add(Root);
		}
		ForEachObjectWithOuter(Root, [&OutSubobjects, &Seen](UObject* Object)
		{
			if (Object && !Seen.Contains(Object))
			{
				Seen.Add(Object);
				OutSubobjects.Add(Object);
			}
		}, true);
	};

	if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
	{
		AddIncludingNested(CDO);
	}

	// Walk the BPGC ancestry: components added in this BP or any ancestor BP live as
	// component templates on the SCS or on the BPGC's InheritableComponentHandler (when overridden).
	// ForEachObjectWithOuter(CDO) does not reach them because their Outer is the BPGC / SCS_Node / ICH.
	for (UClass* Cursor = Blueprint->GeneratedClass; Cursor; Cursor = Cursor->GetSuperClass())
	{
		UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Cursor);
		if (!BPGC)
		{
			continue;
		}

		if (BPGC->SimpleConstructionScript)
		{
			for (USCS_Node* Node : BPGC->SimpleConstructionScript->GetAllNodes())
			{
				if (Node)
				{
					AddIncludingNested(Node->ComponentTemplate);
				}
			}
		}

		if (UInheritableComponentHandler* ICH = BPGC->GetInheritableComponentHandler(false))
		{
			TArray<UActorComponent*> Templates;
			ICH->GetAllTemplates(Templates);
			for (UActorComponent* Template : Templates)
			{
				AddIncludingNested(Template);
			}
		}
	}
}

bool SubobjectMatchesIdentifier(const UObject* Subobject, const FString& Identifier)
{
	if (!Subobject || Identifier.IsEmpty())
	{
		return false;
	}

	const FString Name = Subobject->GetName();
	const FString Path = Subobject->GetPathName();
	return Name.Equals(Identifier, ESearchCase::IgnoreCase)
		|| Path.Equals(Identifier, ESearchCase::IgnoreCase)
		|| Path.EndsWith(FString::Printf(TEXT(":%s"), *Identifier), ESearchCase::IgnoreCase)
		|| Path.EndsWith(FString::Printf(TEXT(".%s"), *Identifier), ESearchCase::IgnoreCase);
}

bool SubobjectMatchesClassFilter(const UObject* Subobject, const FString& ClassPath)
{
	if (!Subobject || ClassPath.IsEmpty())
	{
		return true;
	}

	UClass* Class = LoadClassByPath(ClassPath);
	return Class && Subobject->IsA(Class);
}

TSharedRef<FJsonObject> DescribeSubobjects(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeSubobjects requires params.asset."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load compiled Blueprint '%s'."), *AssetPath));
	}

	FString ClassPath;
	Params->TryGetStringField(TEXT("subobjectClass"), ClassPath);
	bool bIncludeProperties = false;
	Params->TryGetBoolField(TEXT("includeProperties"), bIncludeProperties);

	TArray<UObject*> Subobjects;
	GetBlueprintCDOSubobjects(Blueprint, Subobjects);

	TArray<TSharedPtr<FJsonValue>> SubobjectValues;
	for (UObject* Subobject : Subobjects)
	{
		if (SubobjectMatchesClassFilter(Subobject, ClassPath))
		{
			SubobjectValues.Add(MakeShared<FJsonValueObject>(DescribeSubobject(Subobject, bIncludeProperties)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("subobjects"), SubobjectValues);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> SetSubobjectDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString SubobjectIdentifier;
	FString PropertyName;
	FString Value;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("subobject"), SubobjectIdentifier) ||
		!TryGetRequiredString(Params, TEXT("property"), PropertyName) ||
		!TryGetRequiredString(Params, TEXT("value"), Value))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetSubobjectDefault requires params.asset, params.subobject, params.property, and params.value."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load compiled Blueprint '%s'."), *AssetPath));
	}

	FString ClassPath;
	Params->TryGetStringField(TEXT("subobjectClass"), ClassPath);

	TArray<UObject*> Subobjects;
	GetBlueprintCDOSubobjects(Blueprint, Subobjects);

	TArray<UObject*> Matches;
	for (UObject* Subobject : Subobjects)
	{
		if (SubobjectMatchesIdentifier(Subobject, SubobjectIdentifier) && SubobjectMatchesClassFilter(Subobject, ClassPath))
		{
			Matches.Add(Subobject);
		}
	}

	if (Matches.IsEmpty())
	{
		return MakeBridgeError(Id, TEXT("SubobjectNotFound"), FString::Printf(TEXT("Could not find subobject '%s' on '%s'."), *SubobjectIdentifier, *AssetPath));
	}
	if (Matches.Num() > 1)
	{
		TArray<TSharedPtr<FJsonValue>> MatchValues;
		for (UObject* Match : Matches)
		{
			MatchValues.Add(MakeShared<FJsonValueObject>(DescribeSubobject(Match, false)));
		}
		TSharedRef<FJsonObject> Error = MakeBridgeError(Id, TEXT("AmbiguousSubobject"), FString::Printf(TEXT("Found %d subobjects matching '%s'."), Matches.Num(), *SubobjectIdentifier));
		Error->GetObjectField(TEXT("error"))->SetArrayField(TEXT("matches"), MatchValues);
		return Error;
	}

	UObject* Target = Matches[0];
	FProperty* Property = Target->GetClass()->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		return MakeBridgeError(Id, TEXT("PropertyNotFound"), FString::Printf(TEXT("Could not find property '%s' on subobject '%s'."), *PropertyName, *Target->GetPathName()));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetSubobjectDefault", "Blueprint Bridge: Set Subobject Default"));
	Blueprint->Modify();
	Target->Modify();

	void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Target);
	const TCHAR* ImportResult = Property->ImportText_Direct(*Value, PropertyValue, Target, PPF_None);
	if (!ImportResult)
	{
		return MakeBridgeError(Id, TEXT("ImportFailed"), FString::Printf(TEXT("Could not import '%s' into property '%s' on subobject '%s'."), *Value, *PropertyName, *Target->GetPathName()));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("subobject"), Target->GetPathName());
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("value"), Value);
	return MakeSuccess(Id, Result);
}

FName NormalizePinCategory(const FString& Category)
{
	if (Category.Equals(TEXT("bool"), ESearchCase::IgnoreCase) || Category.Equals(TEXT("boolean"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Boolean;
	}
	if (Category.Equals(TEXT("int"), ESearchCase::IgnoreCase) || Category.Equals(TEXT("integer"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Int;
	}
	if (Category.Equals(TEXT("float"), ESearchCase::IgnoreCase) || Category.Equals(TEXT("double"), ESearchCase::IgnoreCase) || Category.Equals(TEXT("real"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Real;
	}
	if (Category.Equals(TEXT("string"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_String;
	}
	if (Category.Equals(TEXT("name"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Name;
	}
	if (Category.Equals(TEXT("text"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Text;
	}
	if (Category.Equals(TEXT("enum"), ESearchCase::IgnoreCase) || Category.Equals(TEXT("byte"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Byte;
	}
	if (Category.Equals(TEXT("object"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Object;
	}
	if (Category.Equals(TEXT("struct"), ESearchCase::IgnoreCase))
	{
		return UEdGraphSchema_K2::PC_Struct;
	}
	return *Category;
}

TSharedPtr<FJsonObject> ApplyBlueprintVariableFlagsFromParams(const FString& Id, UBlueprint* Blueprint, FBPVariableDescription& Variable, const FString& VariableName, const TSharedPtr<FJsonObject>& Params)
{
	bool bValue = false;
	if (Params->TryGetBoolField(TEXT("instanceEditable"), bValue))
	{
		Variable.PropertyFlags |= CPF_Edit;
		if (bValue)
		{
			Variable.PropertyFlags &= ~CPF_DisableEditOnInstance;
		}
		else
		{
			Variable.PropertyFlags |= CPF_DisableEditOnInstance;
		}
	}

	if (Params->TryGetBoolField(TEXT("blueprintReadOnly"), bValue))
	{
		if (bValue)
		{
			Variable.PropertyFlags |= CPF_BlueprintReadOnly;
		}
		else
		{
			Variable.PropertyFlags &= ~CPF_BlueprintReadOnly;
		}
	}

	if (Params->TryGetBoolField(TEXT("exposeOnSpawn"), bValue))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, *VariableName, nullptr, TEXT("ExposeOnSpawn"), bValue ? TEXT("true") : TEXT("false"));
	}

	if (Params->TryGetBoolField(TEXT("private"), bValue))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, *VariableName, nullptr, TEXT("Private"), bValue ? TEXT("true") : TEXT("false"));
	}

	FString Category;
	if (Params->TryGetStringField(TEXT("categoryName"), Category))
	{
		Variable.Category = FText::FromString(Category);
	}

	FString Tooltip;
	if (Params->TryGetStringField(TEXT("tooltip"), Tooltip))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, *VariableName, nullptr, TEXT("Tooltip"), Tooltip);
	}

	FString Replication;
	if (Params->TryGetStringField(TEXT("replication"), Replication))
	{
		if (Replication.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			Variable.PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
			Variable.RepNotifyFunc = NAME_None;
		}
		else if (Replication.Equals(TEXT("Replicated"), ESearchCase::IgnoreCase))
		{
			Variable.PropertyFlags |= CPF_Net;
			Variable.PropertyFlags &= ~CPF_RepNotify;
			Variable.RepNotifyFunc = NAME_None;
		}
		else if (Replication.Equals(TEXT("RepNotify"), ESearchCase::IgnoreCase))
		{
			FString RepNotifyFunc;
			if (!TryGetRequiredString(Params, TEXT("repNotifyFunc"), RepNotifyFunc))
			{
				return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("RepNotify replication requires params.repNotifyFunc."));
			}
			Variable.PropertyFlags |= CPF_Net | CPF_RepNotify;
			Variable.RepNotifyFunc = *RepNotifyFunc;
		}
		else
		{
			return MakeBridgeError(Id, TEXT("InvalidParams"), FString::Printf(TEXT("Unknown replication mode '%s'."), *Replication));
		}
	}

	return nullptr;
}

TSharedRef<FJsonObject> SetBlueprintVariableFlags(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString VariableName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("variable"), VariableName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetBlueprintVariableFlags requires params.asset and params.variable."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, *VariableName);
	if (VarIndex == INDEX_NONE)
	{
		return MakeBridgeError(Id, TEXT("VariableNotFound"), FString::Printf(TEXT("Could not find variable '%s'."), *VariableName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetBlueprintVariableFlags", "Blueprint Bridge: Set Blueprint Variable Flags"));
	Blueprint->Modify();
	FBPVariableDescription& Variable = Blueprint->NewVariables[VarIndex];
	if (const TSharedPtr<FJsonObject> Error = ApplyBlueprintVariableFlagsFromParams(Id, Blueprint, Variable, VariableName, Params))
	{
		return Error.ToSharedRef();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("variable"), VariableName);
	Result->SetBoolField(TEXT("instanceEditable"), ((Variable.PropertyFlags & CPF_Edit) != 0) && ((Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0));
	Result->SetBoolField(TEXT("blueprintReadOnly"), ((Variable.PropertyFlags & CPF_BlueprintReadOnly) != 0));
	Result->SetBoolField(TEXT("replicated"), ((Variable.PropertyFlags & CPF_Net) != 0));
	Result->SetBoolField(TEXT("repNotify"), ((Variable.PropertyFlags & CPF_RepNotify) != 0));
	Result->SetStringField(TEXT("repNotifyFunc"), Variable.RepNotifyFunc.ToString());
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> AddBlueprintVariable(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Name;
	FString Category;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("name"), Name) ||
		!TryGetRequiredString(Params, TEXT("category"), Category))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddBlueprintVariable requires params.asset, params.name, and params.category."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	FEdGraphPinType PinType;
	PinType.PinCategory = NormalizePinCategory(Category);
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	}
	FString SubCategory;
	if (Params->TryGetStringField(TEXT("subCategory"), SubCategory))
	{
		PinType.PinSubCategory = *SubCategory;
	}

	FString SubCategoryObjectPath;
	if (Params->TryGetStringField(TEXT("subCategoryObject"), SubCategoryObjectPath))
	{
		UObject* SubCategoryObject = StaticLoadObject(UObject::StaticClass(), nullptr, *SubCategoryObjectPath);
		if (!SubCategoryObject)
		{
			return MakeBridgeError(Id, TEXT("TypeNotFound"), FString::Printf(TEXT("Could not load type object '%s'."), *SubCategoryObjectPath));
		}
		PinType.PinSubCategoryObject = SubCategoryObject;
	}

	FString PinTypeError;
	if (!ApplyPinContainerType(Params, PinType, PinTypeError))
	{
		return MakeBridgeError(Id, TEXT("InvalidPinType"), PinTypeError);
	}

	FString DefaultValue;
	Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddBlueprintVariable", "Blueprint Bridge: Add Blueprint Variable"));
	Blueprint->Modify();
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, *Name, PinType, DefaultValue))
	{
		return MakeBridgeError(Id, TEXT("AddVariableFailed"), FString::Printf(TEXT("Could not add variable '%s'."), *Name));
	}

	const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, *Name);
	if (VarIndex != INDEX_NONE)
	{
		if (const TSharedPtr<FJsonObject> Error = ApplyBlueprintVariableFlagsFromParams(Id, Blueprint, Blueprint->NewVariables[VarIndex], Name, Params))
		{
			return Error.ToSharedRef();
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("VariableAdded"));
}
} // namespace BlueprintBridge
