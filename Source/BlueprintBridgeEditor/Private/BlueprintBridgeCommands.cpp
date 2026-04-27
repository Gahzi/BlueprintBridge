// Copyright Odyssey Interactive. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "FileHelpers.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPluginManager.h"
#include "JsonObjectConverter.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetMathLibrary.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SourceControlHelpers.h"
#include "EdGraph/EdGraphNodeUtils.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintBridge, Log, All);

namespace BlueprintBridge
{
static constexpr TCHAR SettingsSection[] = TEXT("/Script/BlueprintBridgeEditor.BlueprintBridge");

static FString GetConfiguredPipeName()
{
	FString PipeName = TEXT("BlueprintBridge");
	GConfig->GetString(SettingsSection, TEXT("PipeName"), PipeName, GEditorPerProjectIni);
	PipeName = PipeName.TrimStartAndEnd();
	return PipeName.IsEmpty() ? TEXT("BlueprintBridge") : PipeName;
}

FString GetPipeNamePath()
{
	return FString::Printf(TEXT("\\\\.\\pipe\\%s"), *GetConfiguredPipeName());
}

static bool IsAuthRequired()
{
	bool bRequireAuthToken = false;
	GConfig->GetBool(SettingsSection, TEXT("bRequireAuthToken"), bRequireAuthToken, GEditorPerProjectIni);
	return bRequireAuthToken;
}

static FString GetConfiguredAuthToken()
{
	FString AuthToken;
	GConfig->GetString(SettingsSection, TEXT("AuthToken"), AuthToken, GEditorPerProjectIni);
	return AuthToken;
}

static bool ValidateAuthToken(const TSharedPtr<FJsonObject>& Request)
{
	if (!IsAuthRequired())
	{
		return true;
	}

	if (!Request.IsValid() || !Request->HasTypedField<EJson::String>(TEXT("authToken")))
	{
		return false;
	}

	return Request->GetStringField(TEXT("authToken")) == GetConfiguredAuthToken();
}

static FString JsonToString(const TSharedRef<FJsonObject>& JsonObject)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonObject, Writer);
	return Output;
}

static TSharedRef<FJsonObject> MakeBridgeError(const FString& Id, const FString& Code, const FString& Message)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), false);

	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	Response->SetObjectField(TEXT("error"), Error);
	return Response;
}

static FString NormalizeBlueprintObjectPath(const FString& AssetPath)
{
	if (AssetPath.Contains(TEXT(".")))
	{
		return AssetPath;
	}

	FString AssetName;
	if (!AssetPath.Split(TEXT("/"), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return AssetPath;
	}

	return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

static TSharedRef<FJsonObject> MakeSuccess(const FString& Id, const TSharedPtr<FJsonObject>& Result)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), true);
	if (Result.IsValid())
	{
		Response->SetObjectField(TEXT("result"), Result.ToSharedRef());
	}
	return Response;
}

static TSharedRef<FJsonObject> MakeSuccessMessage(const FString& Id, const FString& Message)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("result"), Message);
	return Response;
}

static UBlueprint* LoadBlueprint(const FString& AssetPath)
{
	return Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath)));
}

static bool TryGetRequiredString(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FString& OutValue)
{
	if (!Params.IsValid() || !Params->HasTypedField<EJson::String>(FieldName))
	{
		return false;
	}

	OutValue = Params->GetStringField(FieldName);
	return true;
}

static FString GetBlueprintStatusString(const EBlueprintStatus Status);
static FName NormalizePinCategory(const FString& Category);

static TSharedRef<FJsonObject> DescribeBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

	return MakeSuccess(Id, Result);
}

static UEdGraph* FindBlueprintGraph(UBlueprint* Blueprint, const FString& GraphName)
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

static TSharedRef<FJsonObject> DescribeNode(UEdGraphNode* Node)
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

static TSharedRef<FJsonObject> DescribeGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> FindVariableReferences(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static UEdGraphPin* FindPinByNameAndDirection(UEdGraphNode* Node, const FName PinName, const EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName == PinName && Pin->Direction == Direction)
		{
			return Pin;
		}
	}
	return nullptr;
}

static UEdGraphPin* FindFirstDataOutputPin(UEdGraphNode* Node)
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

