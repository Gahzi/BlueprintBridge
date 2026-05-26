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

// $ref path resolution for chained batch requests. A path looks like:
//   "0.result.node.guid"            -> response 0's result.node.guid
//   "0.result.node.pins[2].name"    -> first pin's name on node from response 0
// Segments are dot-separated; bracketed integers index into JSON arrays. The first
// segment must be the integer request index of an *earlier* request in the same batch.
static bool ParseBatchRefPath(const FString& PathSpec, int32& OutIndex, TArray<FString>& OutSegments, FString& OutErrorCode, FString& OutErrorMessage)
{
	int32 FirstDot = INDEX_NONE;
	PathSpec.FindChar(TCHAR('.'), FirstDot);
	const FString IndexStr = (FirstDot == INDEX_NONE) ? PathSpec : PathSpec.Left(FirstDot);
	if (IndexStr.IsEmpty() || !IndexStr.IsNumeric())
	{
		OutErrorCode = TEXT("InvalidRefSyntax");
		OutErrorMessage = FString::Printf(TEXT("$ref must start with a request index, got '%s'."), *IndexStr);
		return false;
	}
	OutIndex = FCString::Atoi(*IndexStr);

	const FString Remaining = (FirstDot == INDEX_NONE) ? FString() : PathSpec.Mid(FirstDot + 1);
	OutSegments.Reset();
	FString Token;
	for (int32 I = 0; I < Remaining.Len(); ++I)
	{
		const TCHAR Ch = Remaining[I];
		if (Ch == TCHAR('.'))
		{
			if (!Token.IsEmpty()) { OutSegments.Add(Token); Token.Reset(); }
		}
		else if (Ch == TCHAR('['))
		{
			if (!Token.IsEmpty()) { OutSegments.Add(Token); Token.Reset(); }
			int32 J = I + 1;
			while (J < Remaining.Len() && Remaining[J] != TCHAR(']')) { Token.AppendChar(Remaining[J]); ++J; }
			if (J >= Remaining.Len() || Token.IsEmpty() || !Token.IsNumeric())
			{
				OutErrorCode = TEXT("InvalidRefSyntax");
				OutErrorMessage = FString::Printf(TEXT("$ref array index must be a non-negative integer enclosed in brackets in path '%s'."), *PathSpec);
				return false;
			}
			OutSegments.Add(FString::Printf(TEXT("[%s]"), *Token));
			Token.Reset();
			I = J; // skip past ']'
		}
		else
		{
			Token.AppendChar(Ch);
		}
	}
	if (!Token.IsEmpty()) { OutSegments.Add(Token); }
	return true;
}

static TSharedPtr<FJsonValue> WalkJsonPath(const TSharedPtr<FJsonValue>& Root, const TArray<FString>& Segments, FString& OutErrorMessage)
{
	TSharedPtr<FJsonValue> Current = Root;
	for (const FString& Segment : Segments)
	{
		if (!Current.IsValid())
		{
			OutErrorMessage = FString::Printf(TEXT("Path segment '%s' applied to null value."), *Segment);
			return nullptr;
		}
		if (Segment.StartsWith(TEXT("[")) && Segment.EndsWith(TEXT("]")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Current->Type != EJson::Array || !Current->TryGetArray(Array) || !Array)
			{
				OutErrorMessage = FString::Printf(TEXT("Cannot index '%s' on non-array value."), *Segment);
				return nullptr;
			}
			const int32 Index = FCString::Atoi(*Segment.Mid(1, Segment.Len() - 2));
			if (Index < 0 || Index >= Array->Num())
			{
				OutErrorMessage = FString::Printf(TEXT("Array index %d out of bounds (size %d)."), Index, Array->Num());
				return nullptr;
			}
			Current = (*Array)[Index];
		}
		else
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Current->Type != EJson::Object || !Current->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				OutErrorMessage = FString::Printf(TEXT("Cannot access field '%s' on non-object value."), *Segment);
				return nullptr;
			}
			const TSharedPtr<FJsonValue> Field = (*Obj)->TryGetField(Segment);
			if (!Field.IsValid())
			{
				OutErrorMessage = FString::Printf(TEXT("Field '%s' not found."), *Segment);
				return nullptr;
			}
			Current = Field;
		}
	}
	return Current;
}

