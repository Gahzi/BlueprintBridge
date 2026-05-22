// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
void MoveLinks(UEdGraphPin* FromPin, UEdGraphPin* ToPin)
{
	if (!FromPin || !ToPin)
	{
		return;
	}

	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	TArray<UEdGraphPin*> LinkedPins = FromPin->LinkedTo;
	FromPin->BreakAllPinLinks();
	for (UEdGraphPin* LinkedPin : LinkedPins)
	{
		if (LinkedPin)
		{
			Schema->TryCreateConnection(ToPin, LinkedPin);
		}
	}
}

UEdGraphPin* FindFirstDataOutputPin(UEdGraphNode* Node)
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			return Pin;
		}
	}
	return nullptr;
}

UK2Node_CallFunction* SpawnFunctionCallNode(UEdGraph* Graph, UFunction* Function, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* Node = NodeCreator.CreateNode();
	Node->SetFromFunction(Function);
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

UK2Node_IfThenElse* SpawnBranchNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_IfThenElse> NodeCreator(*Graph);
	UK2Node_IfThenElse* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

UK2Node_EnumEquality* SpawnEnumEqualityNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_EnumEquality> NodeCreator(*Graph);
	UK2Node_EnumEquality* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

UK2Node_VariableGet* SpawnVariableGetNode(UBlueprint* Blueprint, UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y)
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	return Schema->SpawnVariableGetNode(FVector2D(X, Y), Graph, VariableName, Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass);
}

UK2Node_VariableSet* SpawnVariableSetNode(UBlueprint* Blueprint, UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y)
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	return Schema->SpawnVariableSetNode(FVector2D(X, Y), Graph, VariableName, Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass);
}

UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuid)
{
	FGuid Guid;
	if (!FGuid::Parse(NodeGuid, Guid))
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid)
		{
			return Node;
		}
	}
	return nullptr;
}

FString NormalizePinLookupName(const FString& Name)
{
	FString Result;
	Result.Reserve(Name.Len());
	for (TCHAR Character : Name)
	{
		if (!FChar::IsWhitespace(Character) && Character != TEXT('_'))
		{
			Result.AppendChar(FChar::ToLower(Character));
		}
	}
	if (Result.Equals(TEXT("false"), ESearchCase::IgnoreCase))
	{
		return TEXT("else");
	}
	if (Result.Equals(TEXT("true"), ESearchCase::IgnoreCase))
	{
		return TEXT("then");
	}
	if (Result.Equals(TEXT("exec"), ESearchCase::IgnoreCase))
	{
		return TEXT("execute");
	}
	if (Result.Equals(TEXT("return"), ESearchCase::IgnoreCase))
	{
		return TEXT("returnvalue");
	}
	if (Result.Equals(TEXT("self"), ESearchCase::IgnoreCase))
	{
		return TEXT("self");
	}
	return Result;
}

bool PinDirectionMatches(const UEdGraphPin* Pin, const TOptional<EEdGraphPinDirection> Direction)
{
	return !Direction.IsSet() || (Pin && Pin->Direction == Direction.GetValue());
}

UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, const TOptional<EEdGraphPinDirection> Direction = TOptional<EEdGraphPinDirection>())
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && PinDirectionMatches(Pin, Direction) && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}

	const FString NormalizedName = NormalizePinLookupName(PinName);
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && PinDirectionMatches(Pin, Direction) && NormalizePinLookupName(Pin->PinName.ToString()).Equals(NormalizedName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}

	UEdGraphPin* Match = nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && PinDirectionMatches(Pin, Direction) && NormalizePinLookupName(Pin->PinName.ToString()).Contains(NormalizedName))
		{
			if (Match)
			{
				return nullptr;
			}
			Match = Pin;
		}
	}
	return Match;
}

bool TryLoadGraphForEdit(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, TSharedRef<FJsonObject>& OutError)
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

void ApplyNodePinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* PinDefaults = nullptr;
	if (!Node || !Params.IsValid() || !Params->TryGetObjectField(TEXT("pinDefaults"), PinDefaults) || PinDefaults == nullptr || !PinDefaults->IsValid())
	{
		return;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PinDefaults)->Values)
	{
		UEdGraphPin* Pin = FindPinByName(Node, Pair.Key, TOptional<EEdGraphPinDirection>(EGPD_Input));
		if (Pin && Pair.Value.IsValid())
		{
			FString Value;
			if (Pair.Value->TryGetString(Value))
			{
				Pin->DefaultValue = Value;
			}
			else
			{
				Pin->DefaultValue = Pair.Value->AsString();
			}
		}
	}
}

TSharedRef<FJsonObject> FinishGraphEdit(const FString& Id, UBlueprint* Blueprint, UEdGraph* Graph, UEdGraphNode* Node)
{
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (Node)
	{
		Result->SetObjectField(TEXT("node"), DescribeNode(Node));
	}
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> FinishGraphEdit(const FString& Id, UBlueprint* Blueprint, UEdGraphNode* Node)
{
	return FinishGraphEdit(Id, Blueprint, nullptr, Node);
}

TSharedRef<FJsonObject> AddVariableGetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString VariableName;
	if (!TryGetRequiredString(Params, TEXT("variable"), VariableName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddVariableGetNode requires params.variable."));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddVariableGetNode", "Blueprint Bridge: Add Variable Get Node"));
	Blueprint->Modify();
	Graph->Modify();
	UK2Node_VariableGet* Node = SpawnVariableGetNode(Blueprint, Graph, *VariableName, X, Y);
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddVariableSetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString VariableName;
	if (!TryGetRequiredString(Params, TEXT("variable"), VariableName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddVariableSetNode requires params.variable."));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddVariableSetNode", "Blueprint Bridge: Add Variable Set Node"));
	Blueprint->Modify();
	Graph->Modify();
	UK2Node_VariableSet* Node = SpawnVariableSetNode(Blueprint, Graph, *VariableName, X, Y);
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddBranchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddBranchNode", "Blueprint Bridge: Add Branch Node"));
	Blueprint->Modify();
	Graph->Modify();
	UK2Node_IfThenElse* Node = SpawnBranchNode(Graph, X, Y);
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddEnumEqualityNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddEnumEqualityNode", "Blueprint Bridge: Add Enum Equality Node"));
	Blueprint->Modify();
	Graph->Modify();
	UK2Node_EnumEquality* Node = SpawnEnumEqualityNode(Graph, X, Y);
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddFunctionCallNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString FunctionClassPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("functionClass"), FunctionClassPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddFunctionCallNode requires params.functionClass and params.function."));
	}

	UClass* FunctionClass = FindObject<UClass>(nullptr, *FunctionClassPath);
	if (!FunctionClass)
	{
		FunctionClass = LoadObject<UClass>(nullptr, *FunctionClassPath);
	}
	if (!FunctionClass)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *FunctionClassPath));
	}

	UFunction* Function = FunctionClass->FindFunctionByName(*FunctionName);
	if (!Function)
	{
		return MakeBridgeError(Id, TEXT("FunctionNotFound"), FString::Printf(TEXT("Could not find function '%s' on '%s'."), *FunctionName, *FunctionClassPath));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddFunctionCallNode", "Blueprint Bridge: Add Function Call Node"));
	Blueprint->Modify();
	Graph->Modify();
	UK2Node_CallFunction* Node = SpawnFunctionCallNode(Graph, Function, X, Y);
	ApplyNodePinDefaults(Node, Params);
	return FinishGraphEdit(Id, Blueprint, Node);
}

UFunction* FindFunctionForNodeCommand(const FString& ClassPath, const FString& FunctionName)
{
	UClass* FunctionClass = FindObject<UClass>(nullptr, *ClassPath);
	if (!FunctionClass)
	{
		FunctionClass = LoadObject<UClass>(nullptr, *ClassPath);
	}
	return FunctionClass ? FunctionClass->FindFunctionByName(*FunctionName) : nullptr;
}

