// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

#include "BlueprintBridgeSemanticLowering.h"
#include "Kismet2/CompilerResultsLog.h"

namespace BlueprintBridge
{
struct FGraphPatchContext
{
	FString Id;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TMap<FString, UEdGraphNode*> NodesById;
	TArray<UEdGraphNode*> CreatedNodes;
	bool bValidateOnly = false;
};

static bool GetRequiredPatchString(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FString& OutValue, FString& OutError)
{
	if (!Object.IsValid() || !Object->TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Patch object requires non-empty '%s'."), *FieldName);
		return false;
	}
	return true;
}

static int32 GetPatchInteger(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const int32 DefaultValue = 0)
{
	int32 Value = DefaultValue;
	if (Object.IsValid())
	{
		Object->TryGetNumberField(FieldName, Value);
	}
	return Value;
}

static void PrimePatchVirtualNodes(FGraphPatchContext& Context)
{
	if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Context.Graph))
	{
		Context.NodesById.Add(TEXT("entry"), EntryNode);
	}
	if (UK2Node_FunctionResult* ResultNode = FindFunctionResultNode(Context.Graph))
	{
		Context.NodesById.Add(TEXT("result"), ResultNode);
	}
}

static bool PrimePatchExistingNodes(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	const TSharedPtr<FJsonObject>* ExistingNodes = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("existingNodes"), ExistingNodes) || ExistingNodes == nullptr || !ExistingNodes->IsValid())
	{
		return true;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ExistingNodes)->Values)
	{
		FString Guid;
		if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Guid) || Guid.IsEmpty())
		{
			OutError = FString::Printf(TEXT("existingNodes.%s must be a node GUID string."), *Pair.Key);
			return false;
		}
		UEdGraphNode* Node = FindNodeByGuid(Context.Graph, Guid);
		if (!Node)
		{
			OutError = FString::Printf(TEXT("Could not find existing node '%s'."), *Guid);
			return false;
		}
		Context.NodesById.Add(Pair.Key, Node);
	}
	return true;
}

static UEdGraphNode* SpawnSelfPatchNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_Self> NodeCreator(*Graph);
	UK2Node_Self* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnSequencePatchNode(UEdGraph* Graph, const int32 X, const int32 Y, const int32 ExtraOutputs)
{
	FGraphNodeCreator<UK2Node_ExecutionSequence> NodeCreator(*Graph);
	UK2Node_ExecutionSequence* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	for (int32 Index = 0; Index < ExtraOutputs; ++Index)
	{
		Node->AddInputPin();
	}
	return Node;
}

static UEdGraphNode* SpawnReroutePatchNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_Knot> NodeCreator(*Graph);
	UK2Node_Knot* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnCommentPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject)
{
	UEdGraphNode_Comment* Node = NewObject<UEdGraphNode_Comment>(Graph);
	Node->SetFlags(RF_Transactional);
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	Node->NodeWidth = GetPatchInteger(NodeObject, TEXT("width"), 400);
	Node->NodeHeight = GetPatchInteger(NodeObject, TEXT("height"), 200);
	FString Text;
	NodeObject->TryGetStringField(TEXT("text"), Text);
	Node->NodeComment = Text;
	Graph->AddNode(Node, true, false);
	return Node;
}

static UEdGraphNode* SpawnDynamicCastPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString TargetClassPath;
	if (!GetRequiredPatchString(NodeObject, TEXT("targetClass"), TargetClassPath, OutError))
	{
		return nullptr;
	}

	UClass* TargetClass = LoadClassByPath(TargetClassPath);
	if (!TargetClass)
	{
		OutError = FString::Printf(TEXT("Could not load targetClass '%s'."), *TargetClassPath);
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_DynamicCast> NodeCreator(*Graph);
	UK2Node_DynamicCast* Node = NodeCreator.CreateNode();
	Node->TargetType = TargetClass;
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnStructPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, const bool bMakeStruct, FString& OutError)
{
	FString StructPath;
	if (!GetRequiredPatchString(NodeObject, TEXT("struct"), StructPath, OutError))
	{
		return nullptr;
	}

	UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
	if (!Struct)
	{
		OutError = FString::Printf(TEXT("Could not load struct '%s'."), *StructPath);
		return nullptr;
	}

	if (bMakeStruct)
	{
		FGraphNodeCreator<UK2Node_MakeStruct> NodeCreator(*Graph);
		UK2Node_MakeStruct* Node = NodeCreator.CreateNode();
		Node->StructType = Struct;
		Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
		Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
		NodeCreator.Finalize();
		return Node;
	}

	FGraphNodeCreator<UK2Node_BreakStruct> NodeCreator(*Graph);
	UK2Node_BreakStruct* Node = NodeCreator.CreateNode();
	Node->StructType = Struct;
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnCustomEventPatchNode(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString EventName;
	if (!GetRequiredPatchString(NodeObject, TEXT("name"), EventName, OutError))
	{
		return nullptr;
	}

	FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
	UK2Node_CustomEvent* Node = NodeCreator.CreateNode();
	Node->CustomFunctionName = *EventName;
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	NodeCreator.Finalize();

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (NodeObject->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
	{
		if (!AddUserDefinedOutputPinsFromArray(Blueprint, Node, *Inputs, OutError))
		{
			return nullptr;
		}
		Node->ReconstructNode();
	}
	return Node;
}



static UEdGraphNode* SpawnEnumSwitchPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString EnumPath;
	if (!GetRequiredPatchString(NodeObject, TEXT("enum"), EnumPath, OutError))
	{
		return nullptr;
	}
	UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
	if (!Enum)
	{
		OutError = FString::Printf(TEXT("Could not load enum '%s'."), *EnumPath);
		return nullptr;
	}
	FGraphNodeCreator<UK2Node_SwitchEnum> NodeCreator(*Graph);
	UK2Node_SwitchEnum* Node = NodeCreator.CreateNode();
	Node->SetEnum(Enum);
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnArrayFunctionPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString Operation;
	if (!GetRequiredPatchString(NodeObject, TEXT("operation"), Operation, OutError))
	{
		return nullptr;
	}
	static const TMap<FString, FName> OperationToFunction = {
		{ TEXT("Add"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Add) },
		{ TEXT("AddUnique"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_AddUnique) },
		{ TEXT("Remove"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Remove) },
		{ TEXT("RemoveItem"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_RemoveItem) },
		{ TEXT("Clear"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Clear) },
		{ TEXT("Length"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Length) },
		{ TEXT("Get"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Get) },
		{ TEXT("Contains"), GET_FUNCTION_NAME_CHECKED(UKismetArrayLibrary, Array_Contains) },
	};
	const FName* FunctionName = OperationToFunction.Find(Operation);
	UFunction* Function = FunctionName ? UKismetArrayLibrary::StaticClass()->FindFunctionByName(*FunctionName) : nullptr;
	if (!Function)
	{
		OutError = FString::Printf(TEXT("Unsupported array operation '%s'."), *Operation);
		return nullptr;
	}
	FGraphNodeCreator<UK2Node_CallArrayFunction> NodeCreator(*Graph);
	UK2Node_CallArrayFunction* Node = NodeCreator.CreateNode();
	Node->SetFromFunction(Function);
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnMacroPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, const FString& DefaultMacro, const FString& DefaultMacroLibrary, FString& OutError)
{
	FString MacroName = DefaultMacro;
	NodeObject->TryGetStringField(TEXT("macro"), MacroName);
	FString MacroLibraryPath = DefaultMacroLibrary;
	NodeObject->TryGetStringField(TEXT("macroLibrary"), MacroLibraryPath);
	UBlueprint* MacroLibrary = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *MacroLibraryPath));
	if (!MacroLibrary)
	{
		OutError = FString::Printf(TEXT("Could not load macro library '%s'."), *MacroLibraryPath);
		return nullptr;
	}
	UEdGraph* MacroGraph = FindGraphByName(MacroLibrary, MacroName);
	if (!MacroGraph)
	{
		OutError = FString::Printf(TEXT("Could not find macro '%s'."), *MacroName);
		return nullptr;
	}
	FGraphNodeCreator<UK2Node_MacroInstance> NodeCreator(*Graph);
	UK2Node_MacroInstance* Node = NodeCreator.CreateNode();
	Node->SetMacroGraph(MacroGraph);
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnEventPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString EventClassPath;
	FString EventName;
	if (!GetRequiredPatchString(NodeObject, TEXT("eventClass"), EventClassPath, OutError) || !GetRequiredPatchString(NodeObject, TEXT("event"), EventName, OutError))
	{
		return nullptr;
	}
	UClass* EventClass = LoadClassByPath(EventClassPath);
	UFunction* EventFunction = EventClass ? EventClass->FindFunctionByName(*EventName) : nullptr;
	if (!EventFunction)
	{
		OutError = FString::Printf(TEXT("Could not find event '%s' on '%s'."), *EventName, *EventClassPath);
		return nullptr;
	}
	FGraphNodeCreator<UK2Node_Event> NodeCreator(*Graph);
	UK2Node_Event* Node = NodeCreator.CreateNode();
	Node->EventReference.SetExternalMember(*EventName, EventClass);
	Node->bOverrideFunction = true;
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnSpawnActorPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString ActorClassPath;
	if (!GetRequiredPatchString(NodeObject, TEXT("actorClass"), ActorClassPath, OutError))
	{
		return nullptr;
	}
	UClass* ActorClass = LoadClassByPath(ActorClassPath);
	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{
		OutError = FString::Printf(TEXT("Could not load actor class '%s'."), *ActorClassPath);
		return nullptr;
	}
	UK2Node_SpawnActorFromClass* Node = NewObject<UK2Node_SpawnActorFromClass>(Graph);
	Node->SetFlags(RF_Transactional);
	Graph->AddNode(Node, true, false);
	Node->CreateNewGuid();
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();
	if (UEdGraphPin* ClassPin = Node->GetClassPin())
	{
		ClassPin->DefaultObject = ActorClass;
		ClassPin->DefaultValue = ActorClass->GetPathName();
		Node->PinDefaultValueChanged(ClassPin);
	}
	return Node;
}