static bool ResolveBatchRef(const FString& PathSpec, const TArray<TSharedPtr<FJsonValue>>& Responses, int32 CurrentIndex, int32 TotalRequestCount, TSharedPtr<FJsonValue>& OutValue, FString& OutErrorCode, FString& OutErrorMessage)
{
	int32 Index = INDEX_NONE;
	TArray<FString> Segments;
	if (!ParseBatchRefPath(PathSpec, Index, Segments, OutErrorCode, OutErrorMessage))
	{
		return false;
	}
	if (Index < 0 || Index >= TotalRequestCount)
	{
		OutErrorCode = TEXT("RefOutOfBounds");
		OutErrorMessage = FString::Printf(TEXT("$ref:%d is out of bounds (batch has %d requests)."), Index, TotalRequestCount);
		return false;
	}
	if (Index >= CurrentIndex)
	{
		OutErrorCode = TEXT("RefForwardReference");
		OutErrorMessage = FString::Printf(TEXT("$ref:%d cannot be used by request at index %d (refs must point at earlier requests)."), Index, CurrentIndex);
		return false;
	}
	TSharedPtr<FJsonValue> Predecessor = Responses[Index];
	if (Predecessor.IsValid() && Predecessor->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* PredObj = nullptr;
		if (Predecessor->TryGetObject(PredObj) && PredObj && (*PredObj).IsValid())
		{
			bool bPredOk = true;
			if ((*PredObj)->TryGetBoolField(TEXT("ok"), bPredOk) && !bPredOk)
			{
				OutErrorCode = TEXT("RefPredecessorFailed");
				OutErrorMessage = FString::Printf(TEXT("$ref:%d points at a request that failed; its result is not available."), Index);
				return false;
			}
		}
	}
	FString WalkError;
	OutValue = WalkJsonPath(Predecessor, Segments, WalkError);
	if (!OutValue.IsValid())
	{
		OutErrorCode = TEXT("RefUnresolved");
		OutErrorMessage = FString::Printf(TEXT("$ref path '%s' did not resolve: %s"), *PathSpec, *WalkError);
		return false;
	}
	return true;
}

