// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
TSharedRef<FJsonObject> DescribeComponentNode(USCS_Node* Node)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Node ? Node->GetVariableName().ToString() : TEXT(""));
	Result->SetStringField(TEXT("class"), Node && Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
	Result->SetStringField(TEXT("parent"), TEXT(""));
	Result->SetBoolField(TEXT("root"), false);

	if (!Node)
	{
		return Result;
	}

	if (USimpleConstructionScript* SCS = Node->GetSCS())
	{
		if (USCS_Node* ParentNode = FindSCSParentNode(SCS, Node))
		{
			Result->SetStringField(TEXT("parent"), ParentNode->GetVariableName().ToString());
		}
		Result->SetBoolField(TEXT("root"), SCS->GetRootNodes().Contains(Node));
	}

	if (UActorComponent* Template = Node->ComponentTemplate)
	{
		Result->SetStringField(TEXT("template"), Template->GetPathName());
	}

	return Result;
}

TSharedRef<FJsonObject> DescribeComponents(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeComponents requires params.asset."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> Components;
	for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (Node)
		{
			Components.Add(MakeShared<FJsonValueObject>(DescribeComponentNode(Node)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("components"), Components);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> AddComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	FString ComponentClassPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("name"), ComponentName) ||
		!TryGetRequiredString(Params, TEXT("componentClass"), ComponentClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddComponent requires params.asset, params.name, and params.componentClass."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	UClass* ComponentClass = LoadClassByPath(ComponentClassPath);
	if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load component class '%s'."), *ComponentClassPath));
	}

	if (FindSCSNodeByName(Blueprint->SimpleConstructionScript, ComponentName))
	{
		return MakeBridgeError(Id, TEXT("ComponentAlreadyExists"), FString::Printf(TEXT("Component '%s' already exists on '%s'."), *ComponentName, *AssetPath));
	}

	FString ParentName;
	Params->TryGetStringField(TEXT("parent"), ParentName);
	USCS_Node* ParentNode = ParentName.IsEmpty() ? nullptr : FindSCSNodeByName(Blueprint->SimpleConstructionScript, ParentName);
	if (!ParentName.IsEmpty() && !ParentNode)
	{
		return MakeBridgeError(Id, TEXT("ComponentNotFound"), FString::Printf(TEXT("Could not find parent component '%s'."), *ParentName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddComponent", "Blueprint Bridge: Add Component"));
	Blueprint->Modify();
	Blueprint->SimpleConstructionScript->Modify();

	USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(ComponentClass, FName(*ComponentName));
	if (!Node)
	{
		return MakeBridgeError(Id, TEXT("AddComponentFailed"), FString::Printf(TEXT("Could not add component '%s'."), *ComponentName));
	}

	if (ParentNode)
	{
		ParentNode->Modify();
		ParentNode->AddChildNode(Node);
	}
	else
	{
		Blueprint->SimpleConstructionScript->AddNode(Node);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("component"), DescribeComponentNode(Node));
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> AttachComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	FString ParentName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("name"), ComponentName) ||
		!TryGetRequiredString(Params, TEXT("parent"), ParentName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AttachComponent requires params.asset, params.name, and params.parent."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	USCS_Node* Node = FindSCSNodeByName(Blueprint->SimpleConstructionScript, ComponentName);
	USCS_Node* ParentNode = FindSCSNodeByName(Blueprint->SimpleConstructionScript, ParentName);
	if (!Node || !ParentNode)
	{
		return MakeBridgeError(Id, TEXT("ComponentNotFound"), TEXT("Could not find component or parent component."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AttachComponent", "Blueprint Bridge: Attach Component"));
	Blueprint->Modify();
	Blueprint->SimpleConstructionScript->Modify();
	Node->Modify();
	ParentNode->Modify();
	if (USCS_Node* OldParentNode = FindSCSParentNode(Blueprint->SimpleConstructionScript, Node))
	{
		OldParentNode->RemoveChildNode(Node, false);
	}
	else
	{
		Blueprint->SimpleConstructionScript->RemoveNode(Node, false);
	}
	ParentNode->AddChildNode(Node, false);
	Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("component"), DescribeComponentNode(Node));
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> SetComponentTransform(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("name"), ComponentName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetComponentTransform requires params.asset and params.name."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	USCS_Node* Node = FindSCSNodeByName(Blueprint->SimpleConstructionScript, ComponentName);
	USceneComponent* SceneComponent = Node ? Cast<USceneComponent>(Node->ComponentTemplate) : nullptr;
	if (!SceneComponent)
	{
		return MakeBridgeError(Id, TEXT("ComponentNotFound"), FString::Printf(TEXT("Could not find scene component '%s'."), *ComponentName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetComponentTransform", "Blueprint Bridge: Set Component Transform"));
	Blueprint->Modify();
	SceneComponent->Modify();

	const TSharedPtr<FJsonObject>* LocationObject = nullptr;
	if (Params->TryGetObjectField(TEXT("location"), LocationObject) && LocationObject && LocationObject->IsValid())
	{
		SceneComponent->SetRelativeLocation(FVector(
			(*LocationObject)->GetNumberField(TEXT("x")),
			(*LocationObject)->GetNumberField(TEXT("y")),
			(*LocationObject)->GetNumberField(TEXT("z"))));
	}

	const TSharedPtr<FJsonObject>* RotationObject = nullptr;
	if (Params->TryGetObjectField(TEXT("rotation"), RotationObject) && RotationObject && RotationObject->IsValid())
	{
		SceneComponent->SetRelativeRotation(FRotator(
			(*RotationObject)->GetNumberField(TEXT("pitch")),
			(*RotationObject)->GetNumberField(TEXT("yaw")),
			(*RotationObject)->GetNumberField(TEXT("roll"))));
	}

	const TSharedPtr<FJsonObject>* ScaleObject = nullptr;
	if (Params->TryGetObjectField(TEXT("scale"), ScaleObject) && ScaleObject && ScaleObject->IsValid())
	{
		SceneComponent->SetRelativeScale3D(FVector(
			(*ScaleObject)->GetNumberField(TEXT("x")),
			(*ScaleObject)->GetNumberField(TEXT("y")),
			(*ScaleObject)->GetNumberField(TEXT("z"))));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("ComponentTransformSet"));
}

TSharedRef<FJsonObject> SetComponentProperty(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	FString PropertyName;
	FString Value;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("name"), ComponentName) ||
		!TryGetRequiredString(Params, TEXT("property"), PropertyName) ||
		!TryGetRequiredString(Params, TEXT("value"), Value))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetComponentProperty requires params.asset, params.name, params.property, and params.value."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	USCS_Node* Node = FindSCSNodeByName(Blueprint->SimpleConstructionScript, ComponentName);
	UActorComponent* ComponentTemplate = Node ? Node->ComponentTemplate : nullptr;
	if (!ComponentTemplate)
	{
		return MakeBridgeError(Id, TEXT("ComponentNotFound"), FString::Printf(TEXT("Could not find component '%s'."), *ComponentName));
	}

	FProperty* Property = ComponentTemplate->GetClass()->FindPropertyByName(*PropertyName);
	if (!Property)
	{
		return MakeBridgeError(Id, TEXT("PropertyNotFound"), FString::Printf(TEXT("Could not find property '%s' on component '%s'."), *PropertyName, *ComponentName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetComponentProperty", "Blueprint Bridge: Set Component Property"));
	Blueprint->Modify();
	ComponentTemplate->Modify();

	void* PropertyValue = Property->ContainerPtrToValuePtr<void>(ComponentTemplate);
	const TCHAR* ImportResult = Property->ImportText_Direct(*Value, PropertyValue, ComponentTemplate, PPF_None);
	if (!ImportResult)
	{
		return MakeBridgeError(Id, TEXT("ImportFailed"), FString::Printf(TEXT("Could not import '%s' into property '%s'."), *Value, *PropertyName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("ComponentPropertySet"));
}

bool TryGetComponentTemplate(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UActorComponent*& OutTemplate, TSharedRef<FJsonObject>& OutError)
{
	FString AssetPath;
	FString ComponentName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("name"), ComponentName))
	{
		OutError = MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("Command requires params.asset and params.name."));
		return false;
	}

	OutBlueprint = LoadBlueprint(AssetPath);
	if (!OutBlueprint || !OutBlueprint->SimpleConstructionScript)
	{
		OutError = MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
		return false;
	}

	USCS_Node* Node = FindSCSNodeByName(OutBlueprint->SimpleConstructionScript, ComponentName);
	OutTemplate = Node ? Node->ComponentTemplate : nullptr;
	if (!OutTemplate)
	{
		OutError = MakeBridgeError(Id, TEXT("ComponentNotFound"), FString::Printf(TEXT("Could not find component '%s'."), *ComponentName));
		return false;
	}

	return true;
}

TSharedRef<FJsonObject> SetRootComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ComponentName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("name"), ComponentName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetRootComponent requires params.asset and params.name."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	USCS_Node* Node = FindSCSNodeByName(Blueprint->SimpleConstructionScript, ComponentName);
	if (!Node || !Cast<USceneComponent>(Node->ComponentTemplate))
	{
		return MakeBridgeError(Id, TEXT("ComponentNotFound"), FString::Printf(TEXT("Could not find scene component '%s'."), *ComponentName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetRootComponent", "Blueprint Bridge: Set Root Component"));
	Blueprint->Modify();
	Blueprint->SimpleConstructionScript->Modify();
	Node->Modify();

	if (USCS_Node* ParentNode = FindSCSParentNode(Blueprint->SimpleConstructionScript, Node))
	{
		ParentNode->Modify();
		ParentNode->RemoveChildNode(Node, false);
	}
	else
	{
		Blueprint->SimpleConstructionScript->RemoveNode(Node, false);
	}

	Blueprint->SimpleConstructionScript->AddNode(Node);
	Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("RootComponentSet"));
}

TSharedRef<FJsonObject> SetStaticMesh(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString MeshPath;
	if (!TryGetRequiredString(Params, TEXT("mesh"), MeshPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetStaticMesh requires params.mesh."));
	}

	UBlueprint* Blueprint = nullptr;
	UActorComponent* Template = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryGetComponentTemplate(Id, Params, Blueprint, Template, Error))
	{
		return Error;
	}

	UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Template);
	if (!StaticMeshComponent)
	{
		return MakeBridgeError(Id, TEXT("InvalidComponentClass"), TEXT("Component is not a StaticMeshComponent."));
	}

	UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshPath));
	if (!Mesh)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load static mesh '%s'."), *MeshPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetStaticMesh", "Blueprint Bridge: Set Static Mesh"));
	Blueprint->Modify();
	StaticMeshComponent->Modify();
	StaticMeshComponent->SetStaticMesh(Mesh);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("StaticMeshSet"));
}

TSharedRef<FJsonObject> SetCollisionProfileName(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString ProfileName;
	if (!TryGetRequiredString(Params, TEXT("profile"), ProfileName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetCollisionProfileName requires params.profile."));
	}

	UBlueprint* Blueprint = nullptr;
	UActorComponent* Template = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryGetComponentTemplate(Id, Params, Blueprint, Template, Error))
	{
		return Error;
	}

	UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Template);
	if (!PrimitiveComponent)
	{
		return MakeBridgeError(Id, TEXT("InvalidComponentClass"), TEXT("Component is not a PrimitiveComponent."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetCollisionProfileName", "Blueprint Bridge: Set Collision Profile Name"));
	Blueprint->Modify();
	PrimitiveComponent->Modify();
	PrimitiveComponent->SetCollisionProfileName(*ProfileName);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("CollisionProfileSet"));
}

TSharedRef<FJsonObject> SetBoxExtent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* ExtentObject = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("extent"), ExtentObject) || ExtentObject == nullptr || !ExtentObject->IsValid())
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetBoxExtent requires params.extent."));
	}

	UBlueprint* Blueprint = nullptr;
	UActorComponent* Template = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryGetComponentTemplate(Id, Params, Blueprint, Template, Error))
	{
		return Error;
	}

	UBoxComponent* BoxComponent = Cast<UBoxComponent>(Template);
	if (!BoxComponent)
	{
		return MakeBridgeError(Id, TEXT("InvalidComponentClass"), TEXT("Component is not a BoxComponent."));
	}

	const FVector Extent((*ExtentObject)->GetNumberField(TEXT("x")), (*ExtentObject)->GetNumberField(TEXT("y")), (*ExtentObject)->GetNumberField(TEXT("z")));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetBoxExtent", "Blueprint Bridge: Set Box Extent"));
	Blueprint->Modify();
	BoxComponent->Modify();
	BoxComponent->SetBoxExtent(Extent, false);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("BoxExtentSet"));
}

TSharedRef<FJsonObject> SetGenerateOverlapEvents(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	bool bGenerateOverlapEvents = false;
	if (!Params.IsValid() || !Params->TryGetBoolField(TEXT("generateOverlapEvents"), bGenerateOverlapEvents))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetGenerateOverlapEvents requires params.generateOverlapEvents."));
	}

	UBlueprint* Blueprint = nullptr;
	UActorComponent* Template = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryGetComponentTemplate(Id, Params, Blueprint, Template, Error))
	{
		return Error;
	}

	UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(Template);
	if (!PrimitiveComponent)
	{
		return MakeBridgeError(Id, TEXT("InvalidComponentClass"), TEXT("Component is not a PrimitiveComponent."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetGenerateOverlapEvents", "Blueprint Bridge: Set Generate Overlap Events"));
	Blueprint->Modify();
	PrimitiveComponent->Modify();
	PrimitiveComponent->SetGenerateOverlapEvents(bGenerateOverlapEvents);
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("GenerateOverlapEventsSet"));
}
} // namespace BlueprintBridge