static UEdGraphNode* SpawnCreateWidgetPatchNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString WidgetClassPath;
	if (!GetRequiredPatchString(NodeObject, TEXT("widgetClass"), WidgetClassPath, OutError))
	{
		return nullptr;
	}
	UClass* WidgetClass = LoadClassByPath(WidgetClassPath);
	UClass* CreateWidgetNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
	if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass()) || !CreateWidgetNodeClass)
	{
		OutError = FString::Printf(TEXT("Could not create widget node for '%s'."), *WidgetClassPath);
		return nullptr;
	}
	UK2Node* Node = NewObject<UK2Node>(Graph, CreateWidgetNodeClass);
	Node->SetFlags(RF_Transactional);
	Graph->AddNode(Node, true, false);
	Node->CreateNewGuid();
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();
	if (UEdGraphPin* ClassPin = FindPinByName(Node, TEXT("Class"), TOptional<EEdGraphPinDirection>()))
	{
		ClassPin->DefaultObject = WidgetClass;
		ClassPin->DefaultValue = WidgetClass->GetPathName();
		Node->PinDefaultValueChanged(ClassPin);
	}
	return Node;
}

static UEdGraphNode* SpawnComponentEventPatchNode(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString ComponentName;
	FString DelegateName;
	if (!GetRequiredPatchString(NodeObject, TEXT("component"), ComponentName, OutError) || !GetRequiredPatchString(NodeObject, TEXT("delegate"), DelegateName, OutError))
	{
		return nullptr;
	}
	UClass* BlueprintClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
	FObjectProperty* ComponentProperty = BlueprintClass ? FindFProperty<FObjectProperty>(BlueprintClass, *ComponentName) : nullptr;
	UClass* ComponentClass = ComponentProperty ? Cast<UClass>(ComponentProperty->PropertyClass) : nullptr;
	FMulticastDelegateProperty* DelegateProperty = ComponentClass ? FindFProperty<FMulticastDelegateProperty>(ComponentClass, *DelegateName) : nullptr;
	if (!ComponentProperty || !DelegateProperty)
	{
		OutError = FString::Printf(TEXT("Could not resolve component delegate '%s.%s'."), *ComponentName, *DelegateName);
		return nullptr;
	}
	FGraphNodeCreator<UK2Node_ComponentBoundEvent> NodeCreator(*Graph);
	UK2Node_ComponentBoundEvent* Node = NodeCreator.CreateNode();
	Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
	Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
	Node->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
	NodeCreator.Finalize();
	return Node;
}

static UEdGraphNode* SpawnDelegatePatchNode(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObject, const FString& NodeType, FString& OutError)
{
	if (NodeType.Equals(TEXT("CreateDelegate"), ESearchCase::IgnoreCase))
	{
		FGraphNodeCreator<UK2Node_CreateDelegate> NodeCreator(*Graph);
		UK2Node_CreateDelegate* Node = NodeCreator.CreateNode();
		Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
		Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
		FString FunctionName;
		if (NodeObject->TryGetStringField(TEXT("function"), FunctionName) && !FunctionName.IsEmpty())
		{
			Node->SetFunction(*FunctionName);
		}
		NodeCreator.Finalize();
		return Node;
	}

	UClass* OwnerClass = nullptr;
	FMulticastDelegateProperty* DelegateProperty = FindDelegatePropertyForCommand(Blueprint, NodeObject, OutError, OwnerClass);
	if (!DelegateProperty)
	{
		return nullptr;
	}
	const bool bSelfContext = !NodeObject->HasField(TEXT("ownerClass"));
	if (NodeType.Equals(TEXT("DelegateBind"), ESearchCase::IgnoreCase))
	{
		FGraphNodeCreator<UK2Node_AddDelegate> NodeCreator(*Graph);
		UK2Node_AddDelegate* Node = NodeCreator.CreateNode();
		Node->SetFromProperty(DelegateProperty, bSelfContext, OwnerClass);
		Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
		Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
		NodeCreator.Finalize();
		return Node;
	}
	if (NodeType.Equals(TEXT("DelegateBroadcast"), ESearchCase::IgnoreCase))
	{
		FGraphNodeCreator<UK2Node_CallDelegate> NodeCreator(*Graph);
		UK2Node_CallDelegate* Node = NodeCreator.CreateNode();
		Node->SetFromProperty(DelegateProperty, bSelfContext, OwnerClass);
		Node->NodePosX = GetPatchInteger(NodeObject, TEXT("x"));
		Node->NodePosY = GetPatchInteger(NodeObject, TEXT("y"));
		NodeCreator.Finalize();
		return Node;
	}
	OutError = FString::Printf(TEXT("Unsupported delegate node type '%s'."), *NodeType);
	return nullptr;
}