TSharedRef<FJsonObject> AddSelfNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddSelfNode", "Blueprint Bridge: Add Self Node"));
	Blueprint->Modify();
	Graph->Modify();
	FGraphNodeCreator<UK2Node_Self> NodeCreator(*Graph);
	UK2Node_Self* Node = NodeCreator.CreateNode();
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddArrayFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString Operation;
	if (!TryGetRequiredString(Params, TEXT("operation"), Operation))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddArrayFunctionNode requires params.operation."));
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
	if (!FunctionName)
	{
		return MakeBridgeError(Id, TEXT("InvalidArrayOperation"), FString::Printf(TEXT("Unsupported array operation '%s'."), *Operation));
	}

	UFunction* Function = UKismetArrayLibrary::StaticClass()->FindFunctionByName(*FunctionName);
	if (!Function)
	{
		return MakeBridgeError(Id, TEXT("FunctionNotFound"), FString::Printf(TEXT("Could not find array function for '%s'."), *Operation));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddArrayFunctionNode", "Blueprint Bridge: Add Array Function Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_CallArrayFunction> NodeCreator(*Graph);
	UK2Node_CallArrayFunction* Node = NodeCreator.CreateNode();
	Node->SetFromFunction(Function);
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	NodeCreator.Finalize();
	ApplyNodePinDefaults(Node, Params);
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddTimerNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString Operation;
	if (!TryGetRequiredString(Params, TEXT("operation"), Operation))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddTimerNode requires params.operation."));
	}

	FString FunctionName;
	if (Operation.Equals(TEXT("SetByEvent"), ESearchCase::IgnoreCase))
	{
		FunctionName = TEXT("K2_SetTimerDelegate");
	}
	else if (Operation.Equals(TEXT("SetByFunctionName"), ESearchCase::IgnoreCase))
	{
		FunctionName = TEXT("K2_SetTimer");
	}
	else if (Operation.Equals(TEXT("ClearByHandle"), ESearchCase::IgnoreCase))
	{
		FunctionName = TEXT("K2_ClearTimerHandle");
	}
	else if (Operation.Equals(TEXT("ClearAndInvalidateByHandle"), ESearchCase::IgnoreCase))
	{
		FunctionName = TEXT("K2_ClearAndInvalidateTimerHandle");
	}
	else
	{
		return MakeBridgeError(Id, TEXT("InvalidTimerOperation"), FString::Printf(TEXT("Unsupported timer operation '%s'."), *Operation));
	}

	Params->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetSystemLibrary"));
	Params->SetStringField(TEXT("function"), FunctionName);
	return AddFunctionCallNode(Id, Params);
}

TSharedRef<FJsonObject> AddLineTraceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString Operation;
	Params->TryGetStringField(TEXT("operation"), Operation);
	const FString FunctionName = Operation.Equals(TEXT("Multi"), ESearchCase::IgnoreCase) ? TEXT("LineTraceMulti") : TEXT("LineTraceSingle");
	Params->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetSystemLibrary"));
	Params->SetStringField(TEXT("function"), FunctionName);
	return AddFunctionCallNode(Id, Params);
}

TSharedRef<FJsonObject> AddMathNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddMathNode requires params.function."));
	}

	Params->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetMathLibrary"));
	return AddFunctionCallNode(Id, Params);
}

TSharedRef<FJsonObject> AddWidgetFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetClassPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("widgetClass"), WidgetClassPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddWidgetFunctionNode requires params.widgetClass and params.function."));
	}

	Params->SetStringField(TEXT("functionClass"), WidgetClassPath);
	return AddFunctionCallNode(Id, Params);
}

bool AddUserDefinedOutputPinsFromArray(UBlueprint* Blueprint, UK2Node_EditablePinBase* Node, const TArray<TSharedPtr<FJsonValue>>& Inputs, FString& OutError)
{
	for (const TSharedPtr<FJsonValue>& InputValue : Inputs)
	{
		const TSharedPtr<FJsonObject>* InputObject = nullptr;
		if (!InputValue.IsValid() || !InputValue->TryGetObject(InputObject) || InputObject == nullptr || !InputObject->IsValid())
		{
			OutError = TEXT("Each input must be an object.");
			return false;
		}

		FString PinName;
		if (!TryGetRequiredString(*InputObject, TEXT("name"), PinName))
		{
			OutError = TEXT("Each input requires a name.");
			return false;
		}

		FEdGraphPinType PinType;
		if (!TryMakePinTypeFromParams(Blueprint, *InputObject, PinType, OutError))
		{
			return false;
		}

		if (!Node->CreateUserDefinedPin(*PinName, PinType, EGPD_Output, false))
		{
			OutError = FString::Printf(TEXT("Could not create input pin '%s'."), *PinName);
			return false;
		}
	}

	return true;
}

TSharedRef<FJsonObject> AddCustomEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString EventName;
	if (!TryGetRequiredString(Params, TEXT("name"), EventName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddCustomEventNode requires params.name."));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddCustomEventNode", "Blueprint Bridge: Add Custom Event Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*Graph);
	UK2Node_CustomEvent* Node = NodeCreator.CreateNode();
	Node->CustomFunctionName = *EventName;
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
	{
		FString PinError;
		if (!AddUserDefinedOutputPinsFromArray(Blueprint, Node, *Inputs, PinError))
		{
			return MakeBridgeError(Id, TEXT("CreatePinFailed"), PinError);
		}
		Node->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString EventClassPath;
	FString EventName;
	if (!TryGetRequiredString(Params, TEXT("eventClass"), EventClassPath) || !TryGetRequiredString(Params, TEXT("event"), EventName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddEventNode requires params.eventClass and params.event."));
	}

	UClass* EventClass = LoadClassByPath(EventClassPath);
	if (!EventClass)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *EventClassPath));
	}

	UFunction* EventFunction = EventClass->FindFunctionByName(*EventName);
	if (!EventFunction)
	{
		return MakeBridgeError(Id, TEXT("FunctionNotFound"), FString::Printf(TEXT("Could not find event function '%s' on '%s'."), *EventName, *EventClassPath));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddEventNode", "Blueprint Bridge: Add Event Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_Event> NodeCreator(*Graph);
	UK2Node_Event* Node = NodeCreator.CreateNode();
	Node->EventReference.SetFromField<UFunction>(EventFunction, false);
	Node->bOverrideFunction = true;
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddDynamicCastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString TargetClassPath;
	if (!TryGetRequiredString(Params, TEXT("targetClass"), TargetClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddDynamicCastNode requires params.targetClass."));
	}

	UClass* TargetClass = LoadClassByPath(TargetClassPath);
	if (!TargetClass)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load class '%s'."), *TargetClassPath));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddDynamicCastNode", "Blueprint Bridge: Add Dynamic Cast Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_DynamicCast> NodeCreator(*Graph);
	UK2Node_DynamicCast* Node = NodeCreator.CreateNode();
	Node->TargetType = TargetClass;
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddSpawnActorNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString ActorClassPath;
	if (!TryGetRequiredString(Params, TEXT("actorClass"), ActorClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddSpawnActorNode requires params.actorClass."));
	}

	UClass* ActorClass = LoadClassByPath(ActorClassPath);
	if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load actor class '%s'."), *ActorClassPath));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddSpawnActorNode", "Blueprint Bridge: Add Spawn Actor Node"));
	Blueprint->Modify();
	Graph->Modify();

	UK2Node_SpawnActorFromClass* Node = NewObject<UK2Node_SpawnActorFromClass>(Graph);
	Node->SetFlags(RF_Transactional);
	Graph->AddNode(Node, true, false);
	Node->CreateNewGuid();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();
	if (UEdGraphPin* ClassPin = Node->GetClassPin())
	{
		ClassPin->DefaultObject = ActorClass;
		ClassPin->DefaultValue = ActorClass->GetPathName();
		Node->PinDefaultValueChanged(ClassPin);
	}
	ApplyNodePinDefaults(Node, Params);
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddEventDispatcher(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString DispatcherName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("name"), DispatcherName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddEventDispatcher requires params.asset and params.name."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddEventDispatcher", "Blueprint Bridge: Add Event Dispatcher"));
	Blueprint->Modify();

	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, *DispatcherName, DelegateType))
	{
		return MakeBridgeError(Id, TEXT("AddDispatcherFailed"), FString::Printf(TEXT("Could not add event dispatcher '%s'."), *DispatcherName));
	}

	UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *DispatcherName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!SignatureGraph)
	{
		FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, *DispatcherName);
		return MakeBridgeError(Id, TEXT("CreateGraphFailed"), FString::Printf(TEXT("Could not create signature graph for '%s'."), *DispatcherName));
	}

	SignatureGraph->bEditable = false;
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
	K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, (UClass*)nullptr);
	K2Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
	Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
	{
		if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(SignatureGraph))
		{
			for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
			{
				const TSharedPtr<FJsonObject>* InputObject = nullptr;
				if (!InputValue.IsValid() || !InputValue->TryGetObject(InputObject) || InputObject == nullptr || !InputObject->IsValid())
				{
					return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("Each dispatcher input must be an object."));
				}

				FString PinName;
				if (!TryGetRequiredString(*InputObject, TEXT("name"), PinName))
				{
					return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("Each dispatcher input requires a name."));
				}

				FEdGraphPinType PinType;
				FString PinError;
				if (!TryMakePinTypeFromParams(Blueprint, *InputObject, PinType, PinError))
				{
					return MakeBridgeError(Id, TEXT("InvalidPinType"), PinError);
				}

				EntryNode->CreateUserDefinedPin(*PinName, PinType, EGPD_Output, false);
			}
			EntryNode->ReconstructNode();
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("dispatcher"), DispatcherName);
	Result->SetStringField(TEXT("signatureGraph"), SignatureGraph->GetName());
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> AddComponentEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString ComponentName;
	FString DelegateName;
	if (!TryGetRequiredString(Params, TEXT("component"), ComponentName) || !TryGetRequiredString(Params, TEXT("delegate"), DelegateName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddComponentEventNode requires params.component and params.delegate."));
	}

	UClass* BlueprintClass = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
	FObjectProperty* ComponentProperty = BlueprintClass ? FindFProperty<FObjectProperty>(BlueprintClass, *ComponentName) : nullptr;
	if (!ComponentProperty)
	{
		return MakeBridgeError(Id, TEXT("ComponentNotFound"), FString::Printf(TEXT("Could not find component property '%s'."), *ComponentName));
	}

	UClass* ComponentClass = Cast<UClass>(ComponentProperty->PropertyClass);
	FMulticastDelegateProperty* DelegateProperty = ComponentClass ? FindFProperty<FMulticastDelegateProperty>(ComponentClass, *DelegateName) : nullptr;
	if (!DelegateProperty)
	{
		return MakeBridgeError(Id, TEXT("DelegateNotFound"), FString::Printf(TEXT("Could not find delegate '%s' on component '%s'."), *DelegateName, *ComponentName));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddComponentEventNode", "Blueprint Bridge: Add Component Event Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_ComponentBoundEvent> NodeCreator(*Graph);
	UK2Node_ComponentBoundEvent* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	Node->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
	NodeCreator.Finalize();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

FMulticastDelegateProperty* FindDelegatePropertyForCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError, UClass*& OutOwnerClass);

TSharedRef<FJsonObject> AddDelegateBindNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString DelegateError;
	UClass* BlueprintClass = nullptr;
	FMulticastDelegateProperty* DelegateProperty = FindDelegatePropertyForCommand(Blueprint, Params, DelegateError, BlueprintClass);
	if (!DelegateProperty)
	{
		return MakeBridgeError(Id, TEXT("DelegateNotFound"), DelegateError);
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddDelegateBindNode", "Blueprint Bridge: Add Delegate Bind Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_AddDelegate> NodeCreator(*Graph);
	UK2Node_AddDelegate* Node = NodeCreator.CreateNode();
	Node->SetFromProperty(DelegateProperty, !Params->HasField(TEXT("ownerClass")), BlueprintClass);
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

FMulticastDelegateProperty* FindDelegatePropertyForCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError, UClass*& OutOwnerClass)
{
	FString DelegateName;
	if (!TryGetRequiredString(Params, TEXT("delegate"), DelegateName))
	{
		OutError = TEXT("Command requires params.delegate.");
		return nullptr;
	}

	FString OwnerClassPath;
	Params->TryGetStringField(TEXT("ownerClass"), OwnerClassPath);
	OutOwnerClass = OwnerClassPath.IsEmpty() ? static_cast<UClass*>(Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass) : LoadClassByPath(OwnerClassPath);
	if (!OutOwnerClass)
	{
		OutError = FString::Printf(TEXT("Could not load delegate owner class '%s'."), *OwnerClassPath);
		return nullptr;
	}

	FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(OutOwnerClass, *DelegateName);
	if (!DelegateProperty)
	{
		OutError = FString::Printf(TEXT("Could not find delegate '%s'."), *DelegateName);
		return nullptr;
	}
	return DelegateProperty;
}

TSharedRef<FJsonObject> AddDelegateBroadcastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString DelegateError;
	UClass* OwnerClass = nullptr;
	FMulticastDelegateProperty* DelegateProperty = FindDelegatePropertyForCommand(Blueprint, Params, DelegateError, OwnerClass);
	if (!DelegateProperty)
	{
		return MakeBridgeError(Id, TEXT("DelegateNotFound"), DelegateError);
	}

	const bool bSelfContext = !Params->HasField(TEXT("ownerClass"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddDelegateBroadcastNode", "Blueprint Bridge: Add Delegate Broadcast Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_CallDelegate> NodeCreator(*Graph);
	UK2Node_CallDelegate* Node = NodeCreator.CreateNode();
	Node->SetFromProperty(DelegateProperty, bSelfContext, OwnerClass);
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddCreateDelegateNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddCreateDelegateNode", "Blueprint Bridge: Add Create Delegate Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_CreateDelegate> NodeCreator(*Graph);
	UK2Node_CreateDelegate* Node = NodeCreator.CreateNode();
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	FString FunctionName;
	if (Params->TryGetStringField(TEXT("function"), FunctionName) && !FunctionName.IsEmpty())
	{
		Node->SetFunction(*FunctionName);
	}
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> SetCreateDelegateFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetCreateDelegateFunction requires params.node and params.function."));
	}

	UK2Node_CreateDelegate* Node = Cast<UK2Node_CreateDelegate>(FindNodeByGuid(Graph, NodeGuid));
	if (!Node)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not find Create Delegate node '%s'."), *NodeGuid));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetCreateDelegateFunction", "Blueprint Bridge: Set Create Delegate Function"));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Node->SetFunction(*FunctionName);
	Node->HandleAnyChange(true);
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	return nullptr;
}

