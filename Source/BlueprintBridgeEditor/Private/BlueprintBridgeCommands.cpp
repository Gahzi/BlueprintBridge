// Copyright Odyssey Interactive. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Async/Async.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/BoxComponent.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Interfaces/IPluginManager.h"
#include "Engine/StaticMesh.h"
#include "JsonObjectConverter.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_Event.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "IAssetTools.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SourceControlHelpers.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "WidgetBlueprint.h"

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

static UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath)
{
	return Cast<UWidgetBlueprint>(StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath)));
}

static FString PinContainerTypeToString(const FEdGraphPinType& PinType)
{
	switch (PinType.ContainerType)
	{
	case EPinContainerType::Array:
		return TEXT("Array");
	case EPinContainerType::Set:
		return TEXT("Set");
	case EPinContainerType::Map:
		return TEXT("Map");
	case EPinContainerType::None:
	default:
		return TEXT("None");
	}
}

static bool ApplyPinContainerType(const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& PinType, FString& OutError)
{
	FString ContainerType;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("containerType"), ContainerType))
	{
		bool bIsArray = false;
		if (Params.IsValid() && Params->TryGetBoolField(TEXT("isArray"), bIsArray) && bIsArray)
		{
			PinType.ContainerType = EPinContainerType::Array;
		}
		return true;
	}

	if (ContainerType.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		PinType.ContainerType = EPinContainerType::None;
		return true;
	}
	if (ContainerType.Equals(TEXT("Array"), ESearchCase::IgnoreCase))
	{
		PinType.ContainerType = EPinContainerType::Array;
		return true;
	}
	if (ContainerType.Equals(TEXT("Set"), ESearchCase::IgnoreCase))
	{
		PinType.ContainerType = EPinContainerType::Set;
		return true;
	}

	OutError = FString::Printf(TEXT("Unsupported containerType '%s'. Supported values are None, Array, and Set."), *ContainerType);
	return false;
}

static UObject* LoadAssetObject(const FString& AssetPath)
{
	return StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizeBlueprintObjectPath(AssetPath));
}

static bool DoesAssetExistQuiet(const FString& AssetPath)
{
	const FString ObjectPath = NormalizeBlueprintObjectPath(AssetPath);
	if (FindObject<UObject>(nullptr, *ObjectPath))
	{
		return true;
	}

	FString PackagePath = AssetPath;
	if (PackagePath.Contains(TEXT(".")))
	{
		PackagePath.Split(TEXT("."), &PackagePath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	return FPackageName::DoesPackageExist(PackagePath);
}

static bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName)
{
	FString NormalizedAssetPath = AssetPath;
	if (NormalizedAssetPath.Contains(TEXT(".")))
	{
		NormalizedAssetPath.Split(TEXT("."), &NormalizedAssetPath, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}

	if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath, false))
	{
		return false;
	}

	if (!NormalizedAssetPath.Split(TEXT("/"), &OutPackagePath, &OutAssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || OutPackagePath.IsEmpty() || OutAssetName.IsEmpty())
	{
		return false;
	}

	return true;
}

static UClass* LoadClassByPath(const FString& ClassPath)
{
	if (UClass* Class = LoadObject<UClass>(nullptr, *ClassPath))
	{
		return Class;
	}

	return StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
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
static UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph);
static bool TryMakePinTypeFromParams(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& OutPinType, FString& OutError);

static TSharedRef<FJsonObject> DescribeWidget(UWidget* Widget)
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

static void AddWidgetTreeDescription(TSharedRef<FJsonObject> Result, UWidgetBlueprint* WidgetBlueprint)
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
		PinJson->SetStringField(TEXT("containerType"), PinContainerTypeToString(Pin->PinType));
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

static FString NormalizePinLookupName(const FString& Name)
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
	return Result;
}

static bool PinDirectionMatches(const UEdGraphPin* Pin, const TOptional<EEdGraphPinDirection> Direction)
{
	return !Direction.IsSet() || (Pin && Pin->Direction == Direction.GetValue());
}

static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, const TOptional<EEdGraphPinDirection> Direction = TOptional<EEdGraphPinDirection>())
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

static void ApplyNodePinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> FinishGraphEdit(const FString& Id, UBlueprint* Blueprint, UEdGraph* Graph, UEdGraphNode* Node)
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