static UEdGraphNode* CreatePatchNode(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString NodeId;
	if (!GetRequiredPatchString(NodeObject, TEXT("id"), NodeId, OutError))
	{
		return nullptr;
	}
	if (Context.NodesById.Contains(NodeId))
	{
		OutError = FString::Printf(TEXT("Patch node id '%s' is already in use."), *NodeId);
		return nullptr;
	}

	FString ExistingGuid;
	if (NodeObject->TryGetStringField(TEXT("existingGuid"), ExistingGuid) && !ExistingGuid.IsEmpty())
	{
		UEdGraphNode* ExistingNode = FindNodeByGuid(Context.Graph, ExistingGuid);
		if (!ExistingNode)
		{
			OutError = FString::Printf(TEXT("Could not find existing node '%s'."), *ExistingGuid);
			return nullptr;
		}
		Context.NodesById.Add(NodeId, ExistingNode);
		return ExistingNode;
	}

	FString NodeType;
	if (!GetRequiredPatchString(NodeObject, TEXT("type"), NodeType, OutError))
	{
		return nullptr;
	}

	const int32 X = GetPatchInteger(NodeObject, TEXT("x"));
	const int32 Y = GetPatchInteger(NodeObject, TEXT("y"));
	UEdGraphNode* Node = nullptr;
	if (NodeType.Equals(TEXT("Branch"), ESearchCase::IgnoreCase))
	{
		Node = SpawnBranchNode(Context.Graph, X, Y);
	}
	else if (NodeType.Equals(TEXT("Sequence"), ESearchCase::IgnoreCase))
	{
		Node = SpawnSequencePatchNode(Context.Graph, X, Y, GetPatchInteger(NodeObject, TEXT("extraOutputs")));
	}
	else if (NodeType.Equals(TEXT("Reroute"), ESearchCase::IgnoreCase))
	{
		Node = SpawnReroutePatchNode(Context.Graph, X, Y);
	}
	else if (NodeType.Equals(TEXT("Comment"), ESearchCase::IgnoreCase))
	{
		Node = SpawnCommentPatchNode(Context.Graph, NodeObject);
	}
	else if (NodeType.Equals(TEXT("VariableGet"), ESearchCase::IgnoreCase))
	{
		FString VariableName;
		if (!GetRequiredPatchString(NodeObject, TEXT("variable"), VariableName, OutError))
		{
			return nullptr;
		}
		Node = SpawnVariableGetNode(Context.Blueprint, Context.Graph, *VariableName, X, Y);
	}
	else if (NodeType.Equals(TEXT("VariableSet"), ESearchCase::IgnoreCase))
	{
		FString VariableName;
		if (!GetRequiredPatchString(NodeObject, TEXT("variable"), VariableName, OutError))
		{
			return nullptr;
		}
		Node = SpawnVariableSetNode(Context.Blueprint, Context.Graph, *VariableName, X, Y);
	}
	else if (NodeType.Equals(TEXT("FunctionCall"), ESearchCase::IgnoreCase))
	{
		FString FunctionClassPath;
		FString FunctionName;
		if (!GetRequiredPatchString(NodeObject, TEXT("functionClass"), FunctionClassPath, OutError) || !GetRequiredPatchString(NodeObject, TEXT("function"), FunctionName, OutError))
		{
			return nullptr;
		}
		UFunction* Function = FindFunctionForNodeCommand(FunctionClassPath, FunctionName);
		if (!Function)
		{
			OutError = FString::Printf(TEXT("Could not find function '%s' on '%s'."), *FunctionName, *FunctionClassPath);
			return nullptr;
		}
		Node = SpawnFunctionCallNode(Context.Graph, Function, X, Y);
	}
	else if (NodeType.Equals(TEXT("Self"), ESearchCase::IgnoreCase))
	{
		Node = SpawnSelfPatchNode(Context.Graph, X, Y);
	}
	else if (NodeType.Equals(TEXT("DynamicCast"), ESearchCase::IgnoreCase))
	{
		Node = SpawnDynamicCastPatchNode(Context.Graph, NodeObject, OutError);
	}
	else if (NodeType.Equals(TEXT("MakeStruct"), ESearchCase::IgnoreCase))
	{
		Node = SpawnStructPatchNode(Context.Graph, NodeObject, true, OutError);
	}
	else if (NodeType.Equals(TEXT("BreakStruct"), ESearchCase::IgnoreCase))
	{
		Node = SpawnStructPatchNode(Context.Graph, NodeObject, false, OutError);
	}
	else if (NodeType.Equals(TEXT("CustomEvent"), ESearchCase::IgnoreCase))
	{
		Node = SpawnCustomEventPatchNode(Context.Blueprint, Context.Graph, NodeObject, OutError);
	}
	else if (NodeType.Equals(TEXT("EnumSwitch"), ESearchCase::IgnoreCase))
	{
		Node = SpawnEnumSwitchPatchNode(Context.Graph, NodeObject, OutError);
	}
	else if (NodeType.Equals(TEXT("EnumEquality"), ESearchCase::IgnoreCase))
	{
		Node = SpawnEnumEqualityNode(Context.Graph, X, Y);
	}
	else if (NodeType.Equals(TEXT("ForLoop"), ESearchCase::IgnoreCase))
	{
		Node = SpawnMacroPatchNode(Context.Graph, NodeObject, TEXT("ForLoop"), TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"), OutError);
	}
	else if (NodeType.Equals(TEXT("ForEachLoop"), ESearchCase::IgnoreCase))
	{
		Node = SpawnMacroPatchNode(Context.Graph, NodeObject, TEXT("ForEachLoop"), TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"), OutError);
	}
	else if (NodeType.Equals(TEXT("WhileLoop"), ESearchCase::IgnoreCase))
	{
		Node = SpawnMacroPatchNode(Context.Graph, NodeObject, TEXT("WhileLoop"), TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"), OutError);
	}
	else if (NodeType.Equals(TEXT("AuthoritySwitch"), ESearchCase::IgnoreCase))
	{
		Node = SpawnMacroPatchNode(Context.Graph, NodeObject, TEXT("Switch Has Authority"), TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros"), OutError);
	}
	else if (NodeType.Equals(TEXT("ArrayFunction"), ESearchCase::IgnoreCase))
	{
		Node = SpawnArrayFunctionPatchNode(Context.Graph, NodeObject, OutError);
	}
	else if (NodeType.Equals(TEXT("Timer"), ESearchCase::IgnoreCase))
	{
		FString Operation;
		if (!GetRequiredPatchString(NodeObject, TEXT("operation"), Operation, OutError))
		{
			return nullptr;
		}
		FString FunctionName = Operation.Equals(TEXT("SetByEvent"), ESearchCase::IgnoreCase) ? TEXT("K2_SetTimerDelegate") : Operation.Equals(TEXT("SetByFunctionName"), ESearchCase::IgnoreCase) ? TEXT("K2_SetTimer") : Operation.Equals(TEXT("ClearByHandle"), ESearchCase::IgnoreCase) ? TEXT("K2_ClearTimerHandle") : Operation.Equals(TEXT("ClearAndInvalidateByHandle"), ESearchCase::IgnoreCase) ? TEXT("K2_ClearAndInvalidateTimerHandle") : FString();
		UFunction* Function = FunctionName.IsEmpty() ? nullptr : FindFunctionForNodeCommand(TEXT("/Script/Engine.KismetSystemLibrary"), FunctionName);
		if (!Function)
		{
			OutError = FString::Printf(TEXT("Unsupported timer operation '%s'."), *Operation);
			return nullptr;
		}
		Node = SpawnFunctionCallNode(Context.Graph, Function, X, Y);
	}
	else if (NodeType.Equals(TEXT("SpawnActor"), ESearchCase::IgnoreCase))
	{
		Node = SpawnSpawnActorPatchNode(Context.Graph, NodeObject, OutError);
	}
	else if (NodeType.Equals(TEXT("CreateWidget"), ESearchCase::IgnoreCase))
	{
		Node = SpawnCreateWidgetPatchNode(Context.Graph, NodeObject, OutError);
	}
	else if (NodeType.Equals(TEXT("ComponentEvent"), ESearchCase::IgnoreCase))
	{
		Node = SpawnComponentEventPatchNode(Context.Blueprint, Context.Graph, NodeObject, OutError);
	}
	else if (NodeType.Equals(TEXT("DelegateBind"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("DelegateBroadcast"), ESearchCase::IgnoreCase) || NodeType.Equals(TEXT("CreateDelegate"), ESearchCase::IgnoreCase))
	{
		Node = SpawnDelegatePatchNode(Context.Blueprint, Context.Graph, NodeObject, NodeType, OutError);
	}
	else if (NodeType.Equals(TEXT("Event"), ESearchCase::IgnoreCase))
	{
		Node = SpawnEventPatchNode(Context.Graph, NodeObject, OutError);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unsupported patch node type '%s'."), *NodeType);
		return nullptr;
	}

	if (!Node)
	{
		if (OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Could not create patch node '%s'."), *NodeId);
		}
		return nullptr;
	}

	ApplyNodePinDefaults(Node, NodeObject);
	Context.NodesById.Add(NodeId, Node);
	Context.CreatedNodes.Add(Node);
	return Node;
}

static bool SplitPatchPinRef(const FString& Ref, FString& OutNodeId, FString& OutPinName)
{
	return Ref.Split(TEXT("."), &OutNodeId, &OutPinName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !OutNodeId.IsEmpty() && !OutPinName.IsEmpty();
}

static UEdGraphPin* ResolvePatchPin(FGraphPatchContext& Context, const FString& Ref, const TOptional<EEdGraphPinDirection> Direction, FString& OutError)
{
	FString NodeId;
	FString PinName;
	if (!SplitPatchPinRef(Ref, NodeId, PinName))
	{
		OutError = FString::Printf(TEXT("Pin ref '%s' must use nodeId.pinName form."), *Ref);
		return nullptr;
	}

	UEdGraphNode* ResolvedNode = nullptr;
	if (NodeId.StartsWith(TEXT("existing:")))
	{
		FString ExistingGuid = NodeId.RightChop(9);
		ResolvedNode = FindNodeByGuid(Context.Graph, ExistingGuid);
	}
	else if (UEdGraphNode** NodePtr = Context.NodesById.Find(NodeId))
	{
		ResolvedNode = *NodePtr;
	}
	if (!ResolvedNode)
	{
		OutError = FString::Printf(TEXT("Could not resolve patch node id '%s'."), *NodeId);
		return nullptr;
	}

	UEdGraphPin* Pin = FindPinByName(ResolvedNode, PinName, Direction);
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Could not find pin '%s' on patch node '%s'."), *PinName, *NodeId);
	}
	return Pin;
}

static bool ApplyPatchLink(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& LinkObject, TSharedRef<FJsonObject> LinkResult, FString& OutError)
{
	FString FromRef;
	FString ToRef;
	if (!GetRequiredPatchString(LinkObject, TEXT("from"), FromRef, OutError) || !GetRequiredPatchString(LinkObject, TEXT("to"), ToRef, OutError))
	{
		return false;
	}

	LinkResult->SetStringField(TEXT("from"), FromRef);
	LinkResult->SetStringField(TEXT("to"), ToRef);
	UEdGraphPin* FromPin = ResolvePatchPin(Context, FromRef, TOptional<EEdGraphPinDirection>(EGPD_Output), OutError);
	UEdGraphPin* ToPin = ResolvePatchPin(Context, ToRef, TOptional<EEdGraphPinDirection>(EGPD_Input), OutError);
	if (!FromPin || !ToPin)
	{
		LinkResult->SetBoolField(TEXT("ok"), false);
		return false;
	}

	const bool bConnected = GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(FromPin, ToPin);
	LinkResult->SetBoolField(TEXT("ok"), bConnected);
	if (!bConnected)
	{
		OutError = FString::Printf(TEXT("Could not connect '%s' to '%s'."), *FromRef, *ToRef);
	}
	return bConnected;
}

static bool ApplyPatchDefault(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& DefaultObject, FString& OutError)
{
	FString NodeId;
	FString PinName;
	FString Value;
	if (!GetRequiredPatchString(DefaultObject, TEXT("node"), NodeId, OutError) ||
		!GetRequiredPatchString(DefaultObject, TEXT("pin"), PinName, OutError) ||
		!GetRequiredPatchString(DefaultObject, TEXT("value"), Value, OutError))
	{
		return false;
	}

	UEdGraphNode** NodePtr = Context.NodesById.Find(NodeId);
	if (!NodePtr || !*NodePtr)
	{
		OutError = FString::Printf(TEXT("Could not resolve patch node id '%s'."), *NodeId);
		return false;
	}

	UEdGraphPin* Pin = FindPinByName(*NodePtr, PinName, TOptional<EEdGraphPinDirection>(EGPD_Input));
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Could not find input pin '%s' on patch node '%s'."), *PinName, *NodeId);
		return false;
	}

	Pin->DefaultValue = Value;
	return true;
}



static UEdGraphNode* ResolvePatchNode(FGraphPatchContext& Context, const FString& NodeIdOrGuid)
{
	if (NodeIdOrGuid.StartsWith(TEXT("existing:")))
	{
		return FindNodeByGuid(Context.Graph, NodeIdOrGuid.RightChop(9));
	}
	if (UEdGraphNode** NodePtr = Context.NodesById.Find(NodeIdOrGuid))
	{
		return *NodePtr;
	}
	return FindNodeByGuid(Context.Graph, NodeIdOrGuid);
}

static bool ApplyPatchMoveNode(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& Object, FString& OutError)
{
	FString NodeId;
	if (!GetRequiredPatchString(Object, TEXT("id"), NodeId, OutError))
	{
		return false;
	}
	UEdGraphNode* Node = ResolvePatchNode(Context, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Could not resolve patch node id '%s'."), *NodeId);
		return false;
	}
	Node->Modify();
	Node->NodePosX = GetPatchInteger(Object, TEXT("x"), Node->NodePosX);
	Node->NodePosY = GetPatchInteger(Object, TEXT("y"), Node->NodePosY);
	return true;
}

static bool ApplyPatchBreakLinks(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& Object, FString& OutError)
{
	FString NodeId;
	FString PinName;
	if (!GetRequiredPatchString(Object, TEXT("node"), NodeId, OutError) || !GetRequiredPatchString(Object, TEXT("pin"), PinName, OutError))
	{
		return false;
	}
	UEdGraphNode* Node = ResolvePatchNode(Context, NodeId);
	UEdGraphPin* Pin = FindPinByName(Node, PinName, TOptional<EEdGraphPinDirection>());
	if (!Pin)
	{
		OutError = FString::Printf(TEXT("Could not find pin '%s' on patch node '%s'."), *PinName, *NodeId);
		return false;
	}
	Pin->Modify();
	Pin->BreakAllPinLinks();
	return true;
}

static bool ApplyPatchDeleteNode(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& Object, FString& OutError)
{
	FString NodeId;
	if (!GetRequiredPatchString(Object, TEXT("id"), NodeId, OutError))
	{
		return false;
	}
	UEdGraphNode* Node = ResolvePatchNode(Context, NodeId);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Could not resolve patch node id '%s'."), *NodeId);
		return false;
	}
	Node->Modify();
	FBlueprintEditorUtils::RemoveNode(Context.Blueprint, Node, true);
	Context.NodesById.Remove(NodeId);
	Context.CreatedNodes.Remove(Node);
	return true;
}

static bool ApplyPatchRenamePin(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& Object, FString& OutError)
{
	FString NodeId;
	FString PinName;
	FString NewName;
	if (!GetRequiredPatchString(Object, TEXT("node"), NodeId, OutError) || !GetRequiredPatchString(Object, TEXT("pin"), PinName, OutError) || !GetRequiredPatchString(Object, TEXT("newName"), NewName, OutError))
	{
		return false;
	}
	UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(ResolvePatchNode(Context, NodeId));
	if (!EditableNode)
	{
		OutError = FString::Printf(TEXT("Patch node '%s' does not support user-defined pins."), *NodeId);
		return false;
	}
	EditableNode->Modify();
	const ERenamePinResult RenameResult = EditableNode->RenameUserDefinedPin(*PinName, *NewName, false);
	if (RenameResult == ERenamePinResult_NameCollision)
	{
		OutError = FString::Printf(TEXT("Could not rename pin '%s' to '%s' due to a name collision."), *PinName, *NewName);
		return false;
	}
	EditableNode->ReconstructNode();
	return true;
}

static bool ApplyPatchSetPinFlags(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& Object, FString& OutError)
{
	FString NodeId;
	FString PinName;
	if (!GetRequiredPatchString(Object, TEXT("node"), NodeId, OutError) || !GetRequiredPatchString(Object, TEXT("pin"), PinName, OutError))
	{
		return false;
	}
	UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(ResolvePatchNode(Context, NodeId));
	if (!EditableNode)
	{
		OutError = FString::Printf(TEXT("Patch node '%s' does not support user-defined pins."), *NodeId);
		return false;
	}
	TSharedPtr<FUserPinInfo>* UserPinInfo = EditableNode->UserDefinedPins.FindByPredicate([&PinName](const TSharedPtr<FUserPinInfo>& Candidate)
	{
		return Candidate.IsValid() && Candidate->PinName == *PinName;
	});
	if (!UserPinInfo)
	{
		OutError = FString::Printf(TEXT("Could not find user-defined pin '%s'."), *PinName);
		return false;
	}
	EditableNode->Modify();
	ApplyPinRefAndConstFlags(Object, (*UserPinInfo)->PinType);
	EditableNode->ReconstructNode();
	return true;
}

static bool ApplyPatchOperationArray(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool (*Operation)(FGraphPatchContext&, const TSharedPtr<FJsonObject>&, FString&), TArray<TSharedPtr<FJsonValue>>& Results, FString& OutPhase, int32& OutIndex, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Params->TryGetArrayField(FieldName, Items) || Items == nullptr)
	{
		return true;
	}
	for (int32 Index = 0; Index < Items->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!(*Items)[Index].IsValid() || !(*Items)[Index]->TryGetObject(Object) || Object == nullptr || !Object->IsValid())
		{
			OutPhase = FieldName;
			OutIndex = Index;
			OutError = FString::Printf(TEXT("Each %s item must be an object."), FieldName);
			return false;
		}
		FString Error;
		if (!Operation(Context, *Object, Error))
		{
			OutPhase = FieldName;
			OutIndex = Index;
			OutError = Error;
			return false;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}
	return true;
}

static void RollbackPatchNodes(FGraphPatchContext& Context)
{
	for (int32 Index = Context.CreatedNodes.Num() - 1; Index >= 0; --Index)
	{
		UEdGraphNode* Node = Context.CreatedNodes[Index];
		if (Node && Node->GetGraph() == Context.Graph)
		{
			Node->Modify();
			Node->BreakAllNodeLinks();
			Context.Graph->RemoveNode(Node);
		}
	}
	Context.CreatedNodes.Reset();
}

static TSharedRef<FJsonObject> MakePatchFailure(const FString& Id, FGraphPatchContext& Context, FScopedTransaction& Transaction, const bool bRollbackOnFailure, const FString& Phase, const int32 Index, const FString& Error)
{
	if (bRollbackOnFailure)
	{
		RollbackPatchNodes(Context);
		Transaction.Cancel();
	}

	if (Context.bValidateOnly)
	{
		TSharedRef<FJsonObject> ErrorJson = MakeShared<FJsonObject>();
		ErrorJson->SetStringField(TEXT("phase"), Phase);
		ErrorJson->SetNumberField(TEXT("index"), Index);
		ErrorJson->SetStringField(TEXT("message"), Error);

		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Add(MakeShared<FJsonValueObject>(ErrorJson));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("valid"), false);
		Result->SetArrayField(TEXT("errors"), Errors);
		return MakeSuccess(Id, Result);
	}

	TSharedRef<FJsonObject> Response = MakeBridgeError(Id, TEXT("GraphPatchFailed"), Error);
	TSharedPtr<FJsonObject> ErrorObject = Response->GetObjectField(TEXT("error"));
	ErrorObject->SetStringField(TEXT("phase"), Phase);
	ErrorObject->SetNumberField(TEXT("index"), Index);
	ErrorObject->SetBoolField(TEXT("rolledBack"), bRollbackOnFailure);
	return Response;
}

TSharedRef<FJsonObject> ApplyGraphPatch(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> ErrorResponse = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, ErrorResponse))
	{
		return ErrorResponse;
	}

	bool bRollbackOnFailure = true;
	bool bCompile = false;
	bool bValidateOnly = false;
	Params->TryGetBoolField(TEXT("rollbackOnFailure"), bRollbackOnFailure);
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	Params->TryGetBoolField(TEXT("validateOnly"), bValidateOnly);
	if (bValidateOnly)
	{
		bRollbackOnFailure = true;
		bCompile = false;
	}

	FGraphPatchContext Context;
	Context.Id = Id;
	Context.Blueprint = Blueprint;
	Context.Graph = Graph;
	Context.bValidateOnly = bValidateOnly;
	PrimePatchVirtualNodes(Context);
	FString ExistingNodesError;
	if (!PrimePatchExistingNodes(Context, Params, ExistingNodesError))
	{
		FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "ApplyGraphPatch", "Blueprint Bridge: Apply Graph Patch"));
		return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, TEXT("existingNodes"), 0, ExistingNodesError);
	}

	FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "ApplyGraphPatch", "Blueprint Bridge: Apply Graph Patch"));
	Blueprint->Modify();
	Graph->Modify();

	TArray<TSharedPtr<FJsonValue>> NodeResults;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (Params->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes != nullptr)
	{
		for (int32 Index = 0; Index < Nodes->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (!(*Nodes)[Index].IsValid() || !(*Nodes)[Index]->TryGetObject(NodeObject) || NodeObject == nullptr || !NodeObject->IsValid())
			{
				return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, TEXT("nodes"), Index, TEXT("Each patch node must be an object."));
			}

			FString NodeError;
			UEdGraphNode* Node = CreatePatchNode(Context, *NodeObject, NodeError);
			if (!Node)
			{
				return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, TEXT("nodes"), Index, NodeError);
			}

			FString NodeId;
			(*NodeObject)->TryGetStringField(TEXT("id"), NodeId);
			TSharedRef<FJsonObject> NodeResult = MakeShared<FJsonObject>();
			NodeResult->SetStringField(TEXT("id"), NodeId);
			NodeResult->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			NodeResult->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			NodeResult->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			NodeResults.Add(MakeShared<FJsonValueObject>(NodeResult));
		}
	}

	TArray<TSharedPtr<FJsonValue>> DefaultResults;
	const TArray<TSharedPtr<FJsonValue>>* Defaults = nullptr;
	if (Params->TryGetArrayField(TEXT("defaults"), Defaults) && Defaults != nullptr)
	{
		for (int32 Index = 0; Index < Defaults->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* DefaultObject = nullptr;
			if (!(*Defaults)[Index].IsValid() || !(*Defaults)[Index]->TryGetObject(DefaultObject) || DefaultObject == nullptr || !DefaultObject->IsValid())
			{
				return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, TEXT("defaults"), Index, TEXT("Each patch default must be an object."));
			}

			FString DefaultError;
			if (!ApplyPatchDefault(Context, *DefaultObject, DefaultError))
			{
				return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, TEXT("defaults"), Index, DefaultError);
			}
			TSharedRef<FJsonObject> DefaultResult = MakeShared<FJsonObject>();
			DefaultResult->SetBoolField(TEXT("ok"), true);
			DefaultResults.Add(MakeShared<FJsonValueObject>(DefaultResult));
		}
	}

	TArray<TSharedPtr<FJsonValue>> LinkResults;
	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	if (Params->TryGetArrayField(TEXT("links"), Links) && Links != nullptr)
	{
		for (int32 Index = 0; Index < Links->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* LinkObject = nullptr;
			if (!(*Links)[Index].IsValid() || !(*Links)[Index]->TryGetObject(LinkObject) || LinkObject == nullptr || !LinkObject->IsValid())
			{
				return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, TEXT("links"), Index, TEXT("Each patch link must be an object."));
			}

			FString LinkError;
			TSharedRef<FJsonObject> LinkResult = MakeShared<FJsonObject>();
			if (!ApplyPatchLink(Context, *LinkObject, LinkResult, LinkError))
			{
				LinkResults.Add(MakeShared<FJsonValueObject>(LinkResult));
				return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, TEXT("links"), Index, LinkError);
			}
			LinkResults.Add(MakeShared<FJsonValueObject>(LinkResult));
		}
	}

	TArray<TSharedPtr<FJsonValue>> MoveResults;
	TArray<TSharedPtr<FJsonValue>> BreakResults;
	TArray<TSharedPtr<FJsonValue>> RenamePinResults;
	TArray<TSharedPtr<FJsonValue>> SetPinFlagResults;
	TArray<TSharedPtr<FJsonValue>> DeleteResults;
	FString OperationPhase;
	int32 OperationIndex = INDEX_NONE;
	FString OperationError;
	if (!ApplyPatchOperationArray(Context, Params, TEXT("moveNodes"), &ApplyPatchMoveNode, MoveResults, OperationPhase, OperationIndex, OperationError) ||
		!ApplyPatchOperationArray(Context, Params, TEXT("breakLinks"), &ApplyPatchBreakLinks, BreakResults, OperationPhase, OperationIndex, OperationError) ||
		!ApplyPatchOperationArray(Context, Params, TEXT("renamePins"), &ApplyPatchRenamePin, RenamePinResults, OperationPhase, OperationIndex, OperationError) ||
		!ApplyPatchOperationArray(Context, Params, TEXT("setPinFlags"), &ApplyPatchSetPinFlags, SetPinFlagResults, OperationPhase, OperationIndex, OperationError) ||
		!ApplyPatchOperationArray(Context, Params, TEXT("deleteNodes"), &ApplyPatchDeleteNode, DeleteResults, OperationPhase, OperationIndex, OperationError))
	{
		return MakePatchFailure(Id, Context, Transaction, bRollbackOnFailure, OperationPhase, OperationIndex, OperationError);
	}

	if (bValidateOnly)
	{
		RollbackPatchNodes(Context);
		Transaction.Cancel();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("valid"), true);
		Result->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
		Result->SetArrayField(TEXT("wouldCreateNodes"), NodeResults);
		return MakeSuccess(Id, Result);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), Params->GetStringField(TEXT("asset")));
	Result->SetStringField(TEXT("graph"), Graph->GetName());
	Result->SetArrayField(TEXT("nodes"), NodeResults);
	Result->SetArrayField(TEXT("defaults"), DefaultResults);
	Result->SetArrayField(TEXT("links"), LinkResults);
	Result->SetArrayField(TEXT("moveNodes"), MoveResults);
	Result->SetArrayField(TEXT("breakLinks"), BreakResults);
	Result->SetArrayField(TEXT("renamePins"), RenamePinResults);
	Result->SetArrayField(TEXT("setPinFlags"), SetPinFlagResults);
	Result->SetArrayField(TEXT("deleteNodes"), DeleteResults);
	if (bCompile)
	{
		Result->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
	}
	return MakeSuccess(Id, Result);
}