static UK2Node_CallFunction* SpawnFunctionCallNode(UEdGraph* Graph, UFunction* Function, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_CallFunction> NodeCreator(*Graph);
	UK2Node_CallFunction* Node = NodeCreator.CreateNode();
	Node->SetFromFunction(Function);
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

static UK2Node_IfThenElse* SpawnBranchNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_IfThenElse> NodeCreator(*Graph);
	UK2Node_IfThenElse* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

static UK2Node_EnumEquality* SpawnEnumEqualityNode(UEdGraph* Graph, const int32 X, const int32 Y)
{
	FGraphNodeCreator<UK2Node_EnumEquality> NodeCreator(*Graph);
	UK2Node_EnumEquality* Node = NodeCreator.CreateNode();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	NodeCreator.Finalize();
	return Node;
}

static UK2Node_VariableGet* SpawnVariableGetNode(UBlueprint* Blueprint, UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y)
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	return Schema->SpawnVariableGetNode(FVector2D(X, Y), Graph, VariableName, Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass);
}

static UK2Node_VariableSet* SpawnVariableSetNode(UBlueprint* Blueprint, UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y)
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	return Schema->SpawnVariableSetNode(FVector2D(X, Y), Graph, VariableName, Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass);
}

static UEdGraphPin* SpawnEnabledComparison(UBlueprint* Blueprint, UEdGraph* Graph, const int32 X, const int32 Y, const FString& EnabledValue, TArray<UEdGraphNode*>& OutCreatedNodes)
{
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	UK2Node_VariableGet* StateGet = SpawnVariableGetNode(Blueprint, Graph, TEXT("WeakpointState"), X, Y + 80);
	if (!StateGet)
	{
		return nullptr;
	}
	OutCreatedNodes.Add(StateGet);

	UFunction* EqualFunction = UKismetMathLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, EqualEqual_ByteByte));
	UK2Node_CallFunction* EqualNode = SpawnFunctionCallNode(Graph, EqualFunction, X + 210, Y + 60);
	OutCreatedNodes.Add(EqualNode);

	UEdGraphPin* StateOutputPin = FindFirstDataOutputPin(StateGet);
	UEdGraphPin* APin = EqualNode->FindPin(TEXT("A"));
	UEdGraphPin* BPin = EqualNode->FindPin(TEXT("B"));
	UEdGraphPin* ReturnPin = EqualNode->GetReturnValuePin();
	if (!StateOutputPin || !APin || !BPin || !ReturnPin)
	{
		return nullptr;
	}

	Schema->TryCreateConnection(StateOutputPin, APin);
	BPin->PinType = StateOutputPin->PinType;
	BPin->DefaultValue = EnabledValue;
	return ReturnPin;
}

static void MoveLinks(UEdGraphPin* FromPin, UEdGraphPin* ToPin)
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

static void SetEnumPinDefault(UEdGraphPin* Pin, const FString& Value)
{
	if (Pin)
	{
		Pin->DefaultValue = Value;
	}
}