TSharedRef<FJsonObject> AddMacroInstanceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params, const FString& DefaultMacro, const FString& DefaultMacroLibrary = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"))
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString MacroName = DefaultMacro;
	Params->TryGetStringField(TEXT("macro"), MacroName);
	FString MacroLibraryPath = DefaultMacroLibrary;
	Params->TryGetStringField(TEXT("macroLibrary"), MacroLibraryPath);

	UBlueprint* MacroLibrary = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *MacroLibraryPath));
	if (!MacroLibrary)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load macro library '%s'."), *MacroLibraryPath));
	}

	UEdGraph* MacroGraph = FindGraphByName(MacroLibrary, MacroName);
	if (!MacroGraph)
	{
		return MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Could not find macro '%s'."), *MacroName));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddMacroInstanceNode", "Blueprint Bridge: Add Macro Instance Node"));
	Blueprint->Modify();
	Graph->Modify();

	FGraphNodeCreator<UK2Node_MacroInstance> NodeCreator(*Graph);
	UK2Node_MacroInstance* Node = NodeCreator.CreateNode();
	Node->SetMacroGraph(MacroGraph);
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddForLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddMacroInstanceNode(Id, Params, TEXT("ForLoop"));
}

TSharedRef<FJsonObject> AddForEachLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddMacroInstanceNode(Id, Params, TEXT("ForEachLoop"));
}

TSharedRef<FJsonObject> AddAuthoritySwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddMacroInstanceNode(Id, Params, TEXT("Switch Has Authority"), TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros"));
}

TSharedRef<FJsonObject> AddCreateWidgetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString WidgetClassPath;
	if (!TryGetRequiredString(Params, TEXT("widgetClass"), WidgetClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddCreateWidgetNode requires params.widgetClass."));
	}

	UClass* WidgetClass = LoadClassByPath(WidgetClassPath);
	if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load UserWidget class '%s'."), *WidgetClassPath));
	}

	UClass* CreateWidgetNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
	if (!CreateWidgetNodeClass || !CreateWidgetNodeClass->IsChildOf(UK2Node::StaticClass()))
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), TEXT("Could not find K2Node_CreateWidget class."));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddCreateWidgetNode", "Blueprint Bridge: Add Create Widget Node"));
	Blueprint->Modify();
	Graph->Modify();

	UK2Node* Node = NewObject<UK2Node>(Graph, CreateWidgetNodeClass);
	Node->SetFlags(RF_Transactional);
	Graph->AddNode(Node, true, false);
	Node->CreateNewGuid();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	Node->AllocateDefaultPins();
	Node->PostPlacedNewNode();

	if (UEdGraphPin* ClassPin = FindPinByName(Node, TEXT("Class")))
	{
		ClassPin->DefaultObject = WidgetClass;
		ClassPin->DefaultValue = WidgetClass->GetPathName();
		Node->PinDefaultValueChanged(ClassPin);
	}
	ApplyNodePinDefaults(Node, Params);

	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params, const bool bMakeStruct)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString StructPath;
	if (!TryGetRequiredString(Params, TEXT("struct"), StructPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddMakeStructNode/AddBreakStructNode requires params.struct."));
	}

	UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
	if (!Struct)
	{
		return MakeBridgeError(Id, TEXT("StructNotFound"), FString::Printf(TEXT("Could not load struct '%s'."), *StructPath));
	}

	const int32 X = Params->GetIntegerField(TEXT("x"));
	const int32 Y = Params->GetIntegerField(TEXT("y"));
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddStructNode", "Blueprint Bridge: Add Struct Node"));
	Blueprint->Modify();
	Graph->Modify();

	UK2Node_StructOperation* Node = nullptr;
	if (bMakeStruct)
	{
		FGraphNodeCreator<UK2Node_MakeStruct> NodeCreator(*Graph);
		UK2Node_MakeStruct* MakeNode = NodeCreator.CreateNode();
		MakeNode->StructType = Struct;
		MakeNode->NodePosX = X;
		MakeNode->NodePosY = Y;
		NodeCreator.Finalize();
		Node = MakeNode;
	}
	else
	{
		FGraphNodeCreator<UK2Node_BreakStruct> NodeCreator(*Graph);
		UK2Node_BreakStruct* BreakNode = NodeCreator.CreateNode();
		BreakNode->StructType = Struct;
		BreakNode->NodePosX = X;
		BreakNode->NodePosY = Y;
		NodeCreator.Finalize();
		Node = BreakNode;
	}

	return FinishGraphEdit(Id, Blueprint, Graph, Node);
}

