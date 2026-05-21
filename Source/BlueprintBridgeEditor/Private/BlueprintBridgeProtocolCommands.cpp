// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
TSharedRef<FJsonObject> MakeCommandDescription(const ICommand& Command, const bool bIncludeSchemas)
{
	TSharedRef<FJsonObject> CommandObject = MakeShared<FJsonObject>();
	CommandObject->SetStringField(TEXT("name"), Command.GetName());
	CommandObject->SetStringField(TEXT("description"), Command.GetDescription());
	CommandObject->SetStringField(TEXT("category"), Command.GetCategory());
	CommandObject->SetStringField(TEXT("risk"), CommandRiskToString(Command.GetRisk()));

	if (bIncludeSchemas)
	{
		if (TSharedPtr<FJsonObject> InputSchema = Command.GetInputJsonSchema())
		{
			CommandObject->SetObjectField(TEXT("inputSchema"), InputSchema.ToSharedRef());
		}

		if (TSharedPtr<FJsonObject> OutputSchema = Command.GetOutputJsonSchema())
		{
			CommandObject->SetObjectField(TEXT("outputSchema"), OutputSchema.ToSharedRef());
		}
	}

	return CommandObject;
}

TSharedRef<FJsonObject> ListCommands(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Commands;
	for (const TSharedRef<ICommand>& Command : GetCommandRegistry().GetCommands())
	{
		Commands.Add(MakeShared<FJsonValueObject>(MakeCommandDescription(*Command, false)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("commands"), Commands);
	return MakeSuccess(Id, Result);
}

TSharedRef<FJsonObject> DescribeCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString CommandName;
	if (!TryGetRequiredString(Params, TEXT("command"), CommandName))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("DescribeCommand requires params.command."));
	}

	const TSharedPtr<ICommand> Command = GetCommandRegistry().FindCommand(CommandName);
	if (!Command.IsValid())
	{
		return MakeBridgeError(Id, TEXT("CommandNotFound"), FString::Printf(TEXT("Command '%s' was not found."), *CommandName));
	}

	return MakeSuccess(Id, MakeCommandDescription(*Command, true));
}

TSharedRef<FJsonObject> BatchCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params);

TSharedRef<FJsonObject> ExecuteRequestOnGameThread(const FString& RequestText);

TSharedRef<FJsonObject> PingCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	return MakeSuccessMessage(Id, TEXT("Pong"));
}

TSharedRef<FJsonObject> GetProjectNameCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("result"), FApp::GetProjectName());
	return Response;
}

TSharedRef<FJsonObject> GetEngineVersionCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("id"), Id);
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetStringField(TEXT("result"), FEngineVersion::Current().ToString());
	return Response;
}

TSharedRef<FJsonObject> BatchCommand(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* Requests = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("requests"), Requests) || Requests == nullptr)
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("Batch requires params.requests."));
	}

	const int32 MaxBatchSize = GetMaxBatchSize();
	if (MaxBatchSize > 0 && Requests->Num() > MaxBatchSize)
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), FString::Printf(TEXT("Batch request count %d exceeds MaxBatchSize %d."), Requests->Num(), MaxBatchSize));
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
} // namespace BlueprintBridge