static TSharedRef<FJsonObject> MigrateWeakpointEnabledBoolToState(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("MigrateWeakpointEnabledBoolToState requires params.asset."));
	}

	FString EnabledValue = TEXT("Enabled");
	FString DisabledValue = TEXT("Disabled");
	Params->TryGetStringField(TEXT("enabledValue"), EnabledValue);
	Params->TryGetStringField(TEXT("disabledValue"), DisabledValue);

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "MigrateWeakpointEnabledBoolToState", "Blueprint Bridge: Migrate Weakpoint Enabled Bool To State"));
	Blueprint->Modify();
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

	TArray<UEdGraph*> Graphs;
	Graphs.Append(Blueprint->UbergraphPages);
	Graphs.Append(Blueprint->FunctionGraphs);

	int32 ReplacedGetCount = 0;
	int32 ReplacedSetCount = 0;
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}

		Graph->Modify();
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;
		for (UEdGraphNode* Node : Nodes)
		{
			UK2Node_VariableGet* BoolGet = Cast<UK2Node_VariableGet>(Node);
			if (!BoolGet || BoolGet->GetVarName() != TEXT("bIsEnabled"))
			{
				continue;
			}

			UEdGraphPin* BoolOutputPin = FindFirstDataOutputPin(BoolGet);
			TArray<UEdGraphNode*> CreatedNodes;
			UEdGraphPin* EnabledResultPin = SpawnEnabledComparison(Blueprint, Graph, BoolGet->NodePosX, BoolGet->NodePosY, EnabledValue, CreatedNodes);
			if (!BoolOutputPin || !EnabledResultPin)
			{
				continue;
			}

			MoveLinks(BoolOutputPin, EnabledResultPin);
			FBlueprintEditorUtils::RemoveNode(Blueprint, BoolGet, true);
			++ReplacedGetCount;
		}
	}

	UEdGraph* EventGraph = FindBlueprintGraph(Blueprint, TEXT("EventGraph"));
	if (EventGraph)
	{
		TArray<UEdGraphNode*> Nodes = EventGraph->Nodes;
		for (UEdGraphNode* Node : Nodes)
		{
			UK2Node_VariableSet* BoolSet = Cast<UK2Node_VariableSet>(Node);
			if (!BoolSet || BoolSet->GetVarName() != TEXT("bIsEnabled"))
			{
				continue;
			}

			UEdGraphPin* ExecIn = BoolSet->GetExecPin();
			UEdGraphPin* ThenOut = BoolSet->GetThenPin();
			UEdGraphPin* ValueIn = FindPinByNameAndDirection(BoolSet, TEXT("bIsEnabled"), EGPD_Input);
			UEdGraphPin* OutputGet = FindPinByNameAndDirection(BoolSet, TEXT("Output_Get"), EGPD_Output);
			if (!ExecIn || !ThenOut || !ValueIn || !OutputGet || OutputGet->LinkedTo.Num() != 1)
			{
				continue;
			}

			UK2Node_IfThenElse* OldValueBranch = Cast<UK2Node_IfThenElse>(OutputGet->LinkedTo[0]->GetOwningNode());
			if (!OldValueBranch)
			{
				continue;
			}

			UEdGraphPin* OldThen = OldValueBranch->GetThenPin();
			UEdGraphPin* OldElse = OldValueBranch->GetElsePin();
			TArray<UEdGraphPin*> ThenTargets = OldThen ? OldThen->LinkedTo : TArray<UEdGraphPin*>();
			TArray<UEdGraphPin*> ElseTargets = OldElse ? OldElse->LinkedTo : TArray<UEdGraphPin*>();
			if (ThenTargets.Num() == 0 && ElseTargets.Num() == 0)
			{
				continue;
			}

			UK2Node_IfThenElse* NewBranch = SpawnBranchNode(EventGraph, BoolSet->NodePosX, BoolSet->NodePosY);
			UK2Node_VariableSet* EnabledSet = SpawnVariableSetNode(Blueprint, EventGraph, TEXT("WeakpointState"), BoolSet->NodePosX + 260, BoolSet->NodePosY - 120);
			UK2Node_VariableSet* DisabledSet = SpawnVariableSetNode(Blueprint, EventGraph, TEXT("WeakpointState"), BoolSet->NodePosX + 260, BoolSet->NodePosY + 120);
			if (!NewBranch || !EnabledSet || !DisabledSet)
			{
				continue;
			}

			MoveLinks(ExecIn, NewBranch->GetExecPin());
			MoveLinks(ValueIn, NewBranch->GetConditionPin());
			Schema->TryCreateConnection(NewBranch->GetThenPin(), EnabledSet->GetExecPin());
			Schema->TryCreateConnection(NewBranch->GetElsePin(), DisabledSet->GetExecPin());
			SetEnumPinDefault(FindPinByNameAndDirection(EnabledSet, TEXT("WeakpointState"), EGPD_Input), EnabledValue);
			SetEnumPinDefault(FindPinByNameAndDirection(DisabledSet, TEXT("WeakpointState"), EGPD_Input), DisabledValue);

			if (UEdGraphPin* EnabledThen = EnabledSet->GetThenPin())
			{
				for (UEdGraphPin* Target : ThenTargets)
				{
					if (Target)
					{
						Target->BreakAllPinLinks();
						Schema->TryCreateConnection(EnabledThen, Target);
					}
				}
			}
			if (UEdGraphPin* DisabledThen = DisabledSet->GetThenPin())
			{
				for (UEdGraphPin* Target : ElseTargets)
				{
					if (Target)
					{
						Target->BreakAllPinLinks();
						Schema->TryCreateConnection(DisabledThen, Target);
					}
				}
			}

			FBlueprintEditorUtils::RemoveNode(Blueprint, BoolSet, true);
			FBlueprintEditorUtils::RemoveNode(Blueprint, OldValueBranch, true);
			++ReplacedSetCount;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("replacedGets"), ReplacedGetCount);
	Result->SetNumberField(TEXT("replacedSets"), ReplacedSetCount);
	Result->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
	return MakeSuccess(Id, Result);
}