static TSharedPtr<FJsonValue> CloneJsonValueWithBindings(const TSharedPtr<FJsonValue>& Value, const TSharedPtr<FJsonObject>& Bindings)
{
	if (!Value.IsValid())
	{
		return nullptr;
	}

	FString StringValue;
	if (Value->TryGetString(StringValue))
	{
		if (Bindings.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Binding : Bindings->Values)
			{
				FString Replacement;
				if (Binding.Value.IsValid() && Binding.Value->TryGetString(Replacement))
				{
					StringValue.ReplaceInline(*FString::Printf(TEXT("$%s"), *Binding.Key), *Replacement, ESearchCase::CaseSensitive);
				}
			}
		}
		return MakeShared<FJsonValueString>(StringValue);
	}

	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Value->TryGetObject(Object) && Object != nullptr && Object->IsValid())
	{
		TSharedRef<FJsonObject> NewObject = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
		{
			NewObject->SetField(Pair.Key, CloneJsonValueWithBindings(Pair.Value, Bindings));
		}
		return MakeShared<FJsonValueObject>(NewObject);
	}

	const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
	if (Value->TryGetArray(Array) && Array != nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> NewArray;
		for (const TSharedPtr<FJsonValue>& Item : *Array)
		{
			NewArray.Add(CloneJsonValueWithBindings(Item, Bindings));
		}
		return MakeShared<FJsonValueArray>(NewArray);
	}

	return Value;
}

