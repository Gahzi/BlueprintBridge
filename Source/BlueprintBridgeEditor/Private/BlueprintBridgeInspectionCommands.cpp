// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
TSharedRef<FJsonObject> DescribeWidget(UWidget* Widget)
{
	TSharedRef<FJsonObject> WidgetJson = MakeShared<FJsonObject>();
	WidgetJson->SetStringField(TEXT("name"), Widget ? Widget->GetName() : TEXT(""));
	WidgetJson->SetStringField(TEXT("class"), Widget ? Widget->GetClass()->GetPathName() : TEXT(""));
	WidgetJson->SetBoolField(TEXT("isRoot"), false);

	TArray<TSharedPtr<FJsonValue>> Children;
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		for (int32 ChildIndex = 0; ChildIndex < Panel->GetChildrenCount(); ++ChildIndex)
		{
			if (UWidget* Child = Panel->GetChildAt(ChildIndex))
			{
				Children.Add(MakeShared<FJsonValueString>(Child->GetName()));
			}
		}
	}
	WidgetJson->SetArrayField(TEXT("children"), Children);
	return WidgetJson;
}

void AddWidgetTreeDescription(TSharedRef<FJsonObject> Result, UWidgetBlueprint* WidgetBlueprint)
{
	TSharedRef<FJsonObject> TreeJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Widgets;

	if (WidgetBlueprint && WidgetBlueprint->WidgetTree)
	{
		UWidget* RootWidget = WidgetBlueprint->WidgetTree->RootWidget;
		TreeJson->SetStringField(TEXT("root"), RootWidget ? RootWidget->GetName() : TEXT(""));

		WidgetBlueprint->WidgetTree->ForEachWidget([&Widgets, RootWidget](UWidget* Widget)
		{
			if (!Widget)
			{
				return;
			}

			TSharedRef<FJsonObject> WidgetJson = DescribeWidget(Widget);
			WidgetJson->SetBoolField(TEXT("isRoot"), Widget == RootWidget);
			Widgets.Add(MakeShared<FJsonValueObject>(WidgetJson));
		});
	}
	else
	{
		TreeJson->SetStringField(TEXT("root"), TEXT(""));
	}

	TreeJson->SetArrayField(TEXT("widgets"), Widgets);
	Result->SetObjectField(TEXT("widgetTree"), TreeJson);
}

TSharedRef<FJsonObject> DescribeBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid() || !Params->HasTypedField<EJson::String>(TEXT("asset")))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeBlueprint requires params.asset."));
	}

	const FString AssetPath = Params->GetStringField(TEXT("asset"));
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));

	TArray<TSharedPtr<FJsonValue>> Variables;
	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		TSharedRef<FJsonObject> VariableJson = MakeShared<FJsonObject>();
		VariableJson->SetStringField(TEXT("name"), Variable.VarName.ToString());
		VariableJson->SetStringField(TEXT("category"), Variable.VarType.PinCategory.ToString());
		VariableJson->SetStringField(TEXT("subCategory"), Variable.VarType.PinSubCategory.ToString());
		VariableJson->SetStringField(TEXT("subCategoryObject"), Variable.VarType.PinSubCategoryObject.IsValid() ? Variable.VarType.PinSubCategoryObject->GetPathName() : TEXT(""));
		VariableJson->SetStringField(TEXT("containerType"), PinContainerTypeToString(Variable.VarType));
		VariableJson->SetBoolField(TEXT("instanceEditable"), ((Variable.PropertyFlags & CPF_Edit) != 0) && ((Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0));
		VariableJson->SetBoolField(TEXT("blueprintReadOnly"), ((Variable.PropertyFlags & CPF_BlueprintReadOnly) != 0));
		VariableJson->SetBoolField(TEXT("replicated"), ((Variable.PropertyFlags & CPF_Net) != 0));
		VariableJson->SetBoolField(TEXT("repNotify"), ((Variable.PropertyFlags & CPF_RepNotify) != 0));
		VariableJson->SetStringField(TEXT("repNotifyFunc"), Variable.RepNotifyFunc.ToString());
		Variables.Add(MakeShared<FJsonValueObject>(VariableJson));
	}
	Result->SetArrayField(TEXT("variables"), Variables);

	TArray<TSharedPtr<FJsonValue>> Graphs;
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph)
		{
			Graphs.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		}
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph)
		{
			Graphs.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		}
	}
	Result->SetArrayField(TEXT("graphs"), Graphs);

	if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
	{
		AddWidgetTreeDescription(Result, WidgetBlueprint);
	}

	return MakeSuccess(Id, Result);
}

