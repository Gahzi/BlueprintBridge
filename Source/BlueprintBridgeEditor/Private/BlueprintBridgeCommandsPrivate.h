// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

#include "BlueprintBridgeCommandHandlers.h"
#include "BlueprintBridgeCommandRegistry.h"
#include "BlueprintBridgeSettings.h"

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
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
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
#include "UObject/UObjectIterator.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#include <atomic>


namespace BlueprintBridge
{
TSharedRef<FJsonObject> DuplicateAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CheckoutAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
FString GetBlueprintStatusString(const EBlueprintStatus Status);
TSharedRef<FJsonObject> CompileBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SaveAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateWidgetBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
FString GetPipeNamePath();
bool ValidateAuthToken(const TSharedPtr<FJsonObject>& Request);
FString JsonToString(const TSharedRef<FJsonObject>& JsonObject);
TSharedRef<FJsonObject> MakeBridgeError(const FString& Id, const FString& Code, const FString& Message);
FString NormalizeBlueprintObjectPath(const FString& AssetPath);
TSharedRef<FJsonObject> MakeSuccess(const FString& Id, const TSharedPtr<FJsonObject>& Result);
TSharedRef<FJsonObject> MakeSuccessMessage(const FString& Id, const FString& Message);
UBlueprint* LoadBlueprint(const FString& AssetPath);
UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath);
FString PinContainerTypeToString(const FEdGraphPinType& PinType);
bool ApplyPinContainerType(const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& PinType, FString& OutError);
UObject* LoadAssetObject(const FString& AssetPath);
bool DoesAssetExistQuiet(const FString& AssetPath);
bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName);
UClass* LoadClassByPath(const FString& ClassPath);
bool TryGetRequiredString(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FString& OutValue);
FName NormalizePinCategory(const FString& Category);
UK2Node_FunctionEntry* FindFunctionEntryNode(UEdGraph* Graph);
bool TryMakePinTypeFromParams(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& OutPinType, FString& OutError);
TSharedRef<FJsonObject> DescribeComponentNode(USCS_Node* Node);
TSharedRef<FJsonObject> DescribeComponents(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AttachComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetComponentTransform(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetComponentProperty(const FString& Id, const TSharedPtr<FJsonObject>& Params);
bool TryGetComponentTemplate(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UActorComponent*& OutTemplate, TSharedRef<FJsonObject>& OutError);
TSharedRef<FJsonObject> SetRootComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetStaticMesh(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetCollisionProfileName(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetBoxExtent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetGenerateOverlapEvents(const FString& Id, const TSharedPtr<FJsonObject>& Params);
void MoveLinks(UEdGraphPin* FromPin, UEdGraphPin* ToPin);
UEdGraphPin* FindFirstDataOutputPin(UEdGraphNode* Node);
UK2Node_CallFunction* SpawnFunctionCallNode(UEdGraph* Graph, UFunction* Function, const int32 X, const int32 Y);
UK2Node_IfThenElse* SpawnBranchNode(UEdGraph* Graph, const int32 X, const int32 Y);
UK2Node_EnumEquality* SpawnEnumEqualityNode(UEdGraph* Graph, const int32 X, const int32 Y);
UK2Node_VariableGet* SpawnVariableGetNode(UBlueprint* Blueprint, UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y);
UK2Node_VariableSet* SpawnVariableSetNode(UBlueprint* Blueprint, UEdGraph* Graph, const FName VariableName, const int32 X, const int32 Y);
UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuid);
FString NormalizePinLookupName(const FString& Name);
bool PinDirectionMatches(const UEdGraphPin* Pin, const TOptional<EEdGraphPinDirection> Direction);
UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, const TOptional<EEdGraphPinDirection> Direction);
bool TryLoadGraphForEdit(const FString& Id, const TSharedPtr<FJsonObject>& Params, UBlueprint*& OutBlueprint, UEdGraph*& OutGraph, TSharedRef<FJsonObject>& OutError);
void ApplyNodePinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> FinishGraphEdit(const FString& Id, UBlueprint* Blueprint, UEdGraph* Graph, UEdGraphNode* Node);
TSharedRef<FJsonObject> FinishGraphEdit(const FString& Id, UBlueprint* Blueprint, UEdGraphNode* Node);
TSharedRef<FJsonObject> AddVariableGetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddVariableSetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddBranchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEnumEqualityNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddFunctionCallNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
UFunction* FindFunctionForNodeCommand(const FString& ClassPath, const FString& FunctionName);
TSharedRef<FJsonObject> AddSelfNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddArrayFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddTimerNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddLineTraceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddMathNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddWidgetFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
bool AddUserDefinedOutputPinsFromArray(UBlueprint* Blueprint, UK2Node_EditablePinBase* Node, const TArray<TSharedPtr<FJsonValue>>& Inputs, FString& OutError);
TSharedRef<FJsonObject> AddCustomEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddDynamicCastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddSpawnActorNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEventDispatcher(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddComponentEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
FMulticastDelegateProperty* FindDelegatePropertyForCommand(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Params, FString& OutError, UClass*& OutOwnerClass);
TSharedRef<FJsonObject> AddDelegateBindNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddDelegateBroadcastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddCreateDelegateNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetCreateDelegateFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params);
UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName);
TSharedRef<FJsonObject> AddMacroInstanceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params, const FString& DefaultMacro, const FString& DefaultMacroLibrary);
TSharedRef<FJsonObject> AddForLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddForEachLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddAuthoritySwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddCreateWidgetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params, const bool bMakeStruct);
TSharedRef<FJsonObject> AddMakeStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddBreakStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> ConnectPins(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> MovePinLinksCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetPinDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> BreakPinLinks(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CopyPinType(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DeleteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
bool TryGetBlueprintVariableType(UBlueprint* Blueprint, const FName VariableName, FEdGraphPinType& OutPinType);
UK2Node_FunctionResult* FindFunctionResultNode(UEdGraph* Graph);
TSharedRef<FJsonObject> AddVariableGetterFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params);
void ApplyPinRefAndConstFlags(const TSharedPtr<FJsonObject>& Params, FEdGraphPinType& OutPinType);
TSharedRef<FJsonObject> DescribeNodeCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> FindNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateFunctionGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateEventGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddFunctionPin(const FString& Id, const TSharedPtr<FJsonObject>& Params, const bool bOutput);
TSharedRef<FJsonObject> AddFunctionInput(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddFunctionOutput(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> RenameCustomEvent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> EditUserDefinedPin(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DeleteGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> RenameGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetNodePosition(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddCommentNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddRerouteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddSequenceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEnumSwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
USCS_Node* FindSCSNodeByName(USimpleConstructionScript* SCS, const FString& ComponentName);
USCS_Node* FindSCSParentNode(USimpleConstructionScript* SCS, USCS_Node* ChildNode);
TSharedRef<FJsonObject> DescribeWidget(UWidget* Widget);
void AddWidgetTreeDescription(TSharedRef<FJsonObject> Result, UWidgetBlueprint* WidgetBlueprint);
TSharedRef<FJsonObject> DescribeBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params);
UEdGraph* FindBlueprintGraph(UBlueprint* Blueprint, const FString& GraphName);
TSharedRef<FJsonObject> DescribeNode(UEdGraphNode* Node);
TSharedRef<FJsonObject> DescribeGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> FindVariableReferences(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> MakeCommandDescription(const ICommand& Command, const bool bIncludeSchemas);
TSharedRef<FJsonObject> ListCommands(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> BatchCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> ExecuteRequestOnGameThread(const FString& RequestText);
TSharedRef<FJsonObject> PingCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> GetProjectNameCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> GetEngineVersionCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
bool DoesJsonValueMatchType(const TSharedPtr<FJsonValue>& Value, const FString& ExpectedType);
bool ValidateCommandParamsAgainstSchema(const FString& CommandName, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Schema, FString& OutError);
TSharedRef<FJsonObject> SetBlueprintDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeSubobject(UObject* Subobject, const bool bIncludeProperties);
void GetBlueprintCDOSubobjects(UBlueprint* Blueprint, TArray<UObject*>& OutSubobjects);
bool SubobjectMatchesIdentifier(const UObject* Subobject, const FString& Identifier);
bool SubobjectMatchesClassFilter(const UObject* Subobject, const FString& ClassPath);
TSharedRef<FJsonObject> DescribeSubobjects(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetSubobjectDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetBlueprintVariableFlags(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddBlueprintVariable(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeWidgetTree(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetRootWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddWidgetToParent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetWidgetSlotLayout(const FString& Id, const TSharedPtr<FJsonObject>& Params);
} // namespace BlueprintBridge
