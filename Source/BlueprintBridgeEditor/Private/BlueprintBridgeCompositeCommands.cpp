// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
namespace
{
static TSharedRef<FJsonObject> MakeSpecError(const FString& Id, const FString& Phase, const FString& Code, const FString& Message, bool bRolledBack)
{
	TSharedRef<FJsonObject> Response = MakeBridgeError(Id, Code, Message);
	const TSharedPtr<FJsonObject>* ErrObj = nullptr;
	if (Response->TryGetObjectField(TEXT("error"), ErrObj) && ErrObj != nullptr && (*ErrObj).IsValid())
	{
		(*ErrObj)->SetStringField(TEXT("phase"), Phase);
		(*ErrObj)->SetBoolField(TEXT("rolledBack"), bRolledBack);
	}
	return Response;
}

// Asset creation isn't transactional — FScopedTransaction::Cancel() won't undo CreatePackage / CreateBlueprint.
// To make rollback honest, detach the freshly-created Blueprint from the asset registry, rename it into the
// transient package so it's no longer reachable at the original path, and mark it as garbage.
static void RollbackCreatedBlueprint(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return;
	}
	UPackage* OldPackage = Blueprint->GetOutermost();
	FAssetRegistryModule::AssetDeleted(Blueprint);
	Blueprint->ClearFlags(RF_Public | RF_Standalone);
	Blueprint->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
	Blueprint->MarkAsGarbage();
	if (OldPackage && OldPackage != GetTransientPackage())
	{
		OldPackage->ClearFlags(RF_Public | RF_Standalone);
		OldPackage->MarkAsGarbage();
	}
}
} // anonymous

