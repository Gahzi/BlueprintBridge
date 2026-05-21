// Copyright Odyssey Interactive. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace BlueprintBridge
{
enum class ECommandRisk : uint8
{
	ReadOnly,
	ModifiesAsset,
	CreatesAsset,
	DeletesAsset,
	SourceControl,
	SavePackage
};

FString CommandRiskToString(ECommandRisk Risk);

struct ICommand : TSharedFromThis<ICommand>
{
	virtual ~ICommand() = default;

	virtual FString GetName() const = 0;
	virtual FString GetDescription() const = 0;
	virtual FString GetCategory() const = 0;
	virtual ECommandRisk GetRisk() const = 0;
	virtual TSharedPtr<FJsonObject> GetInputJsonSchema() const = 0;
	virtual TSharedPtr<FJsonObject> GetOutputJsonSchema() const { return nullptr; }

	virtual TSharedRef<FJsonObject> Execute(const FString& Id, const TSharedPtr<FJsonObject>& Params) = 0;
};

class FFunctionCommand final : public ICommand
{
public:
	using FHandler = TFunction<TSharedRef<FJsonObject>(const FString& Id, const TSharedPtr<FJsonObject>& Params)>;

	FFunctionCommand(
		FString InName,
		FString InDescription,
		FString InCategory,
		ECommandRisk InRisk,
		TSharedPtr<FJsonObject> InInputJsonSchema,
		FHandler InHandler);

	FString GetName() const override { return Name; }
	FString GetDescription() const override { return Description; }
	FString GetCategory() const override { return Category; }
	ECommandRisk GetRisk() const override { return Risk; }
	TSharedPtr<FJsonObject> GetInputJsonSchema() const override { return InputJsonSchema; }
	TSharedRef<FJsonObject> Execute(const FString& Id, const TSharedPtr<FJsonObject>& Params) override;

private:
	FString Name;
	FString Description;
	FString Category;
	ECommandRisk Risk = ECommandRisk::ReadOnly;
	TSharedPtr<FJsonObject> InputJsonSchema;
	FHandler Handler;
};
} // namespace BlueprintBridge