static TSharedRef<FJsonObject> CloneJsonObjectWithBindings(const TSharedPtr<FJsonObject>& Object, const TSharedPtr<FJsonObject>& Bindings)
{
	TSharedRef<FJsonObject> NewObject = MakeShared<FJsonObject>();
	if (!Object.IsValid())
	{
		return NewObject;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
	{
		NewObject->SetField(Pair.Key, CloneJsonValueWithBindings(Pair.Value, Bindings));
	}
	return NewObject;
}

static bool AddOrUpdateFunctionPins(UBlueprint* Blueprint, UEdGraph* Graph, const TArray<TSharedPtr<FJsonValue>>* Pins, const bool bOutput, FString& OutError)
{
	if (!Pins || Pins->Num() == 0)
	{
		return true;
	}

	UK2Node_EditablePinBase* EditableNode = bOutput ? Cast<UK2Node_EditablePinBase>(FindFunctionResultNode(Graph)) : Cast<UK2Node_EditablePinBase>(FindFunctionEntryNode(Graph));
	if (bOutput && !EditableNode)
	{
		// UE creates the result node lazily — the first added output is the trigger. Mirror that here.
		FGraphNodeCreator<UK2Node_FunctionResult> ResultNodeCreator(*Graph);
		UK2Node_FunctionResult* ResultNode = ResultNodeCreator.CreateNode();
		ResultNode->NodePosX = 320;
		ResultNode->NodePosY = 0;
		if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Graph))
		{
			ResultNode->FunctionReference = EntryNode->FunctionReference;
		}
		ResultNodeCreator.Finalize();
		EditableNode = ResultNode;
	}
	if (!EditableNode)
	{
		OutError = bOutput ? TEXT("Could not find function result node.") : TEXT("Could not find function entry node.");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
	{
		const TSharedPtr<FJsonObject>* PinObject = nullptr;
		if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObject) || PinObject == nullptr || !PinObject->IsValid())
		{
			OutError = TEXT("Each function pin must be an object.");
			return false;
		}
		FString PinName;
		if (!GetRequiredPatchString(*PinObject, TEXT("name"), PinName, OutError))
		{
			return false;
		}
		FEdGraphPinType PinType;
		if (!TryMakePinTypeFromParams(Blueprint, *PinObject, PinType, OutError))
		{
			return false;
		}
		ApplyPinRefAndConstFlags(*PinObject, PinType);

		TSharedPtr<FUserPinInfo>* ExistingPin = EditableNode->UserDefinedPins.FindByPredicate([&PinName](const TSharedPtr<FUserPinInfo>& Candidate)
		{
			return Candidate.IsValid() && Candidate->PinName == *PinName;
		});
		EditableNode->Modify();
		if (ExistingPin)
		{
			(*ExistingPin)->PinType = PinType;
		}
		else
		{
			EditableNode->CreateUserDefinedPin(*PinName, PinType, bOutput ? EGPD_Input : EGPD_Output, false);
		}
	}
	EditableNode->ReconstructNode();
	return true;
}

