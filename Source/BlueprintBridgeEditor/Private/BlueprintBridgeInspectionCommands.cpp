// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

#include "BlueprintBridgeFieldSelection.h"

namespace BlueprintBridge
{
// Forward decl: defined as a static helper later in this file (see LoadGraphForRead). The new
// DescribeGraph handler below calls it before the definition site, so it needs to be visible here.
static bool LoadGraphForRead(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, TSharedRef<FJsonObject>& OutError);

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

TSharedRef<FJsonObject> BuildBlueprintDescription(UBlueprint* Blueprint)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Blueprint)
	{
		return Result;
	}
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

	return Result;
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

	TSharedRef<FJsonObject> Result = BuildBlueprintDescription(Blueprint);
	Result->SetStringField(TEXT("asset"), AssetPath);
	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
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

TSharedRef<FJsonObject> DescribeGraphFull(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString GraphName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("graph"), GraphName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeGraphFull requires params.asset and params.graph."));
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
	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
}

TSharedRef<FJsonObject> DescribeGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	// Default to the compact summary shape. Callers who need the per-node/per-pin dump should use
	// DescribeGraphFull. Mirrors SummarizeBlueprintGraph's handler.
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FGraphSummaryOptions Options;
	Params->TryGetBoolField(TEXT("includeDefaults"), Options.bIncludeDefaults);
	Params->TryGetBoolField(TEXT("includePins"), Options.bIncludePins);
	Params->TryGetBoolField(TEXT("includeWarnings"), Options.bIncludeWarnings);

	TSharedRef<FJsonObject> Result = BuildGraphSummary(Graph, Options);
	Result->SetStringField(TEXT("asset"), Params->GetStringField(TEXT("asset")));
	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
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

static FString GetNodeGuidString(const UEdGraphNode* Node)
{
	return Node ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString();
}

static FString GetNodeTitleString(const UEdGraphNode* Node)
{
	return Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString();
}

static FString MakeGraphSymbolBase(UEdGraphNode* Node)
{
	if (Node->IsA<UK2Node_FunctionEntry>())
	{
		return TEXT("entry");
	}
	if (Node->IsA<UK2Node_FunctionResult>())
	{
		return TEXT("result");
	}
	if (Node->IsA<UK2Node_IfThenElse>())
	{
		return TEXT("branch");
	}
	if (Node->IsA<UK2Node_ExecutionSequence>())
	{
		return TEXT("sequence");
	}
	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		return FString::Printf(TEXT("call_%s"), *CallNode->FunctionReference.GetMemberName().ToString()).ToLower();
	}
	if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
	{
		return FString::Printf(TEXT("%s_%s"), Node->IsA<UK2Node_VariableSet>() ? TEXT("set") : TEXT("get"), *VariableNode->GetVarNameString()).ToLower();
	}
	return TEXT("node");
}

static void BuildSymbolicNodeIds(UEdGraph* Graph, TMap<UEdGraphNode*, FString>& OutNodeToId, TMap<FString, UEdGraphNode*>& OutIdToNode)
{
	if (!Graph)
	{
		return;
	}

	TMap<FString, int32> Counts;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		const FString Base = MakeGraphSymbolBase(Node);
		int32& Count = Counts.FindOrAdd(Base);
		++Count;
		FString NodeId = Count == 1 ? Base : FString::Printf(TEXT("%s_%d"), *Base, Count);
		while (OutIdToNode.Contains(NodeId))
		{
			++Count;
			NodeId = FString::Printf(TEXT("%s_%d"), *Base, Count);
		}
		OutNodeToId.Add(Node, NodeId);
		OutIdToNode.Add(NodeId, Node);
	}
}

static UEdGraphNode* ResolveGraphNodeRef(UEdGraph* Graph, const FString& Ref, const TMap<FString, UEdGraphNode*>& IdToNode)
{
	if (UEdGraphNode* const* Node = IdToNode.Find(Ref))
	{
		return *Node;
	}
	return FindNodeByGuid(Graph, Ref);
}

static bool IsExecPin(const UEdGraphPin* Pin)
{
	return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
}

static FString GraphLinkKind(const UEdGraphPin* Pin)
{
	return IsExecPin(Pin) ? TEXT("Exec") : TEXT("Data");
}

static TSharedRef<FJsonObject> MakeCompactNodeJson(UEdGraphNode* Node, const TMap<UEdGraphNode*, FString>& NodeToId, const int32 Distance = INDEX_NONE, const FString& Direction = FString())
{
	TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
	if (const FString* NodeId = NodeToId.Find(Node))
	{
		NodeJson->SetStringField(TEXT("id"), *NodeId);
	}
	NodeJson->SetStringField(TEXT("guid"), GetNodeGuidString(Node));
	NodeJson->SetStringField(TEXT("title"), GetNodeTitleString(Node));
	NodeJson->SetStringField(TEXT("class"), Node ? Node->GetClass()->GetPathName() : FString());
	if (Distance != INDEX_NONE)
	{
		NodeJson->SetNumberField(TEXT("distance"), Distance);
	}
	if (!Direction.IsEmpty())
	{
		NodeJson->SetStringField(TEXT("direction"), Direction);
	}
	return NodeJson;
}

