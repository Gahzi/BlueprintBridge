// Copyright Odyssey Interactive. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "BlueprintBridgeInternal.h"

#include "Blueprint/WidgetTree.h"
#include "Components/BoxComponent.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/EngineTypes.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace BlueprintBridgeTests
{
static constexpr TCHAR AuthSettingsSection[] = TEXT("/Script/BlueprintBridgeEditor.BlueprintBridge");

static FString JsonToString(const TSharedRef<FJsonObject>& JsonObject)
{
	FString Output;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonObject, Writer);
	return Output;
}

static TSharedRef<FJsonObject> MakeRequest(const FString& Command, const TSharedPtr<FJsonObject>& Params)
{
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("id"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	Request->SetNumberField(TEXT("version"), 1);
	Request->SetStringField(TEXT("command"), Command);
	Request->SetObjectField(TEXT("params"), Params.IsValid() ? Params.ToSharedRef() : MakeShared<FJsonObject>());
	return Request;
}

static TSharedRef<FJsonObject> ExecuteJsonRequest(const FString& Command, const TSharedPtr<FJsonObject>& Params)
{
	return BlueprintBridge::ExecuteRequest(JsonToString(MakeRequest(Command, Params)));
}

static bool ExpectSuccess(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Response)
{
	bool bOk = false;
	if (!Response->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
	{
		const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
		FString ErrorCode;
		FString ErrorMessage;
		if (Response->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject != nullptr && ErrorObject->IsValid())
		{
			(*ErrorObject)->TryGetStringField(TEXT("code"), ErrorCode);
			(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
			Test.AddError(FString::Printf(TEXT("Bridge request failed with %s: %s"), *ErrorCode, *ErrorMessage));
		}

		Test.TestTrue(TEXT("Response should contain ok=true."), false);
		return false;
	}

	return Test.TestTrue(TEXT("Response should contain result."), Response->HasField(TEXT("result")));
}

static bool ExpectErrorCode(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Response, const FString& ExpectedCode)
{
	bool bOk = true;
	Test.TestTrue(TEXT("Response should contain ok=false."), Response->TryGetBoolField(TEXT("ok"), bOk) && !bOk);

	const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
	if (!Test.TestTrue(TEXT("Response should contain error object."), Response->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject != nullptr && ErrorObject->IsValid()))
	{
		return false;
	}

	FString ActualCode;
	if (!Test.TestTrue(TEXT("Error should contain code."), (*ErrorObject)->TryGetStringField(TEXT("code"), ActualCode)))
	{
		return false;
	}

	return Test.TestEqual(TEXT("Error code should match."), ActualCode, ExpectedCode);
}

static FString GetErrorMessage(const TSharedRef<FJsonObject>& Response)
{
	const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
	if (Response->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject != nullptr && ErrorObject->IsValid())
	{
		FString Message;
		(*ErrorObject)->TryGetStringField(TEXT("message"), Message);
		return Message;
	}

	return FString();
}

static const TSharedPtr<FJsonObject>* GetResultObject(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Response)
{
	const TSharedPtr<FJsonObject>* ResultObject = nullptr;
	Test.TestTrue(TEXT("Result should be an object."), Response->TryGetObjectField(TEXT("result"), ResultObject) && ResultObject != nullptr && ResultObject->IsValid());
	return ResultObject;
}

static FString GetStringResult(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Response)
{
	FString Result;
	Test.TestTrue(TEXT("Result should be a string."), Response->TryGetStringField(TEXT("result"), Result));
	return Result;
}

static bool GetCommandRequiredFields(FAutomationTestBase& Test, const FString& CommandName, TSet<FString>& OutRequiredFields)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("command"), CommandName);
	const TSharedRef<FJsonObject> Response = ExecuteJsonRequest(TEXT("DescribeCommand"), Params);
	if (!ExpectSuccess(Test, Response))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = GetResultObject(Test, Response);
	if (!Result)
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* InputSchema = nullptr;
	if (!Test.TestTrue(TEXT("Command should contain input schema."), (*Result)->TryGetObjectField(TEXT("inputSchema"), InputSchema) && InputSchema != nullptr && InputSchema->IsValid()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RequiredFields = nullptr;
	if (!(*InputSchema)->TryGetArrayField(TEXT("required"), RequiredFields) || RequiredFields == nullptr)
	{
		return true;
	}

	for (const TSharedPtr<FJsonValue>& RequiredField : *RequiredFields)
	{
		FString FieldName;
		if (RequiredField.IsValid() && RequiredField->TryGetString(FieldName))
		{
			OutRequiredFields.Add(FieldName);
		}
	}

	return true;
}

static const TSharedPtr<FJsonObject>* GetNodeObjectFromResponse(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Response)
{
	const TSharedPtr<FJsonObject>* ResultObject = GetResultObject(Test, Response);
	if (!ResultObject)
	{
		return nullptr;
	}

	const TSharedPtr<FJsonObject>* NodeObject = nullptr;
	Test.TestTrue(TEXT("Result should contain a node object."), (*ResultObject)->TryGetObjectField(TEXT("node"), NodeObject) && NodeObject != nullptr && NodeObject->IsValid());
	return NodeObject;
}

static FString GetNodeGuid(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& NodeObject)
{
	FString Guid;
	if (NodeObject.IsValid())
	{
		Test.TestTrue(TEXT("Node object should contain guid."), NodeObject->TryGetStringField(TEXT("guid"), Guid));
	}
	return Guid;
}

static const TArray<TSharedPtr<FJsonValue>>* GetNodePins(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& NodeObject)
{
	const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
	if (NodeObject.IsValid())
	{
		Test.TestTrue(TEXT("Node object should contain pins."), NodeObject->TryGetArrayField(TEXT("pins"), Pins) && Pins != nullptr);
	}
	return Pins;
}

static TSharedPtr<FJsonObject> FindPinObjectByName(const TSharedPtr<FJsonObject>& NodeObject, const FString& PinName)
{
	if (!NodeObject.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
	if (!NodeObject->TryGetArrayField(TEXT("pins"), Pins) || Pins == nullptr)
	{
		return nullptr;
	}

	for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
	{
		const TSharedPtr<FJsonObject>* PinObject = nullptr;
		if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObject) || PinObject == nullptr || !PinObject->IsValid())
		{
			continue;
		}

		FString CurrentPinName;
		if ((*PinObject)->TryGetStringField(TEXT("name"), CurrentPinName) && CurrentPinName == PinName)
		{
			return *PinObject;
		}
	}

	return nullptr;
}

static FString FindPinName(const TSharedPtr<FJsonObject>& NodeObject, const FString& Direction, const FString& Category = FString(), const FString& NameContains = FString())
{
	if (!NodeObject.IsValid())
	{
		return FString();
	}

	const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
	if (!NodeObject->TryGetArrayField(TEXT("pins"), Pins) || Pins == nullptr)
	{
		return FString();
	}

	for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
	{
		const TSharedPtr<FJsonObject>* PinObject = nullptr;
		if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObject) || PinObject == nullptr || !PinObject->IsValid())
		{
			continue;
		}

		FString CurrentDirection;
		FString CurrentCategory;
		FString CurrentName;
		(*PinObject)->TryGetStringField(TEXT("direction"), CurrentDirection);
		(*PinObject)->TryGetStringField(TEXT("category"), CurrentCategory);
		(*PinObject)->TryGetStringField(TEXT("name"), CurrentName);

		if (!Direction.IsEmpty() && CurrentDirection != Direction)
		{
			continue;
		}
		if (!Category.IsEmpty() && CurrentCategory != Category)
		{
			continue;
		}
		if (!NameContains.IsEmpty() && !CurrentName.Contains(NameContains))
		{
			continue;
		}

		return CurrentName;
	}

	return FString();
}

static bool PinLinksToNode(const TSharedPtr<FJsonObject>& NodeObject, const FString& PinName, const FString& LinkedNodeGuid)
{
	const TSharedPtr<FJsonObject> PinObject = FindPinObjectByName(NodeObject, PinName);
	if (!PinObject.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* LinkedTo = nullptr;
	if (!PinObject->TryGetArrayField(TEXT("linkedTo"), LinkedTo) || LinkedTo == nullptr)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& LinkValue : *LinkedTo)
	{
		const TSharedPtr<FJsonObject>* LinkObject = nullptr;
		if (!LinkValue.IsValid() || !LinkValue->TryGetObject(LinkObject) || LinkObject == nullptr || !LinkObject->IsValid())
		{
			continue;
		}

		FString NodeGuid;
		if ((*LinkObject)->TryGetStringField(TEXT("nodeGuid"), NodeGuid) && NodeGuid == LinkedNodeGuid)
		{
			return true;
		}
	}

	return false;
}

class FScopedAuthSettings final
{
public:
	FScopedAuthSettings(bool bInRequireAuthToken, const FString& InAuthToken)
	{
		bHadRequireAuthToken = GConfig->GetBool(AuthSettingsSection, TEXT("bRequireAuthToken"), bOriginalRequireAuthToken, GEditorPerProjectIni);
		bHadAuthToken = GConfig->GetString(AuthSettingsSection, TEXT("AuthToken"), OriginalAuthToken, GEditorPerProjectIni);

		GConfig->SetBool(AuthSettingsSection, TEXT("bRequireAuthToken"), bInRequireAuthToken, GEditorPerProjectIni);
		GConfig->SetString(AuthSettingsSection, TEXT("AuthToken"), *InAuthToken, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	~FScopedAuthSettings()
	{
		if (bHadRequireAuthToken)
		{
			GConfig->SetBool(AuthSettingsSection, TEXT("bRequireAuthToken"), bOriginalRequireAuthToken, GEditorPerProjectIni);
		}
		else
		{
			GConfig->RemoveKey(AuthSettingsSection, TEXT("bRequireAuthToken"), GEditorPerProjectIni);
		}

		if (bHadAuthToken)
		{
			GConfig->SetString(AuthSettingsSection, TEXT("AuthToken"), *OriginalAuthToken, GEditorPerProjectIni);
		}
		else
		{
			GConfig->RemoveKey(AuthSettingsSection, TEXT("AuthToken"), GEditorPerProjectIni);
		}

		GConfig->Flush(false, GEditorPerProjectIni);
	}

private:
	bool bHadRequireAuthToken = false;
	bool bOriginalRequireAuthToken = false;
	bool bHadAuthToken = false;
	FString OriginalAuthToken;
};

class FScopedBoolSetting final
{
public:
	FScopedBoolSetting(const TCHAR* InKey, const bool bInValue)
		: Key(InKey)
	{
		bHadValue = GConfig->GetBool(AuthSettingsSection, Key, bOriginalValue, GEditorPerProjectIni);
		GConfig->SetBool(AuthSettingsSection, Key, bInValue, GEditorPerProjectIni);
		GConfig->Flush(false, GEditorPerProjectIni);
	}

	~FScopedBoolSetting()
	{
		if (bHadValue)
		{
			GConfig->SetBool(AuthSettingsSection, Key, bOriginalValue, GEditorPerProjectIni);
		}
		else
		{
			GConfig->RemoveKey(AuthSettingsSection, Key, GEditorPerProjectIni);
		}

		GConfig->Flush(false, GEditorPerProjectIni);
	}

private:
	const TCHAR* Key = nullptr;
	bool bHadValue = false;
	bool bOriginalValue = false;
};

struct FTestBlueprintAsset
{
	FString AssetPath;
	TObjectPtr<UBlueprint> Blueprint = nullptr;
};

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

	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}

	return nullptr;
}

static FString GetPrimaryEventGraphName(UBlueprint* Blueprint)
{
	return Blueprint && Blueprint->UbergraphPages.Num() > 0 && Blueprint->UbergraphPages[0] ? Blueprint->UbergraphPages[0]->GetName() : TEXT("EventGraph");
}

static FTestBlueprintAsset CreateTestBlueprint(FAutomationTestBase& Test, const FString& Suffix)
{
	const FString AssetName = FString::Printf(TEXT("BP_BridgeTest_%s_%s"), *Suffix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetPath = FString::Printf(TEXT("/Game/BlueprintBridgeTests/%s"), *AssetName);

	UPackage* Package = CreatePackage(*AssetPath);
	Test.TestNotNull(TEXT("CreatePackage should succeed."), Package);

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("BlueprintBridgeTests"));
	Test.TestNotNull(TEXT("CreateBlueprint should succeed."), Blueprint);

	if (Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	return { AssetPath, Blueprint };
}

static TSharedRef<FJsonObject> MakeAssetParams(const FString& AssetPath)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset"), AssetPath);
	return Params;
}

static TSharedRef<FJsonObject> MakeGraphParams(const FString& AssetPath, const FString& GraphName)
{
	TSharedRef<FJsonObject> Params = MakeAssetParams(AssetPath);
	Params->SetStringField(TEXT("graph"), GraphName);
	return Params;
}

// Pre-PR3 tests asserted against the per-node/per-pin verbose shape. Route this helper to
// DescribeGraphFull so those assertions keep working; tests that want the new summary-shaped
// DescribeGraph should call ExecuteJsonRequest(TEXT("DescribeGraph"), ...) directly.
static TSharedRef<FJsonObject> DescribeGraphRequest(const FString& AssetPath, const FString& GraphName)
{
	return ExecuteJsonRequest(TEXT("DescribeGraphFull"), MakeGraphParams(AssetPath, GraphName));
}

static TSharedRef<FJsonObject> DescribeNodeRequest(const FString& AssetPath, const FString& GraphName, const FString& NodeGuid)
{
	TSharedRef<FJsonObject> Params = MakeGraphParams(AssetPath, GraphName);
	Params->SetStringField(TEXT("node"), NodeGuid);
	return ExecuteJsonRequest(TEXT("DescribeNode"), Params);
}