TSharedRef<FJsonObject> ApplyFunctionPatch(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("ApplyFunctionPatch requires params.asset and params.function."));
	}
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	bool bCreateIfMissing = false;
	bool bCompile = false;
	Params->TryGetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	UEdGraph* Graph = FindBlueprintGraph(Blueprint, FunctionName);
	FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "ApplyFunctionPatch", "Blueprint Bridge: Apply Function Patch"));
	Blueprint->Modify();
	if (!Graph)
	{
		if (!bCreateIfMissing)
		{
			return MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Function graph '%s' does not exist."), *FunctionName));
		}
		Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, nullptr);
	}
	Graph->Modify();

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	Params->TryGetArrayField(TEXT("inputs"), Inputs);
	Params->TryGetArrayField(TEXT("outputs"), Outputs);
	FString PinError;
	if (!AddOrUpdateFunctionPins(Blueprint, Graph, Inputs, false, PinError) || !AddOrUpdateFunctionPins(Blueprint, Graph, Outputs, true, PinError))
	{
		Transaction.Cancel();
		return MakeBridgeError(Id, TEXT("FunctionPatchFailed"), PinError);
	}

	TSharedRef<FJsonObject> BodyResult = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* Body = nullptr;
	if (Params->TryGetObjectField(TEXT("body"), Body) && Body != nullptr && Body->IsValid())
	{
		// Body patches frequently link to result.execute (Semantic IR auto-fall-through, explicit `return`).
		// Void functions don't have a result node by default — UE only lazy-creates it when outputs are added.
		// Ensure one exists before applying the body so void-function bodies resolve `result` references.
		if (!FindFunctionResultNode(Graph))
		{
			FGraphNodeCreator<UK2Node_FunctionResult> ResultNodeCreator(*Graph);
			UK2Node_FunctionResult* ResultNode = ResultNodeCreator.CreateNode();
			ResultNode->NodePosX = 320;
			ResultNode->NodePosY = 0;
			if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Graph))
			{
				ResultNode->FunctionReference = EntryNode->FunctionReference;
			}
			ResultNodeCreator.Finalize();
		}

		TSharedRef<FJsonObject> PatchParams = CloneJsonObjectWithBindings(*Body, nullptr);
		PatchParams->SetStringField(TEXT("asset"), AssetPath);
		PatchParams->SetStringField(TEXT("graph"), Graph->GetName());
		PatchParams->SetBoolField(TEXT("compile"), false);
		TSharedRef<FJsonObject> PatchResponse = ApplyGraphPatch(Id, PatchParams);
		const TSharedPtr<FJsonObject>* PatchResult = nullptr;
		if (!PatchResponse->TryGetObjectField(TEXT("result"), PatchResult) || PatchResult == nullptr || !PatchResult->IsValid())
		{
			Transaction.Cancel();
			return PatchResponse;
		}
		BodyResult = (*PatchResult).ToSharedRef();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("function"), FunctionName);
	Result->SetStringField(TEXT("graph"), Graph->GetName());
	Result->SetObjectField(TEXT("body"), BodyResult);
	if (bCompile)
	{
		Result->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
	}
	return MakeSuccess(Id, Result);
}

static bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

TSharedRef<FJsonObject> ApplyGraphSnippet(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString SnippetName;
	if (!TryGetRequiredString(Params, TEXT("snippet"), SnippetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("ApplyGraphSnippet requires params.snippet."));
	}

	FString SnippetPath;
	Params->TryGetStringField(TEXT("snippetPath"), SnippetPath);
	if (SnippetPath.IsEmpty())
	{
		SnippetPath = FPaths::ProjectSavedDir() / TEXT("BlueprintBridge/Snippets") / (SnippetName + TEXT(".json"));
	}

	TSharedPtr<FJsonObject> SnippetObject;
	if (!LoadJsonObjectFromFile(SnippetPath, SnippetObject))
	{
		return MakeBridgeError(Id, TEXT("SnippetNotFound"), FString::Printf(TEXT("Could not load snippet '%s'."), *SnippetPath));
	}

	const TSharedPtr<FJsonObject>* Bindings = nullptr;
	Params->TryGetObjectField(TEXT("bindings"), Bindings);
	TSharedRef<FJsonObject> PatchParams = CloneJsonObjectWithBindings(SnippetObject, Bindings ? *Bindings : nullptr);
	PatchParams->SetStringField(TEXT("asset"), Params->GetStringField(TEXT("asset")));
	PatchParams->SetStringField(TEXT("graph"), Params->GetStringField(TEXT("graph")));
	return ApplyGraphPatch(Id, PatchParams);
}

static FString MakeExportNodeId(UEdGraphNode* Node, TMap<UEdGraphNode*, FString>& NodeIds, const bool bNormalizeIds)
{
	if (const FString* Existing = NodeIds.Find(Node))
	{
		return *Existing;
	}
	const FString Id = bNormalizeIds ? FString::Printf(TEXT("node_%d"), NodeIds.Num() + 1) : Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	NodeIds.Add(Node, Id);
	return Id;
}

TSharedRef<FJsonObject> ExportGraphPatch(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}
	bool bNormalizeIds = true;
	Params->TryGetBoolField(TEXT("normalizeIds"), bNormalizeIds);

	TMap<UEdGraphNode*, FString> NodeIds;
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<TSharedPtr<FJsonValue>> Links;
	TArray<TSharedPtr<FJsonValue>> Defaults;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>())
		{
			continue;
		}
		const FString NodeId = MakeExportNodeId(Node, NodeIds, bNormalizeIds);
		TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("id"), NodeId);
		NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);
		if (Node->IsA<UK2Node_IfThenElse>()) { NodeJson->SetStringField(TEXT("type"), TEXT("Branch")); }
		else if (Node->IsA<UK2Node_ExecutionSequence>()) { NodeJson->SetStringField(TEXT("type"), TEXT("Sequence")); }
		else if (Node->IsA<UK2Node_Knot>()) { NodeJson->SetStringField(TEXT("type"), TEXT("Reroute")); }
		else if (UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node)) { NodeJson->SetStringField(TEXT("type"), TEXT("Comment")); NodeJson->SetStringField(TEXT("text"), Comment->NodeComment); NodeJson->SetNumberField(TEXT("width"), Comment->NodeWidth); NodeJson->SetNumberField(TEXT("height"), Comment->NodeHeight); }
		else if (UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node)) { NodeJson->SetStringField(TEXT("type"), Node->IsA<UK2Node_VariableSet>() ? TEXT("VariableSet") : TEXT("VariableGet")); NodeJson->SetStringField(TEXT("variable"), VariableNode->GetVarNameString()); }
		else if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node)) { NodeJson->SetStringField(TEXT("type"), TEXT("FunctionCall")); NodeJson->SetStringField(TEXT("functionClass"), CallNode->FunctionReference.GetMemberParentClass() ? CallNode->FunctionReference.GetMemberParentClass()->GetPathName() : FString()); NodeJson->SetStringField(TEXT("function"), CallNode->FunctionReference.GetMemberName().ToString()); }
		else if (Node->IsA<UK2Node_Self>()) { NodeJson->SetStringField(TEXT("type"), TEXT("Self")); }
		else { NodeJson->SetStringField(TEXT("type"), TEXT("Unsupported")); NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName()); }
		Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->Direction == EGPD_Input && !Pin->DefaultValue.IsEmpty())
			{
				TSharedRef<FJsonObject> DefaultJson = MakeShared<FJsonObject>();
				DefaultJson->SetStringField(TEXT("node"), NodeId);
				DefaultJson->SetStringField(TEXT("pin"), Pin->PinName.ToString());
				DefaultJson->SetStringField(TEXT("value"), Pin->DefaultValue);
				Defaults.Add(MakeShared<FJsonValueObject>(DefaultJson));
			}
			if (Pin->Direction == EGPD_Output)
			{
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (!LinkedNode)
					{
						continue;
					}
					TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
					LinkJson->SetStringField(TEXT("from"), FString::Printf(TEXT("%s.%s"), *NodeId, *Pin->PinName.ToString()));
					LinkJson->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *MakeExportNodeId(LinkedNode, NodeIds, bNormalizeIds), *LinkedPin->PinName.ToString()));
					Links.Add(MakeShared<FJsonValueObject>(LinkJson));
				}
			}
		}
	}

	TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
	Patch->SetArrayField(TEXT("nodes"), Nodes);
	Patch->SetArrayField(TEXT("defaults"), Defaults);
	Patch->SetArrayField(TEXT("links"), Links);
	return MakeSuccess(Id, Patch);
}

TSharedRef<FJsonObject> ImportGraphPatch(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* PatchObject = nullptr;
	if (!Params->TryGetObjectField(TEXT("patch"), PatchObject) || PatchObject == nullptr || !PatchObject->IsValid())
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("ImportGraphPatch requires params.patch."));
	}
	const TSharedPtr<FJsonObject>* Bindings = nullptr;
	Params->TryGetObjectField(TEXT("bindings"), Bindings);
	TSharedRef<FJsonObject> PatchParams = CloneJsonObjectWithBindings(*PatchObject, Bindings ? *Bindings : nullptr);
	PatchParams->SetStringField(TEXT("asset"), Params->GetStringField(TEXT("asset")));
	PatchParams->SetStringField(TEXT("graph"), Params->GetStringField(TEXT("graph")));
	return ApplyGraphPatch(Id, PatchParams);
}

static TSharedRef<FJsonObject> MakeSemanticLoweringError(const FString& Id, const FString& Pointer, const FString& Message)
{
	TSharedRef<FJsonObject> Response = MakeBridgeError(Id, TEXT("SemanticLoweringFailed"), Message);
	const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
	if (Response->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject != nullptr && ErrorObject->IsValid())
	{
		(*ErrorObject)->SetStringField(TEXT("pointer"), Pointer);
	}
	return Response;
}

TSharedRef<FJsonObject> LowerSemanticFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("LowerSemanticFunction requires params.asset and params.function."));
	}
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	FSemanticLowerResult LowerResult;
	FString ErrorPointer;
	FString ErrorMessage;
	if (!LowerSemanticFunctionIR(Blueprint, Params, LowerResult, ErrorPointer, ErrorMessage))
	{
		return MakeSemanticLoweringError(Id, ErrorPointer, ErrorMessage);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("function"), FunctionName);
	Result->SetObjectField(TEXT("patch"), LowerResult.Patch);
	Result->SetObjectField(TEXT("resolutions"), LowerResult.Resolutions);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> ApplySemanticFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("ApplySemanticFunction requires params.asset and params.function."));
	}
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	FSemanticLowerResult LowerResult;
	FString ErrorPointer;
	FString ErrorMessage;
	if (!LowerSemanticFunctionIR(Blueprint, Params, LowerResult, ErrorPointer, ErrorMessage))
	{
		return MakeSemanticLoweringError(Id, ErrorPointer, ErrorMessage);
	}

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("asset"), AssetPath);
	ApplyParams->SetStringField(TEXT("function"), FunctionName);

	bool bCreateIfMissing = false;
	bool bCompile = false;
	Params->TryGetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	ApplyParams->SetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
	ApplyParams->SetBoolField(TEXT("compile"), bCompile);

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
	{
		ApplyParams->SetArrayField(TEXT("inputs"), *Inputs);
	}
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	if (Params->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs != nullptr)
	{
		ApplyParams->SetArrayField(TEXT("outputs"), *Outputs);
	}

	TSharedRef<FJsonObject> Body = LowerResult.Patch;
	bool bRollbackOnFailure = true;
	if (Params->TryGetBoolField(TEXT("rollbackOnFailure"), bRollbackOnFailure))
	{
		Body->SetBoolField(TEXT("rollbackOnFailure"), bRollbackOnFailure);
	}
	ApplyParams->SetObjectField(TEXT("body"), Body);

	TSharedRef<FJsonObject> Response = ApplyFunctionPatch(Id, ApplyParams);

	bool bOk = false;
	if (Response->TryGetBoolField(TEXT("ok"), bOk) && bOk)
	{
		const TSharedPtr<FJsonObject>* ResultObj = nullptr;
		if (Response->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj != nullptr && ResultObj->IsValid())
		{
			(*ResultObj)->SetObjectField(TEXT("resolutions"), LowerResult.Resolutions);
		}
	}
	return Response;
}