static TSharedRef<FJsonObject> FinishGraphEdit(const FString& Id, UBlueprint* Blueprint, UEdGraphNode* Node)
{
	return FinishGraphEdit(Id, Blueprint, nullptr, Node);
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
	ApplyNodePinDefaults(Node, Params);
	return FinishGraphEdit(Id, Blueprint, Node);
}

static UFunction* FindFunctionForNodeCommand(const FString& ClassPath, const FString& FunctionName)
{
	UClass* FunctionClass = FindObject<UClass>(nullptr, *ClassPath);
	if (!FunctionClass)
	{
		FunctionClass = LoadObject<UClass>(nullptr, *ClassPath);
	}
	return FunctionClass ? FunctionClass->FindFunctionByName(*FunctionName) : nullptr;
}

static TSharedRef<FJsonObject> AddSelfNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddArrayFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddTimerNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddLineTraceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString Operation;
	Params->TryGetStringField(TEXT("operation"), Operation);
	const FString FunctionName = Operation.Equals(TEXT("Multi"), ESearchCase::IgnoreCase) ? TEXT("LineTraceMulti") : TEXT("LineTraceSingle");
	Params->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetSystemLibrary"));
	Params->SetStringField(TEXT("function"), FunctionName);
	return AddFunctionCallNode(Id, Params);
}

static TSharedRef<FJsonObject> AddMathNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString FunctionName;
	if (!TryGetRequiredString(Params, TEXT("function"), FunctionName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("AddMathNode requires params.function."));
	}

	Params->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetMathLibrary"));
	return AddFunctionCallNode(Id, Params);
}

static TSharedRef<FJsonObject> AddWidgetFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static bool AddUserDefinedOutputPinsFromArray(UBlueprint* Blueprint, UK2Node_EditablePinBase* Node, const TArray<TSharedPtr<FJsonValue>>& Inputs, FString& OutError)
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

static TSharedRef<FJsonObject> AddCustomEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddDynamicCastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddSpawnActorNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddEventDispatcher(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddComponentEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static FMulticastDelegateProperty* FindDelegatePropertyForCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError, UClass*& OutOwnerClass);

static TSharedRef<FJsonObject> AddDelegateBindNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static FMulticastDelegateProperty* FindDelegatePropertyForCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError, UClass*& OutOwnerClass)
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

static TSharedRef<FJsonObject> AddDelegateBroadcastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddCreateDelegateNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetCreateDelegateFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
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

static TSharedRef<FJsonObject> AddMacroInstanceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params, const FString& DefaultMacro, const FString& DefaultMacroLibrary = TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"))
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

static TSharedRef<FJsonObject> AddForLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddMacroInstanceNode(Id, Params, TEXT("ForLoop"));
}

static TSharedRef<FJsonObject> AddForEachLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddMacroInstanceNode(Id, Params, TEXT("ForEachLoop"));
}

static TSharedRef<FJsonObject> AddAuthoritySwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddMacroInstanceNode(Id, Params, TEXT("Switch Has Authority"), TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros"));
}

static TSharedRef<FJsonObject> AddCreateWidgetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params, const bool bMakeStruct)
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

static TSharedRef<FJsonObject> AddMakeStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddStructNode(Id, Params, true);
}

static TSharedRef<FJsonObject> AddBreakStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return AddStructNode(Id, Params, false);
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

	return ApplyPinContainerType(Params, OutPinType, OutError);
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

