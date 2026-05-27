// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintBridge, Log, All);

namespace BlueprintBridge
{

// Per-asset monotonic version counter. Versions live in-process and reset on editor
// restart; callers must treat them as session-scoped. First observation returns 1 so
// that any caller passing a stale 0 from prior storage always sees the current state.
static TMap<FString, int64>& GetAssetVersionMap()
{
	static TMap<FString, int64> Versions;
	return Versions;
}

// Normalize asset paths so all three forms ("/Game/Foo/Bar", "/Game/Foo/Bar.Bar",
// "/Game/Foo/Bar.Bar_C") map to the same key. Otherwise a caller that varies the form
// across calls would see distinct, stale counters.
static FString NormalizeAssetPath(const FString& AssetPath)
{
	if (AssetPath.IsEmpty()) { return AssetPath; }
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	return PackageName.IsEmpty() ? AssetPath : PackageName;
}

static int64 GetAssetVersion(const FString& AssetPath)
{
	if (const int64* Existing = GetAssetVersionMap().Find(NormalizeAssetPath(AssetPath)))
	{
		return *Existing;
	}
	return 1;
}

static int64 BumpAssetVersion(const FString& AssetPath)
{
	const int64 Next = GetAssetVersion(AssetPath) + 1;
	GetAssetVersionMap().Add(NormalizeAssetPath(AssetPath), Next);
	return Next;
}

static bool RiskMutatesAsset(ECommandRisk Risk)
{
	return Risk == ECommandRisk::ModifiesAsset || Risk == ECommandRisk::CreatesAsset || Risk == ECommandRisk::DeletesAsset;
}

static int32 ComputeEditDistance(const FString& A, const FString& B)
{
	const int32 LenA = A.Len();
	const int32 LenB = B.Len();
	if (LenA == 0) { return LenB; }
	if (LenB == 0) { return LenA; }

	TArray<int32> Prev;
	TArray<int32> Curr;
	Prev.SetNumUninitialized(LenB + 1);
	Curr.SetNumUninitialized(LenB + 1);
	for (int32 J = 0; J <= LenB; ++J) { Prev[J] = J; }

	for (int32 I = 1; I <= LenA; ++I)
	{
		Curr[0] = I;
		const TCHAR CharA = FChar::ToLower(A[I - 1]);
		for (int32 J = 1; J <= LenB; ++J)
		{
			const TCHAR CharB = FChar::ToLower(B[J - 1]);
			const int32 Cost = (CharA == CharB) ? 0 : 1;
			Curr[J] = FMath::Min3(Curr[J - 1] + 1, Prev[J] + 1, Prev[J - 1] + Cost);
		}
		Swap(Prev, Curr);
	}
	return Prev[LenB];
}

static FString FindClosestCommandName(const FString& Unknown)
{
	if (Unknown.IsEmpty()) { return FString(); }

	const int32 UnknownLen = Unknown.Len();
	// Allow more slack on longer names. Cap at 4 to avoid wild suggestions.
	const int32 MaxDistance = FMath::Clamp(UnknownLen / 3, 1, 4);

	FString Best;
	int32 BestDistance = MaxDistance + 1;
	for (const TSharedRef<ICommand>& Command : GetCommandRegistry().GetCommands())
	{
		const int32 Distance = ComputeEditDistance(Unknown, Command->GetName());
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Command->GetName();
		}
	}
	return (BestDistance <= MaxDistance) ? Best : FString();
}

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

		// Asset-versioning short-circuit: if params.asset is set and the caller's stashed
		// version matches the current asset version, skip dispatch entirely and return a
		// minimal "unchanged" response.
		FString AssetPath;
		if (Params.IsValid()) { Params->TryGetStringField(TEXT("asset"), AssetPath); }
		if (!AssetPath.IsEmpty() && Params.IsValid())
		{
			double ClientVersion = 0.0;
			if (Params->TryGetNumberField(TEXT("ifAssetVersionDiffersFrom"), ClientVersion))
			{
				const int64 CurrentVersion = GetAssetVersion(AssetPath);
				if (static_cast<int64>(ClientVersion) == CurrentVersion)
				{
					TSharedRef<FJsonObject> ShortCircuit = MakeShared<FJsonObject>();
					ShortCircuit->SetBoolField(TEXT("unchanged"), true);
					ShortCircuit->SetNumberField(TEXT("assetVersion"), static_cast<double>(CurrentVersion));
					return MakeSuccess(Id, ShortCircuit);
				}
			}
		}

		TSharedRef<FJsonObject> Response = RegisteredCommand->Execute(Id, Params);

		// Post-process: bump version on a successful mutation, then stamp the current
		// version into any object-shaped result so callers can use it on their next call.
		bool bOk = false;
		if (Response->TryGetBoolField(TEXT("ok"), bOk) && bOk && !AssetPath.IsEmpty())
		{
			if (RiskMutatesAsset(RegisteredCommand->GetRisk()))
			{
				BumpAssetVersion(AssetPath);
			}
			const TSharedPtr<FJsonObject>* ResultObj = nullptr;
			if (Response->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj && (*ResultObj).IsValid())
			{
				(*ResultObj)->SetNumberField(TEXT("assetVersion"), static_cast<double>(GetAssetVersion(AssetPath)));
			}
		}
		return Response;
	}

	const FString Suggestion = FindClosestCommandName(Command);
	const FString Message = Suggestion.IsEmpty()
		? FString::Printf(TEXT("Unknown command '%s'."), *Command)
		: FString::Printf(TEXT("Unknown command '%s'. Did you mean '%s'?"), *Command, *Suggestion);
	return MakeBridgeError(Id, TEXT("UnknownCommand"), Message);
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