TSharedRef<FJsonObject> ApplyAndFix(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("ApplyAndFix requires params.asset and params.function."));
	}
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	bool bRollbackOnCompileError = false;
	bool bCreateIfMissing = false;
	Params->TryGetBoolField(TEXT("rollbackOnCompileError"), bRollbackOnCompileError);
	Params->TryGetBoolField(TEXT("createIfMissing"), bCreateIfMissing);

	// Track whether the function graph existed before this call. Graph creation isn't transactional —
	// FScopedTransaction::Cancel() won't undo CreateNewGraph + AddFunctionGraph — so a rollback
	// from a freshly-created function needs an explicit RemoveGraph step.
	const bool bFunctionPreExisted = (FindBlueprintGraph(Blueprint, FunctionName) != nullptr);

	FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "ApplyAndFix", "Blueprint Bridge: Apply And Fix"));

	FSemanticLowerResult LowerResult;
	FString ErrorPointer;
	FString ErrorMessage;
	if (!LowerSemanticFunctionIR(Blueprint, Params, LowerResult, ErrorPointer, ErrorMessage))
	{
		Transaction.Cancel();
		return MakeSemanticLoweringError(Id, ErrorPointer, ErrorMessage);
	}

	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("asset"), AssetPath);
	ApplyParams->SetStringField(TEXT("function"), FunctionName);
	ApplyParams->SetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
	ApplyParams->SetBoolField(TEXT("compile"), false);

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
	{
		ApplyParams->SetArrayField(TEXT("inputs"), *Inputs);
	}
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	if (Params->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs != nullptr)
	{
		ApplyParams->SetArrayField(TEXT("outputs"), *Outputs);
	}
	ApplyParams->SetObjectField(TEXT("body"), LowerResult.Patch);

	TSharedRef<FJsonObject> ApplyResponse = ApplyFunctionPatch(Id, ApplyParams);
	bool bApplyOk = false;
	if (!ApplyResponse->TryGetBoolField(TEXT("ok"), bApplyOk) || !bApplyOk)
	{
		Transaction.Cancel();
		return ApplyResponse;
	}

	FCompilerResultsLog ResultsLog;
	ResultsLog.SetSourcePath(Blueprint->GetPathName());
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &ResultsLog);
	const bool bCompileSuccess = (ResultsLog.NumErrors == 0) && (Blueprint->Status != BS_Error);

	if (!bCompileSuccess)
	{
		if (bRollbackOnCompileError)
		{
			Transaction.Cancel();
			// Transaction rollback doesn't undo graph creation. If this call created the function,
			// explicitly remove it so the rollback is honest.
			if (!bFunctionPreExisted)
			{
				if (UEdGraph* CreatedGraph = FindBlueprintGraph(Blueprint, FunctionName))
				{
					FBlueprintEditorUtils::RemoveGraph(Blueprint, CreatedGraph, EGraphRemoveFlags::None);
				}
			}
		}
		TSharedRef<FJsonObject> Response = MakeBridgeError(Id, TEXT("CompileFailed"), FString::Printf(TEXT("Blueprint compiled with %d error(s)."), ResultsLog.NumErrors));
		const TSharedPtr<FJsonObject>* ErrObj = nullptr;
		if (Response->TryGetObjectField(TEXT("error"), ErrObj) && ErrObj != nullptr && (*ErrObj).IsValid())
		{
			(*ErrObj)->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
			(*ErrObj)->SetNumberField(TEXT("errorCount"), ResultsLog.NumErrors);
			(*ErrObj)->SetNumberField(TEXT("warningCount"), ResultsLog.NumWarnings);
			(*ErrObj)->SetArrayField(TEXT("messages"), BuildCompileMessages(ResultsLog));
			(*ErrObj)->SetObjectField(TEXT("appliedPatch"), LowerResult.Patch);
			(*ErrObj)->SetObjectField(TEXT("resolutions"), LowerResult.Resolutions);
			(*ErrObj)->SetBoolField(TEXT("rolledBack"), bRollbackOnCompileError);
		}
		return Response;
	}

	const TSharedPtr<FJsonObject>* ResultObj = nullptr;
	if (ApplyResponse->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj != nullptr && (*ResultObj).IsValid())
	{
		(*ResultObj)->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
		(*ResultObj)->SetNumberField(TEXT("warningCount"), ResultsLog.NumWarnings);
		(*ResultObj)->SetObjectField(TEXT("resolutions"), LowerResult.Resolutions);
	}
	return ApplyResponse;
}

TSharedRef<FJsonObject> ReplaceSemanticFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("ReplaceSemanticFunction requires params.asset and params.function."));
	}
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}
	UEdGraph* Graph = FindBlueprintGraph(Blueprint, FunctionName);
	if (!Graph)
	{
		return MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Function graph '%s' does not exist. Use ApplySemanticFunction with createIfMissing for creation."), *FunctionName));
	}

	bool bCompile = false;
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "ReplaceSemanticFunction", "Blueprint Bridge: Replace Semantic Function"));
	Blueprint->Modify();
	Graph->Modify();

	// Wipe non-entry/non-result nodes. Collect first so iteration is stable across removals.
	TArray<UEdGraphNode*> ToRemove;
	ToRemove.Reserve(Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>())
		{
			continue;
		}
		ToRemove.Add(Node);
	}
	for (UEdGraphNode* Node : ToRemove)
	{
		Node->Modify();
		Graph->RemoveNode(Node);
	}

	// Lower the new IR.
	FSemanticLowerResult LowerResult;
	FString ErrorPointer;
	FString ErrorMessage;
	if (!LowerSemanticFunctionIR(Blueprint, Params, LowerResult, ErrorPointer, ErrorMessage))
	{
		Transaction.Cancel();
		return MakeSemanticLoweringError(Id, ErrorPointer, ErrorMessage);
	}

	// Apply via ApplyFunctionPatch (handles signature merge + body patch).
	TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
	ApplyParams->SetStringField(TEXT("asset"), AssetPath);
	ApplyParams->SetStringField(TEXT("function"), FunctionName);
	ApplyParams->SetBoolField(TEXT("createIfMissing"), false);
	ApplyParams->SetBoolField(TEXT("compile"), bCompile);

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
	{
		ApplyParams->SetArrayField(TEXT("inputs"), *Inputs);
	}
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	if (Params->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs != nullptr)
	{
		ApplyParams->SetArrayField(TEXT("outputs"), *Outputs);
	}
	ApplyParams->SetObjectField(TEXT("body"), LowerResult.Patch);

	TSharedRef<FJsonObject> ApplyResponse = ApplyFunctionPatch(Id, ApplyParams);
	bool bOk = false;
	if (!ApplyResponse->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
	{
		Transaction.Cancel();
		return ApplyResponse;
	}

	const TSharedPtr<FJsonObject>* ResultObj = nullptr;
	if (ApplyResponse->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj != nullptr && (*ResultObj).IsValid())
	{
		(*ResultObj)->SetObjectField(TEXT("resolutions"), LowerResult.Resolutions);
	}
	return ApplyResponse;
}

} // namespace BlueprintBridge