static TSharedRef<FJsonObject> RenameCustomEvent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> EditUserDefinedPin(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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
	FBlueprintEditorUtils::RenameGraph(Graph, NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();
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


static USCS_Node* FindSCSNodeByName(USimpleConstructionScript* SCS, const FString& ComponentName)
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

static USCS_Node* FindSCSParentNode(USimpleConstructionScript* SCS, USCS_Node* ChildNode)
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

static TSharedRef<FJsonObject> DescribeComponentNode(USCS_Node* Node)
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

static TSharedRef<FJsonObject> DescribeComponents(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AttachComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetComponentTransform(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetComponentProperty(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static bool TryGetComponentTemplate(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UActorComponent*& OutTemplate, TSharedRef<FJsonObject>& OutError)
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

static TSharedRef<FJsonObject> SetRootComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetStaticMesh(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetCollisionProfileName(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetBoxExtent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetGenerateOverlapEvents(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> CreateBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ParentClassPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("parentClass"), ParentClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CreateBlueprintAsset requires params.asset and params.parentClass."));
	}

	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidAssetPath"), FString::Printf(TEXT("'%s' is not a valid asset path."), *AssetPath));
	}

	if (DoesAssetExistQuiet(AssetPath))
	{
		return MakeBridgeError(Id, TEXT("AssetAlreadyExists"), FString::Printf(TEXT("Asset '%s' already exists."), *AssetPath));
	}

	UClass* ParentClass = LoadClassByPath(ParentClassPath);
	if (!ParentClass)
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load parent class '%s'."), *ParentClassPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CreateBlueprintAsset", "Blueprint Bridge: Create Blueprint Asset"));
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName));
	if (!Package)
	{
		return MakeBridgeError(Id, TEXT("CreatePackageFailed"), FString::Printf(TEXT("Could not create package for '%s'."), *AssetPath));
	}

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("BlueprintBridge"));

	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("CreateBlueprintFailed"), FString::Printf(TEXT("Could not create Blueprint '%s'."), *AssetPath));
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	return MakeSuccess(Id, Result);
}

static TSharedRef<FJsonObject> CreateWidgetBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CreateWidgetBlueprintAsset requires params.asset."));
	}

	FString ParentClassPath = TEXT("/Script/UMG.UserWidget");
	Params->TryGetStringField(TEXT("parentClass"), ParentClassPath);

	FString PackagePath;
	FString AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidAssetPath"), FString::Printf(TEXT("'%s' is not a valid asset path."), *AssetPath));
	}

	if (DoesAssetExistQuiet(AssetPath))
	{
		return MakeBridgeError(Id, TEXT("AssetAlreadyExists"), FString::Printf(TEXT("Asset '%s' already exists."), *AssetPath));
	}

	UClass* ParentClass = LoadClassByPath(ParentClassPath);
	if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
	{
		return MakeBridgeError(Id, TEXT("ClassNotFound"), FString::Printf(TEXT("Could not load UserWidget parent class '%s'."), *ParentClassPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CreateWidgetBlueprintAsset", "Blueprint Bridge: Create Widget Blueprint Asset"));
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName));
	if (!Package)
	{
		return MakeBridgeError(Id, TEXT("CreatePackageFailed"), FString::Printf(TEXT("Could not create package for '%s'."), *AssetPath));
	}

	UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass(),
		TEXT("BlueprintBridge")));

	if (!Blueprint)
	{
		return MakeBridgeError(Id, TEXT("CreateBlueprintFailed"), FString::Printf(TEXT("Could not create Widget Blueprint '%s'."), *AssetPath));
	}

	FAssetRegistryModule::AssetCreated(Blueprint);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	AddWidgetTreeDescription(Result, Blueprint);
	return MakeSuccess(Id, Result);
}

static TSharedRef<FJsonObject> DescribeWidgetTree(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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
	return MakeSuccess(Id, Result);
}

static TSharedRef<FJsonObject> AddWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetRootWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> AddWidgetToParent(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> SetWidgetSlotLayout(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

static TSharedRef<FJsonObject> DuplicateAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString SourceAssetPath;
	FString DestAssetPath;
	if (!TryGetRequiredString(Params, TEXT("sourceAsset"), SourceAssetPath) || !TryGetRequiredString(Params, TEXT("destAsset"), DestAssetPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DuplicateAsset requires params.sourceAsset and params.destAsset."));
	}

	UObject* SourceAsset = LoadAssetObject(SourceAssetPath);
	if (!SourceAsset)
	{
		return MakeBridgeError(Id, TEXT("AssetNotFound"), FString::Printf(TEXT("Could not load source asset '%s'."), *SourceAssetPath));
	}

	FString DestPackagePath;
	FString DestAssetName;
	if (!SplitAssetPath(DestAssetPath, DestPackagePath, DestAssetName))
	{
		return MakeBridgeError(Id, TEXT("InvalidAssetPath"), FString::Printf(TEXT("'%s' is not a valid destination asset path."), *DestAssetPath));
	}

	if (DoesAssetExistQuiet(DestAssetPath))
	{
		return MakeBridgeError(Id, TEXT("AssetAlreadyExists"), FString::Printf(TEXT("Asset '%s' already exists."), *DestAssetPath));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "DuplicateAsset", "Blueprint Bridge: Duplicate Asset"));
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.DuplicateAsset(DestAssetName, DestPackagePath, SourceAsset);
	if (!NewAsset)
	{
		return MakeBridgeError(Id, TEXT("DuplicateAssetFailed"), FString::Printf(TEXT("Could not duplicate '%s' to '%s'."), *SourceAssetPath, *DestAssetPath));
	}

	NewAsset->GetOutermost()->MarkPackageDirty();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), DestAssetPath);
	Result->SetStringField(TEXT("name"), NewAsset->GetName());
	if (const UBlueprint* Blueprint = Cast<UBlueprint>(NewAsset))
	{
		Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	}
	Result->SetStringField(TEXT("sourceAsset"), SourceAssetPath);
	return MakeSuccess(Id, Result);
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
	Result->SetBoolField(TEXT("success"), Blueprint->Status != BS_Error);
	Result->SetArrayField(TEXT("messages"), TArray<TSharedPtr<FJsonValue>>());
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