UEdGraph* FindBlueprintGraph(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	return nullptr;
}

TSharedRef<FJsonObject> DescribeNode(UEdGraphNode* Node)
{
	TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
	NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	NodeJson->SetStringField(TEXT("name"), Node->GetName());
	NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
	NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
	NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);

	if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
	{
		NodeJson->SetStringField(TEXT("variable"), VariableNode->GetVarNameString());
		if (Node->IsA<UK2Node_VariableGet>())
		{
			NodeJson->SetStringField(TEXT("variableAccess"), TEXT("Get"));
		}
		else if (Node->IsA<UK2Node_VariableSet>())
		{
			NodeJson->SetStringField(TEXT("variableAccess"), TEXT("Set"));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Pins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}

		TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
		PinJson->SetStringField(TEXT("id"), Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
		PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
		PinJson->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinJson->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
		PinJson->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT(""));
		PinJson->SetStringField(TEXT("containerType"), PinContainerTypeToString(Pin->PinType));
		PinJson->SetBoolField(TEXT("byRef"), Pin->PinType.bIsReference);
		PinJson->SetBoolField(TEXT("isConst"), Pin->PinType.bIsConst);
		PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);

		TArray<TSharedPtr<FJsonValue>> LinkedTo;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
				LinkJson->SetStringField(TEXT("nodeGuid"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				LinkJson->SetStringField(TEXT("pinId"), LinkedPin->PinId.ToString(EGuidFormats::DigitsWithHyphens));
				LinkJson->SetStringField(TEXT("pinName"), LinkedPin->PinName.ToString());
				LinkedTo.Add(MakeShared<FJsonValueObject>(LinkJson));
			}
		}
		PinJson->SetArrayField(TEXT("linkedTo"), LinkedTo);
		Pins.Add(MakeShared<FJsonValueObject>(PinJson));
	}
	NodeJson->SetArrayField(TEXT("pins"), Pins);
	return NodeJson;
}

TSharedRef<FJsonObject> DescribeGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString GraphName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("graph"), GraphName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeGraph requires params.asset and params.graph."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	UEdGraph* Graph = FindBlueprintGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Could not find graph '%s' on '%s'."), *GraphName, *AssetPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("graph"), Graph->GetName());

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
		}
	}
	Result->SetArrayField(TEXT("nodes"), Nodes);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> FindVariableReferences(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString VariableName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("variable"), VariableName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindVariableReferences requires params.asset and params.variable."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	TArray<UEdGraph*> Graphs;
	Graphs.Append(Blueprint->UbergraphPages);
	Graphs.Append(Blueprint->FunctionGraphs);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("variable"), VariableName);

	TArray<TSharedPtr<FJsonValue>> References;
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
			if (!VariableNode || VariableNode->GetVarName() != *VariableName)
			{
				continue;
			}

			TSharedRef<FJsonObject> ReferenceJson = MakeShared<FJsonObject>();
			ReferenceJson->SetStringField(TEXT("graph"), Graph->GetName());
			ReferenceJson->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			ReferenceJson->SetStringField(TEXT("nodeName"), Node->GetName());
			ReferenceJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			ReferenceJson->SetStringField(TEXT("access"), Node->IsA<UK2Node_VariableGet>() ? TEXT("Get") : Node->IsA<UK2Node_VariableSet>() ? TEXT("Set") : TEXT("Other"));
			ReferenceJson->SetNumberField(TEXT("x"), Node->NodePosX);
			ReferenceJson->SetNumberField(TEXT("y"), Node->NodePosY);
			References.Add(MakeShared<FJsonValueObject>(ReferenceJson));
		}
	}

	Result->SetArrayField(TEXT("references"), References);
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