static void AddPinNameArray(TSharedRef<FJsonObject> NodeJson, UEdGraphNode* Node, const TCHAR* FieldName, const EEdGraphPinDirection Direction)
{
	TArray<TSharedPtr<FJsonValue>> Pins;
	if (Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction)
			{
				Pins.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString()));
			}
		}
	}
	NodeJson->SetArrayField(FieldName, Pins);
}

static void AddOptionalPinDescriptions(TSharedRef<FJsonObject> NodeJson, UEdGraphNode* Node, const bool bIncludePins, const bool bIncludeDefaults)
{
	if (!bIncludePins && !bIncludeDefaults)
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Pins;
	if (Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
			PinJson->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			if (bIncludeDefaults && Pin->Direction == EGPD_Input && !Pin->DefaultValue.IsEmpty())
			{
				PinJson->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
			}
			Pins.Add(MakeShared<FJsonValueObject>(PinJson));
		}
	}
	NodeJson->SetArrayField(TEXT("pins"), Pins);
}

static void AddGraphLinkJson(TArray<TSharedPtr<FJsonValue>>& Links, UEdGraphPin* FromPin, UEdGraphPin* ToPin, const TMap<UEdGraphNode*, FString>& NodeToId)
{
	UEdGraphNode* FromNode = FromPin ? FromPin->GetOwningNode() : nullptr;
	UEdGraphNode* ToNode = ToPin ? ToPin->GetOwningNode() : nullptr;
	if (!FromNode || !ToNode)
	{
		return;
	}

	TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
	LinkJson->SetStringField(TEXT("fromNode"), NodeToId.Contains(FromNode) ? NodeToId[FromNode] : GetNodeGuidString(FromNode));
	LinkJson->SetStringField(TEXT("fromGuid"), GetNodeGuidString(FromNode));
	LinkJson->SetStringField(TEXT("fromPin"), FromPin->PinName.ToString());
	LinkJson->SetStringField(TEXT("toNode"), NodeToId.Contains(ToNode) ? NodeToId[ToNode] : GetNodeGuidString(ToNode));
	LinkJson->SetStringField(TEXT("toGuid"), GetNodeGuidString(ToNode));
	LinkJson->SetStringField(TEXT("toPin"), ToPin->PinName.ToString());
	LinkJson->SetStringField(TEXT("kind"), GraphLinkKind(FromPin));
	Links.Add(MakeShared<FJsonValueObject>(LinkJson));
}

static void GetLinkedNodes(UEdGraphNode* Node, const bool bUpstream, const bool bDownstream, const bool bIncludeExec, const bool bIncludeData, TArray<TPair<UEdGraphNode*, FString>>& OutNodes, TArray<TSharedPtr<FJsonValue>>* OutLinks, const TMap<UEdGraphNode*, FString>& NodeToId)
{
	if (!Node)
	{
		return;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || (IsExecPin(Pin) && !bIncludeExec) || (!IsExecPin(Pin) && !bIncludeData))
		{
			continue;
		}

		const bool bPinDownstream = Pin->Direction == EGPD_Output;
		if ((bPinDownstream && !bDownstream) || (!bPinDownstream && !bUpstream))
		{
			continue;
		}

		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
			if (!LinkedNode)
			{
				continue;
			}
			OutNodes.Add(TPair<UEdGraphNode*, FString>(LinkedNode, bPinDownstream ? TEXT("Downstream") : TEXT("Upstream")));
			if (OutLinks)
			{
				AddGraphLinkJson(*OutLinks, bPinDownstream ? Pin : LinkedPin, bPinDownstream ? LinkedPin : Pin, NodeToId);
			}
		}
	}
}

static TArray<UEdGraphNode*> GetExecSuccessors(UEdGraphNode* Node)
{
	TArray<UEdGraphNode*> Successors;
	if (!Node)
	{
		return Successors;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output || !IsExecPin(Pin))
		{
			continue;
		}
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr)
			{
				Successors.AddUnique(LinkedNode);
			}
		}
	}
	return Successors;
}

static void FindExecPathsRecursive(UEdGraphNode* Node, UEdGraphNode* Target, const TMap<UEdGraphNode*, FString>& NodeToId, TArray<UEdGraphNode*>& Stack, TSet<UEdGraphNode*>& Visiting, TArray<TSharedPtr<FJsonValue>>& Paths, TArray<TSharedPtr<FJsonValue>>& DeadEnds, const bool bIncludeBranches, const int32 MaxPaths)
{
	if (!Node || Paths.Num() >= MaxPaths || Visiting.Contains(Node))
	{
		return;
	}

	Stack.Add(Node);
	Visiting.Add(Node);
	if (Node == Target)
	{
		TSharedRef<FJsonObject> PathJson = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Branches;
		for (UEdGraphNode* PathNode : Stack)
		{
			Nodes.Add(MakeShared<FJsonValueString>(NodeToId.Contains(PathNode) ? NodeToId[PathNode] : GetNodeGuidString(PathNode)));
		}
		if (bIncludeBranches)
		{
			for (int32 Index = 0; Index + 1 < Stack.Num(); ++Index)
			{
				UEdGraphNode* PathNode = Stack[Index];
				if (!PathNode->IsA<UK2Node_IfThenElse>() && !PathNode->IsA<UK2Node_SwitchEnum>())
				{
					continue;
				}
				for (UEdGraphPin* Pin : PathNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output || !IsExecPin(Pin))
					{
						continue;
					}
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode() == Stack[Index + 1])
						{
							TSharedRef<FJsonObject> BranchJson = MakeShared<FJsonObject>();
							BranchJson->SetStringField(TEXT("node"), NodeToId.Contains(PathNode) ? NodeToId[PathNode] : GetNodeGuidString(PathNode));
							BranchJson->SetStringField(TEXT("pin"), Pin->PinName.ToString());
							Branches.Add(MakeShared<FJsonValueObject>(BranchJson));
						}
					}
				}
			}
		}
		PathJson->SetArrayField(TEXT("nodes"), Nodes);
		PathJson->SetArrayField(TEXT("branches"), Branches);
		Paths.Add(MakeShared<FJsonValueObject>(PathJson));
	}
	else
	{
		const TArray<UEdGraphNode*> Successors = GetExecSuccessors(Node);
		if (Successors.IsEmpty())
		{
			DeadEnds.Add(MakeShared<FJsonValueString>(NodeToId.Contains(Node) ? NodeToId[Node] : GetNodeGuidString(Node)));
		}
		for (UEdGraphNode* Successor : Successors)
		{
			FindExecPathsRecursive(Successor, Target, NodeToId, Stack, Visiting, Paths, DeadEnds, bIncludeBranches, MaxPaths);
		}
	}
	Visiting.Remove(Node);
	Stack.Pop();
}

