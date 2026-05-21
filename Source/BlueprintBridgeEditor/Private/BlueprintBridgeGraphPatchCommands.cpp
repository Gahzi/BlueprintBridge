// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
struct FGraphPatchContext
{
	FString Id;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TMap<FString, UEdGraphNode*> NodesById;
	TArray<UEdGraphNode*> CreatedNodes;
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

static UEdGraphNode* CreatePatchNode(FGraphPatchContext& Context, const TSharedPtr<FJsonObject>& NodeObject, FString& OutError)
{
	FString NodeId;
	FString NodeType;
	if (!GetRequiredPatchString(NodeObject, TEXT("id"), NodeId, OutError) || !GetRequiredPatchString(NodeObject, TEXT("type"), NodeType, OutError))
	{
		return nullptr;
	}
	if (Context.NodesById.Contains(NodeId))
	{
		OutError = FString::Printf(TEXT("Patch node id '%s' is already in use."), *NodeId);
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

	UEdGraphNode** NodePtr = Context.NodesById.Find(NodeId);
	if (!NodePtr || !*NodePtr)
	{
		OutError = FString::Printf(TEXT("Could not resolve patch node id '%s'."), *NodeId);
		return nullptr;
	}

	UEdGraphPin* Pin = FindPinByName(*NodePtr, PinName, Direction);
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
	Params->TryGetBoolField(TEXT("rollbackOnFailure"), bRollbackOnFailure);
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FGraphPatchContext Context;
	Context.Id = Id;
	Context.Blueprint = Blueprint;
	Context.Graph = Graph;
	PrimePatchVirtualNodes(Context);

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
	if (bCompile)
	{
		Result->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
	}
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
