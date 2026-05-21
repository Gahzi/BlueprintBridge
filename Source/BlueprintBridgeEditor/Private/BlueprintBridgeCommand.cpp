// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommand.h"

namespace BlueprintBridge
{
FString CommandRiskToString(const ECommandRisk Risk)
{
	switch (Risk)
	{
	case ECommandRisk::ReadOnly:
		return TEXT("ReadOnly");
	case ECommandRisk::ModifiesAsset:
		return TEXT("ModifiesAsset");
	case ECommandRisk::CreatesAsset:
		return TEXT("CreatesAsset");
	case ECommandRisk::DeletesAsset:
		return TEXT("DeletesAsset");
	case ECommandRisk::SourceControl:
		return TEXT("SourceControl");
	case ECommandRisk::SavePackage:
		return TEXT("SavePackage");
	default:
		return TEXT("Unknown");
	}
}

FFunctionCommand::FFunctionCommand(
	FString InName,
	FString InDescription,
	FString InCategory,
	const ECommandRisk InRisk,
	TSharedPtr<FJsonObject> InInputJsonSchema,
	TSharedPtr<FJsonObject> InOutputJsonSchema,
	FHandler InHandler)
	: Name(MoveTemp(InName))
	, Description(MoveTemp(InDescription))
	, Category(MoveTemp(InCategory))
	, Risk(InRisk)
	, InputJsonSchema(MoveTemp(InInputJsonSchema))
	, OutputJsonSchema(MoveTemp(InOutputJsonSchema))
	, Handler(MoveTemp(InHandler))
{
}

TSharedRef<FJsonObject> FFunctionCommand::Execute(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	check(Handler);
	return Handler(Id, Params);
}
} // namespace BlueprintBridge