TSharedRef<FJsonObject> CreateBlueprintFromSpec(const FString& Id, const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString ParentClassPath;
	if (!TryGetRequiredString(Params, TEXT("asset"), AssetPath) || !TryGetRequiredString(Params, TEXT("parentClass"), ParentClassPath))
	{
		return MakeBridgeError(Id, TEXT("InvalidParams"), TEXT("CreateBlueprintFromSpec requires params.asset and params.parentClass."));
	}

	bool bRollbackOnFailure = true;
	bool bCompile = false;
	Params->TryGetBoolField(TEXT("rollbackOnFailure"), bRollbackOnFailure);
	Params->TryGetBoolField(TEXT("compile"), bCompile);

	FScopedTransaction Transaction(NSLOCTEXT("BlueprintBridge", "CreateBlueprintFromSpec", "Blueprint Bridge: Create Blueprint From Spec"));

	FString ErrorCode;
	FString ErrorMessage;

	UBlueprint* Blueprint = CreateBlueprintAssetWorker(AssetPath, ParentClassPath, ErrorCode, ErrorMessage);
	if (!Blueprint)
	{
		if (bRollbackOnFailure)
		{
			Transaction.Cancel();
		}
		return MakeSpecError(Id, TEXT("asset"), ErrorCode, ErrorMessage, bRollbackOnFailure);
	}

	const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
	if (Params->TryGetArrayField(TEXT("variables"), Variables) && Variables != nullptr)
	{
		for (int32 i = 0; i < Variables->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* VarObj = nullptr;
			if (!(*Variables)[i].IsValid() || !(*Variables)[i]->TryGetObject(VarObj) || VarObj == nullptr || !VarObj->IsValid())
			{
				if (bRollbackOnFailure) { Transaction.Cancel(); RollbackCreatedBlueprint(Blueprint); }
				return MakeSpecError(Id, FString::Printf(TEXT("variables[%d]"), i), TEXT("InvalidParams"), TEXT("Each variables[] entry must be an object."), bRollbackOnFailure);
			}
			if (!AddBlueprintVariableWorker(Blueprint, *VarObj, ErrorCode, ErrorMessage))
			{
				if (bRollbackOnFailure) { Transaction.Cancel(); RollbackCreatedBlueprint(Blueprint); }
				return MakeSpecError(Id, FString::Printf(TEXT("variables[%d]"), i), ErrorCode, ErrorMessage, bRollbackOnFailure);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	if (Params->TryGetArrayField(TEXT("components"), Components) && Components != nullptr)
	{
		for (int32 i = 0; i < Components->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* CompObj = nullptr;
			if (!(*Components)[i].IsValid() || !(*Components)[i]->TryGetObject(CompObj) || CompObj == nullptr || !CompObj->IsValid())
			{
				if (bRollbackOnFailure) { Transaction.Cancel(); RollbackCreatedBlueprint(Blueprint); }
				return MakeSpecError(Id, FString::Printf(TEXT("components[%d]"), i), TEXT("InvalidParams"), TEXT("Each components[] entry must be an object."), bRollbackOnFailure);
			}
			USCS_Node* Node = AddComponentWorker(Blueprint, *CompObj, ErrorCode, ErrorMessage);
			if (!Node)
			{
				if (bRollbackOnFailure) { Transaction.Cancel(); RollbackCreatedBlueprint(Blueprint); }
				return MakeSpecError(Id, FString::Printf(TEXT("components[%d]"), i), ErrorCode, ErrorMessage, bRollbackOnFailure);
			}
			// Optional: promote to root.
			bool bRoot = false;
			if ((*CompObj)->TryGetBoolField(TEXT("root"), bRoot) && bRoot && Cast<USceneComponent>(Node->ComponentTemplate))
			{
				Blueprint->SimpleConstructionScript->Modify();
				Node->Modify();
				if (USCS_Node* OldParent = FindSCSParentNode(Blueprint->SimpleConstructionScript, Node))
				{
					OldParent->Modify();
					OldParent->RemoveChildNode(Node, false);
				}
				else
				{
					Blueprint->SimpleConstructionScript->RemoveNode(Node, false);
				}
				Blueprint->SimpleConstructionScript->AddNode(Node);
				Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
	if (Params->TryGetArrayField(TEXT("functions"), Functions) && Functions != nullptr)
	{
		for (int32 i = 0; i < Functions->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* FnObj = nullptr;
			if (!(*Functions)[i].IsValid() || !(*Functions)[i]->TryGetObject(FnObj) || FnObj == nullptr || !FnObj->IsValid())
			{
				if (bRollbackOnFailure) { Transaction.Cancel(); RollbackCreatedBlueprint(Blueprint); }
				return MakeSpecError(Id, FString::Printf(TEXT("functions[%d]"), i), TEXT("InvalidParams"), TEXT("Each functions[] entry must be an object."), bRollbackOnFailure);
			}
			FString FnName;
			if (!(*FnObj)->TryGetStringField(TEXT("name"), FnName) || FnName.IsEmpty())
			{
				if (bRollbackOnFailure) { Transaction.Cancel(); RollbackCreatedBlueprint(Blueprint); }
				return MakeSpecError(Id, FString::Printf(TEXT("functions[%d]"), i), TEXT("InvalidParams"), TEXT("functions[].name is required."), bRollbackOnFailure);
			}

			TSharedRef<FJsonObject> ApplyParams = MakeShared<FJsonObject>();
			ApplyParams->SetStringField(TEXT("asset"), AssetPath);
			ApplyParams->SetStringField(TEXT("function"), FnName);
			ApplyParams->SetBoolField(TEXT("createIfMissing"), true);
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if ((*FnObj)->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
			{
				ApplyParams->SetArrayField(TEXT("inputs"), *Inputs);
			}
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if ((*FnObj)->TryGetArrayField(TEXT("outputs"), Outputs) && Outputs != nullptr)
			{
				ApplyParams->SetArrayField(TEXT("outputs"), *Outputs);
			}
			const TArray<TSharedPtr<FJsonValue>>* Flow = nullptr;
			if ((*FnObj)->TryGetArrayField(TEXT("flow"), Flow) && Flow != nullptr)
			{
				ApplyParams->SetArrayField(TEXT("flow"), *Flow);
			}

			TSharedRef<FJsonObject> FnResponse = ApplySemanticFunction(Id, ApplyParams);
			bool bOk = false;
			if (!FnResponse->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
			{
				FString InnerCode = TEXT("SemanticFunctionFailed");
				FString InnerMessage = FString::Printf(TEXT("Function '%s' failed to apply."), *FnName);
				const TSharedPtr<FJsonObject>* InnerErr = nullptr;
				if (FnResponse->TryGetObjectField(TEXT("error"), InnerErr) && InnerErr != nullptr && (*InnerErr).IsValid())
				{
					(*InnerErr)->TryGetStringField(TEXT("code"), InnerCode);
					(*InnerErr)->TryGetStringField(TEXT("message"), InnerMessage);
				}
				if (bRollbackOnFailure) { Transaction.Cancel(); RollbackCreatedBlueprint(Blueprint); }
				return MakeSpecError(Id, FString::Printf(TEXT("functions[%d]"), i), InnerCode, InnerMessage, bRollbackOnFailure);
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Blueprint->GetOutermost()->MarkPackageDirty();

	if (bCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("name"), Blueprint->GetName());
	Result->SetStringField(TEXT("parentClass"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
	if (bCompile)
	{
		Result->SetStringField(TEXT("compileStatus"), GetBlueprintStatusString(Blueprint->Status));
	}
	return MakeSuccess(Id, Result);
}

} // namespace BlueprintBridge