TSharedRef<FJsonObject> AddMakeStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddStructNode(Id, Params, true);
}

TSharedRef<FJsonObject> AddBreakStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddStructNode(Id, Params, false);
}

TSharedRef<FJsonObject> ConnectPins(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString FromNodeGuid;
	FString FromPinName;
	FString ToNodeGuid;
	FString ToPinName;
	if (!TryGetRequiredString(Params, TEXT("fromNode"), FromNodeGuid) || !TryGetRequiredString(Params, TEXT("fromPin"), FromPinName) ||
		!TryGetRequiredString(Params, TEXT("toNode"), ToNodeGuid) || !TryGetRequiredString(Params, TEXT("toPin"), ToPinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("ConnectPins requires params.fromNode, fromPin, toNode, and toPin."));
	}

	UEdGraphNode* FromNode = FindNodeByGuid(Graph, FromNodeGuid);
	UEdGraphNode* ToNode = FindNodeByGuid(Graph, ToNodeGuid);
	UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName, TOptional<EEdGraphPinDirection>(EGPD_Output));
	UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName, TOptional<EEdGraphPinDirection>(EGPD_Input));
	if (!FromPin || !ToPin)
	{
		return MakeBridgeError(Id, TEXT("PinNotFound"), TEXT("Could not find one or both pins."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "ConnectPins", "Blueprint Bridge: Connect Pins"));
	Blueprint->Modify();
	Graph->Modify();
	const bool bConnected = GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(FromPin, ToPin);
	if (!bConnected)
	{
		return MakeBridgeError(Id, TEXT("ConnectionFailed"), TEXT("Schema rejected the pin connection."));
	}

	return FinishGraphEdit(Id, Blueprint, nullptr);
}

TSharedRef<FJsonObject> MovePinLinksCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString FromNodeGuid;
	FString FromPinName;
	FString ToNodeGuid;
	FString ToPinName;
	if (!TryGetRequiredString(Params, TEXT("fromNode"), FromNodeGuid) || !TryGetRequiredString(Params, TEXT("fromPin"), FromPinName) ||
		!TryGetRequiredString(Params, TEXT("toNode"), ToNodeGuid) || !TryGetRequiredString(Params, TEXT("toPin"), ToPinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("MovePinLinks requires params.fromNode, fromPin, toNode, and toPin."));
	}

	UEdGraphPin* FromPin = FindPinByName(FindNodeByGuid(Graph, FromNodeGuid), FromPinName);
	UEdGraphPin* ToPin = FindPinByName(FindNodeByGuid(Graph, ToNodeGuid), ToPinName);
	if (!FromPin || !ToPin)
	{
		return MakeBridgeError(Id, TEXT("PinNotFound"), TEXT("Could not find one or both pins."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "MovePinLinks", "Blueprint Bridge: Move Pin Links"));
	Blueprint->Modify();
	Graph->Modify();
	MoveLinks(FromPin, ToPin);
	return FinishGraphEdit(Id, Blueprint, nullptr);
}

TSharedRef<FJsonObject> SetPinDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	FString PinName;
	FString Value;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid) || !TryGetRequiredString(Params, TEXT("pin"), PinName) || !TryGetRequiredString(Params, TEXT("value"), Value))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetPinDefault requires params.node, params.pin, and params.value."));
	}

	UEdGraphPin* Pin = FindPinByName(FindNodeByGuid(Graph, NodeGuid), PinName);
	if (!Pin)
	{
		return MakeBridgeError(Id, TEXT("PinNotFound"), FString::Printf(TEXT("Could not find pin '%s'."), *PinName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetPinDefault", "Blueprint Bridge: Set Pin Default"));
	Blueprint->Modify();
	Graph->Modify();
	Pin->DefaultValue = Value;
	return FinishGraphEdit(Id, Blueprint, nullptr);
}

TSharedRef<FJsonObject> BreakPinLinks(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	FString PinName;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid) || !TryGetRequiredString(Params, TEXT("pin"), PinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("BreakPinLinks requires params.node and params.pin."));
	}

	UEdGraphPin* Pin = FindPinByName(FindNodeByGuid(Graph, NodeGuid), PinName);
	if (!Pin)
	{
		return MakeBridgeError(Id, TEXT("PinNotFound"), FString::Printf(TEXT("Could not find pin '%s'."), *PinName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "BreakPinLinks", "Blueprint Bridge: Break Pin Links"));
	Blueprint->Modify();
	Graph->Modify();
	Pin->BreakAllPinLinks();
	return FinishGraphEdit(Id, Blueprint, nullptr);
}

TSharedRef<FJsonObject> CopyPinType(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString FromNodeGuid;
	FString FromPinName;
	FString ToNodeGuid;
	FString ToPinName;
	if (!TryGetRequiredString(Params, TEXT("fromNode"), FromNodeGuid) || !TryGetRequiredString(Params, TEXT("fromPin"), FromPinName) ||
		!TryGetRequiredString(Params, TEXT("toNode"), ToNodeGuid) || !TryGetRequiredString(Params, TEXT("toPin"), ToPinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CopyPinType requires params.fromNode, fromPin, toNode, and toPin."));
	}

	UEdGraphPin* FromPin = FindPinByName(FindNodeByGuid(Graph, FromNodeGuid), FromPinName);
	UEdGraphPin* ToPin = FindPinByName(FindNodeByGuid(Graph, ToNodeGuid), ToPinName);
	if (!FromPin || !ToPin)
	{
		return MakeBridgeError(Id, TEXT("PinNotFound"), TEXT("Could not find one or both pins."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CopyPinType", "Blueprint Bridge: Copy Pin Type"));
	Blueprint->Modify();
	Graph->Modify();
	ToPin->PinType = FromPin->PinType;
	return FinishGraphEdit(Id, Blueprint, ToPin->GetOwningNode());
}

TSharedRef<FJsonObject> DeleteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DeleteNode requires params.node."));
	}

	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
	if (!Node)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not find node '%s'."), *NodeGuid));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "DeleteNode", "Blueprint Bridge: Delete Node"));
	Blueprint->Modify();
	Graph->Modify();
	FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
	return FinishGraphEdit(Id, Blueprint, nullptr);
}

bool TryGetBlueprintVariableType(UBlueprint* Blueprint, const FName VariableName, FEdGraphPinType& OutPinType)
{
	if (!Blueprint)
	{
		return false;
	}

	for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
	{
		if (Variable.VarName == VariableName)
		{
			OutPinType = Variable.VarType;
			return true;
		}
	}

	if (Blueprint->SkeletonGeneratedClass)
	{
		if (const FProperty* Property = Blueprint->SkeletonGeneratedClass->FindPropertyByName(VariableName))
		{
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			Schema->ConvertPropertyToPinType(Property, OutPinType);
			return true;
		}
	}

	if (Blueprint->GeneratedClass)
	{
		if (const FProperty* Property = Blueprint->GeneratedClass->FindPropertyByName(VariableName))
		{
			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			Schema->ConvertPropertyToPinType(Property, OutPinType);
			return true;
		}
	}

	return false;
}

UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph);

UK2Node_FunctionResult* FindFunctionResultNode(UEdGraph* Graph)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
		{
			return ResultNode;
		}
	}
	return nullptr;
}