static void AddUnreachableExecNodes(UEdGraph* Graph, const TArray<UEdGraphNode*>& Starts, const TMap<UEdGraphNode*, FString>& NodeToId, TArray<TSharedPtr<FJsonValue>>& OutNodes)
{
	TSet<UEdGraphNode*> Reachable;
	TArray<UEdGraphNode*> Queue = Starts;
	while (!Queue.IsEmpty())
	{
		UEdGraphNode* Node = Queue[0];
		Queue.RemoveAt(0);
		if (!Node || Reachable.Contains(Node))
		{
			continue;
		}
		Reachable.Add(Node);
		Queue.Append(GetExecSuccessors(Node));
	}

	if (!Graph)
	{
		return;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		bool bHasExecPin = false;
		if (Node)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				bHasExecPin |= IsExecPin(Pin);
			}
		}
		if (bHasExecPin && !Reachable.Contains(Node))
		{
			OutNodes.Add(MakeShared<FJsonValueString>(NodeToId.Contains(Node) ? NodeToId[Node] : GetNodeGuidString(Node)));
		}
	}
}

static bool LoadGraphForRead(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, TSharedRef<FJsonObject>& OutError)
{
	FString AssetPath;
	FString GraphName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("graph"), GraphName))
	{
		OutError = MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("Command requires params.asset and params.graph."));
		return false;
	}

	OutBlueprint = LoadBlueprint(AssetPath);
	if (!OutBlueprint)
	{
		OutError = MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
		return false;
	}

	OutGraph = FindBlueprintGraph(OutBlueprint, GraphName);
	if (!OutGraph)
	{
		OutError = MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Could not find graph '%s' on '%s'."), *GraphName, *AssetPath));
		return false;
	}
	return true;
}