static TSharedRef<FJsonObject> SetBlueprintVariableFlags(const FString& Id, const TSharedPtr<FJsonObject>& Params)
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

	if (Command.Equals(TEXT("Batch"), ESearchCase::IgnoreCase))
	{
		const TArray<TSharedPtr<FJsonValue>>* Requests = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("requests"), Requests) || Requests == nullptr)
		{
			return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("Batch requires params.requests."));
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		for (const TSharedPtr<FJsonValue>& RequestValue : *Requests)
		{
			const TSharedPtr<FJsonObject>* ChildRequest = nullptr;
			if (!RequestValue.IsValid() || !RequestValue->TryGetObject(ChildRequest) || ChildRequest == nullptr || !ChildRequest->IsValid())
			{
				Results.Add(MakeShared<FJsonValueObject>(MakeBridgeError(Id, TEXT("InvalidBatchRequest"), TEXT("Each batch request must be an object."))));
				continue;
			}

			if (!(*ChildRequest)->HasField(TEXT("id")))
			{
				(*ChildRequest)->SetStringField(TEXT("id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
			}
			if (!(*ChildRequest)->HasField(TEXT("version")))
			{
				(*ChildRequest)->SetNumberField(TEXT("version"), 1);
			}
			Results.Add(MakeShared<FJsonValueObject>(ExecuteRequestOnGameThread(JsonToString((*ChildRequest).ToSharedRef()))));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("responses"), Results);
		return MakeSuccess(Id, Result);
	}
	else if (Command.Equals(TEXT("Ping"), ESearchCase::IgnoreCase))
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
	else if (Command.Equals(TEXT("DescribeComponents"), ESearchCase::IgnoreCase))
	{
		return DescribeComponents(Id, Params);
	}
	else if (Command.Equals(TEXT("AddComponent"), ESearchCase::IgnoreCase))
	{
		return AddComponent(Id, Params);
	}
	else if (Command.Equals(TEXT("AttachComponent"), ESearchCase::IgnoreCase))
	{
		return AttachComponent(Id, Params);
	}
	else if (Command.Equals(TEXT("SetComponentTransform"), ESearchCase::IgnoreCase))
	{
		return SetComponentTransform(Id, Params);
	}
	else if (Command.Equals(TEXT("SetComponentProperty"), ESearchCase::IgnoreCase))
	{
		return SetComponentProperty(Id, Params);
	}
	else if (Command.Equals(TEXT("SetRootComponent"), ESearchCase::IgnoreCase))
	{
		return SetRootComponent(Id, Params);
	}
	else if (Command.Equals(TEXT("SetStaticMesh"), ESearchCase::IgnoreCase))
	{
		return SetStaticMesh(Id, Params);
	}
	else if (Command.Equals(TEXT("SetCollisionProfileName"), ESearchCase::IgnoreCase))
	{
		return SetCollisionProfileName(Id, Params);
	}
	else if (Command.Equals(TEXT("SetBoxExtent"), ESearchCase::IgnoreCase))
	{
		return SetBoxExtent(Id, Params);
	}
	else if (Command.Equals(TEXT("SetGenerateOverlapEvents"), ESearchCase::IgnoreCase))
	{
		return SetGenerateOverlapEvents(Id, Params);
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
	else if (Command.Equals(TEXT("RenameCustomEvent"), ESearchCase::IgnoreCase))
	{
		return RenameCustomEvent(Id, Params);
	}
	else if (Command.Equals(TEXT("EditUserDefinedPin"), ESearchCase::IgnoreCase))
	{
		return EditUserDefinedPin(Id, Params);
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
	else if (Command.Equals(TEXT("AddSelfNode"), ESearchCase::IgnoreCase))
	{
		return AddSelfNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddArrayFunctionNode"), ESearchCase::IgnoreCase))
	{
		return AddArrayFunctionNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddTimerNode"), ESearchCase::IgnoreCase))
	{
		return AddTimerNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddLineTraceNode"), ESearchCase::IgnoreCase))
	{
		return AddLineTraceNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddMathNode"), ESearchCase::IgnoreCase))
	{
		return AddMathNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddWidgetFunctionNode"), ESearchCase::IgnoreCase))
	{
		return AddWidgetFunctionNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddCustomEventNode"), ESearchCase::IgnoreCase))
	{
		return AddCustomEventNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddEventNode"), ESearchCase::IgnoreCase))
	{
		return AddEventNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddDynamicCastNode"), ESearchCase::IgnoreCase))
	{
		return AddDynamicCastNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddSpawnActorNode"), ESearchCase::IgnoreCase))
	{
		return AddSpawnActorNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddEventDispatcher"), ESearchCase::IgnoreCase))
	{
		return AddEventDispatcher(Id, Params);
	}
	else if (Command.Equals(TEXT("AddComponentEventNode"), ESearchCase::IgnoreCase))
	{
		return AddComponentEventNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddDelegateBindNode"), ESearchCase::IgnoreCase))
	{
		return AddDelegateBindNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddDelegateBroadcastNode"), ESearchCase::IgnoreCase))
	{
		return AddDelegateBroadcastNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddCreateDelegateNode"), ESearchCase::IgnoreCase))
	{
		return AddCreateDelegateNode(Id, Params);
	}
	else if (Command.Equals(TEXT("SetCreateDelegateFunction"), ESearchCase::IgnoreCase))
	{
		return SetCreateDelegateFunction(Id, Params);
	}
	else if (Command.Equals(TEXT("AddForLoopNode"), ESearchCase::IgnoreCase))
	{
		return AddForLoopNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddForEachLoopNode"), ESearchCase::IgnoreCase))
	{
		return AddForEachLoopNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddAuthoritySwitchNode"), ESearchCase::IgnoreCase))
	{
		return AddAuthoritySwitchNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddMakeStructNode"), ESearchCase::IgnoreCase))
	{
		return AddMakeStructNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddBreakStructNode"), ESearchCase::IgnoreCase))
	{
		return AddBreakStructNode(Id, Params);
	}
	else if (Command.Equals(TEXT("AddCreateWidgetNode"), ESearchCase::IgnoreCase))
	{
		return AddCreateWidgetNode(Id, Params);
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
	else if (Command.Equals(TEXT("CreateBlueprintAsset"), ESearchCase::IgnoreCase))
	{
		return CreateBlueprintAsset(Id, Params);
	}
	else if (Command.Equals(TEXT("CreateWidgetBlueprintAsset"), ESearchCase::IgnoreCase))
	{
		return CreateWidgetBlueprintAsset(Id, Params);
	}
	else if (Command.Equals(TEXT("DescribeWidgetTree"), ESearchCase::IgnoreCase))
	{
		return DescribeWidgetTree(Id, Params);
	}
	else if (Command.Equals(TEXT("AddWidget"), ESearchCase::IgnoreCase))
	{
		return AddWidget(Id, Params);
	}
	else if (Command.Equals(TEXT("SetRootWidget"), ESearchCase::IgnoreCase))
	{
		return SetRootWidget(Id, Params);
	}
	else if (Command.Equals(TEXT("AddWidgetToParent"), ESearchCase::IgnoreCase))
	{
		return AddWidgetToParent(Id, Params);
	}
	else if (Command.Equals(TEXT("SetWidgetSlotLayout"), ESearchCase::IgnoreCase))
	{
		return SetWidgetSlotLayout(Id, Params);
	}
	else if (Command.Equals(TEXT("DuplicateAsset"), ESearchCase::IgnoreCase))
	{
		return DuplicateAsset(Id, Params);
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
	else if (Command.Equals(TEXT("SetBlueprintVariableFlags"), ESearchCase::IgnoreCase))
	{
		return SetBlueprintVariableFlags(Id, Params);
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
