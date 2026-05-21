// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintBridge, Log, All);

namespace BlueprintBridge
{

static void EnsureCommandsRegistered()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}

	bRegistered = true;
	RegisterBlueprintBridgeCommands();
}

TSharedRef<FJsonObject> ExecuteRequestOnGameThread(const FString& RequestText)
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

	EnsureCommandsRegistered();
	if (const TSharedPtr<ICommand> RegisteredCommand = GetCommandRegistry().FindCommand(Command))
	{
		if (ShouldValidateRequestsAgainstSchemas())
		{
			FString ValidationError;
			if (!ValidateCommandParamsAgainstSchema(RegisteredCommand->GetName(), Params, RegisteredCommand->GetInputJsonSchema(), ValidationError))
			{
				return MakeBridgeError(Id, TEXT("InvalidParams"), ValidationError);
			}
		}

		return RegisteredCommand->Execute(Id, Params);
	}

	return MakeBridgeError(Id, TEXT("UnknownCommand"), FString::Printf(TEXT("Unknown command '%s'."), *Command));
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