TSharedRef<FJsonObject> BuildGraphSummary(UEdGraph* Graph, const FGraphSummaryOptions& Options)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Graph)
	{
		Result->SetArrayField(TEXT("entryNodes"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("resultNodes"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("executionChains"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("functionCalls"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetObjectField(TEXT("variables"), MakeShared<FJsonObject>());
		Result->SetArrayField(TEXT("branches"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("disconnectedNodes"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
		return Result;
	}

	const bool bIncludeDefaults = Options.bIncludeDefaults;
	const bool bIncludePins = Options.bIncludePins;
	const bool bIncludeWarnings = Options.bIncludeWarnings;

	TMap<UEdGraphNode*, FString> NodeToId;
	TMap<FString, UEdGraphNode*> IdToNode;
	BuildSymbolicNodeIds(Graph, NodeToId, IdToNode);

	TArray<TSharedPtr<FJsonValue>> Entries;
	TArray<TSharedPtr<FJsonValue>> Results;
	TArray<TSharedPtr<FJsonValue>> Calls;
	TArray<TSharedPtr<FJsonValue>> Branches;
	TArray<TSharedPtr<FJsonValue>> Disconnected;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TSet<FString> Reads;
	TSet<FString> Writes;
	TArray<UEdGraphNode*> EntryNodes;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>())
		{
			EntryNodes.Add(Node);
			TSharedRef<FJsonObject> EntryJson = MakeCompactNodeJson(Node, NodeToId);
			AddPinNameArray(EntryJson, Node, TEXT("outputs"), EGPD_Output);
			AddOptionalPinDescriptions(EntryJson, Node, bIncludePins, bIncludeDefaults);
			Entries.Add(MakeShared<FJsonValueObject>(EntryJson));
		}
		if (Node->IsA<UK2Node_FunctionResult>())
		{
			TSharedRef<FJsonObject> ResultJson = MakeCompactNodeJson(Node, NodeToId);
			AddPinNameArray(ResultJson, Node, TEXT("inputs"), EGPD_Input);
			AddOptionalPinDescriptions(ResultJson, Node, bIncludePins, bIncludeDefaults);
			Results.Add(MakeShared<FJsonValueObject>(ResultJson));
		}
		if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
		{
			TSharedRef<FJsonObject> CallJson = MakeCompactNodeJson(Node, NodeToId);
			CallJson->SetStringField(TEXT("functionClass"), CallNode->FunctionReference.GetMemberParentClass() ? CallNode->FunctionReference.GetMemberParentClass()->GetPathName() : FString());
			CallJson->SetStringField(TEXT("function"), CallNode->FunctionReference.GetMemberName().ToString());
			CallJson->SetBoolField(TEXT("pure"), CallNode->IsNodePure());
			Calls.Add(MakeShared<FJsonValueObject>(CallJson));
		}
		if (const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node))
		{
			if (Node->IsA<UK2Node_VariableSet>())
			{
				Writes.Add(VariableNode->GetVarNameString());
			}
			else
			{
				Reads.Add(VariableNode->GetVarNameString());
			}
		}
		if (Node->IsA<UK2Node_IfThenElse>())
		{
			TSharedRef<FJsonObject> BranchJson = MakeCompactNodeJson(Node, NodeToId);
			if (UEdGraphPin* ConditionPin = FindPinByName(Node, TEXT("Condition"), TOptional<EEdGraphPinDirection>(EGPD_Input)))
			{
				if (ConditionPin->LinkedTo.Num() > 0 && ConditionPin->LinkedTo[0] && ConditionPin->LinkedTo[0]->GetOwningNode())
				{
					UEdGraphNode* SourceNode = ConditionPin->LinkedTo[0]->GetOwningNode();
					BranchJson->SetStringField(TEXT("conditionSource"), FString::Printf(TEXT("%s.%s"), *(NodeToId.Contains(SourceNode) ? NodeToId[SourceNode] : GetNodeGuidString(SourceNode)), *ConditionPin->LinkedTo[0]->PinName.ToString()));
				}
				else if (bIncludeDefaults)
				{
					BranchJson->SetStringField(TEXT("conditionDefault"), ConditionPin->DefaultValue);
				}
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output || !IsExecPin(Pin) || Pin->LinkedTo.IsEmpty())
				{
					continue;
				}
				UEdGraphNode* LinkedNode = Pin->LinkedTo[0] ? Pin->LinkedTo[0]->GetOwningNode() : nullptr;
				if (LinkedNode)
				{
					BranchJson->SetStringField(NormalizePinLookupName(Pin->PinName.ToString()), NodeToId.Contains(LinkedNode) ? NodeToId[LinkedNode] : GetNodeGuidString(LinkedNode));
				}
			}
			Branches.Add(MakeShared<FJsonValueObject>(BranchJson));
		}

		bool bAnyLink = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			bAnyLink |= Pin && Pin->LinkedTo.Num() > 0;
		}
		if (!bAnyLink && !Node->IsA<UK2Node_FunctionEntry>() && !Node->IsA<UK2Node_FunctionResult>() && !Node->IsA<UEdGraphNode_Comment>())
		{
			Disconnected.Add(MakeShared<FJsonValueString>(NodeToId.Contains(Node) ? NodeToId[Node] : GetNodeGuidString(Node)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> ExecutionChains;
	for (UEdGraphNode* EntryNode : EntryNodes)
	{
		TArray<UEdGraphNode*> Chain;
		TSet<UEdGraphNode*> Seen;
		UEdGraphNode* Current = EntryNode;
		while (Current && !Seen.Contains(Current))
		{
			Seen.Add(Current);
			Chain.Add(Current);
			const TArray<UEdGraphNode*> Next = GetExecSuccessors(Current);
			Current = Next.Num() == 1 ? Next[0] : nullptr;
		}

		TSharedRef<FJsonObject> ChainJson = MakeShared<FJsonObject>();
		ChainJson->SetStringField(TEXT("from"), NodeToId.Contains(EntryNode) ? NodeToId[EntryNode] : GetNodeGuidString(EntryNode));
		TArray<TSharedPtr<FJsonValue>> ChainNodes;
		for (UEdGraphNode* ChainNode : Chain)
		{
			ChainNodes.Add(MakeShared<FJsonValueString>(NodeToId.Contains(ChainNode) ? NodeToId[ChainNode] : GetNodeGuidString(ChainNode)));
		}
		ChainJson->SetArrayField(TEXT("nodes"), ChainNodes);
		ExecutionChains.Add(MakeShared<FJsonValueObject>(ChainJson));
	}

	TSharedRef<FJsonObject> VariablesJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ReadArray;
	TArray<TSharedPtr<FJsonValue>> WriteArray;
	for (const FString& Name : Reads) { ReadArray.Add(MakeShared<FJsonValueString>(Name)); }
	for (const FString& Name : Writes) { WriteArray.Add(MakeShared<FJsonValueString>(Name)); }
	VariablesJson->SetArrayField(TEXT("reads"), ReadArray);
	VariablesJson->SetArrayField(TEXT("writes"), WriteArray);

	if (bIncludeWarnings)
	{
		TArray<TSharedPtr<FJsonValue>> Unreachable;
		AddUnreachableExecNodes(Graph, EntryNodes, NodeToId, Unreachable);
		if (!Unreachable.IsEmpty())
		{
			TSharedRef<FJsonObject> WarningJson = MakeShared<FJsonObject>();
			WarningJson->SetStringField(TEXT("message"), TEXT("Graph contains unreachable executable nodes."));
			WarningJson->SetArrayField(TEXT("nodes"), Unreachable);
			Warnings.Add(MakeShared<FJsonValueObject>(WarningJson));
		}
	}

	Result->SetStringField(TEXT("graph"), Graph->GetName());
	Result->SetStringField(TEXT("graphType"), Graph->GetFName() == UEdGraphSchema_K2::GN_EventGraph ? TEXT("EventGraph") : Entries.Num() > 0 && Results.Num() > 0 ? TEXT("Function") : TEXT("Graph"));
	Result->SetArrayField(TEXT("entryNodes"), Entries);
	Result->SetArrayField(TEXT("resultNodes"), Results);
	Result->SetArrayField(TEXT("executionChains"), ExecutionChains);
	Result->SetArrayField(TEXT("functionCalls"), Calls);
	Result->SetObjectField(TEXT("variables"), VariablesJson);
	Result->SetArrayField(TEXT("branches"), Branches);
	Result->SetArrayField(TEXT("disconnectedNodes"), Disconnected);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	return Result;
}

TSharedRef<FJsonObject> SummarizeBlueprintGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FGraphSummaryOptions Options;
	Params->TryGetBoolField(TEXT("includeDefaults"), Options.bIncludeDefaults);
	Params->TryGetBoolField(TEXT("includePins"), Options.bIncludePins);
	Params->TryGetBoolField(TEXT("includeWarnings"), Options.bIncludeWarnings);

	TSharedRef<FJsonObject> Result = BuildGraphSummary(Graph, Options);
	Result->SetStringField(TEXT("asset"), Params->GetStringField(TEXT("asset")));
	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
}

TSharedRef<FJsonObject> GetConnectedNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeRef;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeRef))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("GetConnectedNodes requires params.node."));
	}

	FString Direction = TEXT("Both");
	Params->TryGetStringField(TEXT("direction"), Direction);
	int32 Depth = 1;
	Params->TryGetNumberField(TEXT("depth"), Depth);
	bool bIncludeExec = true;
	bool bIncludeData = true;
	Params->TryGetBoolField(TEXT("includeExecLinks"), bIncludeExec);
	Params->TryGetBoolField(TEXT("includeDataLinks"), bIncludeData);
	const bool bUpstream = Direction.Equals(TEXT("Both"), ESearchCase::IgnoreCase) || Direction.Equals(TEXT("Upstream"), ESearchCase::IgnoreCase);
	const bool bDownstream = Direction.Equals(TEXT("Both"), ESearchCase::IgnoreCase) || Direction.Equals(TEXT("Downstream"), ESearchCase::IgnoreCase);

	TMap<UEdGraphNode*, FString> NodeToId;
	TMap<FString, UEdGraphNode*> IdToNode;
	BuildSymbolicNodeIds(Graph, NodeToId, IdToNode);
	UEdGraphNode* Center = ResolveGraphNodeRef(Graph, NodeRef, IdToNode);
	if (!Center)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not resolve node '%s'."), *NodeRef));
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<TSharedPtr<FJsonValue>> Links;
	TMap<UEdGraphNode*, int32> Distances;
	TMap<UEdGraphNode*, FString> Directions;
	TArray<UEdGraphNode*> Queue;
	Distances.Add(Center, 0);
	Queue.Add(Center);
	while (!Queue.IsEmpty())
	{
		UEdGraphNode* Current = Queue[0];
		Queue.RemoveAt(0);
		const int32 CurrentDistance = Distances[Current];
		if (CurrentDistance >= Depth)
		{
			continue;
		}

		TArray<TPair<UEdGraphNode*, FString>> LinkedNodes;
		GetLinkedNodes(Current, bUpstream, bDownstream, bIncludeExec, bIncludeData, LinkedNodes, &Links, NodeToId);
		for (const TPair<UEdGraphNode*, FString>& Linked : LinkedNodes)
		{
			if (!Distances.Contains(Linked.Key))
			{
				Distances.Add(Linked.Key, CurrentDistance + 1);
				Directions.Add(Linked.Key, Linked.Value);
				Queue.Add(Linked.Key);
			}
		}
	}

	for (const TPair<UEdGraphNode*, int32>& Pair : Distances)
	{
		if (Pair.Key != Center)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(MakeCompactNodeJson(Pair.Key, NodeToId, Pair.Value, Directions.Contains(Pair.Key) ? Directions[Pair.Key] : FString())));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("center"), MakeCompactNodeJson(Center, NodeToId));
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetArrayField(TEXT("links"), Links);
	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
}