static void SubstituteBatchRefsInValue(TSharedPtr<FJsonValue>& InOutValue, const TArray<TSharedPtr<FJsonValue>>& Responses, int32 CurrentIndex, int32 TotalRequestCount, FString& OutErrorCode, FString& OutErrorMessage)
{
	if (!OutErrorCode.IsEmpty() || !InOutValue.IsValid()) { return; }

	if (InOutValue->Type == EJson::String)
	{
		FString Str;
		InOutValue->TryGetString(Str);
		if (Str.StartsWith(TEXT("$$ref:")))
		{
			InOutValue = MakeShared<FJsonValueString>(Str.Mid(1));
			return;
		}
		if (Str.StartsWith(TEXT("$ref:")))
		{
			TSharedPtr<FJsonValue> Resolved;
			if (!ResolveBatchRef(Str.Mid(5), Responses, CurrentIndex, TotalRequestCount, Resolved, OutErrorCode, OutErrorMessage))
			{
				return;
			}
			InOutValue = Resolved.IsValid() ? Resolved : MakeShared<FJsonValueNull>();
		}
		return;
	}
	if (InOutValue->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!InOutValue->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid()) { return; }
		TArray<FString> Keys;
		(*ObjPtr)->Values.GetKeys(Keys);
		for (const FString& Key : Keys)
		{
			TSharedPtr<FJsonValue> Child = (*ObjPtr)->TryGetField(Key);
			SubstituteBatchRefsInValue(Child, Responses, CurrentIndex, TotalRequestCount, OutErrorCode, OutErrorMessage);
			if (!OutErrorCode.IsEmpty()) { return; }
			(*ObjPtr)->SetField(Key, Child);
		}
		return;
	}
	if (InOutValue->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (!InOutValue->TryGetArray(ArrayPtr) || !ArrayPtr) { return; }
		TArray<TSharedPtr<FJsonValue>> NewArray;
		NewArray.Reserve(ArrayPtr->Num());
		for (TSharedPtr<FJsonValue> Element : *ArrayPtr)
		{
			SubstituteBatchRefsInValue(Element, Responses, CurrentIndex, TotalRequestCount, OutErrorCode, OutErrorMessage);
			if (!OutErrorCode.IsEmpty()) { return; }
			NewArray.Add(Element);
		}
		InOutValue = MakeShared<FJsonValueArray>(NewArray);
	}
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

	bool bRollbackOnFailure = false;
	Params->TryGetBoolField(TEXT("rollbackOnFailure"), bRollbackOnFailure);
	TUniquePtr<FScopedTransaction> Transaction;
	if (bRollbackOnFailure)
	{
		Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("BlueprintBridge", "BatchRollback", "Blueprint Bridge: Batch"));
	}

	bool bFailed = false;
	TArray<TSharedPtr<FJsonValue>> Results;
	for (int32 RequestIndex = 0; RequestIndex < Requests->Num(); ++RequestIndex)
	{
		const TSharedPtr<FJsonValue>& RequestValue = (*Requests)[RequestIndex];
		const TSharedPtr<FJsonObject>* ChildRequest = nullptr;
		if (!RequestValue.IsValid() || !RequestValue->TryGetObject(ChildRequest) || ChildRequest == nullptr || !ChildRequest->IsValid())
		{
			Results.Add(MakeShared<FJsonValueObject>(MakeBridgeError(Id, TEXT("InvalidBatchRequest"), TEXT("Each batch request must be an object."))));
			if (bRollbackOnFailure)
			{
				bFailed = true;
				break;
			}
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

		// Substitute $ref:N.path tokens in params against earlier responses in this batch.
		TSharedPtr<FJsonValue> ParamsValue;
		const TSharedPtr<FJsonObject>* ChildParamsObj = nullptr;
		if ((*ChildRequest)->TryGetObjectField(TEXT("params"), ChildParamsObj) && ChildParamsObj && (*ChildParamsObj).IsValid())
		{
			ParamsValue = MakeShared<FJsonValueObject>(*ChildParamsObj);
			FString RefErrorCode;
			FString RefErrorMessage;
			SubstituteBatchRefsInValue(ParamsValue, Results, RequestIndex, Requests->Num(), RefErrorCode, RefErrorMessage);
			if (!RefErrorCode.IsEmpty())
			{
				FString ChildId;
				(*ChildRequest)->TryGetStringField(TEXT("id"), ChildId);
				Results.Add(MakeShared<FJsonValueObject>(MakeBridgeError(ChildId, RefErrorCode, RefErrorMessage)));
				if (bRollbackOnFailure)
				{
					bFailed = true;
					break;
				}
				continue;
			}
			if (ParamsValue.IsValid() && ParamsValue->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>* SubstitutedObj = nullptr;
				if (ParamsValue->TryGetObject(SubstitutedObj) && SubstitutedObj && (*SubstitutedObj).IsValid())
				{
					(*ChildRequest)->SetObjectField(TEXT("params"), *SubstitutedObj);
				}
			}
		}

		TSharedRef<FJsonObject> ChildResponse = ExecuteRequestOnGameThread(JsonToString((*ChildRequest).ToSharedRef()));
		Results.Add(MakeShared<FJsonValueObject>(ChildResponse));

		bool bChildOk = true;
		if (bRollbackOnFailure && ChildResponse->TryGetBoolField(TEXT("ok"), bChildOk) && !bChildOk)
		{
			bFailed = true;
			break;
		}
	}

	if (bFailed && Transaction.IsValid())
	{
		Transaction->Cancel();
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("responses"), Results);
	Result->SetBoolField(TEXT("rolledBack"), bFailed && bRollbackOnFailure);
	return MakeSuccess(Id, Result);
}
} // namespace BlueprintBridge