TSharedRef<FJsonObject> AddVariableGetterFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	FString VariableName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("function"), FunctionName) ||
		!TryGetRequiredString(Params, TEXT("variable"), VariableName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddVariableGetterFunction requires params.asset, params.function, and params.variable."));
	}

	FString OutputName = VariableName;
	Params->TryGetStringField(TEXT("output"), OutputName);

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	if (FindBlueprintGraph(Blueprint, FunctionName))
	{
		return MakeBridgeError(Id, TEXT("FunctionAlreadyExists"), FString::Printf(TEXT("Function graph '%s' already exists on '%s'."), *FunctionName, *AssetPath));
	}

	FEdGraphPinType VariablePinType;
	if (!TryGetBlueprintVariableType(Blueprint, *VariableName, VariablePinType))
	{
		return MakeBridgeError(Id, TEXT("VariableNotFound"), FString::Printf(TEXT("Could not find variable '%s' on '%s'."), *VariableName, *AssetPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddVariableGetterFunction", "Blueprint Bridge: Add Variable Getter Function"));
	Blueprint->Modify();

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, nullptr);
	Graph->Modify();

	UK2Node_FunctionResult* ResultNode = FindFunctionResultNode(Graph);
	if (!ResultNode)
	{
		FGraphNodeCreator<UK2Node_FunctionResult> ResultNodeCreator(*Graph);
		ResultNode = ResultNodeCreator.CreateNode();
		ResultNode->NodePosX = 320;
		ResultNode->NodePosY = 0;
		if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Graph))
		{
			ResultNode->FunctionReference = EntryNode->FunctionReference;
		}
		ResultNodeCreator.Finalize();
	}

	UEdGraphPin* ReturnPin = ResultNode->CreateUserDefinedPin(*OutputName, VariablePinType, EGPD_Input, false);
	if (!ReturnPin)
	{
		return MakeBridgeError(Id, TEXT("CreateReturnPinFailed"), FString::Printf(TEXT("Could not create return pin '%s'."), *OutputName));
	}

	UK2Node_VariableGet* GetNode = SpawnVariableGetNode(Blueprint, Graph, *VariableName, ResultNode->NodePosX - 280, ResultNode->NodePosY + 120);
	if (!GetNode)
	{
		return MakeBridgeError(Id, TEXT("CreateVariableGetFailed"), FString::Printf(TEXT("Could not create get node for variable '%s'."), *VariableName));
	}

	UEdGraphPin* VariableOutputPin = FindFirstDataOutputPin(GetNode);
	if (!VariableOutputPin || !GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(VariableOutputPin, ReturnPin))
	{
		return MakeBridgeError(Id, TEXT("ConnectReturnFailed"), FString::Printf(TEXT("Could not connect variable '%s' to return pin '%s'."), *VariableName, *OutputName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("function"), FunctionName);
	Result->SetStringField(TEXT("variable"), VariableName);
	Result->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
	Result->SetObjectField(TEXT("getterNode"), DescribeNode(GetNode));
	Result->SetObjectField(TEXT("resultNode"), DescribeNode(ResultNode));
	return MakeSuccess(Id, Result);
}

UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
		{
			return EntryNode;
		}
	}
	return nullptr;
}

void ApplyPinRefAndConstFlags(const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& OutPinType)
{
	// Optional flags for matching C++ delegate / function signatures exactly.
	// Required for BP functions bound to DECLARE_DYNAMIC_DELEGATE_* signatures with const-ref or by-ref params —
	// without them, the BP compiler inserts a CREATEDELEGATE_PROXYFUNCTION_N thunk that fails to propagate out-params.
	bool bByRef = false;
	if (Params->TryGetBoolField(TEXT("byRef"), bByRef))
	{
		OutPinType.bIsReference = bByRef;
	}
	bool bIsConst = false;
	if (Params->TryGetBoolField(TEXT("isConst"), bIsConst) || Params->TryGetBoolField(TEXT("const"), bIsConst))
	{
		OutPinType.bIsConst = bIsConst;
	}
}

bool TryMakePinTypeFromParams(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& OutPinType, FString& OutError)
{
	FString SourceVariable;
	if (Params->TryGetStringField(TEXT("sourceVariable"), SourceVariable))
	{
		if (!TryGetBlueprintVariableType(Blueprint, *SourceVariable, OutPinType))
		{
			OutError = FString::Printf(TEXT("Could not find source variable '%s'."), *SourceVariable);
			return false;
		}
		ApplyPinRefAndConstFlags(Params, OutPinType);
		return true;
	}

	FString Category;
	if (!TryGetRequiredString(Params, TEXT("category"), Category))
	{
		OutError = TEXT("Pin type requires params.category or params.sourceVariable.");
		return false;
	}

	OutPinType.PinCategory = NormalizePinCategory(Category);
	if (OutPinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	}
	FString SubCategory;
	if (Params->TryGetStringField(TEXT("subCategory"), SubCategory))
	{
		OutPinType.PinSubCategory = *SubCategory;
	}

	FString SubCategoryObjectPath;
	if (Params->TryGetStringField(TEXT("subCategoryObject"), SubCategoryObjectPath))
	{
		UObject* SubCategoryObject = StaticLoadObject(UObject::StaticClass(), nullptr, *SubCategoryObjectPath);
		if (!SubCategoryObject)
		{
			OutError = FString::Printf(TEXT("Could not load type object '%s'."), *SubCategoryObjectPath);
			return false;
		}
		OutPinType.PinSubCategoryObject = SubCategoryObject;
	}

	if (!ApplyPinContainerType(Params, OutPinType, OutError))
	{
		return false;
	}
	ApplyPinRefAndConstFlags(Params, OutPinType);
	return true;
}

TSharedRef<FJsonObject> DescribeNodeCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeNode requires params.node."));
	}

	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
	if (!Node)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not find node '%s'."), *NodeGuid));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("node"), DescribeNode(Node));
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> FindNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("FindNodes requires params.asset."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	FString GraphFilter;
	FString ClassFilter;
	FString TitleFilter;
	FString VariableFilter;
	Params->TryGetStringField(TEXT("graph"), GraphFilter);
	Params->TryGetStringField(TEXT("class"), ClassFilter);
	Params->TryGetStringField(TEXT("title"), TitleFilter);
	Params->TryGetStringField(TEXT("variable"), VariableFilter);

	TArray<UEdGraph*> Graphs;
	Graphs.Append(Blueprint->UbergraphPages);
	Graphs.Append(Blueprint->FunctionGraphs);

	TArray<TSharedPtr<FJsonValue>> Matches;
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph || (!GraphFilter.IsEmpty() && !Graph->GetName().Equals(GraphFilter, ESearchCase::IgnoreCase)))
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			if (!ClassFilter.IsEmpty() && !Node->GetClass()->GetPathName().Contains(ClassFilter))
			{
				continue;
			}
			if (!TitleFilter.IsEmpty() && !Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(TitleFilter))
			{
				continue;
			}
			if (!VariableFilter.IsEmpty())
			{
				const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
				if (!VariableNode || VariableNode->GetVarName() != *VariableFilter)
				{
					continue;
				}
			}

			TSharedRef<FJsonObject> Match = DescribeNode(Node);
			Match->SetStringField(TEXT("graph"), Graph->GetName());
			Matches.Add(MakeShared<FJsonValueObject>(Match));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("nodes"), Matches);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> AnalyzeGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString GraphName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("graph"), GraphName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AnalyzeGraph requires params.asset and params.graph."));
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

	TSet<UEdGraphNode*> ExecReachable;
	TArray<UEdGraphNode*> Pending;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		bool bHasExecInput = false;
		bool bHasExecOutput = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			bHasExecInput |= Pin->Direction == EGPD_Input;
			bHasExecOutput |= Pin->Direction == EGPD_Output;
		}

		if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_CustomEvent>() || (bHasExecOutput && !bHasExecInput))
		{
			ExecReachable.Add(Node);
			Pending.Add(Node);
		}
	}

	while (!Pending.IsEmpty())
	{
		UEdGraphNode* Node = Pending.Pop(EAllowShrinking::No);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (LinkedNode && !ExecReachable.Contains(LinkedNode))
				{
					ExecReachable.Add(LinkedNode);
					Pending.Add(LinkedNode);
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		bool bHasExecInput = false;
		bool bHasExecOutput = false;
		bool bDataConnected = false;
		bool bAnyConnection = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			const bool bIsExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
			bHasExecInput |= bIsExec && Pin->Direction == EGPD_Input;
			bHasExecOutput |= bIsExec && Pin->Direction == EGPD_Output;
			bAnyConnection |= Pin->LinkedTo.Num() > 0;
			bDataConnected |= !bIsExec && Pin->LinkedTo.Num() > 0;
		}

		const bool bExecReachable = ExecReachable.Contains(Node);
		FString Classification = TEXT("Reachable");
		if (!bExecReachable && bAnyConnection)
		{
			Classification = TEXT("OrphanedBranch");
		}
		else if (!bExecReachable && !bAnyConnection)
		{
			Classification = TEXT("Disconnected");
		}
		else if (bExecReachable && !bHasExecInput && !bHasExecOutput && bDataConnected)
		{
			Classification = TEXT("DataOnlyReachable");
		}

		TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		NodeJson->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		NodeJson->SetBoolField(TEXT("execReachable"), bExecReachable);
		NodeJson->SetBoolField(TEXT("hasExecInput"), bHasExecInput);
		NodeJson->SetBoolField(TEXT("hasExecOutput"), bHasExecOutput);
		NodeJson->SetBoolField(TEXT("dataConnected"), bDataConnected);
		NodeJson->SetStringField(TEXT("classification"), Classification);
		Nodes.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("graph"), Graph->GetName());
	Result->SetArrayField(TEXT("nodes"), Nodes);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> CreateFunctionGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CreateFunctionGraph requires params.asset and params.function."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}
	if (FindBlueprintGraph(Blueprint, FunctionName))
	{
		return MakeBridgeError(Id, TEXT("GraphAlreadyExists"), FString::Printf(TEXT("Graph '%s' already exists."), *FunctionName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CreateFunctionGraph", "Blueprint Bridge: Create Function Graph"));
	Blueprint->Modify();
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *FunctionName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, nullptr);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph"), Graph->GetName());
	return MakeSuccess(Id, Result);
}

void AddStringMapFromObject(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TMap<FName, FName>& OutMap)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, Object) || Object == nullptr || !Object->IsValid())
	{
		return;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Object)->Values)
	{
		FString NewName;
		if (Pair.Value.IsValid() && Pair.Value->TryGetString(NewName) && !Pair.Key.IsEmpty() && !NewName.IsEmpty())
		{
			OutMap.Add(*Pair.Key, *NewName);
		}
	}
}