static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuid)
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

static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName)
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}
	return nullptr;
}

static bool TryLoadGraphForEdit(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, TSharedRef<FJsonObject>& OutError)
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

static TSharedRef<FJsonObject> FinishGraphEdit(const FString& Id, UBlueprint* Blueprint, UEdGraphNode* Node)
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

static TSharedRef<FJsonObject> AddVariableGetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddVariableSetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddBranchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddEnumEqualityNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddFunctionCallNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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
	return FinishGraphEdit(Id, Blueprint, Node);
}

static TSharedRef<FJsonObject> ConnectPins(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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
	UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName);
	UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName);
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

static TSharedRef<FJsonObject> MovePinLinksCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetPinDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> BreakPinLinks(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> CopyPinType(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> DeleteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static bool TryGetBlueprintVariableType(UBlueprint* Blueprint, const FName VariableName, FEdGraphPinType& OutPinType)
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

static UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph);

static UK2Node_FunctionResult* FindFunctionResultNode(UEdGraph* Graph)
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

static TSharedRef<FJsonObject> AddVariableGetterFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph)
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

static bool TryMakePinTypeFromParams(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& OutPinType, FString& OutError)
{
	FString SourceVariable;
	if (Params->TryGetStringField(TEXT("sourceVariable"), SourceVariable))
	{
		if (!TryGetBlueprintVariableType(Blueprint, *SourceVariable, OutPinType))
		{
			OutError = FString::Printf(TEXT("Could not find source variable '%s'."), *SourceVariable);
			return false;
		}
		return true;
	}

	FString Category;
	if (!TryGetRequiredString(Params, TEXT("category"), Category))
	{
		OutError = TEXT("Pin type requires params.category or params.sourceVariable.");
		return false;
	}

	OutPinType.PinCategory = NormalizePinCategory(Category);
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
	return true;
}

static TSharedRef<FJsonObject> DescribeNodeCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> FindNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> CreateFunctionGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> CreateEventGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddFunctionPin(const FString& Id, const TSharedPtr<FJsonObject>& Params, const bool bOutput)
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

static TSharedRef<FJsonObject> AddFunctionInput(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddFunctionPin(Id, Params, false);
}

static TSharedRef<FJsonObject> AddFunctionOutput(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddFunctionPin(Id, Params, true);
}

static TSharedRef<FJsonObject> DeleteGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> RenameGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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
	Graph->Rename(*NewName, nullptr, REN_DontCreateRedirectors);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return MakeSuccessMessage(Id, TEXT("GraphRenamed"));
}

static TSharedRef<FJsonObject> SetNodePosition(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddCommentNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddRerouteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddSequenceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddEnumSwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> CheckoutAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CheckoutAsset requires params.asset."));
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath));
	if (!Asset)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load asset '%s'."), *AssetPath));
	}

	UPackage* Package = Asset->GetOutermost();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	if (!SourceControlHelpers::CheckOutFile(PackageFilename))
	{
		return MakeBridgeError(Id, TEXT("CheckoutFailed"), FString::Printf(TEXT("Could not check out '%s'."), *PackageFilename));
	}

	return MakeSuccessMessage(Id, TEXT("CheckedOut"));
}

static FString GetBlueprintStatusString(const EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Unknown:
		return TEXT("Unknown");
	case BS_Dirty:
		return TEXT("Dirty");
	case BS_Error:
		return TEXT("Error");
	case BS_UpToDate:
		return TEXT("UpToDate");
	case BS_BeingCreated:
		return TEXT("BeingCreated");
	case BS_UpToDateWithWarnings:
		return TEXT("UpToDateWithWarnings");
	default:
		return TEXT("Invalid");
	}
}