TSharedRef<FJsonObject> FindExecutionPath(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString FromRef;
	FString ToRef;
	if (!TryGetRequiredString(Params, TEXT("from"), FromRef) || !TryGetRequiredString(Params, TEXT("to"), ToRef))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindExecutionPath requires params.from and params.to."));
	}
	bool bIncludeBranches = true;
	Params->TryGetBoolField(TEXT("includeBranches"), bIncludeBranches);

	TMap<UEdGraphNode*, FString> NodeToId;
	TMap<FString, UEdGraphNode*> IdToNode;
	BuildSymbolicNodeIds(Graph, NodeToId, IdToNode);
	UEdGraphNode* FromNode = ResolveGraphNodeRef(Graph, FromRef, IdToNode);
	UEdGraphNode* ToNode = ResolveGraphNodeRef(Graph, ToRef, IdToNode);
	if (!FromNode || !ToNode)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), TEXT("Could not resolve from or to node."));
	}

	TArray<TSharedPtr<FJsonValue>> Paths;
	TArray<TSharedPtr<FJsonValue>> DeadEnds;
	TArray<UEdGraphNode*> Stack;
	TSet<UEdGraphNode*> Visiting;
	FindExecPathsRecursive(FromNode, ToNode, NodeToId, Stack, Visiting, Paths, DeadEnds, bIncludeBranches, 32);

	TArray<TSharedPtr<FJsonValue>> Unreachable;
	TArray<UEdGraphNode*> Starts;
	Starts.Add(FromNode);
	AddUnreachableExecNodes(Graph, Starts, NodeToId, Unreachable);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), !Paths.IsEmpty());
	Result->SetArrayField(TEXT("paths"), Paths);
	Result->SetArrayField(TEXT("deadEnds"), DeadEnds);
	Result->SetArrayField(TEXT("unreachableExecNodes"), Unreachable);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> DescribeSubgraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	const TArray<TSharedPtr<FJsonValue>>* Seeds = nullptr;
	if (!Params->TryGetArrayField(TEXT("seeds"), Seeds) || Seeds == nullptr)
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeSubgraph requires params.seeds."));
	}
	int32 Depth = 1;
	Params->TryGetNumberField(TEXT("depth"), Depth);
	bool bIncludeUpstream = true;
	bool bIncludeDownstream = true;
	bool bIncludeData = true;
	bool bIncludeExec = true;
	Params->TryGetBoolField(TEXT("includeUpstream"), bIncludeUpstream);
	Params->TryGetBoolField(TEXT("includeDownstream"), bIncludeDownstream);
	Params->TryGetBoolField(TEXT("includeDataLinks"), bIncludeData);
	Params->TryGetBoolField(TEXT("includeExecLinks"), bIncludeExec);

	TMap<UEdGraphNode*, FString> NodeToId;
	TMap<FString, UEdGraphNode*> IdToNode;
	BuildSymbolicNodeIds(Graph, NodeToId, IdToNode);
	TMap<UEdGraphNode*, int32> Distances;
	TArray<UEdGraphNode*> Queue;
	for (const TSharedPtr<FJsonValue>& SeedValue : *Seeds)
	{
		FString Seed;
		if (SeedValue.IsValid() && SeedValue->TryGetString(Seed))
		{
			if (UEdGraphNode* SeedNode = ResolveGraphNodeRef(Graph, Seed, IdToNode))
			{
				Distances.Add(SeedNode, 0);
				Queue.Add(SeedNode);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> Links;
	while (!Queue.IsEmpty())
	{
		UEdGraphNode* Current = Queue[0];
		Queue.RemoveAt(0);
		const int32 CurrentDistance = Distances[Current];
		if (CurrentDistance >= Depth)
		{
			continue;
		}
		TArray<TPair<UEdGraphNode*, FString>> LinkedNodes;
		GetLinkedNodes(Current, bIncludeUpstream, bIncludeDownstream, bIncludeExec, bIncludeData, LinkedNodes, &Links, NodeToId);
		for (const TPair<UEdGraphNode*, FString>& Linked : LinkedNodes)
		{
			if (!Distances.Contains(Linked.Key))
			{
				Distances.Add(Linked.Key, CurrentDistance + 1);
				Queue.Add(Linked.Key);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const TPair<UEdGraphNode*, int32>& Pair : Distances)
	{
		Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Pair.Key)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Params->GetStringField(TEXT("asset")));
	Result->SetStringField(TEXT("graph"), Graph->GetName());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetArrayField(TEXT("links"), Links);
	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
}

TSharedRef<FJsonObject> FindFunctionCallNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindFunctionCallNodes requires params.function."));
	}
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}
	FString FunctionClass;
	Params->TryGetStringField(TEXT("functionClass"), FunctionClass);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
		if (!CallNode || !CallNode->FunctionReference.GetMemberName().ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass();
		if (!FunctionClass.IsEmpty() && (!ParentClass || !ParentClass->GetPathName().Equals(FunctionClass, ESearchCase::IgnoreCase)))
		{
			continue;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("nodes"), Nodes);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> FindVariableNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString VariableName;
	if (!TryGetRequiredString(Params, TEXT("variable"), VariableName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindVariableNodes requires params.variable."));
	}
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}
	FString Access;
	Params->TryGetStringField(TEXT("access"), Access);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
		if (!VariableNode || !VariableNode->GetVarNameString().Equals(VariableName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString NodeAccess = Node->IsA<UK2Node_VariableSet>() ? TEXT("Set") : Node->IsA<UK2Node_VariableGet>() ? TEXT("Get") : TEXT("Other");
		if (!Access.IsEmpty() && !Access.Equals(NodeAccess, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("nodes"), Nodes);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> FindDelegateNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}
	FString DelegateName;
	Params->TryGetStringField(TEXT("delegate"), DelegateName);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		const bool bDelegateNode = Node && (Node->IsA<UK2Node_AddDelegate>() || Node->IsA<UK2Node_CallDelegate>() || Node->IsA<UK2Node_CreateDelegate>() || Node->IsA<UK2Node_ComponentBoundEvent>());
		if (!bDelegateNode)
		{
			continue;
		}
		if (!DelegateName.IsEmpty() && !GetNodeTitleString(Node).Contains(DelegateName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("nodes"), Nodes);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> FindNodesByPin(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString PinName;
	if (!TryGetRequiredString(Params, TEXT("pin"), PinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindNodesByPin requires params.pin."));
	}
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!LoadGraphForRead(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}
	FString Direction;
	FString Category;
	Params->TryGetStringField(TEXT("direction"), Direction);
	Params->TryGetStringField(TEXT("category"), Category);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		for (UEdGraphPin* Pin : Node ? Node->Pins : TArray<UEdGraphPin*>())
		{
			if (!Pin || !NormalizePinLookupName(Pin->PinName.ToString()).Equals(NormalizePinLookupName(PinName), ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (!Direction.IsEmpty() && ((Direction.Equals(TEXT("Input"), ESearchCase::IgnoreCase) && Pin->Direction != EGPD_Input) || (Direction.Equals(TEXT("Output"), ESearchCase::IgnoreCase) && Pin->Direction != EGPD_Output)))
			{
				continue;
			}
			if (!Category.IsEmpty() && !Pin->PinType.PinCategory.ToString().Equals(Category, ESearchCase::IgnoreCase))
			{
				continue;
			}
			Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
			break;
		}
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("nodes"), Nodes);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> ExplainCompileErrors(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	TSharedRef<FJsonObject> CompileResponse = CompileBlueprint(Id, Params);
	const TSharedPtr<FJsonObject>* CompileResult = nullptr;
	if (!CompileResponse->TryGetObjectField(TEXT("result"), CompileResult) || CompileResult == nullptr || !CompileResult->IsValid())
	{
		return CompileResponse;
	}

	TArray<TSharedPtr<FJsonValue>> Errors;
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if ((*CompileResult)->TryGetArrayField(TEXT("messages"), Messages) && Messages != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& MessageValue : *Messages)
		{
			const TSharedPtr<FJsonObject>* MessageObject = nullptr;
			if (!MessageValue.IsValid() || !MessageValue->TryGetObject(MessageObject) || MessageObject == nullptr || !MessageObject->IsValid())
			{
				continue;
			}
			FString Severity;
			(*MessageObject)->TryGetStringField(TEXT("severity"), Severity);
			if (!Severity.Equals(TEXT("Error"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*MessageObject)->Values)
			{
				ErrorJson->SetField(Pair.Key, Pair.Value);
			}
			FString Message;
			(*MessageObject)->TryGetStringField(TEXT("message"), Message);
			if (Message.Contains(TEXT("delegate"), ESearchCase::IgnoreCase))
			{
				ErrorJson->SetStringField(TEXT("likelyCause"), TEXT("Delegate signature mismatch or unbound delegate function."));
			}
			else if (Message.Contains(TEXT("pin"), ESearchCase::IgnoreCase))
			{
				ErrorJson->SetStringField(TEXT("likelyCause"), TEXT("Invalid, missing, or incompatible pin connection/default."));
			}
			else
			{
				ErrorJson->SetStringField(TEXT("likelyCause"), TEXT("Blueprint compiler reported an error; inspect the referenced graph node."));
			}
			Errors.Add(MakeShared<FJsonValueObject>(ErrorJson));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	bool bSuccess = false;
	(*CompileResult)->TryGetBoolField(TEXT("success"), bSuccess);
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetArrayField(TEXT("errors"), Errors);
	return MakeSuccess(Id, Result);
}

static TSharedRef<FJsonObject> SerializePinTypeFields(const FEdGraphPinType& PinType)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
	Obj->SetStringField(TEXT("subCategory"), PinType.PinSubCategory.ToString());
	Obj->SetStringField(TEXT("subCategoryObject"), PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetPathName() : TEXT(""));
	Obj->SetStringField(TEXT("containerType"), PinContainerTypeToString(PinType));
	Obj->SetBoolField(TEXT("byRef"), PinType.bIsReference);
	Obj->SetBoolField(TEXT("isConst"), PinType.bIsConst);
	return Obj;
}

static TSharedRef<FJsonObject> BuildFunctionSignature(UEdGraph* FunctionGraph)
{
	TSharedRef<FJsonObject> Sig = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;
	if (UK2Node_FunctionEntry* Entry = FindFunctionEntryNode(FunctionGraph))
	{
		for (UEdGraphPin* Pin : Entry->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || IsExecPin(Pin))
			{
				continue;
			}
			TSharedRef<FJsonObject> P = SerializePinTypeFields(Pin->PinType);
			P->SetStringField(TEXT("name"), Pin->PinName.ToString());
			Inputs.Add(MakeShared<FJsonValueObject>(P));
		}
	}
	if (UK2Node_FunctionResult* ResultNode = FindFunctionResultNode(FunctionGraph))
	{
		for (UEdGraphPin* Pin : ResultNode->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input || IsExecPin(Pin))
			{
				continue;
			}
			TSharedRef<FJsonObject> P = SerializePinTypeFields(Pin->PinType);
			P->SetStringField(TEXT("name"), Pin->PinName.ToString());
			Outputs.Add(MakeShared<FJsonValueObject>(P));
		}
	}
	Sig->SetArrayField(TEXT("inputs"), Inputs);
	Sig->SetArrayField(TEXT("outputs"), Outputs);
	return Sig;
}

static void AddGraphEntryToArray(UEdGraph* Graph, bool bIncludeBody, bool bIncludeSignature, TArray<TSharedPtr<FJsonValue>>& OutArray)
{
	if (!Graph)
	{
		return;
	}
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("name"), Graph->GetName());
	if (bIncludeSignature)
	{
		Entry->SetObjectField(TEXT("signature"), BuildFunctionSignature(Graph));
	}
	if (bIncludeBody)
	{
		FGraphSummaryOptions Options;
		Entry->SetObjectField(TEXT("summary"), BuildGraphSummary(Graph, Options));
	}
	OutArray.Add(MakeShared<FJsonValueObject>(Entry));
}

TSharedRef<FJsonObject> SummarizeBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SummarizeBlueprint requires params.asset."));
	}
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	bool bIncludeFunctionBodies = true;
	bool bIncludeEventGraph = true;
	bool bIncludeMacros = true;
	bool bIncludeDelegates = true;
	bool bIncludeWidgetTree = true;
	bool bIncludeReflection = false;
	bool bIncludeSubobjectProperties = false;
	bool bIncludeParent = false;
	Params->TryGetBoolField(TEXT("includeFunctionBodies"), bIncludeFunctionBodies);
	Params->TryGetBoolField(TEXT("includeEventGraph"), bIncludeEventGraph);
	Params->TryGetBoolField(TEXT("includeMacros"), bIncludeMacros);
	Params->TryGetBoolField(TEXT("includeDelegates"), bIncludeDelegates);
	Params->TryGetBoolField(TEXT("includeWidgetTree"), bIncludeWidgetTree);
	Params->TryGetBoolField(TEXT("includeReflection"), bIncludeReflection);
	Params->TryGetBoolField(TEXT("includeSubobjectProperties"), bIncludeSubobjectProperties);
	Params->TryGetBoolField(TEXT("includeParent"), bIncludeParent);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("kind"), Blueprint->GetClass()->GetName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));

	FString ParentBlueprintPath;
	if (Blueprint->ParentClass)
	{
		if (UBlueprint* ParentBP = Cast<UBlueprint>(Blueprint->ParentClass->ClassGeneratedBy))
		{
			ParentBlueprintPath = ParentBP->GetPathName();
		}
	}
	Result->SetStringField(TEXT("parentBlueprint"), ParentBlueprintPath);

	TArray<TSharedPtr<FJsonValue>> Interfaces;
	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (InterfaceDesc.Interface)
		{
			Interfaces.Add(MakeShared<FJsonValueString>(InterfaceDesc.Interface->GetPathName()));
		}
	}
	Result->SetArrayField(TEXT("interfaces"), Interfaces);

	// Variables: copy from BuildBlueprintDescription (it produces the variables shape we want).
	TSharedRef<FJsonObject> BPDesc = BuildBlueprintDescription(Blueprint);
	const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
	if (BPDesc->TryGetArrayField(TEXT("variables"), Vars) && Vars != nullptr)
	{
		Result->SetArrayField(TEXT("variables"), *Vars);
	}

	// Components
	TSharedRef<FJsonObject> ComponentsDesc = BuildComponentsDescription(Blueprint);
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	if (ComponentsDesc->TryGetArrayField(TEXT("components"), Components) && Components != nullptr)
	{
		Result->SetArrayField(TEXT("components"), *Components);
	}

	// Delegates
	if (bIncludeDelegates)
	{
		Result->SetArrayField(TEXT("delegates"), CollectClassDelegates(Blueprint->GeneratedClass));
	}

	// Graphs: event, functions, macros
	TSharedRef<FJsonObject> GraphsJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EventGraphs;
	if (bIncludeEventGraph)
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			AddGraphEntryToArray(Graph, bIncludeFunctionBodies, false, EventGraphs);
		}
	}
	GraphsJson->SetArrayField(TEXT("event"), EventGraphs);

	TArray<TSharedPtr<FJsonValue>> Functions;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		AddGraphEntryToArray(Graph, bIncludeFunctionBodies, true, Functions);
	}
	GraphsJson->SetArrayField(TEXT("functions"), Functions);

	TArray<TSharedPtr<FJsonValue>> Macros;
	if (bIncludeMacros)
	{
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			AddGraphEntryToArray(Graph, bIncludeFunctionBodies, true, Macros);
		}
	}
	GraphsJson->SetArrayField(TEXT("macros"), Macros);

	Result->SetObjectField(TEXT("graphs"), GraphsJson);

	// Widget tree (UMG)
	if (bIncludeWidgetTree)
	{
		if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
		{
			AddWidgetTreeDescription(Result, WidgetBlueprint);
		}
	}

	// Optional opt-in sections (v1 placeholders — not yet wired to specific data).
	// includeReflection / includeSubobjectProperties / includeParent are accepted for forward-compat
	// but currently only the parent name is emitted; deeper recursion is reserved for v2.
	(void)bIncludeReflection;
	(void)bIncludeSubobjectProperties;
	(void)bIncludeParent;

	return MakeSuccess(Id, ApplyFieldSelection(Params, Result));
}

} // namespace BlueprintBridge
