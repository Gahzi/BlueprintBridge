// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace BlueprintBridge
{
TSharedRef<FJsonObject> BatchCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> PingCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> GetProjectNameCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> GetEngineVersionCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> ListCommands(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> GenerateCommandDocs(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeNodeCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> FindNodes(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> FindVariableReferences(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeComponents(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeWidgetTree(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DescribeSubobjects(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AttachComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetComponentTransform(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetComponentProperty(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetRootComponent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetStaticMesh(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetCollisionProfileName(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetBoxExtent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetGenerateOverlapEvents(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateFunctionGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateEventGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddFunctionInput(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddFunctionOutput(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DeleteGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> RenameCustomEvent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> EditUserDefinedPin(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> RenameGraph(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddVariableGetterFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddVariableGetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddVariableSetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddBranchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddSequenceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddRerouteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddCommentNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEnumSwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEnumEqualityNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddFunctionCallNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddSelfNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddArrayFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddTimerNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddLineTraceNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddMathNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddWidgetFunctionNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddCustomEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddDynamicCastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddSpawnActorNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddEventDispatcher(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddComponentEventNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddDelegateBindNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddDelegateBroadcastNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddCreateDelegateNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetCreateDelegateFunction(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddForLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddForEachLoopNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddAuthoritySwitchNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddMakeStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddBreakStructNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddCreateWidgetNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> ConnectPins(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> MovePinLinksCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetPinDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetNodePosition(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> BreakPinLinks(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CopyPinType(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DeleteNode(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CreateWidgetBlueprintAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> DuplicateAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CheckoutAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> CompileBlueprint(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SaveAsset(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetRootWidget(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddWidgetToParent(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetWidgetSlotLayout(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetBlueprintDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetSubobjectDefault(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> SetBlueprintVariableFlags(const FString& Id, const TSharedPtr<FJsonObject>& Params);
TSharedRef<FJsonObject> AddBlueprintVariable(const FString& Id, const TSharedPtr<FJsonObject>& Params);

void RegisterBlueprintBridgeCommands();
} // namespace BlueprintBridge