static TSharedRef<FJsonObject> CompileBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CompileBlueprint requires params.asset."));
	}

	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load Blueprint '%s'."), *AssetPath));
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), GetBlueprintStatusString(Blueprint->Status));
	return MakeSuccess(Id, Result);
}

static TSharedRef<FJsonObject> SaveAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("SaveAsset requires params.asset."));
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath));
	if (!Asset)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load asset '%s'."), *AssetPath));
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Asset->GetOutermost());
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
	if (!bSaved)
	{
		return MakeBridgeError(Id, TEXT("SaveFailed"), FString::Printf(TEXT("Could not save '%s'."), *AssetPath));
	}

	return MakeSuccessMessage(Id, TEXT("Saved"));
}

static TSharedRef<FJsonObject> SetBlueprintDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static FName NormalizePinCategory(const FString& Category)
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

static TSharedRef<FJsonObject> AddBlueprintVariable(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

	FString DefaultValue;
	Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "AddBlueprintVariable", "Blueprint Bridge: Add Blueprint Variable"));
	Blueprint->Modify();
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, *Name, PinType, DefaultValue))
	{
		return MakeBridgeError(Id, TEXT("AddVariableFailed"), FString::Printf(TEXT("Could not add variable '%s'."), *Name));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
	return MakeSuccessMessage(Id, TEXT("VariableAdded"));
}

