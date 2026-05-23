// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

#include "BlueprintBridgeFieldSelection.h"

namespace BlueprintBridge
{
TSharedRef<FJsonObject> DescribeWidgetTree(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeWidgetTree requires params.asset."));
	}

	UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Widget Blueprint '%s'."), *AssetPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	AddWidgetTreeDescription(Result, Blueprint);
	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
}

TSharedRef<FJsonObject> AddWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString WidgetName;
	FString WidgetClassPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("name"), WidgetName) || !TryGetRequiredString(Params, TEXT("widgetClass"), WidgetClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddWidget requires params.asset, params.name, and params.widgetClass."));
	}

	UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->WidgetTree)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Widget Blueprint '%s'."), *AssetPath));
	}

	if (Blueprint->WidgetTree->FindWidget(FName(*WidgetName)))
	{
		return MakeBridgeError(Id, TEXT("WidgetAlreadyExists"), FString::Printf(TEXT("Widget '%s' already exists."), *WidgetName));
	}

	UClass* WidgetClass = LoadClassByPath(WidgetClassPath);
	if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load widget class '%s'."), *WidgetClassPath));
	}

	FString ParentName;
	Params->TryGetStringField(TEXT("parent"), ParentName);
	bool bRootRequested = false;
	Params->TryGetBoolField(TEXT("root"), bRootRequested);
	const bool bSetAsRoot = !Blueprint->WidgetTree->RootWidget || bRootRequested;

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddWidget", "Blueprint Bridge: Add Widget"));
	Blueprint->Modify();
	Blueprint->WidgetTree->Modify();

	UWidget* Widget = Blueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
	if (!Widget)
	{
		return MakeBridgeError(Id, TEXT("AddWidgetFailed"), FString::Printf(TEXT("Could not create widget '%s'."), *WidgetName));
	}

	Blueprint->OnVariableAdded(Widget->GetFName());

	if (bSetAsRoot)
	{
		Blueprint->WidgetTree->RootWidget = Widget;
	}
	else if (!ParentName.IsEmpty())
	{
		UPanelWidget* Parent = Blueprint->WidgetTree->FindWidget<UPanelWidget>(FName(*ParentName));
		if (!Parent)
		{
			return MakeBridgeError(Id, TEXT("WidgetNotFound"), FString::Printf(TEXT("Could not find panel widget '%s'."), *ParentName));
		}

		Parent->Modify();
		Parent->AddChild(Widget);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetObjectField(TEXT("widget"), DescribeWidget(Widget));
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> SetRootWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString WidgetName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("widget"), WidgetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetRootWidget requires params.asset and params.widget."));
	}

	UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->WidgetTree)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Widget Blueprint '%s'."), *AssetPath));
	}

	UWidget* Widget = Blueprint->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
	{
		return MakeBridgeError(Id, TEXT("WidgetNotFound"), FString::Printf(TEXT("Could not find widget '%s'."), *WidgetName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetRootWidget", "Blueprint Bridge: Set Root Widget"));
	Blueprint->Modify();
	Blueprint->WidgetTree->Modify();
	Blueprint->WidgetTree->RootWidget = Widget;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetObjectField(TEXT("widget"), DescribeWidget(Widget));
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> AddWidgetToParent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ParentName;
	FString ChildName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("parent"), ParentName) || !TryGetRequiredString(Params, TEXT("child"), ChildName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddWidgetToParent requires params.asset, params.parent, and params.child."));
	}

	UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->WidgetTree)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Widget Blueprint '%s'."), *AssetPath));
	}

	UPanelWidget* Parent = Blueprint->WidgetTree->FindWidget<UPanelWidget>(FName(*ParentName));
	UWidget* Child = Blueprint->WidgetTree->FindWidget(FName(*ChildName));
	if (!Parent || !Child)
	{
		return MakeBridgeError(Id, TEXT("WidgetNotFound"), FString::Printf(TEXT("Could not find parent '%s' or child '%s'."), *ParentName, *ChildName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddWidgetToParent", "Blueprint Bridge: Add Widget To Parent"));
	Blueprint->Modify();
	Blueprint->WidgetTree->Modify();
	Parent->Modify();
	Parent->AddChild(Child);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetObjectField(TEXT("widget"), DescribeWidget(Child));
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> SetWidgetSlotLayout(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString WidgetName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("widget"), WidgetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetWidgetSlotLayout requires params.asset and params.widget."));
	}

	UWidgetBlueprint* Blueprint = LoadWidgetBlueprint(AssetPath);
	if (!Blueprint || !Blueprint->WidgetTree)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Widget Blueprint '%s'."), *AssetPath));
	}

	UWidget* Widget = Blueprint->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget || !Widget->Slot)
	{
		return MakeBridgeError(Id, TEXT("WidgetNotFound"), FString::Printf(TEXT("Could not find widget slot for '%s'."), *WidgetName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetWidgetSlotLayout", "Blueprint Bridge: Set Widget Slot Layout"));
	Blueprint->Modify();
	Widget->Modify();
	Widget->Slot->Modify();

	const TSharedPtr<FJsonObject>* VectorObject = nullptr;
	if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
	{
		if (Params->TryGetObjectField(TEXT("position"), VectorObject) && VectorObject && VectorObject->IsValid())
		{
			CanvasSlot->SetPosition(FVector2D((*VectorObject)->GetNumberField(TEXT("x")), (*VectorObject)->GetNumberField(TEXT("y"))));
		}
		if (Params->TryGetObjectField(TEXT("size"), VectorObject) && VectorObject && VectorObject->IsValid())
		{
			CanvasSlot->SetSize(FVector2D((*VectorObject)->GetNumberField(TEXT("x")), (*VectorObject)->GetNumberField(TEXT("y"))));
		}
		if (Params->TryGetObjectField(TEXT("alignment"), VectorObject) && VectorObject && VectorObject->IsValid())
		{
			CanvasSlot->SetAlignment(FVector2D((*VectorObject)->GetNumberField(TEXT("x")), (*VectorObject)->GetNumberField(TEXT("y"))));
		}
		const TSharedPtr<FJsonObject>* AnchorsObject = nullptr;
		if (Params->TryGetObjectField(TEXT("anchors"), AnchorsObject) && AnchorsObject && AnchorsObject->IsValid())
		{
			CanvasSlot->SetAnchors(FAnchors((*AnchorsObject)->GetNumberField(TEXT("minimumX")), (*AnchorsObject)->GetNumberField(TEXT("minimumY")), (*AnchorsObject)->GetNumberField(TEXT("maximumX")), (*AnchorsObject)->GetNumberField(TEXT("maximumY"))));
		}
	}

	const TSharedPtr<FJsonObject>* PaddingObject = nullptr;
	if (Params->TryGetObjectField(TEXT("padding"), PaddingObject) && PaddingObject && PaddingObject->IsValid())
	{
		const FMargin Padding((*PaddingObject)->GetNumberField(TEXT("left")), (*PaddingObject)->GetNumberField(TEXT("top")), (*PaddingObject)->GetNumberField(TEXT("right")), (*PaddingObject)->GetNumberField(TEXT("bottom")));
		if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Widget->Slot))
		{
			HorizontalSlot->SetPadding(Padding);
		}
		else if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Widget->Slot))
		{
			VerticalSlot->SetPadding(Padding);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("widget"), DescribeWidget(Widget));
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