int32 ApplyUserPinRenames(UEdGraph* Graph, const TMap<FName, FName>& Renames)
{
	if (!Graph || Renames.IsEmpty())
	{
		return 0;
	}

	int32 RenameCount = 0;
	TArray<UK2Node_EditablePinBase*> EditableNodes;
	Graph->GetNodesOfClass<UK2Node_EditablePinBase>(EditableNodes);
	for (UK2Node_EditablePinBase* EditableNode : EditableNodes)
	{
		if (!EditableNode)
		{
			continue;
		}

		for (const TPair<FName, FName>& Rename : Renames)
		{
			TSharedPtr<FUserPinInfo>* UserPinInfo = EditableNode->UserDefinedPins.FindByPredicate([&Rename](const TSharedPtr<FUserPinInfo>& Candidate)
			{
				return Candidate.IsValid() && Candidate->PinName == Rename.Key;
			});
			if (!UserPinInfo)
			{
				continue;
			}

			EditableNode->Modify();
			const ERenamePinResult RenameResult = EditableNode->RenameUserDefinedPin(Rename.Key, Rename.Value, false);
			if (RenameResult != ERenamePinResult_NameCollision)
			{
				(*UserPinInfo)->PinName = Rename.Value;
				++RenameCount;
			}
		}
		EditableNode->ReconstructNode();
	}
	return RenameCount;
}

int32 ApplyVariableReferenceRenames(UBlueprint* Blueprint, UEdGraph* Graph, const TMap<FName, FName>& Renames)
{
	if (!Blueprint || !Graph || Renames.IsEmpty())
	{
		return 0;
	}

	int32 RenameCount = 0;
	TArray<UK2Node_Variable*> VariableNodes;
	Graph->GetNodesOfClass<UK2Node_Variable>(VariableNodes);
	for (UK2Node_Variable* VariableNode : VariableNodes)
	{
		if (!VariableNode)
		{
			continue;
		}

		const FName* NewName = Renames.Find(VariableNode->GetVarName());
		if (!NewName)
		{
			continue;
		}

		VariableNode->Modify();
		if (VariableNode->VariableReference.IsLocalScope())
		{
			VariableNode->VariableReference.SetLocalMember(*NewName, Graph->GetName(), VariableNode->VariableReference.GetMemberGuid());
		}
		else if (VariableNode->VariableReference.IsSelfContext())
		{
			const FGuid VariableGuid = FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, *NewName);
			VariableNode->VariableReference.SetSelfMember(*NewName, VariableGuid);
		}
		else
		{
			continue;
		}
		VariableNode->ReconstructNode();
		++RenameCount;
	}
	return RenameCount;
}

struct FRenameReport
{
	int32 AppliedCount = 0;
	TSet<FName> MatchedKeys;
	TArray<TSharedPtr<FJsonValue>> Applied;
	TArray<TSharedPtr<FJsonValue>> Unmatched;
	TArray<TSharedPtr<FJsonValue>> Collisions;
	TArray<TSharedPtr<FJsonValue>> MissingTargets;
};

TSharedRef<FJsonObject> MakeRenameJson(const FName From, const FName To, UEdGraphNode* Node = nullptr)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("from"), From.ToString());
	Result->SetStringField(TEXT("to"), To.ToString());
	if (Node)
	{
		Result->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	return Result;
}

TSharedRef<FJsonObject> MakeRenameReportJson(const FRenameReport& Report, const TMap<FName, FName>& RequestedRenames)
{
	FRenameReport ReportWithUnmatched = Report;
	for (const TPair<FName, FName>& Rename : RequestedRenames)
	{
		if (!ReportWithUnmatched.MatchedKeys.Contains(Rename.Key))
		{
			ReportWithUnmatched.Unmatched.Add(MakeShared<FJsonValueObject>(MakeRenameJson(Rename.Key, Rename.Value)));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), ReportWithUnmatched.AppliedCount);
	Result->SetArrayField(TEXT("applied"), ReportWithUnmatched.Applied);
	Result->SetArrayField(TEXT("unmatched"), ReportWithUnmatched.Unmatched);
	Result->SetArrayField(TEXT("collisions"), ReportWithUnmatched.Collisions);
	Result->SetArrayField(TEXT("missingTargets"), ReportWithUnmatched.MissingTargets);
	return Result;
}

FRenameReport ApplyUserPinRenamesWithReport(UEdGraph* Graph, const TMap<FName, FName>& Renames)
{
	FRenameReport Report;
	if (!Graph || Renames.IsEmpty())
	{
		return Report;
	}

	TArray<UK2Node_EditablePinBase*> EditableNodes;
	Graph->GetNodesOfClass<UK2Node_EditablePinBase>(EditableNodes);
	for (UK2Node_EditablePinBase* EditableNode : EditableNodes)
	{
		if (!EditableNode)
		{
			continue;
		}

		bool bChanged = false;
		for (const TPair<FName, FName>& Rename : Renames)
		{
			TSharedPtr<FUserPinInfo>* UserPinInfo = EditableNode->UserDefinedPins.FindByPredicate([&Rename](const TSharedPtr<FUserPinInfo>& Candidate)
			{
				return Candidate.IsValid() && Candidate->PinName == Rename.Key;
			});
			if (!UserPinInfo)
			{
				continue;
			}

			Report.MatchedKeys.Add(Rename.Key);
			const bool bTargetExists = EditableNode->UserDefinedPins.ContainsByPredicate([&Rename](const TSharedPtr<FUserPinInfo>& Candidate)
			{
				return Candidate.IsValid() && Candidate->PinName == Rename.Value;
			});
			if (bTargetExists)
			{
				Report.Collisions.Add(MakeShared<FJsonValueObject>(MakeRenameJson(Rename.Key, Rename.Value, EditableNode)));
				continue;
			}

			EditableNode->Modify();
			const ERenamePinResult RenameResult = EditableNode->RenameUserDefinedPin(Rename.Key, Rename.Value, false);
			if (RenameResult == ERenamePinResult_NameCollision)
			{
				Report.Collisions.Add(MakeShared<FJsonValueObject>(MakeRenameJson(Rename.Key, Rename.Value, EditableNode)));
				continue;
			}

			(*UserPinInfo)->PinName = Rename.Value;
			++Report.AppliedCount;
			bChanged = true;
			Report.Applied.Add(MakeShared<FJsonValueObject>(MakeRenameJson(Rename.Key, Rename.Value, EditableNode)));
		}
		if (bChanged)
		{
			EditableNode->ReconstructNode();
		}
	}
	return Report;
}

FRenameReport ApplyVariableReferenceRenamesWithReport(UBlueprint* Blueprint, UEdGraph* Graph, const TMap<FName, FName>& Renames)
{
	FRenameReport Report;
	if (!Blueprint || !Graph || Renames.IsEmpty())
	{
		return Report;
	}

	if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(Graph))
	{
		for (FBPVariableDescription& LocalVariable : EntryNode->LocalVariables)
		{
			const FName* NewName = Renames.Find(LocalVariable.VarName);
			if (!NewName)
			{
				continue;
			}

			Report.MatchedKeys.Add(LocalVariable.VarName);
			const bool bCollision = EntryNode->LocalVariables.ContainsByPredicate([NewName](const FBPVariableDescription& Candidate)
			{
				return Candidate.VarName == *NewName;
			});
			if (bCollision)
			{
				Report.Collisions.Add(MakeShared<FJsonValueObject>(MakeRenameJson(LocalVariable.VarName, *NewName, EntryNode)));
				continue;
			}

			const FName OldName = LocalVariable.VarName;
			EntryNode->Modify();
			LocalVariable.VarName = *NewName;
			++Report.AppliedCount;
			Report.Applied.Add(MakeShared<FJsonValueObject>(MakeRenameJson(OldName, *NewName, EntryNode)));
		}
	}

	TArray<UK2Node_Variable*> VariableNodes;
	Graph->GetNodesOfClass<UK2Node_Variable>(VariableNodes);
	for (UK2Node_Variable* VariableNode : VariableNodes)
	{
		if (!VariableNode)
		{
			continue;
		}

		const FName OldName = VariableNode->GetVarName();
		const FName* NewName = Renames.Find(OldName);
		if (!NewName)
		{
			continue;
		}

		Report.MatchedKeys.Add(OldName);
		VariableNode->Modify();
		if (VariableNode->VariableReference.IsLocalScope())
		{
			VariableNode->VariableReference.SetLocalMember(*NewName, Graph->GetName(), VariableNode->VariableReference.GetMemberGuid());
		}
		else if (VariableNode->VariableReference.IsSelfContext())
		{
			const FGuid VariableGuid = FBlueprintEditorUtils::FindMemberVariableGuidByName(Blueprint, *NewName);
			if (!VariableGuid.IsValid())
			{
				Report.MissingTargets.Add(MakeShared<FJsonValueObject>(MakeRenameJson(OldName, *NewName, VariableNode)));
				continue;
			}
			VariableNode->VariableReference.SetSelfMember(*NewName, VariableGuid);
		}
		else
		{
			continue;
		}
		VariableNode->ReconstructNode();
		++Report.AppliedCount;
		Report.Applied.Add(MakeShared<FJsonValueObject>(MakeRenameJson(OldName, *NewName, VariableNode)));
	}
	return Report;
}