static TSharedRef<FJsonObject> CompileBlueprintRequest(const FString& AssetPath)
{
	return ExecuteJsonRequest(TEXT("CompileBlueprint"), MakeAssetParams(AssetPath));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgePingTest, "BlueprintBridge.Protocol.Ping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgePingTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Ping"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	return TestEqual(TEXT("Ping should return Pong."), BlueprintBridgeTests::GetStringResult(*this, Response), FString(TEXT("Pong")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeEditorInfoTest, "BlueprintBridge.Protocol.EditorInfo", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeEditorInfoTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> ProjectResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("GetProjectName"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectSuccess(*this, ProjectResponse))
	{
		return false;
	}
	TestFalse(TEXT("Project name should not be empty."), BlueprintBridgeTests::GetStringResult(*this, ProjectResponse).IsEmpty());

	const TSharedRef<FJsonObject> VersionResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("GetEngineVersion"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectSuccess(*this, VersionResponse))
	{
		return false;
	}
	TestFalse(TEXT("Engine version should not be empty."), BlueprintBridgeTests::GetStringResult(*this, VersionResponse).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeInvalidJsonTest, "BlueprintBridge.Protocol.InvalidJson", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeInvalidJsonTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Response = BlueprintBridge::ExecuteRequest(TEXT("not-json"));
	return BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("InvalidJson"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeUnknownCommandTest, "BlueprintBridge.Protocol.UnknownCommand", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeUnknownCommandTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DefinitelyUnknownCommand"), MakeShared<FJsonObject>());
	return BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("UnknownCommand"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeUnknownCommandDidYouMeanTest, "BlueprintBridge.Protocol.UnknownCommandDidYouMean", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeUnknownCommandDidYouMeanTest::RunTest(const FString& Parameters)
{
	// Near-miss typo of a real command should surface a 'Did you mean' suggestion.
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGrafh"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("UnknownCommand")))
	{
		return false;
	}
	const FString Message = BlueprintBridgeTests::GetErrorMessage(Response);
	TestTrue(TEXT("Near-miss unknown command should include a 'Did you mean' suggestion."), Message.Contains(TEXT("Did you mean")));
	TestTrue(TEXT("Suggestion should point at DescribeGraph for the 'DescribeGrafh' typo."), Message.Contains(TEXT("DescribeGraph")));

	// A name with no close match should still error but should NOT include a misleading suggestion.
	const TSharedRef<FJsonObject> WildResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ZzzzNotCloseToAnything9999"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, WildResponse, TEXT("UnknownCommand")))
	{
		return false;
	}
	const FString WildMessage = BlueprintBridgeTests::GetErrorMessage(WildResponse);
	TestFalse(TEXT("Far-from-anything unknown command should NOT include a 'Did you mean' suggestion."), WildMessage.Contains(TEXT("Did you mean")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCaseInsensitiveCommandTest, "BlueprintBridge.Protocol.CaseInsensitiveCommand", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCaseInsensitiveCommandTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ping"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	return TestEqual(TEXT("Lowercase ping should return Pong."), BlueprintBridgeTests::GetStringResult(*this, Response), FString(TEXT("Pong")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeListCommandsTest, "BlueprintBridge.Protocol.ListCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeListCommandsTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ListCommands"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	if (!TestTrue(TEXT("ListCommands should return commands array."), (*Result)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr))
	{
		return false;
	}

	bool bFoundPing = false;
	bool bFoundDescribeBlueprint = false;
	bool bFoundDescribeCommand = false;
	for (const TSharedPtr<FJsonValue>& CommandValue : *Commands)
	{
		const TSharedPtr<FJsonObject>* CommandObject = nullptr;
		if (!TestTrue(TEXT("Each command should be an object."), CommandValue.IsValid() && CommandValue->TryGetObject(CommandObject) && CommandObject != nullptr && CommandObject->IsValid()))
		{
			return false;
		}

		FString Name;
		FString Description;
		FString Category;
		FString Risk;
		TestTrue(TEXT("Command should contain name."), (*CommandObject)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty());
		TestTrue(TEXT("Command should contain description."), (*CommandObject)->TryGetStringField(TEXT("description"), Description) && !Description.IsEmpty());
		TestTrue(TEXT("Command should contain category."), (*CommandObject)->TryGetStringField(TEXT("category"), Category) && !Category.IsEmpty());
		TestTrue(TEXT("Command should contain risk."), (*CommandObject)->TryGetStringField(TEXT("risk"), Risk) && !Risk.IsEmpty());

		bFoundPing |= Name == TEXT("Ping");
		bFoundDescribeBlueprint |= Name == TEXT("DescribeBlueprint");
		bFoundDescribeCommand |= Name == TEXT("DescribeCommand");
	}

	TestTrue(TEXT("ListCommands should include Ping."), bFoundPing);
	TestTrue(TEXT("ListCommands should include DescribeBlueprint."), bFoundDescribeBlueprint);
	return TestTrue(TEXT("ListCommands should include DescribeCommand."), bFoundDescribeCommand);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeDescribeCommandTest, "BlueprintBridge.Protocol.DescribeCommand", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeDescribeCommandTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("command"), TEXT("ping"));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeCommand"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}

	FString Name;
	FString Risk;
	TestTrue(TEXT("DescribeCommand should return canonical command name."), (*Result)->TryGetStringField(TEXT("name"), Name) && Name == TEXT("Ping"));
	TestTrue(TEXT("DescribeCommand should return command risk."), (*Result)->TryGetStringField(TEXT("risk"), Risk) && Risk == TEXT("ReadOnly"));

	const TSharedPtr<FJsonObject>* InputSchema = nullptr;
	if (!TestTrue(TEXT("DescribeCommand should return input schema."), (*Result)->TryGetObjectField(TEXT("inputSchema"), InputSchema) && InputSchema != nullptr && InputSchema->IsValid()))
	{
		return false;
	}

	FString SchemaType;
	if (!TestTrue(TEXT("Input schema should be an object schema."), (*InputSchema)->TryGetStringField(TEXT("type"), SchemaType) && SchemaType == TEXT("object")))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* OutputSchema = nullptr;
	if (!TestTrue(TEXT("DescribeCommand should return Ping output schema."), (*Result)->TryGetObjectField(TEXT("outputSchema"), OutputSchema) && OutputSchema != nullptr && OutputSchema->IsValid()))
	{
		return false;
	}

	FString OutputSchemaType;
	return TestTrue(TEXT("Ping output schema should describe a string result."), (*OutputSchema)->TryGetStringField(TEXT("type"), OutputSchemaType) && OutputSchemaType == TEXT("string"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeGenerateCommandDocsTest, "BlueprintBridge.Protocol.GenerateCommandDocs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeGenerateCommandDocsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("format"), TEXT("json"));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("GenerateCommandDocs"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}

	FString JsonPath;
	double CommandCount = 0.0;
	TestTrue(TEXT("GenerateCommandDocs should return jsonPath."), (*Result)->TryGetStringField(TEXT("jsonPath"), JsonPath) && !JsonPath.IsEmpty());
	return TestTrue(TEXT("GenerateCommandDocs should return commandCount."), (*Result)->TryGetNumberField(TEXT("commandCount"), CommandCount) && CommandCount > 0.0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCommandSchemaTest, "BlueprintBridge.Protocol.CommandSchemas", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCommandSchemaTest::RunTest(const FString& Parameters)
{
	auto ExpectRequiredFields = [this](const FString& CommandName, const TArray<FString>& ExpectedFields)
	{
		TSet<FString> RequiredFields;
		if (!BlueprintBridgeTests::GetCommandRequiredFields(*this, CommandName, RequiredFields))
		{
			return false;
		}

		bool bAllFound = true;
		for (const FString& ExpectedField : ExpectedFields)
		{
			bAllFound &= TestTrue(FString::Printf(TEXT("%s schema should require %s."), *CommandName, *ExpectedField), RequiredFields.Contains(ExpectedField));
		}
		return bAllFound;
	};

	bool bSuccess = true;
	bSuccess &= ExpectRequiredFields(TEXT("DescribeCommand"), { TEXT("command") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeBlueprint"), { TEXT("asset") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeGraph"), { TEXT("asset"), TEXT("graph") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeNode"), { TEXT("asset"), TEXT("graph"), TEXT("node") });
	bSuccess &= ExpectRequiredFields(TEXT("FindVariableReferences"), { TEXT("asset"), TEXT("variable") });
	bSuccess &= ExpectRequiredFields(TEXT("AnalyzeGraph"), { TEXT("asset"), TEXT("graph") });
	bSuccess &= ExpectRequiredFields(TEXT("SummarizeBlueprintGraph"), { TEXT("asset"), TEXT("graph") });
	bSuccess &= ExpectRequiredFields(TEXT("GetConnectedNodes"), { TEXT("asset"), TEXT("graph"), TEXT("node") });
	bSuccess &= ExpectRequiredFields(TEXT("FindExecutionPath"), { TEXT("asset"), TEXT("graph"), TEXT("from"), TEXT("to") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeSubgraph"), { TEXT("asset"), TEXT("graph"), TEXT("seeds") });
	bSuccess &= ExpectRequiredFields(TEXT("FindFunctionCallNodes"), { TEXT("asset"), TEXT("graph"), TEXT("function") });
	bSuccess &= ExpectRequiredFields(TEXT("FindVariableNodes"), { TEXT("asset"), TEXT("graph"), TEXT("variable") });
	bSuccess &= ExpectRequiredFields(TEXT("FindNodesByPin"), { TEXT("asset"), TEXT("graph"), TEXT("pin") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeClass"), { TEXT("class") });
	bSuccess &= ExpectRequiredFields(TEXT("FindFunctions"), { TEXT("class") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeFunction"), { TEXT("class"), TEXT("function") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeProperty"), { TEXT("class"), TEXT("property") });
	bSuccess &= ExpectRequiredFields(TEXT("DescribeDelegate"), { TEXT("class"), TEXT("delegate") });
	bSuccess &= ExpectRequiredFields(TEXT("CheckDelegateCompatibility"), { TEXT("function"), TEXT("delegateOwnerClass"), TEXT("delegate") });
	bSuccess &= ExpectRequiredFields(TEXT("FindReflectionSymbols"), { TEXT("query") });
	bSuccess &= ExpectRequiredFields(TEXT("ConnectPins"), { TEXT("asset"), TEXT("graph"), TEXT("fromNode"), TEXT("fromPin"), TEXT("toNode"), TEXT("toPin") });
	bSuccess &= ExpectRequiredFields(TEXT("SetPinDefault"), { TEXT("asset"), TEXT("graph"), TEXT("node"), TEXT("pin"), TEXT("value") });
	bSuccess &= ExpectRequiredFields(TEXT("SetNodePosition"), { TEXT("asset"), TEXT("graph"), TEXT("node"), TEXT("x"), TEXT("y") });
	bSuccess &= ExpectRequiredFields(TEXT("AddComponent"), { TEXT("asset"), TEXT("name"), TEXT("componentClass") });
	bSuccess &= ExpectRequiredFields(TEXT("AddFunctionCallNode"), { TEXT("asset"), TEXT("graph"), TEXT("x"), TEXT("y"), TEXT("functionClass"), TEXT("function") });
	bSuccess &= ExpectRequiredFields(TEXT("DuplicateFunctionGraph"), { TEXT("asset"), TEXT("sourceGraph"), TEXT("newGraph") });
	bSuccess &= ExpectRequiredFields(TEXT("SetUserDefinedPinFlags"), { TEXT("asset"), TEXT("graph"), TEXT("node"), TEXT("pin") });
	bSuccess &= ExpectRequiredFields(TEXT("ApplyGraphPatch"), { TEXT("asset"), TEXT("graph") });
	bSuccess &= ExpectRequiredFields(TEXT("ApplyFunctionPatch"), { TEXT("asset"), TEXT("function") });
	bSuccess &= ExpectRequiredFields(TEXT("ApplyGraphSnippet"), { TEXT("asset"), TEXT("graph"), TEXT("snippet") });
	bSuccess &= ExpectRequiredFields(TEXT("ExportGraphPatch"), { TEXT("asset"), TEXT("graph") });
	bSuccess &= ExpectRequiredFields(TEXT("ImportGraphPatch"), { TEXT("asset"), TEXT("graph"), TEXT("patch") });
	bSuccess &= ExpectRequiredFields(TEXT("AddWidget"), { TEXT("asset"), TEXT("name"), TEXT("widgetClass") });
	bSuccess &= ExpectRequiredFields(TEXT("AddBlueprintVariable"), { TEXT("asset"), TEXT("name"), TEXT("category") });

	const TSharedRef<FJsonObject> ListResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ListCommands"), MakeShared<FJsonObject>());
	if (!BlueprintBridgeTests::ExpectSuccess(*this, ListResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ListResult = BlueprintBridgeTests::GetResultObject(*this, ListResponse);
	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	if (!ListResult || !TestTrue(TEXT("ListCommands should return command list."), (*ListResult)->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& CommandValue : *Commands)
	{
		const TSharedPtr<FJsonObject>* CommandObject = nullptr;
		if (!TestTrue(TEXT("Command list item should be an object."), CommandValue.IsValid() && CommandValue->TryGetObject(CommandObject) && CommandObject != nullptr && CommandObject->IsValid()))
		{
			return false;
		}

		FString CommandName;
		if (!TestTrue(TEXT("Command list item should contain a name."), (*CommandObject)->TryGetStringField(TEXT("name"), CommandName) && !CommandName.IsEmpty()))
		{
			return false;
		}

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("command"), CommandName);
		const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeCommand"), Params);
		if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
		const TSharedPtr<FJsonObject>* InputSchema = nullptr;
		if (!DescribeResult || !TestTrue(FString::Printf(TEXT("%s should include inputSchema."), *CommandName), (*DescribeResult)->TryGetObjectField(TEXT("inputSchema"), InputSchema) && InputSchema != nullptr && InputSchema->IsValid()))
		{
			return false;
		}

		FString SchemaType;
		bSuccess &= TestTrue(FString::Printf(TEXT("%s inputSchema should be an object."), *CommandName), (*InputSchema)->TryGetStringField(TEXT("type"), SchemaType) && SchemaType == TEXT("object"));
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		bSuccess &= TestTrue(FString::Printf(TEXT("%s inputSchema should include properties."), *CommandName), (*InputSchema)->TryGetObjectField(TEXT("properties"), Properties) && Properties != nullptr && Properties->IsValid());
	}

	return bSuccess;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeReflectionCommandTest, "BlueprintBridge.Protocol.ReflectionCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeReflectionCommandTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> FunctionParams = MakeShared<FJsonObject>();
	FunctionParams->SetStringField(TEXT("class"), TEXT("/Script/Engine.KismetMathLibrary"));
	FunctionParams->SetStringField(TEXT("function"), TEXT("Add_DoubleDouble"));
	const TSharedRef<FJsonObject> FunctionResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeFunction"), FunctionParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, FunctionResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* FunctionResult = BlueprintBridgeTests::GetResultObject(*this, FunctionResponse);
	if (!FunctionResult)
	{
		return false;
	}

	bool bIsPureNode = false;
	bool bHasExecPins = true;
	TestTrue(TEXT("DescribeFunction should report pure math node."), (*FunctionResult)->TryGetBoolField(TEXT("isPureNode"), bIsPureNode) && bIsPureNode);
	TestTrue(TEXT("DescribeFunction should report no exec pins for pure math node."), (*FunctionResult)->TryGetBoolField(TEXT("hasExecPins"), bHasExecPins) && !bHasExecPins);
	const TArray<TSharedPtr<FJsonValue>>* FunctionParamsArray = nullptr;
	TestTrue(TEXT("DescribeFunction should return params."), (*FunctionResult)->TryGetArrayField(TEXT("params"), FunctionParamsArray) && FunctionParamsArray != nullptr && FunctionParamsArray->Num() > 0);

	TSharedRef<FJsonObject> FindParams = MakeShared<FJsonObject>();
	FindParams->SetStringField(TEXT("class"), TEXT("/Script/Engine.KismetMathLibrary"));
	FindParams->SetStringField(TEXT("nameContains"), TEXT("Float"));
	FindParams->SetBoolField(TEXT("blueprintCallableOnly"), true);
	const TSharedRef<FJsonObject> FindResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindFunctions"), FindParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, FindResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> PropertyParams = MakeShared<FJsonObject>();
	PropertyParams->SetStringField(TEXT("class"), TEXT("/Script/Engine.Actor"));
	PropertyParams->SetStringField(TEXT("property"), TEXT("PrimaryActorTick"));
	const TSharedRef<FJsonObject> PropertyResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeProperty"), PropertyParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, PropertyResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> DelegateParams = MakeShared<FJsonObject>();
	DelegateParams->SetStringField(TEXT("class"), TEXT("/Script/Engine.Actor"));
	DelegateParams->SetStringField(TEXT("delegate"), TEXT("OnDestroyed"));
	const TSharedRef<FJsonObject> DelegateResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeDelegate"), DelegateParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DelegateResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* DelegateResult = BlueprintBridgeTests::GetResultObject(*this, DelegateResponse);
	const TSharedPtr<FJsonObject>* Signature = nullptr;
	if (!DelegateResult || !TestTrue(TEXT("DescribeDelegate should include signature."), (*DelegateResult)->TryGetObjectField(TEXT("signature"), Signature) && Signature != nullptr && Signature->IsValid()))
	{
		return false;
	}

	TSharedRef<FJsonObject> CompatibilityParams = MakeShared<FJsonObject>();
	CompatibilityParams->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.Actor"));
	CompatibilityParams->SetStringField(TEXT("function"), TEXT("K2_DestroyActor"));
	CompatibilityParams->SetStringField(TEXT("delegateOwnerClass"), TEXT("/Script/Engine.Actor"));
	CompatibilityParams->SetStringField(TEXT("delegate"), TEXT("OnDestroyed"));
	const TSharedRef<FJsonObject> CompatibilityResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CheckDelegateCompatibility"), CompatibilityParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, CompatibilityResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* CompatibilityResult = BlueprintBridgeTests::GetResultObject(*this, CompatibilityResponse);
	bool bCompatible = true;
	TestTrue(TEXT("CheckDelegateCompatibility should report mismatch for incompatible function."), CompatibilityResult && (*CompatibilityResult)->TryGetBoolField(TEXT("compatible"), bCompatible) && !bCompatible);

	TSharedRef<FJsonObject> SymbolParams = MakeShared<FJsonObject>();
	SymbolParams->SetStringField(TEXT("query"), TEXT("Add_DoubleDouble"));
	SymbolParams->SetBoolField(TEXT("includeEngine"), true);
	SymbolParams->SetBoolField(TEXT("blueprintCallableOnly"), true);
	TArray<TSharedPtr<FJsonValue>> Kinds;
	Kinds.Add(MakeShared<FJsonValueString>(TEXT("function")));
	SymbolParams->SetArrayField(TEXT("kinds"), Kinds);
	const TSharedRef<FJsonObject> SymbolResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindReflectionSymbols"), SymbolParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, SymbolResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* SymbolResult = BlueprintBridgeTests::GetResultObject(*this, SymbolResponse);
	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	return SymbolResult && TestTrue(TEXT("FindReflectionSymbols should return matching results."), (*SymbolResult)->TryGetArrayField(TEXT("results"), Results) && Results != nullptr && Results->Num() > 0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSchemaValidationTest, "BlueprintBridge.Protocol.SchemaValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSchemaValidationTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FScopedBoolSetting ScopedValidation(TEXT("bValidateRequestsAgainstSchemas"), true);

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset"), TEXT("/Game/DoesNotMatter"));
	Params->SetNumberField(TEXT("graph"), 12.0);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraph"), Params);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("InvalidParams")))
	{
		return false;
	}

	return TestTrue(TEXT("Validation error should mention graph type."), BlueprintBridgeTests::GetErrorMessage(Response).Contains(TEXT("params.graph must be a string")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeDescribeUnknownCommandTest, "BlueprintBridge.Protocol.DescribeUnknownCommand", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeDescribeUnknownCommandTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("command"), TEXT("DefinitelyUnknownCommand"));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeCommand"), Params);
	return BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("CommandNotFound"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeUnauthorizedTest, "BlueprintBridge.Protocol.Unauthorized", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeUnauthorizedTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FScopedAuthSettings ScopedAuthSettings(true, TEXT("bridge-test-token"));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Ping"), MakeShared<FJsonObject>());
	return BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("Unauthorized"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeInspectionAndVariableReferenceTest, "BlueprintBridge.Blueprint.InspectionAndVariableReferences", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeInspectionAndVariableReferenceTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Inspect"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("int"));
	AddVariableParams->SetStringField(TEXT("defaultValue"), TEXT("100"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> DescribeBlueprintParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	const TSharedRef<FJsonObject> DescribeBlueprintResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), DescribeBlueprintParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeBlueprintResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* BlueprintResult = BlueprintBridgeTests::GetResultObject(*this, DescribeBlueprintResponse);
	if (!BlueprintResult)
	{
		return false;
	}
	TestTrue(TEXT("DescribeBlueprint should include variables."), (*BlueprintResult)->HasTypedField<EJson::Array>(TEXT("variables")));
	TestTrue(TEXT("DescribeBlueprint should include graphs."), (*BlueprintResult)->HasTypedField<EJson::Array>(TEXT("graphs")));

	TSharedRef<FJsonObject> AddGetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddGetParams->SetStringField(TEXT("variable"), TEXT("Health"));
	AddGetParams->SetNumberField(TEXT("x"), 100);
	AddGetParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> AddGetResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableGetNode"), AddGetParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddGetResponse))
	{
		return false;
	}
	const FString GetNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *BlueprintBridgeTests::GetNodeObjectFromResponse(*this, AddGetResponse));

	TSharedRef<FJsonObject> AddSetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddSetParams->SetStringField(TEXT("variable"), TEXT("Health"));
	AddSetParams->SetNumberField(TEXT("x"), 360);
	AddSetParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> AddSetResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableSetNode"), AddSetParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddSetResponse))
	{
		return false;
	}

	const TSharedRef<FJsonObject> DescribeGraphResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, EventGraphName);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeGraphResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* GraphResult = BlueprintBridgeTests::GetResultObject(*this, DescribeGraphResponse);
	if (!GraphResult)
	{
		return false;
	}
	TestTrue(TEXT("DescribeGraph should include nodes."), (*GraphResult)->HasTypedField<EJson::Array>(TEXT("nodes")));

	const TSharedRef<FJsonObject> DescribeNodeResponse = BlueprintBridgeTests::DescribeNodeRequest(Asset.AssetPath, EventGraphName, GetNodeGuid);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeNodeResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> FindNodesParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	FindNodesParams->SetStringField(TEXT("graph"), EventGraphName);
	FindNodesParams->SetStringField(TEXT("variable"), TEXT("Health"));
	const TSharedRef<FJsonObject> FindNodesResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindNodes"), FindNodesParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, FindNodesResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FindNodesResult = BlueprintBridgeTests::GetResultObject(*this, FindNodesResponse);
	if (!FindNodesResult)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* FoundNodes = nullptr;
	TestTrue(TEXT("FindNodes should return a nodes array."), (*FindNodesResult)->TryGetArrayField(TEXT("nodes"), FoundNodes) && FoundNodes != nullptr);
	TestTrue(TEXT("FindNodes should find at least two Health nodes."), FoundNodes && FoundNodes->Num() >= 2);

	TSharedRef<FJsonObject> FindRefsParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	FindRefsParams->SetStringField(TEXT("variable"), TEXT("Health"));
	const TSharedRef<FJsonObject> FindRefsResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindVariableReferences"), FindRefsParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, FindRefsResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FindRefsResult = BlueprintBridgeTests::GetResultObject(*this, FindRefsResponse);
	if (!FindRefsResult)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* References = nullptr;
	TestTrue(TEXT("FindVariableReferences should return references."), (*FindRefsResult)->TryGetArrayField(TEXT("references"), References) && References != nullptr);
	TestTrue(TEXT("FindVariableReferences should find at least two references."), References && References->Num() >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeGraphNodeCommandTest, "BlueprintBridge.Blueprint.GraphNodeCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeGraphNodeCommandTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Nodes"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("int"));
	AddVariableParams->SetStringField(TEXT("defaultValue"), TEXT("100"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> BranchParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	BranchParams->SetNumberField(TEXT("x"), 100);
	BranchParams->SetNumberField(TEXT("y"), 50);
	const TSharedRef<FJsonObject> BranchResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBranchNode"), BranchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BranchResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> CommentParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	CommentParams->SetStringField(TEXT("text"), TEXT("Bridge comment"));
	CommentParams->SetNumberField(TEXT("x"), 40);
	CommentParams->SetNumberField(TEXT("y"), 300);
	CommentParams->SetNumberField(TEXT("width"), 500);
	CommentParams->SetNumberField(TEXT("height"), 220);
	const TSharedRef<FJsonObject> CommentResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddCommentNode"), CommentParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, CommentResponse))
	{
		return false;
	}
	const FString CommentNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *BlueprintBridgeTests::GetNodeObjectFromResponse(*this, CommentResponse));

	TSharedRef<FJsonObject> RerouteParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	RerouteParams->SetNumberField(TEXT("x"), 600);
	RerouteParams->SetNumberField(TEXT("y"), 80);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddRerouteNode"), RerouteParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> SequenceParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	SequenceParams->SetNumberField(TEXT("x"), 820);
	SequenceParams->SetNumberField(TEXT("y"), 40);
	SequenceParams->SetNumberField(TEXT("extraOutputs"), 1);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddSequenceNode"), SequenceParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> EnumEqualityParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	EnumEqualityParams->SetNumberField(TEXT("x"), 1000);
	EnumEqualityParams->SetNumberField(TEXT("y"), 220);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddEnumEqualityNode"), EnumEqualityParams)))
	{
		return false;
	}

	UEnum* NetRoleEnum = StaticEnum<ENetRole>();
	TestNotNull(TEXT("ENetRole enum should exist."), NetRoleEnum);
	TSharedRef<FJsonObject> EnumSwitchParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	EnumSwitchParams->SetStringField(TEXT("enum"), NetRoleEnum ? NetRoleEnum->GetPathName() : TEXT("/Script/Engine.ENetRole"));
	EnumSwitchParams->SetNumberField(TEXT("x"), 1200);
	EnumSwitchParams->SetNumberField(TEXT("y"), 220);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddEnumSwitchNode"), EnumSwitchParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> CallNodeParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	CallNodeParams->SetStringField(TEXT("functionClass"), UKismetMathLibrary::StaticClass()->GetPathName());
	CallNodeParams->SetStringField(TEXT("function"), TEXT("EqualEqual_IntInt"));
	CallNodeParams->SetNumberField(TEXT("x"), 1400);
	CallNodeParams->SetNumberField(TEXT("y"), 200);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddFunctionCallNode"), CallNodeParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> PositionParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	PositionParams->SetStringField(TEXT("node"), CommentNodeGuid);
	PositionParams->SetNumberField(TEXT("x"), 222);
	PositionParams->SetNumberField(TEXT("y"), 444);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetNodePosition"), PositionParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> PositionedCommentResponse = BlueprintBridgeTests::DescribeNodeRequest(Asset.AssetPath, EventGraphName, CommentNodeGuid);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, PositionedCommentResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* PositionedCommentResult = BlueprintBridgeTests::GetResultObject(*this, PositionedCommentResponse);
	if (!PositionedCommentResult)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* CommentNodeObject = nullptr;
	TestTrue(TEXT("DescribeNode should return a node object."), (*PositionedCommentResult)->TryGetObjectField(TEXT("node"), CommentNodeObject) && CommentNodeObject != nullptr && CommentNodeObject->IsValid());
	if (CommentNodeObject && *CommentNodeObject)
	{
		double X = 0.0;
		double Y = 0.0;
		(*CommentNodeObject)->TryGetNumberField(TEXT("x"), X);
		(*CommentNodeObject)->TryGetNumberField(TEXT("y"), Y);
		TestEqual(TEXT("Comment node X should update."), static_cast<int32>(X), 222);
		TestEqual(TEXT("Comment node Y should update."), static_cast<int32>(Y), 444);
	}

	TSharedRef<FJsonObject> DeleteNodeParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	DeleteNodeParams->SetStringField(TEXT("node"), CommentNodeGuid);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DeleteNode"), DeleteNodeParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> DeletedNodeResponse = BlueprintBridgeTests::DescribeNodeRequest(Asset.AssetPath, EventGraphName, CommentNodeGuid);
	return BlueprintBridgeTests::ExpectErrorCode(*this, DeletedNodeResponse, TEXT("NodeNotFound"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeApplyGraphPatchTest, "BlueprintBridge.Blueprint.ApplyGraphPatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeApplyGraphPatchTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("GraphPatch"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> CreateFunctionParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	CreateFunctionParams->SetStringField(TEXT("function"), TEXT("PatchFunction"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateFunctionGraph"), CreateFunctionParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> PatchParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("PatchFunction"));
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TSharedRef<FJsonObject> BranchNode = MakeShared<FJsonObject>();
	BranchNode->SetStringField(TEXT("id"), TEXT("branch"));
	BranchNode->SetStringField(TEXT("type"), TEXT("Branch"));
	BranchNode->SetNumberField(TEXT("x"), 100);
	BranchNode->SetNumberField(TEXT("y"), 0);
	Nodes.Add(MakeShared<FJsonValueObject>(BranchNode));
	TSharedRef<FJsonObject> SequenceNode = MakeShared<FJsonObject>();
	SequenceNode->SetStringField(TEXT("id"), TEXT("sequence"));
	SequenceNode->SetStringField(TEXT("type"), TEXT("Sequence"));
	SequenceNode->SetNumberField(TEXT("x"), 360);
	SequenceNode->SetNumberField(TEXT("y"), 0);
	Nodes.Add(MakeShared<FJsonValueObject>(SequenceNode));
	PatchParams->SetArrayField(TEXT("nodes"), Nodes);

	TArray<TSharedPtr<FJsonValue>> Defaults;
	TSharedRef<FJsonObject> ConditionDefault = MakeShared<FJsonObject>();
	ConditionDefault->SetStringField(TEXT("node"), TEXT("branch"));
	ConditionDefault->SetStringField(TEXT("pin"), TEXT("Condition"));
	ConditionDefault->SetStringField(TEXT("value"), TEXT("true"));
	Defaults.Add(MakeShared<FJsonValueObject>(ConditionDefault));
	PatchParams->SetArrayField(TEXT("defaults"), Defaults);

	TArray<TSharedPtr<FJsonValue>> Links;
	TSharedRef<FJsonObject> BranchToSequence = MakeShared<FJsonObject>();
	BranchToSequence->SetStringField(TEXT("from"), TEXT("branch.then"));
	BranchToSequence->SetStringField(TEXT("to"), TEXT("sequence.execute"));
	Links.Add(MakeShared<FJsonValueObject>(BranchToSequence));
	PatchParams->SetArrayField(TEXT("links"), Links);

	const TSharedRef<FJsonObject> PatchResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplyGraphPatch"), PatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, PatchResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* PatchResult = BlueprintBridgeTests::GetResultObject(*this, PatchResponse);
	const TArray<TSharedPtr<FJsonValue>>* LinkResults = nullptr;
	TestTrue(TEXT("ApplyGraphPatch should report link results."), PatchResult && (*PatchResult)->TryGetArrayField(TEXT("links"), LinkResults) && LinkResults != nullptr && LinkResults->Num() == 1);

	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, TEXT("PatchFunction"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	const TArray<TSharedPtr<FJsonValue>>* GraphNodes = nullptr;
	bool bFoundBranchDefault = false;
	int32 BranchCount = 0;
	if (DescribeResult && (*DescribeResult)->TryGetArrayField(TEXT("nodes"), GraphNodes) && GraphNodes != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *GraphNodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || NodeObject == nullptr || !NodeObject->IsValid())
			{
				continue;
			}

			FString ClassName;
			(*NodeObject)->TryGetStringField(TEXT("class"), ClassName);
			if (ClassName.Contains(TEXT("K2Node_IfThenElse")))
			{
				++BranchCount;
				if (const TSharedPtr<FJsonObject> ConditionPin = BlueprintBridgeTests::FindPinObjectByName(*NodeObject, TEXT("Condition")))
				{
					FString DefaultValue;
					bFoundBranchDefault |= ConditionPin->TryGetStringField(TEXT("defaultValue"), DefaultValue) && DefaultValue == TEXT("true");
				}
			}
		}
	}
	TestTrue(TEXT("ApplyGraphPatch should set pin defaults."), bFoundBranchDefault);

	TSharedRef<FJsonObject> BadPatchParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("PatchFunction"));
	BadPatchParams->SetBoolField(TEXT("rollbackOnFailure"), true);
	TArray<TSharedPtr<FJsonValue>> BadNodes;
	TSharedRef<FJsonObject> BadBranchNode = MakeShared<FJsonObject>();
	BadBranchNode->SetStringField(TEXT("id"), TEXT("bad_branch"));
	BadBranchNode->SetStringField(TEXT("type"), TEXT("Branch"));
	BadBranchNode->SetNumberField(TEXT("x"), 600);
	BadBranchNode->SetNumberField(TEXT("y"), 0);
	BadNodes.Add(MakeShared<FJsonValueObject>(BadBranchNode));
	BadPatchParams->SetArrayField(TEXT("nodes"), BadNodes);
	TArray<TSharedPtr<FJsonValue>> BadLinks;
	TSharedRef<FJsonObject> BadLink = MakeShared<FJsonObject>();
	BadLink->SetStringField(TEXT("from"), TEXT("bad_branch.not_a_pin"));
	BadLink->SetStringField(TEXT("to"), TEXT("sequence.execute"));
	BadLinks.Add(MakeShared<FJsonValueObject>(BadLink));
	BadPatchParams->SetArrayField(TEXT("links"), BadLinks);
	const TSharedRef<FJsonObject> BadPatchResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplyGraphPatch"), BadPatchParams);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, BadPatchResponse, TEXT("GraphPatchFailed")))
	{
		return false;
	}

	const TSharedRef<FJsonObject> RollbackDescribeResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, TEXT("PatchFunction"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, RollbackDescribeResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* RollbackDescribeResult = BlueprintBridgeTests::GetResultObject(*this, RollbackDescribeResponse);
	const TArray<TSharedPtr<FJsonValue>>* RollbackNodes = nullptr;
	int32 RollbackBranchCount = 0;
	if (RollbackDescribeResult && (*RollbackDescribeResult)->TryGetArrayField(TEXT("nodes"), RollbackNodes) && RollbackNodes != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *RollbackNodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (NodeValue.IsValid() && NodeValue->TryGetObject(NodeObject) && NodeObject != nullptr && NodeObject->IsValid())
			{
				FString ClassName;
				(*NodeObject)->TryGetStringField(TEXT("class"), ClassName);
				if (ClassName.Contains(TEXT("K2Node_IfThenElse")))
				{
					++RollbackBranchCount;
				}
			}
		}
	}
	return TestEqual(TEXT("ApplyGraphPatch rollback should remove nodes created by failed patch."), RollbackBranchCount, BranchCount);
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeGraphQueryCommandTest, "BlueprintBridge.Blueprint.GraphQueryCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeGraphQueryCommandTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("GraphQueries"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> FunctionParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	FunctionParams->SetStringField(TEXT("function"), TEXT("QueryFunction"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateFunctionGraph"), FunctionParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> PatchParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("QueryFunction"));
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TSharedRef<FJsonObject> BranchNode = MakeShared<FJsonObject>();
	BranchNode->SetStringField(TEXT("id"), TEXT("branch"));
	BranchNode->SetStringField(TEXT("type"), TEXT("Branch"));
	BranchNode->SetNumberField(TEXT("x"), 100);
	BranchNode->SetNumberField(TEXT("y"), 0);
	Nodes.Add(MakeShared<FJsonValueObject>(BranchNode));
	TSharedRef<FJsonObject> SequenceNode = MakeShared<FJsonObject>();
	SequenceNode->SetStringField(TEXT("id"), TEXT("sequence"));
	SequenceNode->SetStringField(TEXT("type"), TEXT("Sequence"));
	SequenceNode->SetNumberField(TEXT("x"), 360);
	SequenceNode->SetNumberField(TEXT("y"), 0);
	Nodes.Add(MakeShared<FJsonValueObject>(SequenceNode));
	PatchParams->SetArrayField(TEXT("nodes"), Nodes);

	TArray<TSharedPtr<FJsonValue>> Links;
	TSharedRef<FJsonObject> BranchToSequence = MakeShared<FJsonObject>();
	BranchToSequence->SetStringField(TEXT("from"), TEXT("branch.true"));
	BranchToSequence->SetStringField(TEXT("to"), TEXT("sequence.exec"));
	Links.Add(MakeShared<FJsonValueObject>(BranchToSequence));
	PatchParams->SetArrayField(TEXT("links"), Links);

	const TSharedRef<FJsonObject> PatchResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplyGraphPatch"), PatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, PatchResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> SummaryParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("QueryFunction"));
	const TSharedRef<FJsonObject> SummaryResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SummarizeBlueprintGraph"), SummaryParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, SummaryResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* SummaryResult = BlueprintBridgeTests::GetResultObject(*this, SummaryResponse);
	const TArray<TSharedPtr<FJsonValue>>* Branches = nullptr;
	TestTrue(TEXT("Summary should include branch descriptions."), SummaryResult && (*SummaryResult)->TryGetArrayField(TEXT("branches"), Branches) && Branches != nullptr && Branches->Num() > 0);

	TSharedRef<FJsonObject> ConnectedParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("QueryFunction"));
	ConnectedParams->SetStringField(TEXT("node"), TEXT("branch"));
	ConnectedParams->SetStringField(TEXT("direction"), TEXT("Downstream"));
	ConnectedParams->SetNumberField(TEXT("depth"), 1);
	const TSharedRef<FJsonObject> ConnectedResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("GetConnectedNodes"), ConnectedParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, ConnectedResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ConnectedResult = BlueprintBridgeTests::GetResultObject(*this, ConnectedResponse);
	const TArray<TSharedPtr<FJsonValue>>* ConnectedNodes = nullptr;
	TestTrue(TEXT("Connected query should find downstream nodes."), ConnectedResult && (*ConnectedResult)->TryGetArrayField(TEXT("nodes"), ConnectedNodes) && ConnectedNodes != nullptr && ConnectedNodes->Num() > 0);

	TSharedRef<FJsonObject> PathParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("QueryFunction"));
	PathParams->SetStringField(TEXT("from"), TEXT("branch"));
	PathParams->SetStringField(TEXT("to"), TEXT("sequence"));
	const TSharedRef<FJsonObject> PathResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindExecutionPath"), PathParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, PathResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* PathResult = BlueprintBridgeTests::GetResultObject(*this, PathResponse);
	bool bFoundPath = false;
	TestTrue(TEXT("Execution path should be found."), PathResult && (*PathResult)->TryGetBoolField(TEXT("found"), bFoundPath) && bFoundPath);

	TSharedRef<FJsonObject> SubgraphParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("QueryFunction"));
	TArray<TSharedPtr<FJsonValue>> Seeds;
	Seeds.Add(MakeShared<FJsonValueString>(TEXT("branch")));
	SubgraphParams->SetArrayField(TEXT("seeds"), Seeds);
	SubgraphParams->SetNumberField(TEXT("depth"), 1);
	return BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeSubgraph"), SubgraphParams));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgePinEditingCommandTest, "BlueprintBridge.Blueprint.PinEditingCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgePinEditingCommandTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Pins"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("int"));
	AddVariableParams->SetStringField(TEXT("defaultValue"), TEXT("100"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddGetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddGetParams->SetStringField(TEXT("variable"), TEXT("Health"));
	AddGetParams->SetNumberField(TEXT("x"), 100);
	AddGetParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> AddGetResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableGetNode"), AddGetParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddGetResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* GetNodeObject = BlueprintBridgeTests::GetNodeObjectFromResponse(*this, AddGetResponse);
	if (!GetNodeObject)
	{
		return false;
	}
	const FString GetNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *GetNodeObject);
	const FString GetOutputPinName = BlueprintBridgeTests::FindPinName(*GetNodeObject, TEXT("Output"), TEXT("int"), TEXT("Health"));

	TSharedRef<FJsonObject> AddCallParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddCallParams->SetStringField(TEXT("functionClass"), UKismetMathLibrary::StaticClass()->GetPathName());
	AddCallParams->SetStringField(TEXT("function"), TEXT("EqualEqual_IntInt"));
	AddCallParams->SetNumberField(TEXT("x"), 420);
	AddCallParams->SetNumberField(TEXT("y"), 90);
	const TSharedRef<FJsonObject> AddCallResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddFunctionCallNode"), AddCallParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddCallResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* CallNodeObject = BlueprintBridgeTests::GetNodeObjectFromResponse(*this, AddCallResponse);
	if (!CallNodeObject)
	{
		return false;
	}
	const FString CallNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *CallNodeObject);
	const FString InputAPin = BlueprintBridgeTests::FindPinName(*CallNodeObject, TEXT("Input"), TEXT("int"), TEXT("A"));
	const FString InputBPin = BlueprintBridgeTests::FindPinName(*CallNodeObject, TEXT("Input"), TEXT("int"), TEXT("B"));

	TSharedRef<FJsonObject> ConnectParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	ConnectParams->SetStringField(TEXT("fromNode"), GetNodeGuid);
	ConnectParams->SetStringField(TEXT("fromPin"), GetOutputPinName);
	ConnectParams->SetStringField(TEXT("toNode"), CallNodeGuid);
	ConnectParams->SetStringField(TEXT("toPin"), InputAPin);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ConnectPins"), ConnectParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> SetDefaultParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	SetDefaultParams->SetStringField(TEXT("node"), CallNodeGuid);
	SetDefaultParams->SetStringField(TEXT("pin"), InputBPin);
	SetDefaultParams->SetStringField(TEXT("value"), TEXT("5"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetPinDefault"), SetDefaultParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> MoveParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	MoveParams->SetStringField(TEXT("fromNode"), CallNodeGuid);
	MoveParams->SetStringField(TEXT("fromPin"), InputAPin);
	MoveParams->SetStringField(TEXT("toNode"), CallNodeGuid);
	MoveParams->SetStringField(TEXT("toPin"), InputBPin);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("MovePinLinks"), MoveParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> AfterMoveDescribeResponse = BlueprintBridgeTests::DescribeNodeRequest(Asset.AssetPath, EventGraphName, CallNodeGuid);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AfterMoveDescribeResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* AfterMoveResult = BlueprintBridgeTests::GetResultObject(*this, AfterMoveDescribeResponse);
	if (!AfterMoveResult)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* AfterMoveNodeObject = nullptr;
	TestTrue(TEXT("DescribeNode should return node object after move."), (*AfterMoveResult)->TryGetObjectField(TEXT("node"), AfterMoveNodeObject) && AfterMoveNodeObject != nullptr && AfterMoveNodeObject->IsValid());
	if (AfterMoveNodeObject && *AfterMoveNodeObject)
	{
		TestFalse(TEXT("Input A should no longer link to the getter node after MovePinLinks."), BlueprintBridgeTests::PinLinksToNode(*AfterMoveNodeObject, InputAPin, GetNodeGuid));
		TestTrue(TEXT("Input B should link to the getter node after MovePinLinks."), BlueprintBridgeTests::PinLinksToNode(*AfterMoveNodeObject, InputBPin, GetNodeGuid));
	}

	TSharedRef<FJsonObject> BreakParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	BreakParams->SetStringField(TEXT("node"), CallNodeGuid);
	BreakParams->SetStringField(TEXT("pin"), InputBPin);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("BreakPinLinks"), BreakParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddRerouteParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddRerouteParams->SetNumberField(TEXT("x"), 760);
	AddRerouteParams->SetNumberField(TEXT("y"), 90);
	const TSharedRef<FJsonObject> AddRerouteResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddRerouteNode"), AddRerouteParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddRerouteResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* RerouteNodeObject = BlueprintBridgeTests::GetNodeObjectFromResponse(*this, AddRerouteResponse);
	if (!RerouteNodeObject)
	{
		return false;
	}
	const FString RerouteNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *RerouteNodeObject);
	const FString RerouteInputPin = BlueprintBridgeTests::FindPinName(*RerouteNodeObject, TEXT("Input"));

	TSharedRef<FJsonObject> CopyTypeParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	CopyTypeParams->SetStringField(TEXT("fromNode"), CallNodeGuid);
	CopyTypeParams->SetStringField(TEXT("fromPin"), InputAPin);
	CopyTypeParams->SetStringField(TEXT("toNode"), RerouteNodeGuid);
	CopyTypeParams->SetStringField(TEXT("toPin"), RerouteInputPin);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CopyPinType"), CopyTypeParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> RerouteDescribeResponse = BlueprintBridgeTests::DescribeNodeRequest(Asset.AssetPath, EventGraphName, RerouteNodeGuid);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, RerouteDescribeResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* RerouteDescribeResult = BlueprintBridgeTests::GetResultObject(*this, RerouteDescribeResponse);
	if (!RerouteDescribeResult)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* RerouteDescribeNode = nullptr;
	TestTrue(TEXT("DescribeNode should return reroute node."), (*RerouteDescribeResult)->TryGetObjectField(TEXT("node"), RerouteDescribeNode) && RerouteDescribeNode != nullptr && RerouteDescribeNode->IsValid());
	if (RerouteDescribeNode && *RerouteDescribeNode)
	{
		const TSharedPtr<FJsonObject> PinObject = BlueprintBridgeTests::FindPinObjectByName(*RerouteDescribeNode, RerouteInputPin);
		if (PinObject.IsValid())
		{
			FString Category;
			PinObject->TryGetStringField(TEXT("category"), Category);
			TestEqual(TEXT("CopyPinType should make the reroute pin an int pin."), Category, FString(TEXT("int")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFunctionGraphCommandTest, "BlueprintBridge.Blueprint.FunctionGraphCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFunctionGraphCommandTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Functions"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("int"));
	AddVariableParams->SetStringField(TEXT("defaultValue"), TEXT("100"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> CreateFunctionParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	CreateFunctionParams->SetStringField(TEXT("function"), TEXT("BridgeFunction"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateFunctionGraph"), CreateFunctionParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddInputParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("BridgeFunction"));
	AddInputParams->SetStringField(TEXT("name"), TEXT("Enabled"));
	AddInputParams->SetStringField(TEXT("category"), TEXT("bool"));
	const TSharedRef<FJsonObject> AddInputResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddFunctionInput"), AddInputParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddInputResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* EntryNodeObject = BlueprintBridgeTests::GetNodeObjectFromResponse(*this, AddInputResponse);
	if (!EntryNodeObject)
	{
		return false;
	}
	const FString EntryNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *EntryNodeObject);
	const FString EntryPinName = BlueprintBridgeTests::FindPinName(*EntryNodeObject, TEXT("Output"), TEXT("bool"), TEXT("Enabled"));

	TSharedRef<FJsonObject> AddOutputParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("BridgeFunction"));
	AddOutputParams->SetStringField(TEXT("name"), TEXT("HealthOut"));
	AddOutputParams->SetStringField(TEXT("sourceVariable"), TEXT("Health"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddFunctionOutput"), AddOutputParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> EditPinParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("BridgeFunction"));
	EditPinParams->SetStringField(TEXT("node"), EntryNodeGuid);
	EditPinParams->SetStringField(TEXT("pin"), EntryPinName.IsEmpty() ? TEXT("Enabled") : EntryPinName);
	EditPinParams->SetStringField(TEXT("newName"), TEXT("bEnabled"));
	EditPinParams->SetStringField(TEXT("category"), TEXT("bool"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("EditUserDefinedPin"), EditPinParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> PinFlagsParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("BridgeFunction"));
	PinFlagsParams->SetStringField(TEXT("node"), EntryNodeGuid);
	PinFlagsParams->SetStringField(TEXT("pin"), TEXT("bEnabled"));
	PinFlagsParams->SetBoolField(TEXT("byRef"), true);
	PinFlagsParams->SetBoolField(TEXT("isConst"), true);
	const TSharedRef<FJsonObject> PinFlagsResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetUserDefinedPinFlags"), PinFlagsParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, PinFlagsResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* PinFlagsNode = BlueprintBridgeTests::GetNodeObjectFromResponse(*this, PinFlagsResponse);
	const TSharedPtr<FJsonObject> FlaggedPin = PinFlagsNode ? BlueprintBridgeTests::FindPinObjectByName(*PinFlagsNode, TEXT("bEnabled")) : nullptr;
	bool bByRef = false;
	bool bIsConst = false;
	TestTrue(TEXT("SetUserDefinedPinFlags should set byRef."), FlaggedPin.IsValid() && FlaggedPin->TryGetBoolField(TEXT("byRef"), bByRef) && bByRef);
	TestTrue(TEXT("SetUserDefinedPinFlags should set isConst."), FlaggedPin.IsValid() && FlaggedPin->TryGetBoolField(TEXT("isConst"), bIsConst) && bIsConst);

	const TSharedRef<FJsonObject> DescribeFunctionGraphResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, TEXT("BridgeFunction"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeFunctionGraphResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> AnalyzeParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("BridgeFunction"));
	const TSharedRef<FJsonObject> AnalyzeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AnalyzeGraph"), AnalyzeParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AnalyzeResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* AnalyzeResult = BlueprintBridgeTests::GetResultObject(*this, AnalyzeResponse);
	const TArray<TSharedPtr<FJsonValue>>* AnalyzedNodes = nullptr;
	TestTrue(TEXT("AnalyzeGraph should return nodes."), AnalyzeResult && (*AnalyzeResult)->TryGetArrayField(TEXT("nodes"), AnalyzedNodes) && AnalyzedNodes != nullptr && AnalyzedNodes->Num() > 0);

	TSharedRef<FJsonObject> DuplicateFunctionParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	DuplicateFunctionParams->SetStringField(TEXT("sourceGraph"), TEXT("BridgeFunction"));
	DuplicateFunctionParams->SetStringField(TEXT("newGraph"), TEXT("BridgeFunctionCopy"));
	TSharedRef<FJsonObject> PinRenames = MakeShared<FJsonObject>();
	PinRenames->SetStringField(TEXT("bEnabled"), TEXT("bCopied"));
	DuplicateFunctionParams->SetObjectField(TEXT("pinRenames"), PinRenames);
	const TSharedRef<FJsonObject> DuplicateFunctionResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DuplicateFunctionGraph"), DuplicateFunctionParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DuplicateFunctionResponse))
	{
		return false;
	}

	const TSharedRef<FJsonObject> DuplicateDescribeResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, TEXT("BridgeFunctionCopy"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DuplicateDescribeResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* DuplicateResult = BlueprintBridgeTests::GetResultObject(*this, DuplicateDescribeResponse);
	const TArray<TSharedPtr<FJsonValue>>* DuplicateNodes = nullptr;
	bool bFoundCopiedPin = false;
	if (DuplicateResult && (*DuplicateResult)->TryGetArrayField(TEXT("nodes"), DuplicateNodes) && DuplicateNodes != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *DuplicateNodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (NodeValue.IsValid() && NodeValue->TryGetObject(NodeObject) && NodeObject != nullptr && NodeObject->IsValid())
			{
				bFoundCopiedPin |= BlueprintBridgeTests::FindPinObjectByName(*NodeObject, TEXT("bCopied")).IsValid();
			}
		}
	}
	TestTrue(TEXT("Duplicated function should apply pin renames."), bFoundCopiedPin);

	TSharedRef<FJsonObject> RenameGraphParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	RenameGraphParams->SetStringField(TEXT("graph"), TEXT("BridgeFunction"));
	RenameGraphParams->SetStringField(TEXT("newName"), TEXT("BridgeFunctionRenamed"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("RenameGraph"), RenameGraphParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> RenamedGraphDescribeResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, TEXT("BridgeFunctionRenamed"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, RenamedGraphDescribeResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> GetterFunctionParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	GetterFunctionParams->SetStringField(TEXT("function"), TEXT("GetHealthViaBridge"));
	GetterFunctionParams->SetStringField(TEXT("variable"), TEXT("Health"));
	const TSharedRef<FJsonObject> GetterFunctionResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableGetterFunction"), GetterFunctionParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, GetterFunctionResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* GetterFunctionResult = BlueprintBridgeTests::GetResultObject(*this, GetterFunctionResponse);
	if (!GetterFunctionResult)
	{
		return false;
	}
	FString CompileStatus;
	TestTrue(TEXT("AddVariableGetterFunction should include compileStatus."), (*GetterFunctionResult)->TryGetStringField(TEXT("compileStatus"), CompileStatus));
	TestTrue(TEXT("Getter function should compile successfully."), CompileStatus == TEXT("UpToDate") || CompileStatus == TEXT("UpToDateWithWarnings"));

	TSharedRef<FJsonObject> DeleteGraphParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	DeleteGraphParams->SetStringField(TEXT("graph"), TEXT("BridgeFunctionRenamed"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DeleteGraph"), DeleteGraphParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> DeletedGraphDescribeResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, TEXT("BridgeFunctionRenamed"));
	return BlueprintBridgeTests::ExpectErrorCode(*this, DeletedGraphDescribeResponse, TEXT("GraphNotFound"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCustomEventAndEventGraphCommandTest, "BlueprintBridge.Blueprint.CustomEventAndEventGraphCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCustomEventAndEventGraphCommandTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Events"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> CreateEventGraphParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	CreateEventGraphParams->SetStringField(TEXT("graph"), TEXT("BridgeEvents"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateEventGraph"), CreateEventGraphParams)))
	{
		return false;
	}

	UEdGraph* EventGraph = BlueprintBridgeTests::FindBlueprintGraph(Asset.Blueprint, TEXT("BridgeEvents"));
	TestNotNull(TEXT("BridgeEvents graph should exist."), EventGraph);
	if (!EventGraph)
	{
		return false;
	}

	FGraphNodeCreator<UK2Node_CustomEvent> NodeCreator(*EventGraph);
	UK2Node_CustomEvent* EventNode = NodeCreator.CreateNode();
	EventNode->CustomFunctionName = TEXT("OldBridgeEvent");
	EventNode->NodePosX = 100;
	EventNode->NodePosY = 100;
	NodeCreator.Finalize();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Asset.Blueprint);

	TSharedRef<FJsonObject> RenameEventParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("BridgeEvents"));
	RenameEventParams->SetStringField(TEXT("node"), EventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	RenameEventParams->SetStringField(TEXT("newName"), TEXT("RenamedBridgeEvent"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("RenameCustomEvent"), RenameEventParams)))
	{
		return false;
	}

	return TestTrue(TEXT("RenameCustomEvent should rename the custom event close to the requested name."), EventNode->CustomFunctionName.ToString().StartsWith(TEXT("RenamedBridgeEvent")));
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeComponentEditingTest, "BlueprintBridge.Blueprint.ComponentEditing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeComponentEditingTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Components"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> AddRootParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddRootParams->SetStringField(TEXT("name"), TEXT("BridgeRoot"));
	AddRootParams->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.SceneComponent"));
	const TSharedRef<FJsonObject> AddRootResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddComponent"), AddRootParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddRootResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddChildParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddChildParams->SetStringField(TEXT("name"), TEXT("BridgeChild"));
	AddChildParams->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.SceneComponent"));
	AddChildParams->SetStringField(TEXT("parent"), TEXT("BridgeRoot"));
	const TSharedRef<FJsonObject> AddChildResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddComponent"), AddChildParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddChildResponse))
	{
		return false;
	}

	TSharedRef<FJsonObject> TransformParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	TransformParams->SetStringField(TEXT("name"), TEXT("BridgeChild"));
	TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
	Location->SetNumberField(TEXT("x"), 10.0);
	Location->SetNumberField(TEXT("y"), 20.0);
	Location->SetNumberField(TEXT("z"), 30.0);
	TransformParams->SetObjectField(TEXT("location"), Location);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetComponentTransform"), TransformParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> PropertyParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	PropertyParams->SetStringField(TEXT("name"), TEXT("BridgeChild"));
	PropertyParams->SetStringField(TEXT("property"), TEXT("bHiddenInGame"));
	PropertyParams->SetStringField(TEXT("value"), TEXT("true"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetComponentProperty"), PropertyParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeComponents"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
	{
		return false;
	}

	USimpleConstructionScript* SCS = Asset.Blueprint->SimpleConstructionScript;
	TestNotNull(TEXT("Blueprint should have an SCS."), SCS);
	if (!SCS)
	{
		return false;
	}

	USCS_Node* ChildNode = nullptr;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName() == TEXT("BridgeChild"))
		{
			ChildNode = Node;
			break;
		}
	}

	TestNotNull(TEXT("Child component should exist."), ChildNode);
	USceneComponent* ChildTemplate = ChildNode ? Cast<USceneComponent>(ChildNode->ComponentTemplate) : nullptr;
	TestNotNull(TEXT("Child component template should be a scene component."), ChildTemplate);
	if (!ChildTemplate)
	{
		return false;
	}

	FName ParentName = NAME_None;
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetChildNodes().Contains(ChildNode))
		{
			ParentName = Node->GetVariableName();
			break;
		}
	}
	TestEqual(TEXT("Child component should have parent."), ParentName, FName(TEXT("BridgeRoot")));
	TestEqual(TEXT("Relative location should be set."), ChildTemplate->GetRelativeLocation(), FVector(10.0, 20.0, 30.0));
	TestTrue(TEXT("Component property should be set."), ChildTemplate->bHiddenInGame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetLifecycleAndDefaultsTest, "BlueprintBridge.Blueprint.AssetLifecycleAndDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetLifecycleAndDefaultsTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Lifecycle"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("int"));
	AddVariableParams->SetStringField(TEXT("defaultValue"), TEXT("100"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> CompileResponse = BlueprintBridgeTests::CompileBlueprintRequest(Asset.AssetPath);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, CompileResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* CompileResult = BlueprintBridgeTests::GetResultObject(*this, CompileResponse);
	if (!CompileResult)
	{
		return false;
	}
	FString CompileStatus;
	bool bCompileSuccess = false;
	double CompileErrorCount = -1.0;
	double CompileWarningCount = -1.0;
	const TArray<TSharedPtr<FJsonValue>>* CompileMessages = nullptr;
	TestTrue(TEXT("CompileBlueprint should include status."), (*CompileResult)->TryGetStringField(TEXT("status"), CompileStatus));
	TestTrue(TEXT("CompileBlueprint should include success."), (*CompileResult)->TryGetBoolField(TEXT("success"), bCompileSuccess));
	TestTrue(TEXT("CompileBlueprint should include errorCount."), (*CompileResult)->TryGetNumberField(TEXT("errorCount"), CompileErrorCount));
	TestTrue(TEXT("CompileBlueprint should include warningCount."), (*CompileResult)->TryGetNumberField(TEXT("warningCount"), CompileWarningCount));
	TestTrue(TEXT("CompileBlueprint should include messages."), (*CompileResult)->TryGetArrayField(TEXT("messages"), CompileMessages) && CompileMessages != nullptr);
	TestTrue(TEXT("CompileBlueprint should succeed."), bCompileSuccess && (CompileStatus == TEXT("UpToDate") || CompileStatus == TEXT("UpToDateWithWarnings")));

	TSharedRef<FJsonObject> SetDefaultParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	SetDefaultParams->SetStringField(TEXT("property"), TEXT("Health"));
	SetDefaultParams->SetStringField(TEXT("value"), TEXT("123"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetBlueprintDefault"), SetDefaultParams)))
	{
		return false;
	}

	Asset.Blueprint->GeneratedClass = Asset.Blueprint->GeneratedClass ? Asset.Blueprint->GeneratedClass : Asset.Blueprint->GeneratedClass;
	UObject* CDO = Asset.Blueprint->GeneratedClass ? Asset.Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
	TestNotNull(TEXT("Compiled blueprint should have a CDO."), CDO);
	if (CDO)
	{
		FProperty* Property = Asset.Blueprint->GeneratedClass->FindPropertyByName(TEXT("Health"));
		TestNotNull(TEXT("Health property should exist on the generated class."), Property);
		if (const FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			const int32 CurrentValue = IntProperty->GetPropertyValue_InContainer(CDO);
			TestEqual(TEXT("SetBlueprintDefault should update the CDO default."), CurrentValue, 123);
		}
	}

	TSharedRef<FJsonObject> GetDefaultParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	GetDefaultParams->SetStringField(TEXT("property"), TEXT("Health"));
	const TSharedRef<FJsonObject> GetDefaultResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("GetBlueprintDefault"), GetDefaultParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, GetDefaultResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* GetDefaultResult = BlueprintBridgeTests::GetResultObject(*this, GetDefaultResponse);
	if (GetDefaultResult)
	{
		FString ReturnedValue;
		FString ReturnedProperty;
		FString ReturnedType;
		TestTrue(TEXT("GetBlueprintDefault should include value."), (*GetDefaultResult)->TryGetStringField(TEXT("value"), ReturnedValue));
		TestTrue(TEXT("GetBlueprintDefault should include property."), (*GetDefaultResult)->TryGetStringField(TEXT("property"), ReturnedProperty));
		TestTrue(TEXT("GetBlueprintDefault should include type."), (*GetDefaultResult)->TryGetStringField(TEXT("type"), ReturnedType));
		TestEqual(TEXT("GetBlueprintDefault should round-trip the CDO value."), ReturnedValue, FString(TEXT("123")));
		TestEqual(TEXT("GetBlueprintDefault should echo the property name."), ReturnedProperty, FString(TEXT("Health")));
		TestEqual(TEXT("GetBlueprintDefault should report the int type."), ReturnedType, FString(TEXT("int32")));
	}

	TSharedRef<FJsonObject> GetMissingParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	GetMissingParams->SetStringField(TEXT("property"), TEXT("NoSuchProperty"));
	const TSharedRef<FJsonObject> GetMissingResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("GetBlueprintDefault"), GetMissingParams);
	bool bMissingOk = true;
	GetMissingResponse->TryGetBoolField(TEXT("ok"), bMissingOk);
	TestFalse(TEXT("GetBlueprintDefault should fail for unknown properties."), bMissingOk);

	const TSharedRef<FJsonObject> SaveResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SaveAsset"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, SaveResponse))
	{
		return false;
	}
	return TestEqual(TEXT("SaveAsset should return Saved."), BlueprintBridgeTests::GetStringResult(*this, SaveResponse), FString(TEXT("Saved")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCreateBlueprintAssetTest, "BlueprintBridge.Blueprint.CreateBlueprintAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCreateBlueprintAssetTest::RunTest(const FString& Parameters)
{
	const FString AssetName = FString::Printf(TEXT("BP_BridgeCreated_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetPath = FString::Printf(TEXT("/Game/BlueprintBridgeTests/%s"), *AssetName);

	TSharedRef<FJsonObject> CreateParams = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	CreateParams->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
	const TSharedRef<FJsonObject> CreateResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateBlueprintAsset"), CreateParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, CreateResponse))
	{
		return false;
	}

	UBlueprint* Blueprint = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName)));
	TestNotNull(TEXT("CreateBlueprintAsset should create a loadable Blueprint."), Blueprint);
	if (!Blueprint)
	{
		return false;
	}

	TestTrue(TEXT("Created Blueprint should use the requested parent class."), Blueprint->ParentClass == AActor::StaticClass());

	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(AssetPath));
	return BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeDuplicateAssetTest, "BlueprintBridge.Blueprint.DuplicateAsset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeDuplicateAssetTest::RunTest(const FString& Parameters)
{
	const FString SourceName = FString::Printf(TEXT("BP_BridgeDuplicateSource_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString SourcePath = FString::Printf(TEXT("/Game/BlueprintBridgeTests/%s"), *SourceName);
	const FString DestName = FString::Printf(TEXT("BP_BridgeDuplicateDest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString DestPath = FString::Printf(TEXT("/Game/BlueprintBridgeTests/%s"), *DestName);

	TSharedRef<FJsonObject> CreateParams = BlueprintBridgeTests::MakeAssetParams(SourcePath);
	CreateParams->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateBlueprintAsset"), CreateParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(SourcePath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("ProjectileSpeed"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("real"));
	AddVariableParams->SetStringField(TEXT("subCategory"), TEXT("float"));
	AddVariableParams->SetStringField(TEXT("defaultValue"), TEXT("1200.0"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> DuplicateParams = MakeShared<FJsonObject>();
	DuplicateParams->SetStringField(TEXT("sourceAsset"), SourcePath);
	DuplicateParams->SetStringField(TEXT("destAsset"), DestPath);
	const TSharedRef<FJsonObject> DuplicateResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DuplicateAsset"), DuplicateParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DuplicateResponse))
	{
		return false;
	}

	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(DestPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	if (!DescribeResult)
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	if (!TestTrue(TEXT("Duplicated Blueprint should describe variables."), (*DescribeResult)->TryGetArrayField(TEXT("variables"), Variables) && Variables != nullptr))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
	{
		const TSharedPtr<FJsonObject>* VariableObject = nullptr;
		if (VariableValue.IsValid() && VariableValue->TryGetObject(VariableObject) && VariableObject != nullptr && VariableObject->IsValid())
		{
			FString VariableName;
			if ((*VariableObject)->TryGetStringField(TEXT("name"), VariableName) && VariableName == TEXT("ProjectileSpeed"))
			{
				return true;
			}
		}
	}

	AddError(TEXT("Duplicated Blueprint should preserve source Blueprint variables."));
	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeGraphPrimitiveCommandsTest, "BlueprintBridge.Blueprint.GraphPrimitives", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeGraphPrimitiveCommandsTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("GraphPrimitives"));
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> CustomEventParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	CustomEventParams->SetStringField(TEXT("name"), TEXT("BridgeCustomEvent"));
	CustomEventParams->SetNumberField(TEXT("x"), 100);
	CustomEventParams->SetNumberField(TEXT("y"), 100);
	TArray<TSharedPtr<FJsonValue>> CustomInputs;
	TSharedRef<FJsonObject> CustomInput = MakeShared<FJsonObject>();
	CustomInput->SetStringField(TEXT("name"), TEXT("Count"));
	CustomInput->SetStringField(TEXT("category"), TEXT("int"));
	CustomInputs.Add(MakeShared<FJsonValueObject>(CustomInput));
	CustomEventParams->SetArrayField(TEXT("inputs"), CustomInputs);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddCustomEventNode"), CustomEventParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> EventParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	EventParams->SetStringField(TEXT("eventClass"), TEXT("/Script/Engine.Actor"));
	EventParams->SetStringField(TEXT("event"), TEXT("ReceiveBeginPlay"));
	EventParams->SetNumberField(TEXT("x"), 100);
	EventParams->SetNumberField(TEXT("y"), 300);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddEventNode"), EventParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> CastParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	CastParams->SetStringField(TEXT("targetClass"), TEXT("/Script/Engine.Pawn"));
	CastParams->SetNumberField(TEXT("x"), 400);
	CastParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddDynamicCastNode"), CastParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> SpawnParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	SpawnParams->SetStringField(TEXT("actorClass"), TEXT("/Script/Engine.StaticMeshActor"));
	SpawnParams->SetNumberField(TEXT("x"), 650);
	SpawnParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddSpawnActorNode"), SpawnParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> DispatcherParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	DispatcherParams->SetStringField(TEXT("name"), TEXT("OnBridgeEvent"));
	TArray<TSharedPtr<FJsonValue>> DispatcherInputs;
	TSharedRef<FJsonObject> DispatcherInput = MakeShared<FJsonObject>();
	DispatcherInput->SetStringField(TEXT("name"), TEXT("Instigator"));
	DispatcherInput->SetStringField(TEXT("category"), TEXT("object"));
	DispatcherInput->SetStringField(TEXT("subCategoryObject"), TEXT("/Script/Engine.Actor"));
	DispatcherInputs.Add(MakeShared<FJsonValueObject>(DispatcherInput));
	DispatcherParams->SetArrayField(TEXT("inputs"), DispatcherInputs);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddEventDispatcher"), DispatcherParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddComponentParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddComponentParams->SetStringField(TEXT("name"), TEXT("TriggerBox"));
	AddComponentParams->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.BoxComponent"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddComponent"), AddComponentParams)))
	{
		return false;
	}

	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::CompileBlueprintRequest(Asset.AssetPath)))
	{
		return false;
	}

	TSharedRef<FJsonObject> ComponentEventParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	ComponentEventParams->SetStringField(TEXT("component"), TEXT("TriggerBox"));
	ComponentEventParams->SetStringField(TEXT("delegate"), TEXT("OnComponentBeginOverlap"));
	ComponentEventParams->SetNumberField(TEXT("x"), 100);
	ComponentEventParams->SetNumberField(TEXT("y"), 500);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddComponentEventNode"), ComponentEventParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> DelegateBindParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	DelegateBindParams->SetStringField(TEXT("delegate"), TEXT("OnBridgeEvent"));
	DelegateBindParams->SetNumberField(TEXT("x"), 400);
	DelegateBindParams->SetNumberField(TEXT("y"), 500);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddDelegateBindNode"), DelegateBindParams)))
	{
		return false;
	}

	UEdGraph* EventGraph = BlueprintBridgeTests::FindBlueprintGraph(Asset.Blueprint, EventGraphName);
	TestNotNull(TEXT("Event graph should exist."), EventGraph);
	TestTrue(TEXT("Custom event should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Cast<UK2Node_CustomEvent>(Node) && Cast<UK2Node_CustomEvent>(Node)->CustomFunctionName == TEXT("BridgeCustomEvent"); }));
	TestTrue(TEXT("Native event node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Cast<UK2Node_Event>(Node) && Cast<UK2Node_Event>(Node)->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay"); }));
	TestTrue(TEXT("Dynamic cast node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Cast<UK2Node_DynamicCast>(Node) && Cast<UK2Node_DynamicCast>(Node)->TargetType == APawn::StaticClass(); }));
	TestTrue(TEXT("Spawn actor node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Cast<UK2Node_SpawnActorFromClass>(Node) != nullptr; }));
	TestTrue(TEXT("Component event node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Cast<UK2Node_ComponentBoundEvent>(Node) && Cast<UK2Node_ComponentBoundEvent>(Node)->DelegatePropertyName == TEXT("OnComponentBeginOverlap"); }));
	TestTrue(TEXT("Delegate bind node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Cast<UK2Node_AddDelegate>(Node) && Cast<UK2Node_AddDelegate>(Node)->GetPropertyName() == TEXT("OnBridgeEvent"); }));
	TestTrue(TEXT("Dispatcher signature graph should be present."), BlueprintBridgeTests::FindBlueprintGraph(Asset.Blueprint, TEXT("OnBridgeEvent")) != nullptr);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeVariableFlagsTest, "BlueprintBridge.Blueprint.VariableFlags", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeVariableFlagsTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("VariableFlags"));

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("ClearedCount"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("int"));
	AddVariableParams->SetStringField(TEXT("defaultValue"), TEXT("0"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> FlagsParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	FlagsParams->SetStringField(TEXT("variable"), TEXT("ClearedCount"));
	FlagsParams->SetBoolField(TEXT("instanceEditable"), true);
	FlagsParams->SetBoolField(TEXT("blueprintReadOnly"), true);
	FlagsParams->SetBoolField(TEXT("exposeOnSpawn"), true);
	FlagsParams->SetBoolField(TEXT("private"), true);
	FlagsParams->SetStringField(TEXT("categoryName"), TEXT("Hoop Trial"));
	FlagsParams->SetStringField(TEXT("tooltip"), TEXT("Number of hoops cleared."));
	FlagsParams->SetStringField(TEXT("replication"), TEXT("RepNotify"));
	FlagsParams->SetStringField(TEXT("repNotifyFunc"), TEXT("OnRep_ClearedCount"));
	const TSharedRef<FJsonObject> FlagsResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetBlueprintVariableFlags"), FlagsParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, FlagsResponse))
	{
		return false;
	}

	const int32 VarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Asset.Blueprint, TEXT("ClearedCount"));
	if (!TestTrue(TEXT("Variable should exist."), VarIndex != INDEX_NONE))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddCombinedFlagsParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddCombinedFlagsParams->SetStringField(TEXT("name"), TEXT("AddedEditable"));
	AddCombinedFlagsParams->SetStringField(TEXT("category"), TEXT("bool"));
	AddCombinedFlagsParams->SetBoolField(TEXT("instanceEditable"), true);
	AddCombinedFlagsParams->SetBoolField(TEXT("blueprintReadOnly"), true);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddCombinedFlagsParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddArrayVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddArrayVariableParams->SetStringField(TEXT("name"), TEXT("SpawnVolumes"));
	AddArrayVariableParams->SetStringField(TEXT("category"), TEXT("object"));
	AddArrayVariableParams->SetStringField(TEXT("subCategoryObject"), TEXT("/Script/Engine.Actor"));
	AddArrayVariableParams->SetStringField(TEXT("containerType"), TEXT("Array"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddArrayVariableParams)))
	{
		return false;
	}

	const int32 AddedFlagsVarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Asset.Blueprint, TEXT("AddedEditable"));
	if (!TestTrue(TEXT("AddBlueprintVariable flags variable should exist."), AddedFlagsVarIndex != INDEX_NONE))
	{
		return false;
	}
	TestTrue(TEXT("AddBlueprintVariable should apply instanceEditable."), ((Asset.Blueprint->NewVariables[AddedFlagsVarIndex].PropertyFlags & CPF_Edit) != 0) && ((Asset.Blueprint->NewVariables[AddedFlagsVarIndex].PropertyFlags & CPF_DisableEditOnInstance) == 0));
	TestTrue(TEXT("AddBlueprintVariable should apply blueprintReadOnly."), ((Asset.Blueprint->NewVariables[AddedFlagsVarIndex].PropertyFlags & CPF_BlueprintReadOnly) != 0));

	const int32 ArrayVarIndex = FBlueprintEditorUtils::FindNewVariableIndex(Asset.Blueprint, TEXT("SpawnVolumes"));
	if (!TestTrue(TEXT("Array variable should exist."), ArrayVarIndex != INDEX_NONE))
	{
		return false;
	}
	TestEqual(TEXT("Array variable should use Array container type."), Asset.Blueprint->NewVariables[ArrayVarIndex].VarType.ContainerType, EPinContainerType::Array);

	const FBPVariableDescription& Variable = Asset.Blueprint->NewVariables[VarIndex];
	TestTrue(TEXT("Variable should be instance editable."), ((Variable.PropertyFlags & CPF_Edit) != 0) && ((Variable.PropertyFlags & CPF_DisableEditOnInstance) == 0));
	TestTrue(TEXT("Variable should be Blueprint read only."), ((Variable.PropertyFlags & CPF_BlueprintReadOnly) != 0));
	TestTrue(TEXT("Variable should be replicated."), ((Variable.PropertyFlags & CPF_Net) != 0));
	TestTrue(TEXT("Variable should use RepNotify."), ((Variable.PropertyFlags & CPF_RepNotify) != 0));
	TestEqual(TEXT("RepNotify function should be set."), Variable.RepNotifyFunc, FName(TEXT("OnRep_ClearedCount")));
	TestEqual(TEXT("Category should be set."), Variable.Category.ToString(), FString(TEXT("Hoop Trial")));

	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	if (!DescribeResult || !TestTrue(TEXT("DescribeBlueprint should include variables."), (*DescribeResult)->TryGetArrayField(TEXT("variables"), Variables) && Variables != nullptr))
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
	{
		const TSharedPtr<FJsonObject>* VariableObject = nullptr;
		if (VariableValue.IsValid() && VariableValue->TryGetObject(VariableObject) && VariableObject != nullptr && VariableObject->IsValid())
		{
			FString Name;
			(*VariableObject)->TryGetStringField(TEXT("name"), Name);
			if (Name == TEXT("ClearedCount"))
			{
				bool bRepNotify = false;
				FString RepNotifyFunc;
				(*VariableObject)->TryGetBoolField(TEXT("repNotify"), bRepNotify);
				(*VariableObject)->TryGetStringField(TEXT("repNotifyFunc"), RepNotifyFunc);
				TestTrue(TEXT("DescribeBlueprint should expose RepNotify."), bRepNotify);
				TestEqual(TEXT("DescribeBlueprint should expose RepNotify function."), RepNotifyFunc, FString(TEXT("OnRep_ClearedCount")));
			}
			else if (Name == TEXT("SpawnVolumes"))
			{
				FString ContainerType;
				(*VariableObject)->TryGetStringField(TEXT("containerType"), ContainerType);
				return TestEqual(TEXT("DescribeBlueprint should expose array container type."), ContainerType, FString(TEXT("Array")));
			}
		}
	}

	AddError(TEXT("DescribeBlueprint should include ClearedCount."));
	return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeComponentSpecializedSettersTest, "BlueprintBridge.Blueprint.ComponentSpecializedSetters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeComponentSpecializedSettersTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ComponentSetters"));

	TSharedRef<FJsonObject> AddMeshParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddMeshParams->SetStringField(TEXT("name"), TEXT("MeshRoot"));
	AddMeshParams->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.StaticMeshComponent"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddComponent"), AddMeshParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddBoxParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddBoxParams->SetStringField(TEXT("name"), TEXT("TriggerBox"));
	AddBoxParams->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.BoxComponent"));
	AddBoxParams->SetStringField(TEXT("parent"), TEXT("MeshRoot"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddComponent"), AddBoxParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> RootParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	RootParams->SetStringField(TEXT("name"), TEXT("MeshRoot"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetRootComponent"), RootParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> MeshParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	MeshParams->SetStringField(TEXT("name"), TEXT("MeshRoot"));
	MeshParams->SetStringField(TEXT("mesh"), TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetStaticMesh"), MeshParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> CollisionParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	CollisionParams->SetStringField(TEXT("name"), TEXT("TriggerBox"));
	CollisionParams->SetStringField(TEXT("profile"), TEXT("OverlapAllDynamic"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetCollisionProfileName"), CollisionParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> ExtentParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	ExtentParams->SetStringField(TEXT("name"), TEXT("TriggerBox"));
	TSharedRef<FJsonObject> ExtentObject = MakeShared<FJsonObject>();
	ExtentObject->SetNumberField(TEXT("x"), 10.0);
	ExtentObject->SetNumberField(TEXT("y"), 20.0);
	ExtentObject->SetNumberField(TEXT("z"), 30.0);
	ExtentParams->SetObjectField(TEXT("extent"), ExtentObject);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetBoxExtent"), ExtentParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> OverlapParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	OverlapParams->SetStringField(TEXT("name"), TEXT("TriggerBox"));
	OverlapParams->SetBoolField(TEXT("generateOverlapEvents"), true);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetGenerateOverlapEvents"), OverlapParams)))
	{
		return false;
	}

	USCS_Node* MeshNode = Asset.Blueprint->SimpleConstructionScript ? Asset.Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("MeshRoot")) : nullptr;
	USCS_Node* BoxNode = Asset.Blueprint->SimpleConstructionScript ? Asset.Blueprint->SimpleConstructionScript->FindSCSNode(TEXT("TriggerBox")) : nullptr;
	UStaticMeshComponent* MeshComponent = MeshNode ? Cast<UStaticMeshComponent>(MeshNode->ComponentTemplate) : nullptr;
	UBoxComponent* BoxComponent = BoxNode ? Cast<UBoxComponent>(BoxNode->ComponentTemplate) : nullptr;

	TestTrue(TEXT("MeshRoot should be a root node."), Asset.Blueprint->SimpleConstructionScript && MeshNode && Asset.Blueprint->SimpleConstructionScript->GetRootNodes().Contains(MeshNode));
	TestTrue(TEXT("Static mesh should be assigned."), MeshComponent && MeshComponent->GetStaticMesh() != nullptr);
	TestNotNull(TEXT("Box component should exist."), BoxComponent);
	if (BoxComponent)
	{
		TestEqual(TEXT("Collision profile should be set."), BoxComponent->GetCollisionProfileName(), FName(TEXT("OverlapAllDynamic")));
		TestEqual(TEXT("Box extent should be set."), BoxComponent->GetUnscaledBoxExtent(), FVector(10.0, 20.0, 30.0));
		TestTrue(TEXT("Generate overlap events should be set."), BoxComponent->GetGenerateOverlapEvents());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeControlFlowHelpersTest, "BlueprintBridge.Blueprint.ControlFlowHelpers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeControlFlowHelpersTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ControlFlow"));
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> ForLoopParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	ForLoopParams->SetNumberField(TEXT("x"), 100);
	ForLoopParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddForLoopNode"), ForLoopParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> ForEachParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	ForEachParams->SetNumberField(TEXT("x"), 350);
	ForEachParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddForEachLoopNode"), ForEachParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AuthorityParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AuthorityParams->SetNumberField(TEXT("x"), 600);
	AuthorityParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddAuthoritySwitchNode"), AuthorityParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> MakeStructParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	MakeStructParams->SetStringField(TEXT("struct"), TEXT("/Script/CoreUObject.Vector"));
	MakeStructParams->SetNumberField(TEXT("x"), 100);
	MakeStructParams->SetNumberField(TEXT("y"), 450);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddMakeStructNode"), MakeStructParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> BreakStructParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	BreakStructParams->SetStringField(TEXT("struct"), TEXT("/Script/CoreUObject.Vector"));
	BreakStructParams->SetNumberField(TEXT("x"), 350);
	BreakStructParams->SetNumberField(TEXT("y"), 450);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBreakStructNode"), BreakStructParams)))
	{
		return false;
	}

	UEdGraph* EventGraph = BlueprintBridgeTests::FindBlueprintGraph(Asset.Blueprint, EventGraphName);
	TestNotNull(TEXT("Event graph should exist."), EventGraph);
	TestTrue(TEXT("ForLoop macro should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node); return Macro && Macro->GetMacroGraph() && Macro->GetMacroGraph()->GetName().Equals(TEXT("ForLoop"), ESearchCase::IgnoreCase); }));
	TestTrue(TEXT("ForEachLoop macro should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node); return Macro && Macro->GetMacroGraph() && Macro->GetMacroGraph()->GetName().Equals(TEXT("ForEachLoop"), ESearchCase::IgnoreCase); }));
	TestTrue(TEXT("Authority switch macro should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node); return Macro && Macro->GetMacroGraph() && Macro->GetMacroGraph()->GetName().Contains(TEXT("Authority")); }));
	TestTrue(TEXT("Make Vector node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { const UK2Node_MakeStruct* MakeStruct = Cast<UK2Node_MakeStruct>(Node); return MakeStruct && MakeStruct->StructType == TBaseStructure<FVector>::Get(); }));
	TestTrue(TEXT("Break Vector node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { const UK2Node_BreakStruct* BreakStruct = Cast<UK2Node_BreakStruct>(Node); return BreakStruct && BreakStruct->StructType == TBaseStructure<FVector>::Get(); }));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeWidgetTreeCommandsTest, "BlueprintBridge.UMG.WidgetTreeCommands", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeWidgetTreeCommandsTest::RunTest(const FString& Parameters)
{
	const FString AssetName = FString::Printf(TEXT("WBP_BridgeTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetPath = FString::Printf(TEXT("/Game/BlueprintBridgeTests/%s"), *AssetName);

	TSharedRef<FJsonObject> CreateParams = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateWidgetBlueprintAsset"), CreateParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> RootParams = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	RootParams->SetStringField(TEXT("name"), TEXT("RootCanvas"));
	RootParams->SetStringField(TEXT("widgetClass"), TEXT("/Script/UMG.CanvasPanel"));
	RootParams->SetBoolField(TEXT("root"), true);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddWidget"), RootParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> TextParams = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	TextParams->SetStringField(TEXT("name"), TEXT("TitleText"));
	TextParams->SetStringField(TEXT("widgetClass"), TEXT("/Script/UMG.TextBlock"));
	TextParams->SetStringField(TEXT("parent"), TEXT("RootCanvas"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddWidget"), TextParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> SlotParams = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	SlotParams->SetStringField(TEXT("widget"), TEXT("TitleText"));
	TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), 24);
	Position->SetNumberField(TEXT("y"), 36);
	SlotParams->SetObjectField(TEXT("position"), Position);
	TSharedRef<FJsonObject> Size = MakeShared<FJsonObject>();
	Size->SetNumberField(TEXT("x"), 320);
	Size->SetNumberField(TEXT("y"), 48);
	SlotParams->SetObjectField(TEXT("size"), Size);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetWidgetSlotLayout"), SlotParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeWidgetTree"), BlueprintBridgeTests::MakeAssetParams(AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ResultObject = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	const TSharedPtr<FJsonObject>* WidgetTreeObject = nullptr;
	TestTrue(TEXT("Result should contain widgetTree."), ResultObject && (*ResultObject)->TryGetObjectField(TEXT("widgetTree"), WidgetTreeObject) && WidgetTreeObject && WidgetTreeObject->IsValid());

	FString RootName;
	TestTrue(TEXT("Widget tree should report RootCanvas as root."), WidgetTreeObject && (*WidgetTreeObject)->TryGetStringField(TEXT("root"), RootName) && RootName == TEXT("RootCanvas"));

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(StaticLoadObject(UWidgetBlueprint::StaticClass(), nullptr, *FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName)));
	TestNotNull(TEXT("Created Widget Blueprint should load."), WidgetBlueprint);
	TestTrue(TEXT("RootCanvas should be the root widget."), WidgetBlueprint && WidgetBlueprint->WidgetTree && Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget) && WidgetBlueprint->WidgetTree->RootWidget->GetName() == TEXT("RootCanvas"));
	UTextBlock* TitleText = WidgetBlueprint && WidgetBlueprint->WidgetTree ? Cast<UTextBlock>(WidgetBlueprint->WidgetTree->FindWidget(TEXT("TitleText"))) : nullptr;
	TestNotNull(TEXT("TitleText should exist."), TitleText);
	TestEqual(TEXT("RootCanvas should have one child."), WidgetBlueprint && WidgetBlueprint->WidgetTree && Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget) ? Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget)->GetChildrenCount() : INDEX_NONE, 1);
	const UCanvasPanelSlot* TitleSlot = TitleText ? Cast<UCanvasPanelSlot>(TitleText->Slot) : nullptr;
	TestTrue(TEXT("Canvas slot position should be set."), TitleSlot && TitleSlot->GetPosition().Equals(FVector2D(24, 36)));
	TestTrue(TEXT("Canvas slot size should be set."), TitleSlot && TitleSlot->GetSize().Equals(FVector2D(320, 48)));

	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::CompileBlueprintRequest(AssetPath)))
	{
		return false;
	}

	const BlueprintBridgeTests::FTestBlueprintAsset OwnerAsset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("CreateWidgetNode"));
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(OwnerAsset.Blueprint);
	TSharedRef<FJsonObject> CreateWidgetNodeParams = BlueprintBridgeTests::MakeGraphParams(OwnerAsset.AssetPath, EventGraphName);
	CreateWidgetNodeParams->SetStringField(TEXT("widgetClass"), FString::Printf(TEXT("%s.%s_C"), *AssetPath, *AssetName));
	CreateWidgetNodeParams->SetNumberField(TEXT("x"), 100);
	CreateWidgetNodeParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddCreateWidgetNode"), CreateWidgetNodeParams)))
	{
		return false;
	}

	UEdGraph* EventGraph = BlueprintBridgeTests::FindBlueprintGraph(OwnerAsset.Blueprint, EventGraphName);
	TestTrue(TEXT("Create Widget node should be present."), EventGraph && EventGraph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Node && Node->GetClass()->GetPathName().Contains(TEXT("K2Node_CreateWidget")); }));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeExtendedGraphHelpersTest, "BlueprintBridge.Blueprint.ExtendedGraphHelpers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeExtendedGraphHelpersTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ExtendedGraphHelpers"));
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> IntArrayParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	IntArrayParams->SetStringField(TEXT("name"), TEXT("Numbers"));
	IntArrayParams->SetStringField(TEXT("category"), UEdGraphSchema_K2::PC_Int.ToString());
	IntArrayParams->SetStringField(TEXT("containerType"), TEXT("Array"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), IntArrayParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> SelfParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	SelfParams->SetNumberField(TEXT("x"), -120);
	SelfParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddSelfNode"), SelfParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> ArrayParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	ArrayParams->SetStringField(TEXT("operation"), TEXT("Length"));
	ArrayParams->SetNumberField(TEXT("x"), 100);
	ArrayParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddArrayFunctionNode"), ArrayParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> TimerParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	TimerParams->SetStringField(TEXT("operation"), TEXT("SetByFunctionName"));
	TimerParams->SetNumberField(TEXT("x"), 320);
	TimerParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddTimerNode"), TimerParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> TraceParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	TraceParams->SetNumberField(TEXT("x"), 560);
	TraceParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddLineTraceNode"), TraceParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> MathParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	MathParams->SetStringField(TEXT("function"), TEXT("RandomIntegerInRange"));
	MathParams->SetNumberField(TEXT("x"), 820);
	MathParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddMathNode"), MathParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> DispatcherParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	DispatcherParams->SetStringField(TEXT("name"), TEXT("OnBridgeTest"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddEventDispatcher"), DispatcherParams)))
	{
		return false;
	}
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::CompileBlueprintRequest(Asset.AssetPath)))
	{
		return false;
	}

	TSharedRef<FJsonObject> BroadcastParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	BroadcastParams->SetStringField(TEXT("delegate"), TEXT("OnBridgeTest"));
	BroadcastParams->SetNumberField(TEXT("x"), 1060);
	BroadcastParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddDelegateBroadcastNode"), BroadcastParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> BindParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	BindParams->SetStringField(TEXT("delegate"), TEXT("OnBridgeTest"));
	BindParams->SetNumberField(TEXT("x"), 1280);
	BindParams->SetNumberField(TEXT("y"), 100);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddDelegateBindNode"), BindParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> DelegateParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	DelegateParams->SetNumberField(TEXT("x"), 1500);
	DelegateParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> DelegateResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddCreateDelegateNode"), DelegateParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DelegateResponse))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* DelegateResult = BlueprintBridgeTests::GetResultObject(*this, DelegateResponse);
	const TSharedPtr<FJsonObject>* DelegateNode = nullptr;
	FString DelegateNodeGuid;
	TestTrue(TEXT("Create delegate response should contain node."), DelegateResult && (*DelegateResult)->TryGetObjectField(TEXT("node"), DelegateNode) && DelegateNode && DelegateNode->IsValid());
	TestTrue(TEXT("Create delegate node should have guid."), DelegateNode && (*DelegateNode)->TryGetStringField(TEXT("guid"), DelegateNodeGuid));

	TSharedRef<FJsonObject> SetDelegateParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	SetDelegateParams->SetStringField(TEXT("node"), DelegateNodeGuid);
	SetDelegateParams->SetStringField(TEXT("function"), TEXT("ReceiveBeginPlay"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SetCreateDelegateFunction"), SetDelegateParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> DescribeParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	TSharedRef<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Requests;
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("DescribeGraph"), DescribeParams)));
	BatchParams->SetArrayField(TEXT("requests"), Requests);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Batch"), BatchParams)))
	{
		return false;
	}

	UEdGraph* EventGraph = BlueprintBridgeTests::FindBlueprintGraph(Asset.Blueprint, EventGraphName);
	TestTrue(TEXT("Extended helper nodes should have been created."), EventGraph && EventGraph->Nodes.Num() >= 7);
	return true;
}

namespace BlueprintBridgeTests
{
static FString MakeUniqueSpecAssetPath(const TCHAR* Suffix)
{
	const FString AssetName = FString::Printf(TEXT("BP_BridgeTest_Spec%s_%s"), Suffix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	return FString::Printf(TEXT("/Game/BlueprintBridgeTests/%s"), *AssetName);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCreateFromSpecMinimalTest, "BlueprintBridge.Blueprint.CreateFromSpec.Minimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCreateFromSpecMinimalTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = BlueprintBridgeTests::MakeUniqueSpecAssetPath(TEXT("Min"));
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	Params->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateBlueprintFromSpec"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	// Verify asset exists by round-tripping through the bridge.
	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(AssetPath));
	return BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCreateFromSpecVariablesAndComponentsTest, "BlueprintBridge.Blueprint.CreateFromSpec.VariablesAndComponents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCreateFromSpecVariablesAndComponentsTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = BlueprintBridgeTests::MakeUniqueSpecAssetPath(TEXT("VarComp"));
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	Params->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));

	TArray<TSharedPtr<FJsonValue>> Variables;
	TSharedRef<FJsonObject> Var = MakeShared<FJsonObject>();
	Var->SetStringField(TEXT("name"), TEXT("Health"));
	Var->SetStringField(TEXT("category"), TEXT("int"));
	Var->SetStringField(TEXT("defaultValue"), TEXT("100"));
	Variables.Add(MakeShared<FJsonValueObject>(Var));
	Params->SetArrayField(TEXT("variables"), Variables);

	TArray<TSharedPtr<FJsonValue>> Components;
	TSharedRef<FJsonObject> Comp = MakeShared<FJsonObject>();
	Comp->SetStringField(TEXT("name"), TEXT("Hitbox"));
	Comp->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.BoxComponent"));
	Components.Add(MakeShared<FJsonValueObject>(Comp));
	Params->SetArrayField(TEXT("components"), Components);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateBlueprintFromSpec"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	// Verify variable + component via the bridge so the test doesn't depend on internal helpers.
	const TSharedRef<FJsonObject> SummaryResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SummarizeBlueprint"), BlueprintBridgeTests::MakeAssetParams(AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, SummaryResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, SummaryResponse);
	if (!Result)
	{
		return false;
	}

	bool bFoundVar = false;
	const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
	if ((*Result)->TryGetArrayField(TEXT("variables"), Vars) && Vars)
	{
		for (const TSharedPtr<FJsonValue>& V : *Vars)
		{
			const TSharedPtr<FJsonObject>* VObj = nullptr;
			FString Name;
			if (V->TryGetObject(VObj) && VObj && (*VObj)->TryGetStringField(TEXT("name"), Name) && Name == TEXT("Health"))
			{
				bFoundVar = true;
			}
		}
	}
	TestTrue(TEXT("'Health' variable should be present."), bFoundVar);

	bool bFoundComp = false;
	const TArray<TSharedPtr<FJsonValue>>* ResultComponents = nullptr;
	if ((*Result)->TryGetArrayField(TEXT("components"), ResultComponents) && ResultComponents)
	{
		for (const TSharedPtr<FJsonValue>& C : *ResultComponents)
		{
			const TSharedPtr<FJsonObject>* CObj = nullptr;
			FString Name;
			if (C->TryGetObject(CObj) && CObj && (*CObj)->TryGetStringField(TEXT("name"), Name) && Name == TEXT("Hitbox"))
			{
				bFoundComp = true;
			}
		}
	}
	TestTrue(TEXT("'Hitbox' component should be present."), bFoundComp);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCreateFromSpecFunctionTest, "BlueprintBridge.Blueprint.CreateFromSpec.Function", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCreateFromSpecFunctionTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = BlueprintBridgeTests::MakeUniqueSpecAssetPath(TEXT("Fn"));
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	Params->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
	Params->SetBoolField(TEXT("compile"), true);

	// Function: return Out = 42
	TSharedRef<FJsonObject> Fn = MakeShared<FJsonObject>();
	Fn->SetStringField(TEXT("name"), TEXT("Constant"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), TEXT("Out"));
	Out->SetStringField(TEXT("category"), TEXT("int"));
	Outputs.Add(MakeShared<FJsonValueObject>(Out));
	Fn->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetNumberField(TEXT("Out"), 42);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Fn->SetArrayField(TEXT("flow"), Flow);

	TArray<TSharedPtr<FJsonValue>> Functions;
	Functions.Add(MakeShared<FJsonValueObject>(Fn));
	Params->SetArrayField(TEXT("functions"), Functions);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateBlueprintFromSpec"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		AddError(FString::Printf(TEXT("CreateBlueprintFromSpec failed: %s"), *BlueprintBridgeTests::GetErrorMessage(Response)));
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	FString CompileStatus;
	TestTrue(TEXT("Response should include compile status."), Result && (*Result)->TryGetStringField(TEXT("compileStatus"), CompileStatus));
	TestEqual(TEXT("Compiled function should be UpToDate."), CompileStatus, FString(TEXT("UpToDate")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeCreateFromSpecRollbackTest, "BlueprintBridge.Blueprint.CreateFromSpec.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeCreateFromSpecRollbackTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = BlueprintBridgeTests::MakeUniqueSpecAssetPath(TEXT("Rollback"));
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(AssetPath);
	Params->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));

	// Function references a non-existent UFunction → lowering fails → rollback.
	TSharedRef<FJsonObject> Fn = MakeShared<FJsonObject>();
	Fn->SetStringField(TEXT("name"), TEXT("BadFn"));
	TSharedRef<FJsonObject> CallStmt = MakeShared<FJsonObject>();
	CallStmt->SetStringField(TEXT("call"), TEXT("/Script/Engine.NonExistentClass.NoSuchFunction"));
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CallStmt));
	Fn->SetArrayField(TEXT("flow"), Flow);

	TArray<TSharedPtr<FJsonValue>> Functions;
	Functions.Add(MakeShared<FJsonValueObject>(Fn));
	Params->SetArrayField(TEXT("functions"), Functions);
	
	AddExpectedError(
	TEXT("The package to load does not exist on disk or in the loader"),
	EAutomationExpectedErrorFlags::Contains,
	1
	);

	AddExpectedError(
		TEXT("Failed to find object 'Blueprint"),
		EAutomationExpectedErrorFlags::Contains,
		1
	);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateBlueprintFromSpec"), Params);
	bool bOk = true;
	Response->TryGetBoolField(TEXT("ok"), bOk);
	TestFalse(TEXT("Bad function spec should fail the whole creation."), bOk);

	// The asset should not have survived the rollback. Round-trip through the bridge so the test
	// doesn't depend on internal helpers; AssetNotFound is the expected error from DescribeBlueprint.
	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(AssetPath));
	return BlueprintBridgeTests::ExpectErrorCode(*this, DescribeResponse, TEXT("AssetNotFound"));
}

namespace BlueprintBridgeTests
{
// Create a function via ApplySemanticFunction returning a literal Out=int. Returns true on success.
static bool CreateInitialFunction(FAutomationTestBase& Test, const FTestBlueprintAsset& Asset, const FString& FunctionName, int32 InitialValue)
{
	TSharedRef<FJsonObject> Params = MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), FunctionName);
	Params->SetBoolField(TEXT("createIfMissing"), true);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), TEXT("Out"));
	Out->SetStringField(TEXT("category"), TEXT("int"));
	Outputs.Add(MakeShared<FJsonValueObject>(Out));
	Params->SetArrayField(TEXT("outputs"), Outputs);
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetNumberField(TEXT("Out"), InitialValue);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);
	return ExpectSuccess(Test, ExecuteJsonRequest(TEXT("ApplySemanticFunction"), Params));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeReplaceFunctionBodyTest, "BlueprintBridge.Blueprint.ReplaceSemanticFunction.Body", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeReplaceFunctionBodyTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ReplBody"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	if (!BlueprintBridgeTests::CreateInitialFunction(*this, Asset, TEXT("Body"), 1))
	{
		return false;
	}

	// Count nodes via SummarizeBlueprintGraph before/after — body change should preserve the count of result nodes
	// while function-call nodes for the new IR aren't present (return-with-literal lowers to defaults + exec links only).
	// Easiest verification: replace with a different literal, then read the result default via DescribeGraphFull.
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("Body"));
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetNumberField(TEXT("Out"), 99);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ReplaceSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	// Verify the new default landed: DescribeGraphFull on the function, look for result-node Out pin default "99".
	const TSharedRef<FJsonObject> Describe = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("Body")));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Describe))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Describe);
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	bool bFoundNewDefault = false;
	if (Result && (*Result)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
	{
		for (const TSharedPtr<FJsonValue>& N : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NObj = nullptr;
			FString ClassName;
			if (!N->TryGetObject(NObj) || !NObj) continue;
			(*NObj)->TryGetStringField(TEXT("class"), ClassName);
			if (!ClassName.Contains(TEXT("K2Node_FunctionResult"))) continue;
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			if (!(*NObj)->TryGetArrayField(TEXT("pins"), Pins) || !Pins) continue;
			for (const TSharedPtr<FJsonValue>& P : *Pins)
			{
				const TSharedPtr<FJsonObject>* PObj = nullptr;
				if (!P->TryGetObject(PObj) || !PObj) continue;
				FString PinName, DefaultValue;
				(*PObj)->TryGetStringField(TEXT("name"), PinName);
				(*PObj)->TryGetStringField(TEXT("defaultValue"), DefaultValue);
				if (PinName == TEXT("Out") && DefaultValue == TEXT("99"))
				{
					bFoundNewDefault = true;
				}
			}
		}
	}
	return TestTrue(TEXT("Out default should reflect the replaced body (99)."), bFoundNewDefault);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeReplaceFunctionPreservesSignatureTest, "BlueprintBridge.Blueprint.ReplaceSemanticFunction.PreservesSignature", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeReplaceFunctionPreservesSignatureTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ReplSigKeep"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	if (!BlueprintBridgeTests::CreateInitialFunction(*this, Asset, TEXT("Keep"), 1))
	{
		return false;
	}

	// Replace WITHOUT providing inputs/outputs. Signature (Out: int) should survive.
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("Keep"));
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetNumberField(TEXT("Out"), 7);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ReplaceSemanticFunction"), Params)))
	{
		return false;
	}

	// Verify the Out output pin is still present.
	const TSharedRef<FJsonObject> Summary = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SummarizeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Summary))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Summary);
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	bool bSignatureKept = false;
	if (Result && (*Result)->TryGetObjectField(TEXT("graphs"), Graphs) && Graphs && (*Graphs)->TryGetArrayField(TEXT("functions"), Functions) && Functions)
	{
		for (const TSharedPtr<FJsonValue>& F : *Functions)
		{
			const TSharedPtr<FJsonObject>* FObj = nullptr;
			FString FName;
			if (!F->TryGetObject(FObj) || !FObj) continue;
			(*FObj)->TryGetStringField(TEXT("name"), FName);
			if (FName != TEXT("Keep")) continue;
			const TSharedPtr<FJsonObject>* Sig = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if (!(*FObj)->TryGetObjectField(TEXT("signature"), Sig) || !Sig) continue;
			if (!(*Sig)->TryGetArrayField(TEXT("outputs"), Outputs) || !Outputs) continue;
			for (const TSharedPtr<FJsonValue>& O : *Outputs)
			{
				const TSharedPtr<FJsonObject>* OObj = nullptr;
				FString OName;
				if (O->TryGetObject(OObj) && OObj && (*OObj)->TryGetStringField(TEXT("name"), OName) && OName == TEXT("Out"))
				{
					bSignatureKept = true;
				}
			}
		}
	}
	return TestTrue(TEXT("Original 'Out' output pin should survive when inputs/outputs are omitted."), bSignatureKept);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeReplaceFunctionRollbackTest, "BlueprintBridge.Blueprint.ReplaceSemanticFunction.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeReplaceFunctionRollbackTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ReplRollback"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	if (!BlueprintBridgeTests::CreateInitialFunction(*this, Asset, TEXT("Rb"), 5))
	{
		return false;
	}

	// Replace with IR that fails lowering (non-existent function) — transaction should roll back the wipe.
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("Rb"));
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> BadCall = MakeShared<FJsonObject>();
	BadCall->SetStringField(TEXT("call"), TEXT("/Script/Engine.NonExistentClass.NoSuchPureFn"));
	ReturnFields->SetObjectField(TEXT("Out"), BadCall);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ReplaceSemanticFunction"), Params);
	bool bOk = true;
	Response->TryGetBoolField(TEXT("ok"), bOk);
	TestFalse(TEXT("Bad lowering should fail."), bOk);

	// Verify rollback: original default value (5) should still be wired on the result node.
	const TSharedRef<FJsonObject> Describe = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, TEXT("Rb")));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Describe))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Describe);
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	bool bOriginalDefaultIntact = false;
	if (Result && (*Result)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
	{
		for (const TSharedPtr<FJsonValue>& N : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NObj = nullptr;
			FString ClassName;
			if (!N->TryGetObject(NObj) || !NObj) continue;
			(*NObj)->TryGetStringField(TEXT("class"), ClassName);
			if (!ClassName.Contains(TEXT("K2Node_FunctionResult"))) continue;
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			if (!(*NObj)->TryGetArrayField(TEXT("pins"), Pins) || !Pins) continue;
			for (const TSharedPtr<FJsonValue>& P : *Pins)
			{
				const TSharedPtr<FJsonObject>* PObj = nullptr;
				if (!P->TryGetObject(PObj) || !PObj) continue;
				FString PinName, DefaultValue;
				(*PObj)->TryGetStringField(TEXT("name"), PinName);
				(*PObj)->TryGetStringField(TEXT("defaultValue"), DefaultValue);
				if (PinName == TEXT("Out") && DefaultValue == TEXT("5"))
				{
					bOriginalDefaultIntact = true;
				}
			}
		}
	}
	return TestTrue(TEXT("Original default (5) should be intact after rollback."), bOriginalDefaultIntact);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeReplaceFunctionCompileTest, "BlueprintBridge.Blueprint.ReplaceSemanticFunction.Compile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeReplaceFunctionCompileTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ReplCompile"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	if (!BlueprintBridgeTests::CreateInitialFunction(*this, Asset, TEXT("C"), 0))
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("C"));
	Params->SetBoolField(TEXT("compile"), true);
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetNumberField(TEXT("Out"), 42);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ReplaceSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	FString CompileStatus;
	TestTrue(TEXT("Compile status should be present."), Result && (*Result)->TryGetStringField(TEXT("compileStatus"), CompileStatus));
	return TestEqual(TEXT("Compiled function should be UpToDate."), CompileStatus, FString(TEXT("UpToDate")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeResolveSymbolEngineFunctionTest, "BlueprintBridge.Reflection.ResolveSymbol.EngineFunction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeResolveSymbolEngineFunctionTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("kind"), TEXT("function"));
	Params->SetStringField(TEXT("class"), TEXT("/Script/Engine.Actor"));
	Params->SetStringField(TEXT("member"), TEXT("K2_GetActorLocation"));

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ResolveSymbol"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}
	FString Kind;
	(*Result)->TryGetStringField(TEXT("kind"), Kind);
	TestEqual(TEXT("Engine UFUNCTION should resolve as kind=function (has ModuleRelativePath)."), Kind, FString(TEXT("function")));
	FString HeaderPath;
	TestTrue(TEXT("Engine UFUNCTION should expose headerPath."), (*Result)->TryGetStringField(TEXT("headerPath"), HeaderPath) && HeaderPath.EndsWith(TEXT(".h")));
	FString Signature;
	TestTrue(TEXT("Should expose C++-style signature."), (*Result)->TryGetStringField(TEXT("signature"), Signature) && Signature.Contains(TEXT("K2_GetActorLocation")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeResolveSymbolProjectClassTest, "BlueprintBridge.Reflection.ResolveSymbol.ProjectClass", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeResolveSymbolProjectClassTest::RunTest(const FString& Parameters)
{
	// Use UObject as a stand-in for "any C++ class with ModuleRelativePath" — guaranteed present on every UE install,
	// and avoids hard-coding a Biscuit-specific class that may rename. The point of the test is the resolution path,
	// not which project the class lives in.
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("kind"), TEXT("class"));
	Params->SetStringField(TEXT("class"), TEXT("/Script/CoreUObject.Object"));

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ResolveSymbol"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	FString Module;
	TestTrue(TEXT("Should expose module name."), Result && (*Result)->TryGetStringField(TEXT("module"), Module) && !Module.IsEmpty());
	FString HeaderPath;
	TestTrue(TEXT("Should expose headerPath."), Result && (*Result)->TryGetStringField(TEXT("headerPath"), HeaderPath) && HeaderPath.EndsWith(TEXT(".h")));
	// absoluteHeaderPath only resolves for game-module/plugin sources; engine modules are intentionally skipped.
	// So we don't assert its presence here — only that when present, it points to an existing file.
	FString Absolute;
	if (Result && (*Result)->TryGetStringField(TEXT("absoluteHeaderPath"), Absolute))
	{
		TestTrue(TEXT("absoluteHeaderPath when reported should exist on disk."), FPaths::FileExists(Absolute));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeResolveSymbolBlueprintFunctionTest, "BlueprintBridge.Reflection.ResolveSymbol.BlueprintFunction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeResolveSymbolBlueprintFunctionTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ResolveBP"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	TSharedRef<FJsonObject> CreateFnParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	CreateFnParams->SetStringField(TEXT("function"), TEXT("BPOnlyFunction"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateFunctionGraph"), CreateFnParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("kind"), TEXT("function"));
	Params->SetStringField(TEXT("class"), Asset.AssetPath);
	Params->SetStringField(TEXT("member"), TEXT("BPOnlyFunction"));

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ResolveSymbol"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	FString Kind;
	TestTrue(TEXT("BP-defined function should resolve as kind=blueprint (no C++ source)."), Result && (*Result)->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("blueprint"));
	TestFalse(TEXT("BP-defined function should not expose headerPath."), Result && (*Result)->HasField(TEXT("headerPath")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeResolveSymbolUnknownTest, "BlueprintBridge.Reflection.ResolveSymbol.Unknown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeResolveSymbolUnknownTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("kind"), TEXT("function"));
	Params->SetStringField(TEXT("class"), TEXT("/Script/Engine.Actor"));
	Params->SetStringField(TEXT("member"), TEXT("NoSuchFunctionExistsHere"));

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ResolveSymbol"), Params);
	return BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("FunctionNotFound"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeApplyAndFixSuccessTest, "BlueprintBridge.Blueprint.ApplyAndFix.Success", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeApplyAndFixSuccessTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FixOk"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("Constant"));
	Params->SetBoolField(TEXT("createIfMissing"), true);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("name"), TEXT("Out"));
	Out->SetStringField(TEXT("category"), TEXT("int"));
	Outputs.Add(MakeShared<FJsonValueObject>(Out));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetNumberField(TEXT("Out"), 42);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplyAndFix"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		AddError(FString::Printf(TEXT("ApplyAndFix failed: %s"), *BlueprintBridgeTests::GetErrorMessage(Response)));
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	FString CompileStatus;
	TestTrue(TEXT("Success response should carry compileStatus."), Result && (*Result)->TryGetStringField(TEXT("compileStatus"), CompileStatus));
	TestEqual(TEXT("compileStatus should be UpToDate."), CompileStatus, FString(TEXT("UpToDate")));
	TestTrue(TEXT("Success response should include resolutions map."), Result && (*Result)->HasField(TEXT("resolutions")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeApplyAndFixCompileErrorTest, "BlueprintBridge.Blueprint.ApplyAndFix.CompileError", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeApplyAndFixCompileErrorTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FixErr"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	// Latent function (KismetSystemLibrary.Delay) in a function graph — Blueprint compiler errors
	// with "Latent functions can only be called in event graphs". A reliable, narrow compile error.
	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("BadFn"));
	Params->SetBoolField(TEXT("createIfMissing"), true);

	TSharedRef<FJsonObject> CallStmt = MakeShared<FJsonObject>();
	CallStmt->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetSystemLibrary.Delay"));
	TSharedRef<FJsonObject> CallArgs = MakeShared<FJsonObject>();
	CallArgs->SetNumberField(TEXT("Duration"), 1.0);
	CallStmt->SetObjectField(TEXT("args"), CallArgs);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CallStmt));
	Params->SetArrayField(TEXT("flow"), Flow);
	
	AddExpectedError(TEXT("contains a latent call, which cannot exist outside of the event graph"),
	EAutomationExpectedErrorFlags::Contains,
	1);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplyAndFix"), Params);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("CompileFailed")))
	{
		AddError(FString::Printf(TEXT("Underlying error message: %s"), *BlueprintBridgeTests::GetErrorMessage(Response)));
		return false;
	}
	const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
	Response->TryGetObjectField(TEXT("error"), ErrorObj);
	if (!ErrorObj)
	{
		return false;
	}
	TestTrue(TEXT("CompileFailed should include compileStatus."), (*ErrorObj)->HasField(TEXT("compileStatus")));
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	TestTrue(TEXT("CompileFailed should include non-empty messages array."), (*ErrorObj)->TryGetArrayField(TEXT("messages"), Messages) && Messages != nullptr && Messages->Num() > 0);
	TestTrue(TEXT("CompileFailed should echo appliedPatch for IR↔node correlation."), (*ErrorObj)->HasField(TEXT("appliedPatch")));
	TestTrue(TEXT("CompileFailed should echo resolutions."), (*ErrorObj)->HasField(TEXT("resolutions")));
	bool bRolledBack = true;
	TestTrue(TEXT("rolledBack should be reported."), (*ErrorObj)->TryGetBoolField(TEXT("rolledBack"), bRolledBack));
	TestFalse(TEXT("Default is no rollback — the function should still be in the BP after a compile error."), bRolledBack);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeApplyAndFixRollbackTest, "BlueprintBridge.Blueprint.ApplyAndFix.Rollback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeApplyAndFixRollbackTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FixRollback"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("BadFn"));
	Params->SetBoolField(TEXT("createIfMissing"), true);
	Params->SetBoolField(TEXT("rollbackOnCompileError"), true);

	TSharedRef<FJsonObject> CallStmt = MakeShared<FJsonObject>();
	CallStmt->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetSystemLibrary.Delay"));
	TSharedRef<FJsonObject> CallArgs = MakeShared<FJsonObject>();
	CallArgs->SetNumberField(TEXT("Duration"), 1.0);
	CallStmt->SetObjectField(TEXT("args"), CallArgs);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CallStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	AddExpectedError(TEXT("contains a latent call, which cannot exist outside of the event graph"),
	EAutomationExpectedErrorFlags::Contains,
	1);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplyAndFix"), Params);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("CompileFailed")))
	{
		AddError(FString::Printf(TEXT("Underlying error message: %s"), *BlueprintBridgeTests::GetErrorMessage(Response)));
		return false;
	}
	const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
	Response->TryGetObjectField(TEXT("error"), ErrorObj);
	bool bRolledBack = false;
	TestTrue(TEXT("rolledBack should be true when rollbackOnCompileError is set."), ErrorObj && (*ErrorObj)->TryGetBoolField(TEXT("rolledBack"), bRolledBack) && bRolledBack);

	// Verify rollback: BadFn function should NOT exist on the Blueprint.
	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	bool bFoundBadFn = false;
	if (DescribeResult && (*DescribeResult)->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs)
	{
		for (const TSharedPtr<FJsonValue>& G : *Graphs)
		{
			FString Name;
			if (G->TryGetString(Name) && Name == TEXT("BadFn"))
			{
				bFoundBadFn = true;
			}
		}
	}
	TestFalse(TEXT("Rolled-back function should not appear in DescribeBlueprint graphs list."), bFoundBadFn);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeApplyAndFixMessagesShapeTest, "BlueprintBridge.Blueprint.ApplyAndFix.MessagesShape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeApplyAndFixMessagesShapeTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FixMsg"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("BadFn"));
	Params->SetBoolField(TEXT("createIfMissing"), true);
	TSharedRef<FJsonObject> CallStmt = MakeShared<FJsonObject>();
	CallStmt->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetSystemLibrary.Delay"));
	TSharedRef<FJsonObject> CallArgs = MakeShared<FJsonObject>();
	CallArgs->SetNumberField(TEXT("Duration"), 1.0);
	CallStmt->SetObjectField(TEXT("args"), CallArgs);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CallStmt));
	Params->SetArrayField(TEXT("flow"), Flow);
	
	AddExpectedError(TEXT("contains a latent call, which cannot exist outside of the event graph"),
	EAutomationExpectedErrorFlags::Contains,
	1);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplyAndFix"), Params);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("CompileFailed")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
	Response->TryGetObjectField(TEXT("error"), ErrorObj);
	const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
	if (!ErrorObj || !(*ErrorObj)->TryGetArrayField(TEXT("messages"), Messages) || !Messages || Messages->Num() == 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FirstMsg = nullptr;
	(*Messages)[0]->TryGetObject(FirstMsg);
	TestTrue(TEXT("Message should carry severity."), FirstMsg && (*FirstMsg)->HasField(TEXT("severity")));
	TestTrue(TEXT("Message should carry message text."), FirstMsg && (*FirstMsg)->HasField(TEXT("message")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetCommandErrorsTest, "BlueprintBridge.Blueprint.AssetCommandErrors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetCommandErrorsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> MissingAssetParams = BlueprintBridgeTests::MakeAssetParams(TEXT("/Game/BlueprintBridgeTests/BP_DoesNotExist"));
	const TSharedRef<FJsonObject> CheckoutResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CheckoutAsset"), MissingAssetParams);
	return BlueprintBridgeTests::ExpectErrorCode(*this, CheckoutResponse, TEXT("AssetNotFound"));
}

namespace BlueprintBridgeTests
{
static TSharedRef<FJsonObject> MakePinSpec(const FString& Name, const FString& Category)
{
	TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
	Pin->SetStringField(TEXT("name"), Name);
	Pin->SetStringField(TEXT("category"), Category);
	return Pin;
}

static TSharedRef<FJsonObject> MakeStatement(const FString& Key, const TSharedPtr<FJsonValue>& Value)
{
	TSharedRef<FJsonObject> Stmt = MakeShared<FJsonObject>();
	Stmt->SetField(Key, Value);
	return Stmt;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLowerLiteralReturnTest, "BlueprintBridge.Blueprint.SemanticIR.LowerLiteralReturn", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLowerLiteralReturnTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLitReturn"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("ScoreFn"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Score"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);
	TArray<TSharedPtr<FJsonValue>> Flow;
	TSharedRef<FJsonObject> ReturnExpr = MakeShared<FJsonObject>();
	ReturnExpr->SetNumberField(TEXT("Score"), 42);
	Flow.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeStatement(TEXT("return"), MakeShared<FJsonValueObject>(ReturnExpr))));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Patch = nullptr;
	TestTrue(TEXT("Result should contain patch."), Result && (*Result)->TryGetObjectField(TEXT("patch"), Patch) && Patch && Patch->IsValid());
	if (!Patch || !Patch->IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	(*Patch)->TryGetArrayField(TEXT("nodes"), Nodes);
	TestTrue(TEXT("Literal return should emit no body nodes."), Nodes != nullptr && Nodes->Num() == 0);

	const TArray<TSharedPtr<FJsonValue>>* Defaults = nullptr;
	bool bFoundScoreDefault = false;
	if ((*Patch)->TryGetArrayField(TEXT("defaults"), Defaults) && Defaults != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Defaults)
		{
			const TSharedPtr<FJsonObject>* DefObj = nullptr;
			if (Value->TryGetObject(DefObj) && DefObj && DefObj->IsValid())
			{
				FString Node, Pin, Lit;
				(*DefObj)->TryGetStringField(TEXT("node"), Node);
				(*DefObj)->TryGetStringField(TEXT("pin"), Pin);
				(*DefObj)->TryGetStringField(TEXT("value"), Lit);
				if (Node == TEXT("result") && Pin == TEXT("Score") && Lit == TEXT("42"))
				{
					bFoundScoreDefault = true;
				}
			}
		}
	}
	TestTrue(TEXT("Result.Score default should be the integer 42, not 42.0."), bFoundScoreDefault);

	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	bool bFoundExecLink = false;
	if ((*Patch)->TryGetArrayField(TEXT("links"), Links) && Links != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Links)
		{
			const TSharedPtr<FJsonObject>* LinkObj = nullptr;
			if (Value->TryGetObject(LinkObj) && LinkObj && LinkObj->IsValid())
			{
				FString From, To;
				(*LinkObj)->TryGetStringField(TEXT("from"), From);
				(*LinkObj)->TryGetStringField(TEXT("to"), To);
				if (From == TEXT("entry.then") && To == TEXT("result.execute"))
				{
					bFoundExecLink = true;
				}
			}
		}
	}
	return TestTrue(TEXT("Lowering should link entry.then to result.execute on return."), bFoundExecLink);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRImplicitResolutionTest, "BlueprintBridge.Blueprint.SemanticIR.ImplicitResolution", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRImplicitResolutionTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemResolve"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> AddVarParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVarParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVarParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVarParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("ResolveFn"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Offset"), TEXT("int"))));
	Params->SetArrayField(TEXT("inputs"), Inputs);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetStringField(TEXT("Out"), TEXT("Health"));
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	TestTrue(TEXT("Result should contain resolutions."), Result && (*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) && Resolutions && Resolutions->IsValid());
	if (!Resolutions || !Resolutions->IsValid())
	{
		return false;
	}
	FString Resolved;
	TestTrue(TEXT("'Health' should resolve to member variable."), (*Resolutions)->TryGetStringField(TEXT("flow[0].return.Out"), Resolved) && Resolved == TEXT("var:Health"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRImpureInExpressionTest, "BlueprintBridge.Blueprint.SemanticIR.ImpureInExpression", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRImpureInExpressionTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemImpureExpr"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("BadFn"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("string"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	// v2 behavior: impure call (PrintString) used in expression position auto-hoists
	// into the exec chain rather than erroring (the v1 error required the caller to
	// manually hoist via set/var). The lowering should succeed and annotate the call
	// site with 'hoist:impure' in the resolutions map.
	TSharedRef<FJsonObject> CallExpr = MakeShared<FJsonObject>();
	CallExpr->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetSystemLibrary.PrintString"));
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetObjectField(TEXT("Out"), CallExpr);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	TestTrue(TEXT("Lowering should succeed and surface a resolutions map."), Result && (*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) && Resolutions && Resolutions->IsValid());
	if (!Resolutions || !Resolutions->IsValid()) { return false; }
	FString Hoist;
	TestTrue(TEXT("Auto-hoist should annotate the impure call site as 'hoist:impure'."), (*Resolutions)->TryGetStringField(TEXT("flow[0].return.Out.call"), Hoist) && Hoist == TEXT("hoist:impure"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRStrictPureExpressionsTest, "BlueprintBridge.Blueprint.SemanticIR.StrictPureExpressions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRStrictPureExpressionsTest::RunTest(const FString& Parameters)
{
	// Opt-in: strictPureExpressions=true restores the v1 hard error for impure calls
	// in expression position, for callers that want validation rather than auto-hoist.
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemStrictPure"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("StrictFn"));
	Params->SetBoolField(TEXT("strictPureExpressions"), true);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("string"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> CallExpr = MakeShared<FJsonObject>();
	CallExpr->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetSystemLibrary.PrintString"));
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetObjectField(TEXT("Out"), CallExpr);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("SemanticLoweringFailed"))) { return false; }
	const TSharedPtr<FJsonObject>* Error = nullptr;
	Response->TryGetObjectField(TEXT("error"), Error);
	FString Message;
	Error && (*Error)->TryGetStringField(TEXT("message"), Message);
	return TestTrue(TEXT("Strict error should mention strictPureExpressions."), Message.Contains(TEXT("strictPureExpressions")));
}

namespace BlueprintBridgeTests
{
// Count nodes of a specific 'type' (or matching FunctionCall by function name) in a lowered patch.
static int32 CountPatchNodes(const TSharedPtr<FJsonObject>& Result, const FString& NodeType, const FString& FunctionName = FString())
{
	if (!Result.IsValid()) { return 0; }
	const TSharedPtr<FJsonObject>* Patch = nullptr;
	if (!Result->TryGetObjectField(TEXT("patch"), Patch) || !Patch || !(*Patch).IsValid()) { return 0; }
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!(*Patch)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes) { return 0; }
	int32 Count = 0;
	for (const TSharedPtr<FJsonValue>& V : *Nodes)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!V->TryGetObject(NodeObj) || !NodeObj) { continue; }
		FString Type;
		(*NodeObj)->TryGetStringField(TEXT("type"), Type);
		if (Type != NodeType) { continue; }
		if (FunctionName.IsEmpty()) { ++Count; continue; }
		FString Func;
		if ((*NodeObj)->TryGetStringField(TEXT("function"), Func) && Func == FunctionName) { ++Count; }
	}
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLetMultipleUseTest, "BlueprintBridge.Blueprint.SemanticIR.Let.MultipleUse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLetMultipleUseTest::RunTest(const FString& Parameters)
{
	// A let-bound value referenced twice should produce ONE source node, not two.
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLetMulti"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> AddVarParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVarParams->SetStringField(TEXT("name"), TEXT("Hp"));
	AddVarParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVarParams))) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("LetMulti"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	// let n = Hp in [ if (n == 5) then return Out=n else return Out=n ]
	TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
	Binding->SetStringField(TEXT("name"), TEXT("n"));
	Binding->SetStringField(TEXT("value"), TEXT("Hp"));
	TArray<TSharedPtr<FJsonValue>> Bindings; Bindings.Add(MakeShared<FJsonValueObject>(Binding));

	TSharedRef<FJsonObject> EqExpr = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EqOps;
	EqOps.Add(MakeShared<FJsonValueString>(TEXT("n")));
	EqOps.Add(MakeShared<FJsonValueNumber>(5));
	EqExpr->SetArrayField(TEXT("=="), EqOps);

	auto MakeReturnN = []()
	{
		TSharedRef<FJsonObject> Ret = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		Fields->SetStringField(TEXT("Out"), TEXT("n"));
		Ret->SetObjectField(TEXT("return"), Fields);
		return Ret;
	};

	TSharedRef<FJsonObject> IfStmt = MakeShared<FJsonObject>();
	IfStmt->SetObjectField(TEXT("if"), EqExpr);
	TArray<TSharedPtr<FJsonValue>> ThenArr; ThenArr.Add(MakeShared<FJsonValueObject>(MakeReturnN()));
	TArray<TSharedPtr<FJsonValue>> ElseArr; ElseArr.Add(MakeShared<FJsonValueObject>(MakeReturnN()));
	IfStmt->SetArrayField(TEXT("then"), ThenArr);
	IfStmt->SetArrayField(TEXT("else"), ElseArr);

	TSharedRef<FJsonObject> LetStmt = MakeShared<FJsonObject>();
	LetStmt->SetArrayField(TEXT("let"), Bindings);
	TArray<TSharedPtr<FJsonValue>> InArr; InArr.Add(MakeShared<FJsonValueObject>(IfStmt));
	LetStmt->SetArrayField(TEXT("in"), InArr);

	TArray<TSharedPtr<FJsonValue>> Flow; Flow.Add(MakeShared<FJsonValueObject>(LetStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const int32 GetCount = BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("VariableGet"));
	return TestEqual(TEXT("Let-bound variable referenced 3 times should produce exactly 1 VariableGet node."), GetCount, 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLetShadowingTest, "BlueprintBridge.Blueprint.SemanticIR.Let.Shadowing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLetShadowingTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLetShadow"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("LetShadow"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	// Outer: let n = 1 in [ Inner: let n = 2 in [ return Out=n ] ]
	auto MakeLit = [](double V)
	{
		TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
		Binding->SetStringField(TEXT("name"), TEXT("n"));
		Binding->SetNumberField(TEXT("value"), V);
		return Binding;
	};

	TSharedRef<FJsonObject> ReturnN = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
	Fields->SetStringField(TEXT("Out"), TEXT("n"));
	ReturnN->SetObjectField(TEXT("return"), Fields);

	TSharedRef<FJsonObject> InnerLet = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> InnerBindings; InnerBindings.Add(MakeShared<FJsonValueObject>(MakeLit(2)));
	InnerLet->SetArrayField(TEXT("let"), InnerBindings);
	TArray<TSharedPtr<FJsonValue>> InnerIn; InnerIn.Add(MakeShared<FJsonValueObject>(ReturnN));
	InnerLet->SetArrayField(TEXT("in"), InnerIn);

	TSharedRef<FJsonObject> OuterLet = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> OuterBindings; OuterBindings.Add(MakeShared<FJsonValueObject>(MakeLit(1)));
	OuterLet->SetArrayField(TEXT("let"), OuterBindings);
	TArray<TSharedPtr<FJsonValue>> OuterIn; OuterIn.Add(MakeShared<FJsonValueObject>(InnerLet));
	OuterLet->SetArrayField(TEXT("in"), OuterIn);

	TArray<TSharedPtr<FJsonValue>> Flow; Flow.Add(MakeShared<FJsonValueObject>(OuterLet));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	TestTrue(TEXT("'n' in inner block should resolve to scope binding (inner shadow)."), (*Resolutions)->TryGetStringField(TEXT("flow[0].in[0].in[0].return.Out"), Resolved) && Resolved == TEXT("scope:n"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLetScopePopTest, "BlueprintBridge.Blueprint.SemanticIR.Let.ScopePop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLetScopePopTest::RunTest(const FString& Parameters)
{
	// A name introduced by let must not leak past its 'in' block. A reference after the
	// let should resolve as a string literal (existing bare-string fallback), not as the
	// scoped binding.
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLetPop"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("LetPop"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("string"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> LetStmt = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
	Binding->SetStringField(TEXT("name"), TEXT("scoped_name"));
	Binding->SetNumberField(TEXT("value"), 99);
	TArray<TSharedPtr<FJsonValue>> Bindings; Bindings.Add(MakeShared<FJsonValueObject>(Binding));
	LetStmt->SetArrayField(TEXT("let"), Bindings);
	TArray<TSharedPtr<FJsonValue>> InArr; // empty body — binding is unused
	LetStmt->SetArrayField(TEXT("in"), InArr);

	TSharedRef<FJsonObject> AfterReturn = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
	Fields->SetStringField(TEXT("Out"), TEXT("scoped_name"));
	AfterReturn->SetObjectField(TEXT("return"), Fields);

	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(LetStmt));
	Flow.Add(MakeShared<FJsonValueObject>(AfterReturn));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	TestTrue(TEXT("After let exits, name should fall through to string literal — not the popped scope binding."), (*Resolutions)->TryGetStringField(TEXT("flow[1].return.Out"), Resolved) && Resolved == TEXT("literal:string"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIROperatorEqIntTest, "BlueprintBridge.Blueprint.SemanticIR.Operator.EqInt", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIROperatorEqIntTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemOpEqInt"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("OpEqInt"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("X"), TEXT("int"))));
	Params->SetArrayField(TEXT("inputs"), Inputs);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Eq"), TEXT("bool"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> EqExpr = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> EqOps;
	EqOps.Add(MakeShared<FJsonValueString>(TEXT("X")));
	EqOps.Add(MakeShared<FJsonValueNumber>(5));
	EqExpr->SetArrayField(TEXT("=="), EqOps);

	TSharedRef<FJsonObject> Ret = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
	Fields->SetObjectField(TEXT("Eq"), EqExpr);
	Ret->SetObjectField(TEXT("return"), Fields);

	TArray<TSharedPtr<FJsonValue>> Flow; Flow.Add(MakeShared<FJsonValueObject>(Ret));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const int32 EqIntInt = BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("FunctionCall"), TEXT("EqualEqual_IntInt"));
	return TestEqual(TEXT("int == int should dispatch to EqualEqual_IntInt."), EqIntInt, 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIROperatorLogicalAndTest, "BlueprintBridge.Blueprint.SemanticIR.Operator.LogicalAnd", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIROperatorLogicalAndTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemOpAnd"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("OpAnd"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("A"), TEXT("bool"))));
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("B"), TEXT("bool"))));
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("C"), TEXT("bool"))));
	Params->SetArrayField(TEXT("inputs"), Inputs);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("R"), TEXT("bool"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> AndExpr = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AndOps;
	AndOps.Add(MakeShared<FJsonValueString>(TEXT("A")));
	AndOps.Add(MakeShared<FJsonValueString>(TEXT("B")));
	AndOps.Add(MakeShared<FJsonValueString>(TEXT("C")));
	AndExpr->SetArrayField(TEXT("and"), AndOps);

	TSharedRef<FJsonObject> Ret = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
	Fields->SetObjectField(TEXT("R"), AndExpr);
	Ret->SetObjectField(TEXT("return"), Fields);

	TArray<TSharedPtr<FJsonValue>> Flow; Flow.Add(MakeShared<FJsonValueObject>(Ret));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const int32 AndCount = BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("FunctionCall"), TEXT("BooleanAND"));
	return TestEqual(TEXT("Ternary 'and' should chain into 2 BooleanAND nodes."), AndCount, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIROperatorLogicalNotTest, "BlueprintBridge.Blueprint.SemanticIR.Operator.LogicalNot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIROperatorLogicalNotTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemOpNot"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("OpNot"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Flag"), TEXT("bool"))));
	Params->SetArrayField(TEXT("inputs"), Inputs);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("R"), TEXT("bool"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> NotExpr = MakeShared<FJsonObject>();
	NotExpr->SetStringField(TEXT("not"), TEXT("Flag"));

	TSharedRef<FJsonObject> Ret = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
	Fields->SetObjectField(TEXT("R"), NotExpr);
	Ret->SetObjectField(TEXT("return"), Fields);

	TArray<TSharedPtr<FJsonValue>> Flow; Flow.Add(MakeShared<FJsonValueObject>(Ret));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const int32 NotCount = BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("FunctionCall"), TEXT("Not_PreBool"));
	return TestEqual(TEXT("'not' should produce exactly one Not_PreBool node."), NotCount, 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRAutoHoistInIfTest, "BlueprintBridge.Blueprint.SemanticIR.AutoHoist.InIfCondition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRAutoHoistInIfTest::RunTest(const FString& Parameters)
{
	// Impure call as an 'if' condition should auto-hoist with no error.
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemHoistIf"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("HoistIf"));

	TSharedRef<FJsonObject> CallExpr = MakeShared<FJsonObject>();
	// PrintString returns nothing useful but the lowering shouldn't care about that —
	// the hoist path just needs an impure UFunction. We pick PrintString because it's
	// reliably present in the engine.
	CallExpr->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetSystemLibrary.PrintString"));

	TSharedRef<FJsonObject> IfStmt = MakeShared<FJsonObject>();
	IfStmt->SetObjectField(TEXT("if"), CallExpr);
	IfStmt->SetArrayField(TEXT("then"), TArray<TSharedPtr<FJsonValue>>());
	IfStmt->SetArrayField(TEXT("else"), TArray<TSharedPtr<FJsonValue>>());

	TArray<TSharedPtr<FJsonValue>> Flow; Flow.Add(MakeShared<FJsonValueObject>(IfStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Hoist;
	return TestTrue(TEXT("Impure call in if-condition should be annotated as 'hoist:impure'."), (*Resolutions)->TryGetStringField(TEXT("flow[0].if.call"), Hoist) && Hoist == TEXT("hoist:impure"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRPureCallAsStatementTest, "BlueprintBridge.Blueprint.SemanticIR.PureCallAsStatement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRPureCallAsStatementTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemPureStmt"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("BadFn"));
	TSharedRef<FJsonObject> CallStmt = MakeShared<FJsonObject>();
	CallStmt->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetMathLibrary.EqualEqual_IntInt"));
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CallStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("SemanticLoweringFailed")))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Error = nullptr;
	Response->TryGetObjectField(TEXT("error"), Error);
	FString Message;
	Error && (*Error)->TryGetStringField(TEXT("message"), Message);
	TestTrue(TEXT("Error should mention dead-code purity."), Message.Contains(TEXT("dead code")));
	return true;
}

namespace BlueprintBridgeTests
{
static int32 CountPatchLinksTo(const TSharedPtr<FJsonObject>& Result, const FString& FromSuffix, const FString& To)
{
	if (!Result.IsValid()) { return 0; }
	const TSharedPtr<FJsonObject>* Patch = nullptr;
	if (!Result->TryGetObjectField(TEXT("patch"), Patch) || !Patch || !(*Patch).IsValid()) { return 0; }
	const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
	if (!(*Patch)->TryGetArrayField(TEXT("links"), Links) || !Links) { return 0; }

	int32 Count = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Links)
	{
		const TSharedPtr<FJsonObject>* LinkObj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(LinkObj) || !LinkObj) { continue; }
		FString From;
		FString LinkTo;
		(*LinkObj)->TryGetStringField(TEXT("from"), From);
		(*LinkObj)->TryGetStringField(TEXT("to"), LinkTo);
		if (From.EndsWith(FromSuffix) && LinkTo == To)
		{
			++Count;
		}
	}
	return Count;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRCastBasicTest, "BlueprintBridge.Blueprint.SemanticIR.Cast.Basic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRCastBasicTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemCastBasic"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("CastBasic"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("object"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> SelfExpr = MakeShared<FJsonObject>();
	SelfExpr->SetBoolField(TEXT("self"), true);
	TSharedRef<FJsonObject> CastStmt = MakeShared<FJsonObject>();
	CastStmt->SetObjectField(TEXT("cast"), SelfExpr);
	CastStmt->SetStringField(TEXT("to"), TEXT("/Script/Engine.Actor"));
	CastStmt->SetStringField(TEXT("as"), TEXT("AsActor"));

	TSharedRef<FJsonObject> ThenReturnFields = MakeShared<FJsonObject>();
	ThenReturnFields->SetStringField(TEXT("Out"), TEXT("AsActor"));
	TSharedRef<FJsonObject> ThenReturn = MakeShared<FJsonObject>();
	ThenReturn->SetObjectField(TEXT("return"), ThenReturnFields);
	TArray<TSharedPtr<FJsonValue>> ThenBlock;
	ThenBlock.Add(MakeShared<FJsonValueObject>(ThenReturn));
	CastStmt->SetArrayField(TEXT("then"), ThenBlock);

	TSharedRef<FJsonObject> ElseSelf = MakeShared<FJsonObject>();
	ElseSelf->SetBoolField(TEXT("self"), true);
	TSharedRef<FJsonObject> ElseReturnFields = MakeShared<FJsonObject>();
	ElseReturnFields->SetObjectField(TEXT("Out"), ElseSelf);
	TSharedRef<FJsonObject> ElseReturn = MakeShared<FJsonObject>();
	ElseReturn->SetObjectField(TEXT("return"), ElseReturnFields);
	TArray<TSharedPtr<FJsonValue>> ElseBlock;
	ElseBlock.Add(MakeShared<FJsonValueObject>(ElseReturn));
	CastStmt->SetArrayField(TEXT("else"), ElseBlock);

	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CastStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("cast statement should emit exactly one DynamicCast node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("DynamicCast")), 1);

	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	return TestTrue(TEXT("Cast result name should resolve from scope inside then."), (*Resolutions)->TryGetStringField(TEXT("flow[0].then[0].return.Out"), Resolved) && Resolved == TEXT("scope:AsActor"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRCastScopePopTest, "BlueprintBridge.Blueprint.SemanticIR.Cast.ScopePop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRCastScopePopTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemCastPop"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("CastPop"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("string"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> SelfExpr = MakeShared<FJsonObject>();
	SelfExpr->SetBoolField(TEXT("self"), true);
	TSharedRef<FJsonObject> CastStmt = MakeShared<FJsonObject>();
	CastStmt->SetObjectField(TEXT("cast"), SelfExpr);
	CastStmt->SetStringField(TEXT("to"), TEXT("/Script/Engine.Actor"));
	CastStmt->SetStringField(TEXT("as"), TEXT("AsActor"));
	CastStmt->SetArrayField(TEXT("then"), TArray<TSharedPtr<FJsonValue>>());
	CastStmt->SetArrayField(TEXT("else"), TArray<TSharedPtr<FJsonValue>>());

	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetStringField(TEXT("Out"), TEXT("AsActor"));
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);

	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CastStmt));
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	return TestTrue(TEXT("Cast result name should not leak after then exits."), (*Resolutions)->TryGetStringField(TEXT("flow[1].return.Out"), Resolved) && Resolved == TEXT("literal:string"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRCastElseBranchTest, "BlueprintBridge.Blueprint.SemanticIR.Cast.ElseBranch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRCastElseBranchTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemCastElse"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("CastElse"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("string"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> SelfExpr = MakeShared<FJsonObject>();
	SelfExpr->SetBoolField(TEXT("self"), true);
	TSharedRef<FJsonObject> CastStmt = MakeShared<FJsonObject>();
	CastStmt->SetObjectField(TEXT("cast"), SelfExpr);
	CastStmt->SetStringField(TEXT("to"), TEXT("/Script/Engine.Pawn"));
	CastStmt->SetStringField(TEXT("as"), TEXT("AsPawn"));
	CastStmt->SetArrayField(TEXT("then"), TArray<TSharedPtr<FJsonValue>>());

	TSharedRef<FJsonObject> ElseReturnFields = MakeShared<FJsonObject>();
	ElseReturnFields->SetStringField(TEXT("Out"), TEXT("failed"));
	TSharedRef<FJsonObject> ElseReturn = MakeShared<FJsonObject>();
	ElseReturn->SetObjectField(TEXT("return"), ElseReturnFields);
	TArray<TSharedPtr<FJsonValue>> ElseBlock;
	ElseBlock.Add(MakeShared<FJsonValueObject>(ElseReturn));
	CastStmt->SetArrayField(TEXT("else"), ElseBlock);

	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(CastStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("cast else branch should still emit exactly one DynamicCast node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("DynamicCast")), 1);
	return TestEqual(TEXT("CastFailed pin should link into the else return path."), BlueprintBridgeTests::CountPatchLinksTo(*Result, TEXT(".CastFailed"), TEXT("result.execute")), 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRForEachBasicTest, "BlueprintBridge.Blueprint.SemanticIR.ForEach.Basic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRForEachBasicTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemForEach"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("ForEachBasic"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	TSharedRef<FJsonObject> TargetsInput = BlueprintBridgeTests::MakePinSpec(TEXT("Targets"), TEXT("object"));
	TargetsInput->SetStringField(TEXT("subCategoryObject"), TEXT("/Script/Engine.Actor"));
	TargetsInput->SetStringField(TEXT("containerType"), TEXT("Array"));
	Inputs.Add(MakeShared<FJsonValueObject>(TargetsInput));
	Params->SetArrayField(TEXT("inputs"), Inputs);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("object"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetStringField(TEXT("Out"), TEXT("Target"));
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Body;
	Body.Add(MakeShared<FJsonValueObject>(ReturnStmt));

	TSharedRef<FJsonObject> LoopStmt = MakeShared<FJsonObject>();
	LoopStmt->SetStringField(TEXT("forEach"), TEXT("Targets"));
	LoopStmt->SetStringField(TEXT("as"), TEXT("Target"));
	LoopStmt->SetArrayField(TEXT("body"), Body);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(LoopStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("forEach should emit one ForEachLoop node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("ForEachLoop")), 1);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	return TestTrue(TEXT("Induction variable should resolve from forEach body scope."), (*Resolutions)->TryGetStringField(TEXT("flow[0].body[0].return.Out"), Resolved) && Resolved == TEXT("scope:Target"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRForBasicTest, "BlueprintBridge.Blueprint.SemanticIR.For.Basic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRForBasicTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemFor"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("ForBasic"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
	Range->SetNumberField(TEXT("from"), 0);
	Range->SetNumberField(TEXT("to"), 5);
	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetStringField(TEXT("Out"), TEXT("i"));
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Body;
	Body.Add(MakeShared<FJsonValueObject>(ReturnStmt));

	TSharedRef<FJsonObject> LoopStmt = MakeShared<FJsonObject>();
	LoopStmt->SetObjectField(TEXT("for"), Range);
	LoopStmt->SetStringField(TEXT("as"), TEXT("i"));
	LoopStmt->SetArrayField(TEXT("body"), Body);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(LoopStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("for should emit one ForLoop node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("ForLoop")), 1);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	return TestTrue(TEXT("Index variable should resolve from for body scope."), (*Resolutions)->TryGetStringField(TEXT("flow[0].body[0].return.Out"), Resolved) && Resolved == TEXT("scope:i"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLoopInductionVarScopePopTest, "BlueprintBridge.Blueprint.SemanticIR.Loop.InductionVarScopePop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLoopInductionVarScopePopTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLoopPop"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("LoopPop"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("string"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
	Range->SetNumberField(TEXT("from"), 0);
	Range->SetNumberField(TEXT("to"), 1);
	TSharedRef<FJsonObject> LoopStmt = MakeShared<FJsonObject>();
	LoopStmt->SetObjectField(TEXT("for"), Range);
	LoopStmt->SetStringField(TEXT("as"), TEXT("i"));
	LoopStmt->SetArrayField(TEXT("body"), TArray<TSharedPtr<FJsonValue>>());

	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetStringField(TEXT("Out"), TEXT("i"));
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(LoopStmt));
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	return TestTrue(TEXT("Induction variable should not leak after loop body exits."), (*Resolutions)->TryGetStringField(TEXT("flow[1].return.Out"), Resolved) && Resolved == TEXT("literal:string"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRWhileBasicTest, "BlueprintBridge.Blueprint.SemanticIR.While.Basic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRWhileBasicTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemWhile"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("WhileBasic"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Flag"), TEXT("bool"))));
	Params->SetArrayField(TEXT("inputs"), Inputs);

	TSharedRef<FJsonObject> WhileStmt = MakeShared<FJsonObject>();
	WhileStmt->SetStringField(TEXT("while"), TEXT("Flag"));
	WhileStmt->SetArrayField(TEXT("body"), TArray<TSharedPtr<FJsonValue>>());
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(WhileStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("while should emit one WhileLoop node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("WhileLoop")), 1);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString Resolved;
	return TestTrue(TEXT("while should annotate loop resolution."), (*Resolutions)->TryGetStringField(TEXT("flow[0]"), Resolved) && Resolved == TEXT("loop:while"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLoopBreakTest, "BlueprintBridge.Blueprint.SemanticIR.Loop.Break", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLoopBreakTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLoopBreak"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("LoopBreak"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> BreakStmt = MakeShared<FJsonObject>();
	BreakStmt->SetBoolField(TEXT("break"), true);
	TArray<TSharedPtr<FJsonValue>> Body;
	Body.Add(MakeShared<FJsonValueObject>(BreakStmt));
	TSharedRef<FJsonObject> WhileStmt = MakeShared<FJsonObject>();
	WhileStmt->SetBoolField(TEXT("while"), true);
	WhileStmt->SetArrayField(TEXT("body"), Body);

	TSharedRef<FJsonObject> ReturnFields = MakeShared<FJsonObject>();
	ReturnFields->SetNumberField(TEXT("Out"), 1);
	TSharedRef<FJsonObject> ReturnStmt = MakeShared<FJsonObject>();
	ReturnStmt->SetObjectField(TEXT("return"), ReturnFields);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(WhileStmt));
	Flow.Add(MakeShared<FJsonValueObject>(ReturnStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("break should keep one WhileLoop node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("WhileLoop")), 1);
	return TestEqual(TEXT("break exit should link from loop body to downstream return."), BlueprintBridgeTests::CountPatchLinksTo(*Result, TEXT(".LoopBody"), TEXT("result.execute")), 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLoopContinueTest, "BlueprintBridge.Blueprint.SemanticIR.Loop.Continue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLoopContinueTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLoopContinue"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("LoopContinue"));
	TSharedRef<FJsonObject> ContinueStmt = MakeShared<FJsonObject>();
	ContinueStmt->SetBoolField(TEXT("continue"), true);
	TArray<TSharedPtr<FJsonValue>> Body;
	Body.Add(MakeShared<FJsonValueObject>(ContinueStmt));
	TSharedRef<FJsonObject> WhileStmt = MakeShared<FJsonObject>();
	WhileStmt->SetBoolField(TEXT("while"), true);
	WhileStmt->SetArrayField(TEXT("body"), Body);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(WhileStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("continue should keep one WhileLoop node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("WhileLoop")), 1);
	return TestEqual(TEXT("continue should link loop body back to loop execute."), BlueprintBridgeTests::CountPatchLinksTo(*Result, TEXT(".LoopBody"), TEXT("n_1.execute")), 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLoopBreakOutsideLoopTest, "BlueprintBridge.Blueprint.SemanticIR.Loop.BreakOutsideLoop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLoopBreakOutsideLoopTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemBreakOutside"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("BreakOutside"));
	TSharedRef<FJsonObject> BreakStmt = MakeShared<FJsonObject>();
	BreakStmt->SetBoolField(TEXT("break"), true);
	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(BreakStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	return BlueprintBridgeTests::ExpectErrorCode(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params), TEXT("SemanticLoweringFailed"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRLoopNestedScopesTest, "BlueprintBridge.Blueprint.SemanticIR.Loop.NestedScopes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRLoopNestedScopesTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemLoopNested"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("LoopNested"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
	Binding->SetStringField(TEXT("name"), TEXT("i"));
	Binding->SetNumberField(TEXT("value"), 7);
	TArray<TSharedPtr<FJsonValue>> Bindings;
	Bindings.Add(MakeShared<FJsonValueObject>(Binding));

	TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
	Range->SetNumberField(TEXT("from"), 0);
	Range->SetNumberField(TEXT("to"), 1);
	TSharedRef<FJsonObject> BodyReturnFields = MakeShared<FJsonObject>();
	BodyReturnFields->SetStringField(TEXT("Out"), TEXT("i"));
	TSharedRef<FJsonObject> BodyReturn = MakeShared<FJsonObject>();
	BodyReturn->SetObjectField(TEXT("return"), BodyReturnFields);
	TArray<TSharedPtr<FJsonValue>> Body;
	Body.Add(MakeShared<FJsonValueObject>(BodyReturn));
	TSharedRef<FJsonObject> LoopStmt = MakeShared<FJsonObject>();
	LoopStmt->SetObjectField(TEXT("for"), Range);
	LoopStmt->SetStringField(TEXT("as"), TEXT("i"));
	LoopStmt->SetArrayField(TEXT("body"), Body);

	TSharedRef<FJsonObject> AfterReturnFields = MakeShared<FJsonObject>();
	AfterReturnFields->SetStringField(TEXT("Out"), TEXT("i"));
	TSharedRef<FJsonObject> AfterReturn = MakeShared<FJsonObject>();
	AfterReturn->SetObjectField(TEXT("return"), AfterReturnFields);
	TArray<TSharedPtr<FJsonValue>> InBlock;
	InBlock.Add(MakeShared<FJsonValueObject>(LoopStmt));
	InBlock.Add(MakeShared<FJsonValueObject>(AfterReturn));
	TSharedRef<FJsonObject> LetStmt = MakeShared<FJsonObject>();
	LetStmt->SetArrayField(TEXT("let"), Bindings);
	LetStmt->SetArrayField(TEXT("in"), InBlock);

	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(LetStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("LowerSemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	TestEqual(TEXT("nested-scope loop should emit one ForLoop node."), BlueprintBridgeTests::CountPatchNodes(*Result, TEXT("ForLoop")), 1);
	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) || !Resolutions) { return false; }
	FString InnerResolved;
	FString OuterResolved;
	TestTrue(TEXT("Loop body should resolve i from inner loop scope."), (*Resolutions)->TryGetStringField(TEXT("flow[0].in[0].body[0].return.Out"), InnerResolved) && InnerResolved == TEXT("scope:i"));
	return TestTrue(TEXT("After loop, outer let binding should be restored."), (*Resolutions)->TryGetStringField(TEXT("flow[0].in[1].return.Out"), OuterResolved) && OuterResolved == TEXT("scope:i"));
}

namespace BlueprintBridgeTests
{
// Helper: create a populated function graph for field-selection tests — Branch wired to Sequence.
static FString CreateFieldSelectionTestGraph(FAutomationTestBase& Test, const FTestBlueprintAsset& Asset)
{
	const FString GraphName(TEXT("FieldFn"));
	TSharedRef<FJsonObject> CreateParams = MakeAssetParams(Asset.AssetPath);
	CreateParams->SetStringField(TEXT("function"), GraphName);
	if (!ExpectSuccess(Test, ExecuteJsonRequest(TEXT("CreateFunctionGraph"), CreateParams)))
	{
		return FString();
	}
	TSharedRef<FJsonObject> Patch = MakeGraphParams(Asset.AssetPath, GraphName);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TSharedRef<FJsonObject> BranchNode = MakeShared<FJsonObject>();
	BranchNode->SetStringField(TEXT("id"), TEXT("branch"));
	BranchNode->SetStringField(TEXT("type"), TEXT("Branch"));
	BranchNode->SetNumberField(TEXT("x"), 100);
	BranchNode->SetNumberField(TEXT("y"), 0);
	Nodes.Add(MakeShared<FJsonValueObject>(BranchNode));
	Patch->SetArrayField(TEXT("nodes"), Nodes);
	if (!ExpectSuccess(Test, ExecuteJsonRequest(TEXT("ApplyGraphPatch"), Patch)))
	{
		return FString();
	}
	return GraphName;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFieldSelectionWholeObjectTest, "BlueprintBridge.Blueprint.FieldSelection.WholeObject", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFieldSelectionWholeObjectTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FieldWhole"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString GraphName = BlueprintBridgeTests::CreateFieldSelectionTestGraph(*this, Asset);
	if (GraphName.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, GraphName);
	TArray<TSharedPtr<FJsonValue>> Fields;
	Fields.Add(MakeShared<FJsonValueString>(TEXT("nodes.title")));
	Params->SetArrayField(TEXT("fields"), Fields);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result || !Result->IsValid())
	{
		return false;
	}
	TestFalse(TEXT("'asset' should be filtered out."), (*Result)->HasField(TEXT("asset")));
	TestFalse(TEXT("'graph' should be filtered out."), (*Result)->HasField(TEXT("graph")));
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("'nodes' should be kept."), (*Result)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes != nullptr && Nodes->Num() > 0);
	if (!Nodes || Nodes->Num() == 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FirstNode = nullptr;
	(*Nodes)[0]->TryGetObject(FirstNode);
	TestTrue(TEXT("Node should keep 'title'."), FirstNode && (*FirstNode)->HasField(TEXT("title")));
	TestFalse(TEXT("Node should drop 'guid'."), FirstNode && (*FirstNode)->HasField(TEXT("guid")));
	TestFalse(TEXT("Node should drop 'class'."), FirstNode && (*FirstNode)->HasField(TEXT("class")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFieldSelectionArrayElementTest, "BlueprintBridge.Blueprint.FieldSelection.ArrayElement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFieldSelectionArrayElementTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FieldPin"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString GraphName = BlueprintBridgeTests::CreateFieldSelectionTestGraph(*this, Asset);
	if (GraphName.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, GraphName);
	TArray<TSharedPtr<FJsonValue>> Fields;
	Fields.Add(MakeShared<FJsonValueString>(TEXT("nodes.pins.name")));
	Params->SetArrayField(TEXT("fields"), Fields);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Result || !(*Result)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes || Nodes->Num() == 0)
	{
		return false;
	}
	bool bAnyPinSeen = false;
	for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
	{
		const TSharedPtr<FJsonObject>* NodeObj = nullptr;
		if (!NodeValue->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!(*NodeObj)->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
		{
			const TSharedPtr<FJsonObject>* PinObj = nullptr;
			if (PinValue->TryGetObject(PinObj) && PinObj && PinObj->IsValid())
			{
				TestTrue(TEXT("Pin should keep 'name'."), (*PinObj)->HasField(TEXT("name")));
				TestFalse(TEXT("Pin should drop 'direction'."), (*PinObj)->HasField(TEXT("direction")));
				TestFalse(TEXT("Pin should drop 'category'."), (*PinObj)->HasField(TEXT("category")));
				bAnyPinSeen = true;
			}
		}
	}
	return TestTrue(TEXT("At least one pin should be observed for the assertion to be meaningful."), bAnyPinSeen);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFieldSelectionMultiplePathsTest, "BlueprintBridge.Blueprint.FieldSelection.MultiplePaths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFieldSelectionMultiplePathsTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FieldMulti"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString GraphName = BlueprintBridgeTests::CreateFieldSelectionTestGraph(*this, Asset);
	if (GraphName.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, GraphName);
	TArray<TSharedPtr<FJsonValue>> Fields;
	Fields.Add(MakeShared<FJsonValueString>(TEXT("nodes.guid")));
	Fields.Add(MakeShared<FJsonValueString>(TEXT("nodes.title")));
	Params->SetArrayField(TEXT("fields"), Fields);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (!Result || !(*Result)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes || Nodes->Num() == 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FirstNode = nullptr;
	(*Nodes)[0]->TryGetObject(FirstNode);
	TestTrue(TEXT("Node should keep 'guid'."), FirstNode && (*FirstNode)->HasField(TEXT("guid")));
	TestTrue(TEXT("Node should keep 'title'."), FirstNode && (*FirstNode)->HasField(TEXT("title")));
	TestFalse(TEXT("Node should drop 'class'."), FirstNode && (*FirstNode)->HasField(TEXT("class")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFieldSelectionUnknownPathTest, "BlueprintBridge.Blueprint.FieldSelection.UnknownPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFieldSelectionUnknownPathTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FieldUnknown"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString GraphName = BlueprintBridgeTests::CreateFieldSelectionTestGraph(*this, Asset);
	if (GraphName.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, GraphName);
	TArray<TSharedPtr<FJsonValue>> Fields;
	Fields.Add(MakeShared<FJsonValueString>(TEXT("nodes.banana")));
	Params->SetArrayField(TEXT("fields"), Fields);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("'nodes' should still be kept (prefix match)."), Result && (*Result)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes != nullptr);
	if (!Nodes || Nodes->Num() == 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FirstNode = nullptr;
	(*Nodes)[0]->TryGetObject(FirstNode);
	TestTrue(TEXT("Unknown sub-field yields an empty node object, not an error."), FirstNode && (*FirstNode)->Values.Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFieldSelectionPassthroughTest, "BlueprintBridge.Blueprint.FieldSelection.Passthrough", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFieldSelectionPassthroughTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("FieldPass"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString GraphName = BlueprintBridgeTests::CreateFieldSelectionTestGraph(*this, Asset);
	if (GraphName.IsEmpty())
	{
		return false;
	}

	// No `fields` param → full response shape, asserted via DescribeGraphFull's documented top-level keys.
	// (Routes to DescribeGraphFull after the PR3 default flip — DescribeGraph now returns the summary shape.)
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, GraphName));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}
	TestTrue(TEXT("Passthrough should keep 'asset'."), (*Result)->HasField(TEXT("asset")));
	TestTrue(TEXT("Passthrough should keep 'graph'."), (*Result)->HasField(TEXT("graph")));
	TestTrue(TEXT("Passthrough should keep 'nodes'."), (*Result)->HasField(TEXT("nodes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeDescribeGraphReturnsSummaryTest, "BlueprintBridge.Blueprint.DescribeGraph.ReturnsSummary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeDescribeGraphReturnsSummaryTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("DescGraphSum"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString GraphName = BlueprintBridgeTests::CreateFieldSelectionTestGraph(*this, Asset);
	if (GraphName.IsEmpty())
	{
		return false;
	}

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraph"), BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, GraphName));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}
	// Summary-shape: top-level keys mirror SummarizeBlueprintGraph, NOT a per-node 'nodes' array.
	TestTrue(TEXT("DescribeGraph should expose summary 'entryNodes' key."), (*Result)->HasField(TEXT("entryNodes")));
	TestTrue(TEXT("DescribeGraph should expose summary 'executionChains' key."), (*Result)->HasField(TEXT("executionChains")));
	TestTrue(TEXT("DescribeGraph should expose summary 'functionCalls' key."), (*Result)->HasField(TEXT("functionCalls")));
	TestFalse(TEXT("DescribeGraph should NOT expose the verbose 'nodes' array (that is DescribeGraphFull)."), (*Result)->HasField(TEXT("nodes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeDescribeGraphFullReturnsVerboseTest, "BlueprintBridge.Blueprint.DescribeGraph.FullReturnsVerbose", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeDescribeGraphFullReturnsVerboseTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("DescGraphFull"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString GraphName = BlueprintBridgeTests::CreateFieldSelectionTestGraph(*this, Asset);
	if (GraphName.IsEmpty())
	{
		return false;
	}

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeGraphFull"), BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, GraphName));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	TestTrue(TEXT("DescribeGraphFull should expose the per-node 'nodes' array."), (*Result)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes != nullptr && Nodes->Num() > 0);
	if (!Nodes || Nodes->Num() == 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FirstNode = nullptr;
	(*Nodes)[0]->TryGetObject(FirstNode);
	TestTrue(TEXT("Verbose nodes should expose per-pin detail."), FirstNode && (*FirstNode)->HasField(TEXT("pins")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSummarizeBlueprintMinimalTest, "BlueprintBridge.Blueprint.SummarizeBlueprint.Minimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSummarizeBlueprintMinimalTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SumMin"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SummarizeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}
	TestTrue(TEXT("Should include parentClass."), (*Result)->HasField(TEXT("parentClass")));
	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	TestTrue(TEXT("Should include variables array (empty)."), (*Result)->TryGetArrayField(TEXT("variables"), Variables) && Variables != nullptr && Variables->Num() == 0);
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	TestTrue(TEXT("Should include graphs object."), (*Result)->TryGetObjectField(TEXT("graphs"), Graphs) && Graphs && Graphs->IsValid());
	const TArray<TSharedPtr<FJsonValue>>* EventGraphs = nullptr;
	TestTrue(TEXT("Graphs should include an 'event' array with at least one entry."), Graphs && (*Graphs)->TryGetArrayField(TEXT("event"), EventGraphs) && EventGraphs != nullptr && EventGraphs->Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSummarizeBlueprintPopulatedTest, "BlueprintBridge.Blueprint.SummarizeBlueprint.Populated", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSummarizeBlueprintPopulatedTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SumPop"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> AddVarParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVarParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVarParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVarParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddCompParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddCompParams->SetStringField(TEXT("name"), TEXT("Hitbox"));
	AddCompParams->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.BoxComponent"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddComponent"), AddCompParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> CreateFnParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	CreateFnParams->SetStringField(TEXT("function"), TEXT("GetHealth"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateFunctionGraph"), CreateFnParams)))
	{
		return false;
	}
	TSharedRef<FJsonObject> AddOutParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddOutParams->SetStringField(TEXT("graph"), TEXT("GetHealth"));
	AddOutParams->SetStringField(TEXT("name"), TEXT("OutValue"));
	AddOutParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddFunctionOutput"), AddOutParams)))
	{
		return false;
	}

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SummarizeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}
	// Variable present
	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	bool bFoundHealth = false;
	if ((*Result)->TryGetArrayField(TEXT("variables"), Variables) && Variables)
	{
		for (const TSharedPtr<FJsonValue>& V : *Variables)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			FString Name;
			if (V->TryGetObject(Obj) && Obj && (*Obj)->TryGetStringField(TEXT("name"), Name) && Name == TEXT("Health"))
			{
				bFoundHealth = true;
			}
		}
	}
	TestTrue(TEXT("'Health' variable should appear."), bFoundHealth);

	// Component present
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	bool bFoundHitbox = false;
	if ((*Result)->TryGetArrayField(TEXT("components"), Components) && Components)
	{
		for (const TSharedPtr<FJsonValue>& C : *Components)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			FString Name;
			if (C->TryGetObject(Obj) && Obj && (*Obj)->TryGetStringField(TEXT("name"), Name) && Name == TEXT("Hitbox"))
			{
				bFoundHitbox = true;
			}
		}
	}
	TestTrue(TEXT("'Hitbox' component should appear."), bFoundHitbox);

	// Function with signature.outputs containing OutValue
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	bool bFoundFn = false;
	bool bFoundOutValue = false;
	if ((*Result)->TryGetObjectField(TEXT("graphs"), Graphs) && Graphs && (*Graphs)->TryGetArrayField(TEXT("functions"), Functions) && Functions)
	{
		for (const TSharedPtr<FJsonValue>& F : *Functions)
		{
			const TSharedPtr<FJsonObject>* FnObj = nullptr;
			FString FnName;
			if (F->TryGetObject(FnObj) && FnObj && (*FnObj)->TryGetStringField(TEXT("name"), FnName) && FnName == TEXT("GetHealth"))
			{
				bFoundFn = true;
				const TSharedPtr<FJsonObject>* Sig = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
				if ((*FnObj)->TryGetObjectField(TEXT("signature"), Sig) && Sig && (*Sig)->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs)
				{
					for (const TSharedPtr<FJsonValue>& O : *Outputs)
					{
						const TSharedPtr<FJsonObject>* OObj = nullptr;
						FString OName;
						if (O->TryGetObject(OObj) && OObj && (*OObj)->TryGetStringField(TEXT("name"), OName) && OName == TEXT("OutValue"))
						{
							bFoundOutValue = true;
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("'GetHealth' function should appear in graphs.functions."), bFoundFn);
	TestTrue(TEXT("Function signature.outputs should include 'OutValue'."), bFoundOutValue);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSummarizeBlueprintFlagGatingTest, "BlueprintBridge.Blueprint.SummarizeBlueprint.FlagGating", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSummarizeBlueprintFlagGatingTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SumGate"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	TSharedRef<FJsonObject> CreateFnParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	CreateFnParams->SetStringField(TEXT("function"), TEXT("Gated"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateFunctionGraph"), CreateFnParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetBoolField(TEXT("includeFunctionBodies"), false);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SummarizeBlueprint"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	if (!Result || !(*Result)->TryGetObjectField(TEXT("graphs"), Graphs) || !Graphs || !(*Graphs)->TryGetArrayField(TEXT("functions"), Functions) || !Functions || Functions->Num() == 0)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* FnObj = nullptr;
	(*Functions)[0]->TryGetObject(FnObj);
	TestTrue(TEXT("Function entry should keep 'signature' when bodies are off."), FnObj && (*FnObj)->HasField(TEXT("signature")));
	TestFalse(TEXT("Function entry should drop 'summary' when bodies are off."), FnObj && (*FnObj)->HasField(TEXT("summary")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSummarizeBlueprintWidgetTest, "BlueprintBridge.Blueprint.SummarizeBlueprint.Widget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSummarizeBlueprintWidgetTest::RunTest(const FString& Parameters)
{
	const FString AssetName = FString::Printf(TEXT("WBP_BridgeTest_SumWidget_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetPath = FString::Printf(TEXT("/Game/BlueprintBridgeTests/%s"), *AssetName);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("CreateWidgetBlueprintAsset"), BlueprintBridgeTests::MakeAssetParams(AssetPath))))
	{
		return false;
	}

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("SummarizeBlueprint"), BlueprintBridgeTests::MakeAssetParams(AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result)
	{
		return false;
	}
	FString Kind;
	TestTrue(TEXT("Kind should be WidgetBlueprint."), (*Result)->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("WidgetBlueprint"));
	TestTrue(TEXT("widgetTree should be present for UMG."), (*Result)->HasField(TEXT("widgetTree")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeSemanticIRApplyAndCompileTest, "BlueprintBridge.Blueprint.SemanticIR.ApplyAndCompile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeSemanticIRApplyAndCompileTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("SemApply"));
	if (!Asset.Blueprint)
	{
		return false;
	}

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("function"), TEXT("CompareFn"));
	Params->SetBoolField(TEXT("createIfMissing"), true);
	Params->SetBoolField(TEXT("compile"), true);

	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Input"), TEXT("int"))));
	Params->SetArrayField(TEXT("inputs"), Inputs);
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakePinSpec(TEXT("Out"), TEXT("int"))));
	Params->SetArrayField(TEXT("outputs"), Outputs);

	// flow: if EqualEqual_IntInt(Input, 0) then return Out=1 else return Out=0
	TSharedRef<FJsonObject> CondCall = MakeShared<FJsonObject>();
	CondCall->SetStringField(TEXT("call"), TEXT("/Script/Engine.KismetMathLibrary.EqualEqual_IntInt"));
	TSharedRef<FJsonObject> CondArgs = MakeShared<FJsonObject>();
	CondArgs->SetStringField(TEXT("A"), TEXT("Input"));
	CondArgs->SetNumberField(TEXT("B"), 0);
	CondCall->SetObjectField(TEXT("args"), CondArgs);

	TSharedRef<FJsonObject> ThenReturn = MakeShared<FJsonObject>();
	{
		TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		Fields->SetNumberField(TEXT("Out"), 1);
		ThenReturn->SetObjectField(TEXT("return"), Fields);
	}
	TSharedRef<FJsonObject> ElseReturn = MakeShared<FJsonObject>();
	{
		TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		Fields->SetNumberField(TEXT("Out"), 0);
		ElseReturn->SetObjectField(TEXT("return"), Fields);
	}

	TSharedRef<FJsonObject> IfStmt = MakeShared<FJsonObject>();
	IfStmt->SetObjectField(TEXT("if"), CondCall);
	TArray<TSharedPtr<FJsonValue>> ThenBlock;
	ThenBlock.Add(MakeShared<FJsonValueObject>(ThenReturn));
	IfStmt->SetArrayField(TEXT("then"), ThenBlock);
	TArray<TSharedPtr<FJsonValue>> ElseBlock;
	ElseBlock.Add(MakeShared<FJsonValueObject>(ElseReturn));
	IfStmt->SetArrayField(TEXT("else"), ElseBlock);

	TArray<TSharedPtr<FJsonValue>> Flow;
	Flow.Add(MakeShared<FJsonValueObject>(IfStmt));
	Params->SetArrayField(TEXT("flow"), Flow);

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ApplySemanticFunction"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response))
	{
		AddError(FString::Printf(TEXT("ApplySemanticFunction failed: %s"), *BlueprintBridgeTests::GetErrorMessage(Response)));
		return false;
	}

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	FString CompileStatus;
	TestTrue(TEXT("Apply response should include compile status."), Result && (*Result)->TryGetStringField(TEXT("compileStatus"), CompileStatus));
	TestEqual(TEXT("Compiled function should be UpToDate."), CompileStatus, FString(TEXT("UpToDate")));

	const TSharedPtr<FJsonObject>* Resolutions = nullptr;
	TestTrue(TEXT("Apply response should include resolutions map."), Result && (*Result)->TryGetObjectField(TEXT("resolutions"), Resolutions) && Resolutions && Resolutions->IsValid());
	if (Resolutions && Resolutions->IsValid())
	{
		FString InputRes;
		TestTrue(TEXT("'Input' in args should resolve to param."), (*Resolutions)->TryGetStringField(TEXT("flow[0].if.args.A"), InputRes) && InputRes == TEXT("param:Input"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeConnectPinsSourceTargetAliasTest, "BlueprintBridge.Blueprint.ConnectPins.SourceTargetAlias", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeConnectPinsSourceTargetAliasTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("ConnectAlias"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> AddVariableParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVariableParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVariableParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVariableParams)))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddGetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddGetParams->SetStringField(TEXT("variable"), TEXT("Health"));
	AddGetParams->SetNumberField(TEXT("x"), 100);
	AddGetParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> AddGetResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableGetNode"), AddGetParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddGetResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* GetNodeObject = BlueprintBridgeTests::GetNodeObjectFromResponse(*this, AddGetResponse);
	if (!GetNodeObject)
	{
		return false;
	}
	// Successful AddVariableGetNode for a real class member should return populated pins
	// so callers can wire the new node in the same batch without a follow-up DescribeNode.
	const TArray<TSharedPtr<FJsonValue>>* GetPins = nullptr;
	TestTrue(TEXT("AddVariableGetNode response should include pins for a resolved variable."), (*GetNodeObject)->TryGetArrayField(TEXT("pins"), GetPins) && GetPins && GetPins->Num() > 0);
	const FString GetNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *GetNodeObject);

	TSharedRef<FJsonObject> AddSetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddSetParams->SetStringField(TEXT("variable"), TEXT("Health"));
	AddSetParams->SetNumberField(TEXT("x"), 400);
	AddSetParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> AddSetResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableSetNode"), AddSetParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, AddSetResponse))
	{
		return false;
	}
	const FString SetNodeGuid = BlueprintBridgeTests::GetNodeGuid(*this, *BlueprintBridgeTests::GetNodeObjectFromResponse(*this, AddSetResponse));

	// Use the source/target alias keys exclusively. If the dispatcher ignored them, this would fail with InvalidParams.
	TSharedRef<FJsonObject> ConnectParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	ConnectParams->SetStringField(TEXT("sourceNode"), GetNodeGuid);
	ConnectParams->SetStringField(TEXT("sourcePin"), TEXT("Health"));
	ConnectParams->SetStringField(TEXT("targetNode"), SetNodeGuid);
	ConnectParams->SetStringField(TEXT("targetPin"), TEXT("Health"));
	return BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("ConnectPins"), ConnectParams));
}

namespace BlueprintBridgeTests
{
static const TSharedPtr<FJsonObject>* GetBatchChildResult(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& BatchResponse, int32 ChildIndex, FString& OutChildErrorCode)
{
	OutChildErrorCode.Reset();
	const TSharedPtr<FJsonObject>* BatchResult = GetResultObject(Test, BatchResponse);
	if (!BatchResult) { return nullptr; }
	const TArray<TSharedPtr<FJsonValue>>* Responses = nullptr;
	if (!(*BatchResult)->TryGetArrayField(TEXT("responses"), Responses) || !Responses || ChildIndex >= Responses->Num())
	{
		Test.AddError(FString::Printf(TEXT("Batch response did not contain index %d."), ChildIndex));
		return nullptr;
	}
	const TSharedPtr<FJsonObject>* ChildObj = nullptr;
	if (!(*Responses)[ChildIndex]->TryGetObject(ChildObj) || !ChildObj || !(*ChildObj).IsValid())
	{
		Test.AddError(FString::Printf(TEXT("Batch response[%d] was not an object."), ChildIndex));
		return nullptr;
	}
	bool bOk = true;
	(*ChildObj)->TryGetBoolField(TEXT("ok"), bOk);
	if (!bOk)
	{
		const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
		if ((*ChildObj)->TryGetObjectField(TEXT("error"), ErrorObj) && ErrorObj && (*ErrorObj).IsValid())
		{
			(*ErrorObj)->TryGetStringField(TEXT("code"), OutChildErrorCode);
		}
		return nullptr;
	}
	const TSharedPtr<FJsonObject>* ResultObj = nullptr;
	(*ChildObj)->TryGetObjectField(TEXT("result"), ResultObj);
	return ResultObj;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeBatchRefBasicTest, "BlueprintBridge.Protocol.Batch.RefBasic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeBatchRefBasicTest::RunTest(const FString& Parameters)
{
	// AddBlueprintVariable is inside the batch as request 0 so this also verifies the
	// structural recompile it triggers is synchronous enough for the just-shipped
	// BlueprintHasMemberVariable pre-check to see the new variable in requests 1 and 2.
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("BatchRefBasic"));
	if (!Asset.Blueprint) { return false; }
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> AddVarParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVarParams->SetStringField(TEXT("name"), TEXT("Health"));
	AddVarParams->SetStringField(TEXT("category"), TEXT("int"));

	auto MakeNodeParams = [&](double X, double Y)
	{
		TSharedRef<FJsonObject> P = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
		P->SetStringField(TEXT("variable"), TEXT("Health"));
		P->SetNumberField(TEXT("x"), X);
		P->SetNumberField(TEXT("y"), Y);
		return P;
	};

	TSharedRef<FJsonObject> ConnectParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	ConnectParams->SetStringField(TEXT("fromNode"), TEXT("$ref:1.result.node.guid"));
	ConnectParams->SetStringField(TEXT("fromPin"), TEXT("Health"));
	ConnectParams->SetStringField(TEXT("toNode"), TEXT("$ref:2.result.node.guid"));
	ConnectParams->SetStringField(TEXT("toPin"), TEXT("Health"));

	TArray<TSharedPtr<FJsonValue>> Requests;
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("AddBlueprintVariable"), AddVarParams)));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("AddVariableGetNode"), MakeNodeParams(100, 100))));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("AddVariableSetNode"), MakeNodeParams(400, 100))));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("ConnectPins"), ConnectParams)));

	TSharedRef<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("requests"), Requests);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Batch"), BatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	// AddBlueprintVariable returns a string result ("VariableAdded"), so use the error-code
	// channel rather than asserting on an object result. The downstream children carry the
	// usual {node: ...} object result.
	FString ChildErr;
	BlueprintBridgeTests::GetBatchChildResult(*this, Response, 0, ChildErr);
	TestTrue(TEXT("AddBlueprintVariable child should succeed (no error code)."), ChildErr.IsEmpty());
	TestNotNull(TEXT("AddVariableGetNode child should have result."), BlueprintBridgeTests::GetBatchChildResult(*this, Response, 1, ChildErr));
	TestNotNull(TEXT("AddVariableSetNode child should have result."), BlueprintBridgeTests::GetBatchChildResult(*this, Response, 2, ChildErr));

	// ConnectPins returns FinishGraphEdit which sets an empty object result, not the {node} shape.
	// Verify it via ChildErr instead.
	BlueprintBridgeTests::GetBatchChildResult(*this, Response, 3, ChildErr);
	TestTrue(TEXT("ConnectPins child should succeed (refs resolved)."), ChildErr.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeBatchRefArrayIndexTest, "BlueprintBridge.Protocol.Batch.RefArrayIndex", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeBatchRefArrayIndexTest::RunTest(const FString& Parameters)
{
	// ListCommands returns { commands: [{name, ...}, ...] }. We pull the first command's
	// name through an array-indexed $ref and pass it to DescribeCommand. Verifies bracket
	// indexing in the path resolver end-to-end without depending on graph state.
	TSharedRef<FJsonObject> DescribeParams = MakeShared<FJsonObject>();
	DescribeParams->SetStringField(TEXT("command"), TEXT("$ref:0.result.commands[0].name"));

	TArray<TSharedPtr<FJsonValue>> Requests;
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("ListCommands"), MakeShared<FJsonObject>())));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("DescribeCommand"), DescribeParams)));

	TSharedRef<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("requests"), Requests);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Batch"), BatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	FString ChildErr;
	const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetBatchChildResult(*this, Response, 1, ChildErr);
	TestNotNull(TEXT("DescribeCommand child should succeed (array-indexed $ref resolved)."), DescribeResult);
	if (DescribeResult)
	{
		FString ResolvedName;
		TestTrue(TEXT("DescribeCommand result should have a name field."), (*DescribeResult)->TryGetStringField(TEXT("name"), ResolvedName) && !ResolvedName.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeBatchRefForwardReferenceTest, "BlueprintBridge.Protocol.Batch.RefForwardReference", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeBatchRefForwardReferenceTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Bad = MakeShared<FJsonObject>();
	Bad->SetStringField(TEXT("command"), TEXT("$ref:1.result.commands[0].name"));

	TArray<TSharedPtr<FJsonValue>> Requests;
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("DescribeCommand"), Bad)));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("ListCommands"), MakeShared<FJsonObject>())));

	TSharedRef<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("requests"), Requests);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Batch"), BatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	FString ChildErr;
	BlueprintBridgeTests::GetBatchChildResult(*this, Response, 0, ChildErr);
	return TestEqual(TEXT("Forward $ref should produce RefForwardReference."), ChildErr, FString(TEXT("RefForwardReference")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeBatchRefOutOfBoundsTest, "BlueprintBridge.Protocol.Batch.RefOutOfBounds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeBatchRefOutOfBoundsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Bad = MakeShared<FJsonObject>();
	Bad->SetStringField(TEXT("command"), TEXT("$ref:99.result.commands[0].name"));

	TArray<TSharedPtr<FJsonValue>> Requests;
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("ListCommands"), MakeShared<FJsonObject>())));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("DescribeCommand"), Bad)));

	TSharedRef<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("requests"), Requests);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Batch"), BatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	FString ChildErr;
	BlueprintBridgeTests::GetBatchChildResult(*this, Response, 1, ChildErr);
	return TestEqual(TEXT("Past-end $ref should produce RefOutOfBounds."), ChildErr, FString(TEXT("RefOutOfBounds")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeBatchRefUnresolvedPathTest, "BlueprintBridge.Protocol.Batch.RefUnresolvedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeBatchRefUnresolvedPathTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Bad = MakeShared<FJsonObject>();
	Bad->SetStringField(TEXT("command"), TEXT("$ref:0.result.definitelyNotAField"));

	TArray<TSharedPtr<FJsonValue>> Requests;
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("ListCommands"), MakeShared<FJsonObject>())));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("DescribeCommand"), Bad)));

	TSharedRef<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("requests"), Requests);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Batch"), BatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	FString ChildErr;
	BlueprintBridgeTests::GetBatchChildResult(*this, Response, 1, ChildErr);
	return TestEqual(TEXT("Missing path segment should produce RefUnresolved."), ChildErr, FString(TEXT("RefUnresolved")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeBatchRefPredecessorFailedTest, "BlueprintBridge.Protocol.Batch.RefPredecessorFailed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeBatchRefPredecessorFailedTest::RunTest(const FString& Parameters)
{
	// Request 0 fails with UnknownCommand; request 1's $ref:0... should report
	// RefPredecessorFailed rather than a path-walking error.
	TSharedRef<FJsonObject> Bad = MakeShared<FJsonObject>();
	Bad->SetStringField(TEXT("command"), TEXT("$ref:0.result.node.guid"));

	TArray<TSharedPtr<FJsonValue>> Requests;
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("DefinitelyUnknownCommand"), MakeShared<FJsonObject>())));
	Requests.Add(MakeShared<FJsonValueObject>(BlueprintBridgeTests::MakeRequest(TEXT("DescribeCommand"), Bad)));

	TSharedRef<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("requests"), Requests);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("Batch"), BatchParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	FString ChildErr;
	BlueprintBridgeTests::GetBatchChildResult(*this, Response, 1, ChildErr);
	return TestEqual(TEXT("Ref to failed predecessor should produce RefPredecessorFailed."), ChildErr, FString(TEXT("RefPredecessorFailed")));
}

namespace BlueprintBridgeTests
{
static int64 GetAssetVersionFromResponse(FAutomationTestBase& Test, const TSharedRef<FJsonObject>& Response)
{
	const TSharedPtr<FJsonObject>* Result = GetResultObject(Test, Response);
	if (!Result) { return -1; }
	double Version = -1.0;
	if (!(*Result)->TryGetNumberField(TEXT("assetVersion"), Version))
	{
		Test.AddError(TEXT("Expected response to include result.assetVersion."));
		return -1;
	}
	return static_cast<int64>(Version);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetVersionResponseIncludesVersionTest, "BlueprintBridge.Protocol.AssetVersion.ResponseIncludesVersion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetVersionResponseIncludesVersionTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("AssetVerResp"));
	if (!Asset.Blueprint) { return false; }

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const int64 Version = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, Response);
	return TestTrue(TEXT("Inspection response should include assetVersion >= 1."), Version >= 1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetVersionUnchangedShortCircuitsTest, "BlueprintBridge.Protocol.AssetVersion.UnchangedShortCircuits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetVersionUnchangedShortCircuitsTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("AssetVerUnchanged"));
	if (!Asset.Blueprint) { return false; }

	const TSharedRef<FJsonObject> First = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, First)) { return false; }
	const int64 Version = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, First);
	if (Version < 1) { return false; }

	TSharedRef<FJsonObject> SecondParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	SecondParams->SetNumberField(TEXT("ifAssetVersionDiffersFrom"), static_cast<double>(Version));
	const TSharedRef<FJsonObject> Second = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), SecondParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Second)) { return false; }

	const TSharedPtr<FJsonObject>* SecondResult = BlueprintBridgeTests::GetResultObject(*this, Second);
	if (!SecondResult) { return false; }
	bool bUnchanged = false;
	TestTrue(TEXT("Matching version guard should produce unchanged:true."), (*SecondResult)->TryGetBoolField(TEXT("unchanged"), bUnchanged) && bUnchanged);
	// The short-circuit response should be minimal — no full payload fields like 'variables'.
	TestFalse(TEXT("Short-circuited response should NOT carry the full payload."), (*SecondResult)->HasField(TEXT("variables")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetVersionMutationBumpsVersionTest, "BlueprintBridge.Protocol.AssetVersion.MutationBumpsVersion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetVersionMutationBumpsVersionTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("AssetVerBump"));
	if (!Asset.Blueprint) { return false; }

	const TSharedRef<FJsonObject> Before = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Before)) { return false; }
	const int64 V0 = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, Before);

	TSharedRef<FJsonObject> AddVarParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVarParams->SetStringField(TEXT("name"), TEXT("BumpProbe"));
	AddVarParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVarParams))) { return false; }

	const TSharedRef<FJsonObject> After = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, After)) { return false; }
	const int64 V1 = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, After);
	if (!TestTrue(FString::Printf(TEXT("Mutation should bump assetVersion (V0=%lld, V1=%lld)."), V0, V1), V1 > V0)) { return false; }

	// Verify the version stamp also appears on object-result mutations (FinishGraphEdit shape).
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);
	TSharedRef<FJsonObject> AddGetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddGetParams->SetStringField(TEXT("variable"), TEXT("BumpProbe"));
	AddGetParams->SetNumberField(TEXT("x"), 100);
	AddGetParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> GetResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableGetNode"), AddGetParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, GetResponse)) { return false; }
	const int64 V2 = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, GetResponse);
	return TestTrue(FString::Printf(TEXT("Object-result mutation should also stamp and bump (V1=%lld, V2=%lld)."), V1, V2), V2 > V1);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetVersionPathNormalizationTest, "BlueprintBridge.Protocol.AssetVersion.PathNormalization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetVersionPathNormalizationTest::RunTest(const FString& Parameters)
{
	// The same asset can be referenced as /Game/X or /Game/X.X (or /Game/X.X_C for class
	// lookups, though that form requires the asset to be saved). All forms must share a
	// single version counter so a caller that varies the form across calls doesn't silently
	// miss real changes. This test covers the two LoadBlueprint-compatible forms; the _C
	// form is normalized to the same key by NormalizeAssetPath but isn't load-testable
	// against unsaved in-memory test BPs.
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("AssetVerPathNorm"));
	if (!Asset.Blueprint) { return false; }

	const FString BasePath = Asset.AssetPath;
	const FString ShortName = FPackageName::GetShortName(BasePath);
	const FString DotForm = FString::Printf(TEXT("%s.%s"), *BasePath, *ShortName);

	// 1) Read via the bare package form to seed the counter.
	const TSharedRef<FJsonObject> First = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(BasePath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, First)) { return false; }
	const int64 V0 = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, First);

	// 2) Mutate via the .Asset.Asset form — must bump the SAME counter, not a sibling.
	TSharedRef<FJsonObject> AddVarParams = BlueprintBridgeTests::MakeAssetParams(DotForm);
	AddVarParams->SetStringField(TEXT("name"), TEXT("NormProbe"));
	AddVarParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVarParams))) { return false; }

	// 3) Read via the bare package form again with the OLD version as guard. If the bump
	// in (2) keyed on a different normalization, this would short-circuit with unchanged:true
	// and silently miss the mutation. We assert the FULL payload comes back.
	TSharedRef<FJsonObject> StaleParams = BlueprintBridgeTests::MakeAssetParams(BasePath);
	StaleParams->SetNumberField(TEXT("ifAssetVersionDiffersFrom"), static_cast<double>(V0));
	const TSharedRef<FJsonObject> Third = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), StaleParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Third)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Third);
	if (!Result) { return false; }
	bool bUnchanged = false;
	(*Result)->TryGetBoolField(TEXT("unchanged"), bUnchanged);
	TestFalse(TEXT("Cross-form mutation must invalidate cached version (path normalization)."), bUnchanged);
	const int64 V1 = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, Third);
	return TestTrue(FString::Printf(TEXT("Cross-form bump should advance the version (V0=%lld, V1=%lld)."), V0, V1), V1 > V0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetVersionDivergedReturnsFullTest, "BlueprintBridge.Protocol.AssetVersion.DivergedReturnsFull", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetVersionDivergedReturnsFullTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("AssetVerDiverged"));
	if (!Asset.Blueprint) { return false; }

	const TSharedRef<FJsonObject> Before = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Before)) { return false; }
	const int64 V0 = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, Before);

	TSharedRef<FJsonObject> AddVarParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddVarParams->SetStringField(TEXT("name"), TEXT("DivergeProbe"));
	AddVarParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddVarParams))) { return false; }

	// Pass the stale V0 — bridge should NOT short-circuit; it should return the full payload with the new version.
	TSharedRef<FJsonObject> StaleParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	StaleParams->SetNumberField(TEXT("ifAssetVersionDiffersFrom"), static_cast<double>(V0));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), StaleParams);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result) { return false; }
	// Default false so a missing 'unchanged' field (the full-payload path) doesn't accidentally
	// satisfy a "should NOT short-circuit" assertion.
	bool bUnchanged = false;
	(*Result)->TryGetBoolField(TEXT("unchanged"), bUnchanged);
	TestFalse(TEXT("Diverged version should NOT short-circuit."), bUnchanged);
	TestTrue(TEXT("Diverged response should include the full payload."), (*Result)->HasField(TEXT("variables")));
	const int64 V1 = BlueprintBridgeTests::GetAssetVersionFromResponse(*this, Response);
	return TestTrue(FString::Printf(TEXT("Diverged response should report newer version (V0=%lld, V1=%lld)."), V0, V1), V1 > V0);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAssetVersionZeroNeverMatchesTest, "BlueprintBridge.Protocol.AssetVersion.ZeroNeverMatches", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAssetVersionZeroNeverMatchesTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("AssetVerZero"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetNumberField(TEXT("ifAssetVersionDiffersFrom"), 0);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }

	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result) { return false; }
	bool bUnchanged = false;
	(*Result)->TryGetBoolField(TEXT("unchanged"), bUnchanged);
	TestFalse(TEXT("ifAssetVersionDiffersFrom:0 should never short-circuit (versions start at 1)."), bUnchanged);
	return TestTrue(TEXT("Stale-zero response should include the full payload."), (*Result)->HasField(TEXT("variables")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFindAssetDependenciesShapeTest, "BlueprintBridge.Reference.FindAssetDependencies.Shape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFindAssetDependenciesShapeTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Deps"));
	if (!Asset.Blueprint) { return false; }

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindAssetDependencies"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result) { return false; }
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	double Count = -1.0;
	TestTrue(TEXT("FindAssetDependencies should return assets array."), (*Result)->TryGetArrayField(TEXT("assets"), Assets) && Assets);
	TestTrue(TEXT("FindAssetDependencies should return count."), (*Result)->TryGetNumberField(TEXT("count"), Count) && Count >= 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFindAssetReferencesShapeTest, "BlueprintBridge.Reference.FindAssetReferences.Shape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFindAssetReferencesShapeTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Refs"));
	if (!Asset.Blueprint) { return false; }

	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindAssetReferences"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result) { return false; }
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	double Count = -1.0;
	TestTrue(TEXT("FindAssetReferences should return assets array."), (*Result)->TryGetArrayField(TEXT("assets"), Assets) && Assets);
	TestTrue(TEXT("FindAssetReferences should return count."), (*Result)->TryGetNumberField(TEXT("count"), Count) && Count >= 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeFindInterfaceImplementationsShapeTest, "BlueprintBridge.Reference.FindInterfaceImplementations.Shape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeFindInterfaceImplementationsShapeTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("interfaceClass"), TEXT("/Script/Engine.Interface_AssetUserData"));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindInterfaceImplementations"), Params);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, Response)) { return false; }
	const TSharedPtr<FJsonObject>* Result = BlueprintBridgeTests::GetResultObject(*this, Response);
	if (!Result) { return false; }
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	double Count = -1.0;
	TestTrue(TEXT("FindInterfaceImplementations should return assets array."), (*Result)->TryGetArrayField(TEXT("assets"), Assets) && Assets);
	TestTrue(TEXT("FindInterfaceImplementations should return count."), (*Result)->TryGetNumberField(TEXT("count"), Count) && Count >= 0.0);

	// Unknown interface class returns ClassNotFound, not a silent empty result.
	TSharedRef<FJsonObject> BadParams = MakeShared<FJsonObject>();
	BadParams->SetStringField(TEXT("interfaceClass"), TEXT("/Script/Engine.DefinitelyNotAnInterface_9001"));
	const TSharedRef<FJsonObject> BadResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("FindInterfaceImplementations"), BadParams);
	return BlueprintBridgeTests::ExpectErrorCode(*this, BadResponse, TEXT("ClassNotFound"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeRenameBlueprintVariableBasicTest, "BlueprintBridge.Blueprint.RenameBlueprintVariable.Basic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeRenameBlueprintVariableBasicTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Rename"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> AddParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddParams->SetStringField(TEXT("name"), TEXT("OldName"));
	AddParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddParams))) { return false; }

	TSharedRef<FJsonObject> RenameParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	RenameParams->SetStringField(TEXT("variable"), TEXT("OldName"));
	RenameParams->SetStringField(TEXT("newName"), TEXT("NewName"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("RenameBlueprintVariable"), RenameParams))) { return false; }

	// Verify via a bridge round-trip rather than poking the in-memory UBlueprint directly.
	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse)) { return false; }
	const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	if (!DescribeResult) { return false; }
	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	if (!(*DescribeResult)->TryGetArrayField(TEXT("variables"), Variables) || !Variables) { return false; }
	bool bHasNewName = false;
	bool bHasOldName = false;
	for (const TSharedPtr<FJsonValue>& V : *Variables)
	{
		const TSharedPtr<FJsonObject>* VarObj = nullptr;
		FString Name;
		if (V->TryGetObject(VarObj) && VarObj && (*VarObj)->TryGetStringField(TEXT("name"), Name))
		{
			if (Name == TEXT("NewName")) bHasNewName = true;
			if (Name == TEXT("OldName")) bHasOldName = true;
		}
	}
	TestTrue(TEXT("DescribeBlueprint should include the new variable name post-rename."), bHasNewName);
	TestFalse(TEXT("DescribeBlueprint should not include the old variable name post-rename."), bHasOldName);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeRenameBlueprintVariableCollisionTest, "BlueprintBridge.Blueprint.RenameBlueprintVariable.Collision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeRenameBlueprintVariableCollisionTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("RenameClash"));
	if (!Asset.Blueprint) { return false; }

	for (const TCHAR* Name : { TEXT("Alpha"), TEXT("Beta") })
	{
		TSharedRef<FJsonObject> AddParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
		AddParams->SetStringField(TEXT("name"), Name);
		AddParams->SetStringField(TEXT("category"), TEXT("int"));
		if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddParams))) { return false; }
	}

	TSharedRef<FJsonObject> RenameParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	RenameParams->SetStringField(TEXT("variable"), TEXT("Beta"));
	RenameParams->SetStringField(TEXT("newName"), TEXT("Alpha"));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("RenameBlueprintVariable"), RenameParams);
	return BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("VariableAlreadyExists"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeRemoveBlueprintVariableBasicTest, "BlueprintBridge.Blueprint.RemoveBlueprintVariable.Basic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeRemoveBlueprintVariableBasicTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Remove"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> AddParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	AddParams->SetStringField(TEXT("name"), TEXT("DoomedVar"));
	AddParams->SetStringField(TEXT("category"), TEXT("int"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddBlueprintVariable"), AddParams))) { return false; }

	TSharedRef<FJsonObject> RemoveParams = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	RemoveParams->SetStringField(TEXT("variable"), TEXT("DoomedVar"));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, BlueprintBridgeTests::ExecuteJsonRequest(TEXT("RemoveBlueprintVariable"), RemoveParams))) { return false; }

	// Verify via a bridge round-trip.
	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("DescribeBlueprint"), BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath));
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse)) { return false; }
	const TSharedPtr<FJsonObject>* DescribeResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	if (!DescribeResult) { return false; }
	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	if (!(*DescribeResult)->TryGetArrayField(TEXT("variables"), Variables) || !Variables) { return false; }
	for (const TSharedPtr<FJsonValue>& V : *Variables)
	{
		const TSharedPtr<FJsonObject>* VarObj = nullptr;
		FString Name;
		if (V->TryGetObject(VarObj) && VarObj && (*VarObj)->TryGetStringField(TEXT("name"), Name))
		{
			TestFalse(TEXT("DescribeBlueprint should not include the removed variable."), Name == TEXT("DoomedVar"));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeRemoveBlueprintVariableNotFoundTest, "BlueprintBridge.Blueprint.RemoveBlueprintVariable.NotFound", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeRemoveBlueprintVariableNotFoundTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("RemoveMissing"));
	if (!Asset.Blueprint) { return false; }

	TSharedRef<FJsonObject> Params = BlueprintBridgeTests::MakeAssetParams(Asset.AssetPath);
	Params->SetStringField(TEXT("variable"), TEXT("NotAThing_9001"));
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("RemoveBlueprintVariable"), Params);
	return BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("VariableNotFound"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintBridgeAddVariableGetNodeUnresolvedTest, "BlueprintBridge.Blueprint.AddVariableGetNode.Unresolved", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBridgeAddVariableGetNodeUnresolvedTest::RunTest(const FString& Parameters)
{
	const BlueprintBridgeTests::FTestBlueprintAsset Asset = BlueprintBridgeTests::CreateTestBlueprint(*this, TEXT("Unresolved"));
	if (!Asset.Blueprint)
	{
		return false;
	}
	const FString EventGraphName = BlueprintBridgeTests::GetPrimaryEventGraphName(Asset.Blueprint);

	TSharedRef<FJsonObject> AddGetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddGetParams->SetStringField(TEXT("variable"), TEXT("DefinitelyNotAClassMember_9001"));
	AddGetParams->SetNumberField(TEXT("x"), 100);
	AddGetParams->SetNumberField(TEXT("y"), 100);
	const TSharedRef<FJsonObject> Response = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableGetNode"), AddGetParams);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, Response, TEXT("VariableNotFound")))
	{
		return false;
	}

	TSharedRef<FJsonObject> AddSetParams = BlueprintBridgeTests::MakeGraphParams(Asset.AssetPath, EventGraphName);
	AddSetParams->SetStringField(TEXT("variable"), TEXT("DefinitelyNotAClassMember_9001"));
	AddSetParams->SetNumberField(TEXT("x"), 200);
	AddSetParams->SetNumberField(TEXT("y"), 200);
	const TSharedRef<FJsonObject> SetResponse = BlueprintBridgeTests::ExecuteJsonRequest(TEXT("AddVariableSetNode"), AddSetParams);
	if (!BlueprintBridgeTests::ExpectErrorCode(*this, SetResponse, TEXT("VariableNotFound")))
	{
		return false;
	}

	// After failure, the orphan node should not be left in the graph.
	const TSharedRef<FJsonObject> DescribeResponse = BlueprintBridgeTests::DescribeGraphRequest(Asset.AssetPath, EventGraphName);
	if (!BlueprintBridgeTests::ExpectSuccess(*this, DescribeResponse))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* GraphResult = BlueprintBridgeTests::GetResultObject(*this, DescribeResponse);
	if (!GraphResult)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if ((*GraphResult)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			FString Title;
			if (NodeValue->TryGetObject(NodeObj) && NodeObj && (*NodeObj)->TryGetStringField(TEXT("title"), Title))
			{
				TestFalse(TEXT("Failed AddVariableGetNode should not leave an orphan node in the graph."), Title.Contains(TEXT("DefinitelyNotAClassMember_9001")));
			}
		}
	}
	return true;
}

#endif