static TSharedRef<FJsonObject> ExecuteRequestOnGameThread(const FString& RequestText)
{
	TSharedPtr<FJsonObject> Request;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestText);
	if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
	{
		return MakeBridgeError(TEXT(""), TEXT("InvalidJson"), TEXT("Request was not valid JSON."));
	}

	const FString Id = Request->GetStringField(TEXT("id"));
	if (!ValidateAuthToken(Request))
	{
		return MakeBridgeError(Id, TEXT("Unauthorized"), TEXT("Missing or invalid auth token."));
	}

	const FString Command = Request->GetStringField(TEXT("command"));
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	Request->TryGetObjectField(TEXT("params"), ParamsPtr);
	const TSharedPtr<FJsonObject> Params = ParamsPtr ? *ParamsPtr : nullptr;

	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), true);

	if (Command.Equals(TEXT("Ping"), ESearchCase::IgnoreCase))
	{
		Response->SetStringField(TEXT("result"), TEXT("Pong"));
	}
	else if (Command.Equals(TEXT("GetProjectName"), ESearchCase::IgnoreCase))
	{
		Response->SetStringField(TEXT("result"), FApp::GetProjectName());
	}
	else if (Command.Equals(TEXT("GetEngineVersion"), ESearchCase::IgnoreCase))
	{
		Response->SetStringField(TEXT("result"), FEngineVersion::Current().ToString());
	}
	else if (Command.Equals(TEXT("DescribeBlueprint"), ESearchCase::IgnoreCase))
	{
		return DescribeBlueprint(Id, Params);
	}
	else if (Command.Equals(TEXT("DescribeGraph"), ESearchCase::IgnoreCase))
	{
		return DescribeGraph(Id, Params);
	}
	else if (Command.Equals(TEXT("DescribeNode"), ESearchCase::IgnoreCase))
	{
		return DescribeNodeCommand(Id, Params);
	}
	else if (Command.Equals(TEXT("FindNodes"), ESearchCase::IgnoreCase))
	{
		return FindNodes(Id, Params);
	}
	else if (Command.Equals(TEXT("FindVariableReferences"), ESearchCase::IgnoreCase))
	{
		return FindVariableReferences(Id, Params);
	}
	else if (Command.Equals(TEXT("AddVariableGetNode"), ESearchCase::IgnoreCase))
	{
		return AddVariableGetNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddVariableSetNode"), ESearchCase::IgnoreCase))
	{
		return AddVariableSetNode(Id, Params);
	}
	else if (Command.Equals(TEXT("CreateFunctionGraph"), ESearchCase::IgnoreCase))
	{
		return CreateFunctionGraph(Id, Params);
	}
	else if (Command.Equals(TEXT("CreateEventGraph"), ESearchCase::IgnoreCase))
	{
		return CreateEventGraph(Id, Params);
	}
	else if (Command.Equals(TEXT("AddFunctionInput"), ESearchCase::IgnoreCase))
	{
		return AddFunctionInput(Id, Params);
	}
	else if (Command.Equals(TEXT("AddFunctionOutput"), ESearchCase::IgnoreCase))
	{
		return AddFunctionOutput(Id, Params);
	}
	else if (Command.Equals(TEXT("DeleteGraph"), ESearchCase::IgnoreCase))
	{
		return DeleteGraph(Id, Params);
	}
	else if (Command.Equals(TEXT("RenameGraph"), ESearchCase::IgnoreCase))
	{
		return RenameGraph(Id, Params);
	}
	else if (Command.Equals(TEXT("AddVariableGetterFunction"), ESearchCase::IgnoreCase))
	{
		return AddVariableGetterFunction(Id, Params);
	}
	else if (Command.Equals(TEXT("AddBranchNode"), ESearchCase::IgnoreCase))
	{
		return AddBranchNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddSequenceNode"), ESearchCase::IgnoreCase))
	{
		return AddSequenceNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddRerouteNode"), ESearchCase::IgnoreCase))
	{
		return AddRerouteNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddCommentNode"), ESearchCase::IgnoreCase))
	{
		return AddCommentNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddEnumSwitchNode"), ESearchCase::IgnoreCase))
	{
		return AddEnumSwitchNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddEnumEqualityNode"), ESearchCase::IgnoreCase))
	{
		return AddEnumEqualityNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddFunctionCallNode"), ESearchCase::IgnoreCase))
	{
		return AddFunctionCallNode(Id, Params);
	}
	else if (Command.Equals(TEXT("ConnectPins"), ESearchCase::IgnoreCase))
	{
		return ConnectPins(Id, Params);
	}
	else if (Command.Equals(TEXT("MovePinLinks"), ESearchCase::IgnoreCase))
	{
		return MovePinLinksCommand(Id, Params);
	}
	else if (Command.Equals(TEXT("SetPinDefault"), ESearchCase::IgnoreCase))
	{
		return SetPinDefault(Id, Params);
	}
	else if (Command.Equals(TEXT("SetNodePosition"), ESearchCase::IgnoreCase))
	{
		return SetNodePosition(Id, Params);
	}
	else if (Command.Equals(TEXT("BreakPinLinks"), ESearchCase::IgnoreCase))
	{
		return BreakPinLinks(Id, Params);
	}
	else if (Command.Equals(TEXT("CopyPinType"), ESearchCase::IgnoreCase))
	{
		return CopyPinType(Id, Params);
	}
	else if (Command.Equals(TEXT("DeleteNode"), ESearchCase::IgnoreCase))
	{
		return DeleteNode(Id, Params);
	}
	else if (Command.Equals(TEXT("CheckoutAsset"), ESearchCase::IgnoreCase))
	{
		return CheckoutAsset(Id, Params);
	}
	else if (Command.Equals(TEXT("CompileBlueprint"), ESearchCase::IgnoreCase))
	{
		return CompileBlueprint(Id, Params);
	}
	else if (Command.Equals(TEXT("SaveAsset"), ESearchCase::IgnoreCase))
	{
		return SaveAsset(Id, Params);
	}
	else if (Command.Equals(TEXT("SetBlueprintDefault"), ESearchCase::IgnoreCase))
	{
		return SetBlueprintDefault(Id, Params);
	}
	else if (Command.Equals(TEXT("AddBlueprintVariable"), ESearchCase::IgnoreCase))
	{
		return AddBlueprintVariable(Id, Params);
	}
	else
	{
		return MakeBridgeError(Id, TEXT("UnknownCommand"), FString::Printf(TEXT("Unknown command '%s'."), *Command));
	}

	return Response;
}

TSharedRef<FJsonObject> ExecuteRequest(const FString& RequestText)
{
	if (IsInGameThread())
	{
		return ExecuteRequestOnGameThread(RequestText);
	}

	FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(false);
	TSharedPtr<FJsonObject> Response;
	AsyncTask(ENamedThreads::GameThread, [&Response, DoneEvent, RequestText]()
	{
		Response = ExecuteRequestOnGameThread(RequestText);
		DoneEvent->Trigger();
	});
	DoneEvent->Wait();
	FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
	return Response.ToSharedRef();
}


} // namespace BlueprintBridge