bool RenameReportHasStrictFailure(const FRenameReport& Report, const TMap<FName, FName>& RequestedRenames)
{
	if (Report.Collisions.Num() > 0 || Report.MissingTargets.Num() > 0)
	{
		return true;
	}

	for (const TPair<FName, FName>& Rename : RequestedRenames)
	{
		if (!Report.MatchedKeys.Contains(Rename.Key))
		{
			return true;
		}
	}
	return false;
}

TSharedRef<FJsonObject> DuplicateFunctionGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString SourceGraphName;
	FString NewGraphName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) ||
		!TryGetRequiredString(Params, TEXT("sourceGraph"), SourceGraphName) ||
		!TryGetRequiredString(Params, TEXT("newGraph"), NewGraphName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DuplicateFunctionGraph requires params.asset, params.sourceGraph, and params.newGraph."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	UEdGraph* SourceGraph = FindBlueprintGraph(Blueprint, SourceGraphName);
	if (!SourceGraph || !Blueprint->FunctionGraphs.Contains(SourceGraph))
	{
		return MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Could not find function graph '%s' on '%s'."), *SourceGraphName, *AssetPath));
	}
	if (FindBlueprintGraph(Blueprint, NewGraphName))
	{
		return MakeBridgeError(Id, TEXT("GraphAlreadyExists"), FString::Printf(TEXT("Graph '%s' already exists."), *NewGraphName));
	}
	if (!SourceGraph->GetSchema() || !SourceGraph->GetSchema()->CanDuplicateGraph(SourceGraph))
	{
		return MakeBridgeError(Id, TEXT("DuplicateGraphUnsupported"), FString::Printf(TEXT("Graph '%s' cannot be duplicated by its schema."), *SourceGraphName));
	}

	TMap<FName, FName> PinRenames;
	TMap<FName, FName> VariableRenames;
	AddStringMapFromObject(Params, TEXT("renames"), PinRenames);
	AddStringMapFromObject(Params, TEXT("renames"), VariableRenames);
	AddStringMapFromObject(Params, TEXT("pinRenames"), PinRenames);
	AddStringMapFromObject(Params, TEXT("variableRenames"), VariableRenames);

	bool bStrictRenames = false;
	bool bCompile = false;
	Params->TryGetBoolField(TEXT("strictRenames"), bStrictRenames);
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "DuplicateFunctionGraph", "Blueprint Bridge: Duplicate Function Graph"));
	Blueprint->Modify();
	UEdGraph* DuplicatedGraph = SourceGraph->GetSchema()->DuplicateGraph(SourceGraph);
	if (!DuplicatedGraph)
	{
		return MakeBridgeError(Id, TEXT("DuplicateGraphFailed"), FString::Printf(TEXT("Could not duplicate function graph '%s'."), *SourceGraphName));
	}

	DuplicatedGraph->Modify();
	for (UEdGraphNode* Node : DuplicatedGraph->Nodes)
	{
		if (Node)
		{
			Node->CreateNewGuid();
		}
	}

	Blueprint->FunctionGraphs.Add(DuplicatedGraph);
	FBlueprintEditorUtils::RenameGraph(DuplicatedGraph, NewGraphName);
	const FRenameReport UserPinRenameReport = ApplyUserPinRenamesWithReport(DuplicatedGraph, PinRenames);
	const FRenameReport VariableRenameReport = ApplyVariableReferenceRenamesWithReport(Blueprint, DuplicatedGraph, VariableRenames);

	if (bStrictRenames && (RenameReportHasStrictFailure(UserPinRenameReport, PinRenames) || RenameReportHasStrictFailure(VariableRenameReport, VariableRenames)))
	{
		Blueprint->FunctionGraphs.Remove(DuplicatedGraph);
		DuplicatedGraph->MarkAsGarbage();
		Transaction.Cancel();
		return MakeBridgeError(Id, TEXT("StrictRenameFailed"), FString::Printf(TEXT("One or more renames could not be applied while duplicating '%s'."), *SourceGraphName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("sourceGraph"), SourceGraphName);
	Result->SetStringField(TEXT("graph"), DuplicatedGraph->GetName());
	Result->SetNumberField(TEXT("pinRenameCount"), UserPinRenameReport.AppliedCount);
	Result->SetNumberField(TEXT("variableRenameCount"), VariableRenameReport.AppliedCount);
	Result->SetObjectField(TEXT("pinRenames"), MakeRenameReportJson(UserPinRenameReport, PinRenames));
	Result->SetObjectField(TEXT("variableRenames"), MakeRenameReportJson(VariableRenameReport, VariableRenames));
	if (bCompile)
	{
		Result->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
	}
	if (UK2Node_FunctionEntry* EntryNode = FindFunctionEntryNode(DuplicatedGraph))
	{
		Result->SetObjectField(TEXT("entryNode"), DescribeNode(EntryNode));
	}
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> CreateEventGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString GraphName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("graph"), GraphName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CreateEventGraph requires params.asset and params.graph."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}
	if (FindBlueprintGraph(Blueprint, GraphName))
	{
		return MakeBridgeError(Id, TEXT("GraphAlreadyExists"), FString::Printf(TEXT("Graph '%s' already exists."), *GraphName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CreateEventGraph", "Blueprint Bridge: Create Event Graph"));
	Blueprint->Modify();
	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, *GraphName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddUbergraphPage(Blueprint, Graph);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph"), Graph->GetName());
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> AddFunctionPin(const FString& Id, const TSharedPtr<FJsonObject>& Params, const bool bOutput)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString PinName;
	if (!TryGetRequiredString(Params, TEXT("name"), PinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddFunctionInput/Output requires params.name."));
	}

	FString TypeError;
	FEdGraphPinType PinType;
	if (!TryMakePinTypeFromParams(Blueprint, Params, PinType, TypeError))
	{
		return MakeBridgeError(Id, TEXT("InvalidPinType"), TypeError);
	}

	UK2Node_EditablePinBase* Terminator = bOutput ? Cast<UK2Node_EditablePinBase>(FindFunctionResultNode(Graph)) : Cast<UK2Node_EditablePinBase>(FindFunctionEntryNode(Graph));

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddFunctionPin", "Blueprint Bridge: Add Function Pin"));
	Blueprint->Modify();
	Graph->Modify();
	if (bOutput && !Terminator)
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
		Terminator = ResultNode;
	}
	if (!Terminator)
	{
		return MakeBridgeError(Id, TEXT("TerminatorNotFound"), TEXT("Could not find function entry/result node."));
	}
	UEdGraphPin* Pin = Terminator->CreateUserDefinedPin(*PinName, PinType, bOutput ? EGPD_Input : EGPD_Output, false);
	if (!Pin)
	{
		return MakeBridgeError(Id, TEXT("CreatePinFailed"), FString::Printf(TEXT("Could not create function pin '%s'."), *PinName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return FinishGraphEdit(Id, Blueprint, Cast<UEdGraphNode>(Terminator));
}

TSharedRef<FJsonObject> AddFunctionInput(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddFunctionPin(Id, Params, false);
}

TSharedRef<FJsonObject> AddFunctionOutput(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddFunctionPin(Id, Params, true);
}

TSharedRef<FJsonObject> RenameCustomEvent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	FString NewName;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid) || !TryGetRequiredString(Params, TEXT("newName"), NewName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("RenameCustomEvent requires params.node and params.newName."));
	}

	UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(FindNodeByGuid(Graph, NodeGuid));
	if (!EventNode)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not find custom event node '%s'."), *NodeGuid));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "RenameCustomEvent", "Blueprint Bridge: Rename Custom Event"));
	Blueprint->Modify();
	Graph->Modify();
	EventNode->Modify();
	EventNode->CustomFunctionName = *NewName;
	EventNode->RenameCustomEventCloseToName();
	EventNode->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return FinishGraphEdit(Id, Blueprint, Graph, EventNode);
}

TSharedRef<FJsonObject> EditUserDefinedPin(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	FString PinName;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid) || !TryGetRequiredString(Params, TEXT("pin"), PinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("EditUserDefinedPin requires params.node and params.pin."));
	}

	UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(FindNodeByGuid(Graph, NodeGuid));
	if (!EditableNode)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not find editable pin node '%s'."), *NodeGuid));
	}

	TSharedPtr<FUserPinInfo>* UserPinInfo = EditableNode->UserDefinedPins.FindByPredicate([&PinName](const TSharedPtr<FUserPinInfo>& Candidate)
	{
		return Candidate.IsValid() && Candidate->PinName == *PinName;
	});
	if (!UserPinInfo)
	{
		return MakeBridgeError(Id, TEXT("PinNotFound"), FString::Printf(TEXT("Could not find user-defined pin '%s'."), *PinName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "EditUserDefinedPin", "Blueprint Bridge: Edit User Defined Pin"));
	Blueprint->Modify();
	Graph->Modify();
	EditableNode->Modify();

	FName FinalPinName = (*UserPinInfo)->PinName;
	FString NewPinName;
	if (Params->TryGetStringField(TEXT("newName"), NewPinName) && !NewPinName.IsEmpty() && NewPinName != PinName)
	{
		const FName NewPinFName(*NewPinName);
		const ERenamePinResult RenameResult = EditableNode->RenameUserDefinedPin(FinalPinName, NewPinFName, false);
		if (RenameResult == ERenamePinResult_NameCollision)
		{
			return MakeBridgeError(Id, TEXT("RenamePinFailed"), FString::Printf(TEXT("Could not rename pin '%s' to '%s'."), *PinName, *NewPinName));
		}
		(*UserPinInfo)->PinName = NewPinFName;
		FinalPinName = NewPinFName;
	}

	FString TypeError;
	FEdGraphPinType PinType;
	if (Params->HasField(TEXT("sourceVariable")) || Params->HasField(TEXT("category")))
	{
		if (!TryMakePinTypeFromParams(Blueprint, Params, PinType, TypeError))
		{
			return MakeBridgeError(Id, TEXT("InvalidPinType"), TypeError);
		}

		(*UserPinInfo)->PinType = PinType;
		(*UserPinInfo)->PinDefaultValue.Reset();
	}

	EditableNode->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return FinishGraphEdit(Id, Blueprint, Graph, EditableNode);
}

TSharedRef<FJsonObject> SetUserDefinedPinFlags(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	FString PinName;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid) || !TryGetRequiredString(Params, TEXT("pin"), PinName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetUserDefinedPinFlags requires params.node and params.pin."));
	}

	UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(FindNodeByGuid(Graph, NodeGuid));
	if (!EditableNode)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not find editable pin node '%s'."), *NodeGuid));
	}

	TSharedPtr<FUserPinInfo>* UserPinInfo = EditableNode->UserDefinedPins.FindByPredicate([&PinName](const TSharedPtr<FUserPinInfo>& Candidate)
	{
		return Candidate.IsValid() && Candidate->PinName == *PinName;
	});
	if (!UserPinInfo)
	{
		return MakeBridgeError(Id, TEXT("PinNotFound"), FString::Printf(TEXT("Could not find user-defined pin '%s'."), *PinName));
	}

	if (!Params->HasField(TEXT("byRef")) && !Params->HasField(TEXT("isConst")) && !Params->HasField(TEXT("const")))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetUserDefinedPinFlags requires at least one of params.byRef or params.isConst."));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetUserDefinedPinFlags", "Blueprint Bridge: Set User Defined Pin Flags"));
	Blueprint->Modify();
	Graph->Modify();
	EditableNode->Modify();

	ApplyPinRefAndConstFlags(Params, (*UserPinInfo)->PinType);
	EditableNode->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return FinishGraphEdit(Id, Blueprint, Graph, EditableNode);
}

TSharedRef<FJsonObject> DeleteGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString GraphName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("graph"), GraphName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DeleteGraph requires params.asset and params.graph."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	UEdGraph* Graph = FindBlueprintGraph(Blueprint, GraphName);
	if (!Blueprint || !Graph)
	{
		return MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Could not find graph '%s'."), *GraphName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "DeleteGraph", "Blueprint Bridge: Delete Graph"));
	Blueprint->Modify();
	FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);
	return MakeSuccessMessage(Id, TEXT("GraphDeleted"));
}

TSharedRef<FJsonObject> RenameGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString GraphName;
	FString NewName;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("graph"), GraphName) || !TryGetRequiredString(Params, TEXT("newName"), NewName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("RenameGraph requires params.asset, params.graph, and params.newName."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	UEdGraph* Graph = FindBlueprintGraph(Blueprint, GraphName);
	if (!Blueprint || !Graph)
	{
		return MakeBridgeError(Id, TEXT("GraphNotFound"), FString::Printf(TEXT("Could not find graph '%s'."), *GraphName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "RenameGraph", "Blueprint Bridge: Rename Graph"));
	Blueprint->Modify();
	Graph->Modify();
	FBlueprintEditorUtils::RenameGraph(Graph, NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("GraphRenamed"));
}

TSharedRef<FJsonObject> SetNodePosition(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString NodeGuid;
	if (!TryGetRequiredString(Params, TEXT("node"), NodeGuid))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SetNodePosition requires params.node."));
	}

	UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
	if (!Node)
	{
		return MakeBridgeError(Id, TEXT("NodeNotFound"), FString::Printf(TEXT("Could not find node '%s'."), *NodeGuid));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "SetNodePosition", "Blueprint Bridge: Set Node Position"));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddCommentNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString Text;
	Params->TryGetStringField(TEXT("text"), Text);
	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddCommentNode", "Blueprint Bridge: Add Comment Node"));
	Blueprint->Modify();
	Graph->Modify();
	UEdGraphNode_Comment* Node = NewObject<UEdGraphNode_Comment>(Graph);
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	Node->NodeWidth = Params->HasField(TEXT("width")) ? Params->GetIntegerField(TEXT("width")) : 400;
	Node->NodeHeight = Params->HasField(TEXT("height")) ? Params->GetIntegerField(TEXT("height")) : 200;
	Node->NodeComment = Text;
	Graph->AddNode(Node, true, false);
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddRerouteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddRerouteNode", "Blueprint Bridge: Add Reroute Node"));
	Blueprint->Modify();
	Graph->Modify();
	FGraphNodeCreator<UK2Node_Knot> NodeCreator(*Graph);
	UK2Node_Knot* Node = NodeCreator.CreateNode();
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddSequenceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddSequenceNode", "Blueprint Bridge: Add Sequence Node"));
	Blueprint->Modify();
	Graph->Modify();
	FGraphNodeCreator<UK2Node_ExecutionSequence> NodeCreator(*Graph);
	UK2Node_ExecutionSequence* Node = NodeCreator.CreateNode();
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	NodeCreator.Finalize();
	int32 ExtraOutputs = 0;
	Params->TryGetNumberField(TEXT("extraOutputs"), ExtraOutputs);
	for (int32 Index = 0; Index < ExtraOutputs; ++Index)
	{
		Node->AddInputPin();
	}
	return FinishGraphEdit(Id, Blueprint, Node);
}

TSharedRef<FJsonObject> AddEnumSwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	if (!TryLoadGraphForEdit(Id, Params, Blueprint, Graph, Error))
	{
		return Error;
	}

	FString EnumPath;
	if (!TryGetRequiredString(Params, TEXT("enum"), EnumPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddEnumSwitchNode requires params.enum."));
	}

	UEnum* Enum = LoadObject<UEnum>(nullptr, *EnumPath);
	if (!Enum)
	{
		return MakeBridgeError(Id, TEXT("EnumNotFound"), FString::Printf(TEXT("Could not load enum '%s'."), *EnumPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddEnumSwitchNode", "Blueprint Bridge: Add Enum Switch Node"));
	Blueprint->Modify();
	Graph->Modify();
	FGraphNodeCreator<UK2Node_SwitchEnum> NodeCreator(*Graph);
	UK2Node_SwitchEnum* Node = NodeCreator.CreateNode();
	Node->SetEnum(Enum);
	Node->NodePosX = Params->GetIntegerField(TEXT("x"));
	Node->NodePosY = Params->GetIntegerField(TEXT("y"));
	NodeCreator.Finalize();
	return FinishGraphEdit(Id, Blueprint, Node);
}


USCS_Node* FindSCSNodeByName(USimpleConstructionScript* SCS, const FString& ComponentName)
{
	if (!SCS)
	{
		return nullptr;
	}

	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			return Node;
		}
	}

	return nullptr;
}

USCS_Node* FindSCSParentNode(USimpleConstructionScript* SCS, USCS_Node* ChildNode)
{
	if (!SCS || !ChildNode)
	{
		return nullptr;
	}

	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetChildNodes().Contains(ChildNode))
		{
			return Node;
		}
	}

	return nullptr;
}
} // namespace BlueprintBridge
