// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeSemanticLowering.h"

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
namespace
{
struct FLowerCtx
{
	UBlueprint* Blueprint = nullptr;
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<TSharedPtr<FJsonValue>> Links;
	TArray<TSharedPtr<FJsonValue>> Defaults;
	TSharedRef<FJsonObject> Resolutions = MakeShared<FJsonObject>();
	TSet<FString> Inputs;
	TArray<FString> ExecFrontier;
	int32 IdCounter = 0;

	FString NewId() { return FString::Printf(TEXT("n_%d"), ++IdCounter); }
};

struct FExprResult
{
	bool bIsLiteral = false;
	FString LiteralValue;
	FString PinRef;
};

static FString ConcatPtr(const FString& Base, const FString& Suffix)
{
	return Base.IsEmpty() ? Suffix : Base + TEXT(".") + Suffix;
}

static FString FormatNumberLiteral(double N)
{
	// Format whole numbers as integers so int-typed pins parse them correctly.
	if (FMath::IsNearlyEqual(N, FMath::RoundToDouble(N)))
	{
		return FString::Printf(TEXT("%lld"), static_cast<int64>(N));
	}
	return FString::SanitizeFloat(N);
}

static FString JsonValueToLiteral(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}
	FString S;
	if (Value->TryGetString(S))
	{
		return S;
	}
	bool B = false;
	if (Value->TryGetBool(B))
	{
		return B ? TEXT("true") : TEXT("false");
	}
	double N = 0.0;
	if (Value->TryGetNumber(N))
	{
		return FormatNumberLiteral(N);
	}
	return FString();
}

static bool SplitFunctionPath(const FString& Path, FString& OutClassPath, FString& OutFuncName)
{
	int32 LastDot = INDEX_NONE;
	Path.FindLastChar(TEXT('.'), LastDot);
	if (LastDot == INDEX_NONE)
	{
		return false;
	}
	OutClassPath = Path.Left(LastDot);
	OutFuncName = Path.Mid(LastDot + 1);
	return !OutClassPath.IsEmpty() && !OutFuncName.IsEmpty();
}

static bool IsFunctionPure(UFunction* Function)
{
	return Function && Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
}

static void AddLink(FLowerCtx& Ctx, const FString& From, const FString& To)
{
	TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetStringField(TEXT("from"), From);
	Link->SetStringField(TEXT("to"), To);
	Ctx.Links.Add(MakeShared<FJsonValueObject>(Link));
}

static void AddDefault(FLowerCtx& Ctx, const FString& NodeId, const FString& Pin, const FString& Value)
{
	TSharedRef<FJsonObject> Def = MakeShared<FJsonObject>();
	Def->SetStringField(TEXT("node"), NodeId);
	Def->SetStringField(TEXT("pin"), Pin);
	Def->SetStringField(TEXT("value"), Value);
	Ctx.Defaults.Add(MakeShared<FJsonValueObject>(Def));
}

static TSharedRef<FJsonObject> AddNode(FLowerCtx& Ctx, const FString& Id, const FString& Type)
{
	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), Id);
	Node->SetStringField(TEXT("type"), Type);
	Ctx.Nodes.Add(MakeShared<FJsonValueObject>(Node));
	return Node;
}

static void LinkFrontier(FLowerCtx& Ctx, const FString& ToRef)
{
	for (const FString& From : Ctx.ExecFrontier)
	{
		AddLink(Ctx, From, ToRef);
	}
	Ctx.ExecFrontier.Reset();
}

static void LinkFrontierAndAdvance(FLowerCtx& Ctx, const FString& NodeId)
{
	LinkFrontier(Ctx, NodeId + TEXT(".execute"));
	Ctx.ExecFrontier.Add(NodeId + TEXT(".then"));
}

static void WireExprToPin(FLowerCtx& Ctx, const FExprResult& Expr, const FString& NodeId, const FString& PinName)
{
	if (Expr.bIsLiteral)
	{
		AddDefault(Ctx, NodeId, PinName, Expr.LiteralValue);
	}
	else
	{
		AddLink(Ctx, Expr.PinRef, NodeId + TEXT(".") + PinName);
	}
}

static bool LowerExpression(const TSharedPtr<FJsonValue>& Value, const FString& Pointer, FLowerCtx& Ctx, bool bAllowImpureCall, FExprResult& OutExpr, FString& OutErrorPointer, FString& OutError);
static bool LowerStatement(const TSharedPtr<FJsonValue>& StmtValue, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError);
static bool LowerBlock(const TArray<TSharedPtr<FJsonValue>>& Block, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError);

static bool ResolveBareString(const FString& Name, const FString& Pointer, FLowerCtx& Ctx, FExprResult& OutExpr)
{
	if (Ctx.Inputs.Contains(Name))
	{
		OutExpr.bIsLiteral = false;
		OutExpr.PinRef = FString::Printf(TEXT("entry.%s"), *Name);
		Ctx.Resolutions->SetStringField(Pointer, FString::Printf(TEXT("param:%s"), *Name));
		return true;
	}
	FEdGraphPinType VarType;
	if (Ctx.Blueprint && TryGetBlueprintVariableType(Ctx.Blueprint, *Name, VarType))
	{
		const FString NodeId = Ctx.NewId();
		TSharedRef<FJsonObject> Node = AddNode(Ctx, NodeId, TEXT("VariableGet"));
		Node->SetStringField(TEXT("variable"), Name);
		OutExpr.bIsLiteral = false;
		OutExpr.PinRef = FString::Printf(TEXT("%s.%s"), *NodeId, *Name);
		Ctx.Resolutions->SetStringField(Pointer, FString::Printf(TEXT("var:%s"), *Name));
		return true;
	}
	OutExpr.bIsLiteral = true;
	OutExpr.LiteralValue = Name;
	Ctx.Resolutions->SetStringField(Pointer, TEXT("literal:string"));
	return true;
}

static bool LowerCallExpression(const TSharedPtr<FJsonObject>& Obj, const FString& Pointer, FLowerCtx& Ctx, bool bAllowImpureCall, FExprResult& OutExpr, FString& OutErrorPointer, FString& OutError)
{
	FString FunctionPath;
	if (!Obj->TryGetStringField(TEXT("call"), FunctionPath) || FunctionPath.IsEmpty())
	{
		OutErrorPointer = Pointer;
		OutError = TEXT("'call' must be a non-empty function path.");
		return false;
	}
	FString ClassPath;
	FString FuncName;
	if (!SplitFunctionPath(FunctionPath, ClassPath, FuncName))
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("call"));
		OutError = FString::Printf(TEXT("Function path '%s' must be /Package.Class.Function form."), *FunctionPath);
		return false;
	}
	UFunction* Function = FindFunctionForNodeCommand(ClassPath, FuncName);
	if (!Function)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("call"));
		OutError = FString::Printf(TEXT("Could not find function '%s' on '%s'."), *FuncName, *ClassPath);
		return false;
	}
	const bool bPure = IsFunctionPure(Function);
	if (!bPure && !bAllowImpureCall)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("call"));
		OutError = FString::Printf(TEXT("'%s' is not BlueprintPure; hoist it into a statement before using its return value."), *FuncName);
		return false;
	}
	if (bPure && bAllowImpureCall)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("call"));
		OutError = FString::Printf(TEXT("'%s' is BlueprintPure; calling it in statement position is dead code. Use it inside an expression (e.g. 'if' condition or 'return' value)."), *FuncName);
		return false;
	}

	const FString NodeId = Ctx.NewId();
	TSharedRef<FJsonObject> Node = AddNode(Ctx, NodeId, TEXT("FunctionCall"));
	Node->SetStringField(TEXT("functionClass"), ClassPath);
	Node->SetStringField(TEXT("function"), FuncName);

	if (!bPure)
	{
		LinkFrontierAndAdvance(Ctx, NodeId);
	}

	const TSharedPtr<FJsonObject>* Args = nullptr;
	if (Obj->TryGetObjectField(TEXT("args"), Args) && Args != nullptr && Args->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Arg : (*Args)->Values)
		{
			FExprResult ArgExpr;
			const FString ArgPointer = ConcatPtr(Pointer, FString::Printf(TEXT("args.%s"), *Arg.Key));
			if (!LowerExpression(Arg.Value, ArgPointer, Ctx, false, ArgExpr, OutErrorPointer, OutError))
			{
				return false;
			}
			WireExprToPin(Ctx, ArgExpr, NodeId, Arg.Key);
		}
	}

	OutExpr.bIsLiteral = false;
	OutExpr.PinRef = NodeId + TEXT(".ReturnValue");
	return true;
}

static bool LowerExpression(const TSharedPtr<FJsonValue>& Value, const FString& Pointer, FLowerCtx& Ctx, bool bAllowImpureCall, FExprResult& OutExpr, FString& OutErrorPointer, FString& OutError)
{
	if (!Value.IsValid())
	{
		OutErrorPointer = Pointer;
		OutError = TEXT("Expression is null.");
		return false;
	}

	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (Value->TryGetObject(Obj) && Obj != nullptr && Obj->IsValid())
	{
		const TSharedPtr<FJsonObject>& Object = *Obj;

		if (Object->HasField(TEXT("var")))
		{
			FString Name;
			Object->TryGetStringField(TEXT("var"), Name);
			FEdGraphPinType VarType;
			if (Name.IsEmpty() || !Ctx.Blueprint || !TryGetBlueprintVariableType(Ctx.Blueprint, *Name, VarType))
			{
				OutErrorPointer = ConcatPtr(Pointer, TEXT("var"));
				OutError = FString::Printf(TEXT("Blueprint variable '%s' not found."), *Name);
				return false;
			}
			const FString NodeId = Ctx.NewId();
			TSharedRef<FJsonObject> Node = AddNode(Ctx, NodeId, TEXT("VariableGet"));
			Node->SetStringField(TEXT("variable"), Name);
			OutExpr.bIsLiteral = false;
			OutExpr.PinRef = FString::Printf(TEXT("%s.%s"), *NodeId, *Name);
			return true;
		}
		if (Object->HasField(TEXT("in")))
		{
			FString Name;
			Object->TryGetStringField(TEXT("in"), Name);
			if (Name.IsEmpty() || !Ctx.Inputs.Contains(Name))
			{
				OutErrorPointer = ConcatPtr(Pointer, TEXT("in"));
				OutError = FString::Printf(TEXT("Function input '%s' is not declared."), *Name);
				return false;
			}
			OutExpr.bIsLiteral = false;
			OutExpr.PinRef = FString::Printf(TEXT("entry.%s"), *Name);
			return true;
		}
		if (Object->HasField(TEXT("self")))
		{
			const FString NodeId = Ctx.NewId();
			AddNode(Ctx, NodeId, TEXT("Self"));
			OutExpr.bIsLiteral = false;
			OutExpr.PinRef = NodeId + TEXT(".self");
			return true;
		}
		if (Object->HasField(TEXT("lit")))
		{
			OutExpr.bIsLiteral = true;
			OutExpr.LiteralValue = JsonValueToLiteral(Object->TryGetField(TEXT("lit")));
			return true;
		}
		if (Object->HasField(TEXT("call")))
		{
			return LowerCallExpression(Object, Pointer, Ctx, bAllowImpureCall, OutExpr, OutErrorPointer, OutError);
		}

		OutErrorPointer = Pointer;
		OutError = TEXT("Unknown expression form. Expected one of: var, in, self, lit, call.");
		return false;
	}

	FString StringValue;
	if (Value->TryGetString(StringValue))
	{
		return ResolveBareString(StringValue, Pointer, Ctx, OutExpr);
	}
	bool BoolValue = false;
	if (Value->TryGetBool(BoolValue))
	{
		OutExpr.bIsLiteral = true;
		OutExpr.LiteralValue = BoolValue ? TEXT("true") : TEXT("false");
		Ctx.Resolutions->SetStringField(Pointer, TEXT("literal:bool"));
		return true;
	}
	double NumberValue = 0.0;
	if (Value->TryGetNumber(NumberValue))
	{
		OutExpr.bIsLiteral = true;
		OutExpr.LiteralValue = FormatNumberLiteral(NumberValue);
		Ctx.Resolutions->SetStringField(Pointer, TEXT("literal:number"));
		return true;
	}

	OutErrorPointer = Pointer;
	OutError = TEXT("Unsupported expression value type.");
	return false;
}

static bool LowerStatement(const TSharedPtr<FJsonValue>& StmtValue, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError)
{
	const TSharedPtr<FJsonObject>* StmtObj = nullptr;
	if (!StmtValue.IsValid() || !StmtValue->TryGetObject(StmtObj) || StmtObj == nullptr || !StmtObj->IsValid())
	{
		OutErrorPointer = Pointer;
		OutError = TEXT("Statement must be an object.");
		return false;
	}
	const TSharedPtr<FJsonObject>& Stmt = *StmtObj;

	if (Ctx.ExecFrontier.Num() == 0)
	{
		OutErrorPointer = Pointer;
		OutError = TEXT("Statement is unreachable; preceding control flow already returned.");
		return false;
	}

	if (Stmt->HasField(TEXT("return")))
	{
		const TSharedPtr<FJsonObject>* Outputs = nullptr;
		if (Stmt->TryGetObjectField(TEXT("return"), Outputs) && Outputs != nullptr && Outputs->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Out : (*Outputs)->Values)
			{
				FExprResult Expr;
				const FString OutPointer = ConcatPtr(Pointer, FString::Printf(TEXT("return.%s"), *Out.Key));
				if (!LowerExpression(Out.Value, OutPointer, Ctx, false, Expr, OutErrorPointer, OutError))
				{
					return false;
				}
				if (Expr.bIsLiteral)
				{
					AddDefault(Ctx, TEXT("result"), Out.Key, Expr.LiteralValue);
				}
				else
				{
					AddLink(Ctx, Expr.PinRef, FString::Printf(TEXT("result.%s"), *Out.Key));
				}
			}
		}
		LinkFrontier(Ctx, TEXT("result.execute"));
		return true;
	}

	if (Stmt->HasField(TEXT("seq")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Block = nullptr;
		if (!Stmt->TryGetArrayField(TEXT("seq"), Block) || Block == nullptr)
		{
			OutErrorPointer = ConcatPtr(Pointer, TEXT("seq"));
			OutError = TEXT("'seq' must be an array of statements.");
			return false;
		}
		return LowerBlock(*Block, ConcatPtr(Pointer, TEXT("seq")), Ctx, OutErrorPointer, OutError);
	}

	if (Stmt->HasField(TEXT("if")))
	{
		FExprResult Cond;
		if (!LowerExpression(Stmt->TryGetField(TEXT("if")), ConcatPtr(Pointer, TEXT("if")), Ctx, false, Cond, OutErrorPointer, OutError))
		{
			return false;
		}
		const FString BranchId = Ctx.NewId();
		AddNode(Ctx, BranchId, TEXT("Branch"));
		LinkFrontier(Ctx, BranchId + TEXT(".execute"));
		WireExprToPin(Ctx, Cond, BranchId, TEXT("Condition"));

		const TArray<TSharedPtr<FJsonValue>>* ThenBlock = nullptr;
		Stmt->TryGetArrayField(TEXT("then"), ThenBlock);
		Ctx.ExecFrontier.Reset();
		Ctx.ExecFrontier.Add(BranchId + TEXT(".then"));
		if (ThenBlock != nullptr)
		{
			if (!LowerBlock(*ThenBlock, ConcatPtr(Pointer, TEXT("then")), Ctx, OutErrorPointer, OutError))
			{
				return false;
			}
		}
		TArray<FString> ThenExits = Ctx.ExecFrontier;

		const TArray<TSharedPtr<FJsonValue>>* ElseBlock = nullptr;
		Stmt->TryGetArrayField(TEXT("else"), ElseBlock);
		Ctx.ExecFrontier.Reset();
		Ctx.ExecFrontier.Add(BranchId + TEXT(".else"));
		if (ElseBlock != nullptr)
		{
			if (!LowerBlock(*ElseBlock, ConcatPtr(Pointer, TEXT("else")), Ctx, OutErrorPointer, OutError))
			{
				return false;
			}
		}
		TArray<FString> ElseExits = Ctx.ExecFrontier;

		Ctx.ExecFrontier.Reset();
		Ctx.ExecFrontier.Append(ThenExits);
		Ctx.ExecFrontier.Append(ElseExits);
		return true;
	}

	if (Stmt->HasField(TEXT("call")))
	{
		FExprResult Discard;
		return LowerCallExpression(Stmt, Pointer, Ctx, true, Discard, OutErrorPointer, OutError);
	}

	if (Stmt->HasField(TEXT("set")))
	{
		FString VarName;
		Stmt->TryGetStringField(TEXT("set"), VarName);
		FEdGraphPinType VarType;
		if (VarName.IsEmpty() || !Ctx.Blueprint || !TryGetBlueprintVariableType(Ctx.Blueprint, *VarName, VarType))
		{
			OutErrorPointer = ConcatPtr(Pointer, TEXT("set"));
			OutError = FString::Printf(TEXT("Blueprint variable '%s' not found."), *VarName);
			return false;
		}
		const TSharedPtr<FJsonValue> ValueField = Stmt->TryGetField(TEXT("to"));
		if (!ValueField.IsValid())
		{
			OutErrorPointer = Pointer;
			OutError = TEXT("'set' requires 'to' (value expression).");
			return false;
		}
		FExprResult ValueExpr;
		if (!LowerExpression(ValueField, ConcatPtr(Pointer, TEXT("to")), Ctx, false, ValueExpr, OutErrorPointer, OutError))
		{
			return false;
		}
		const FString NodeId = Ctx.NewId();
		TSharedRef<FJsonObject> Node = AddNode(Ctx, NodeId, TEXT("VariableSet"));
		Node->SetStringField(TEXT("variable"), VarName);
		LinkFrontierAndAdvance(Ctx, NodeId);
		WireExprToPin(Ctx, ValueExpr, NodeId, VarName);
		return true;
	}

	OutErrorPointer = Pointer;
	OutError = TEXT("Unknown statement form. Expected one of: return, seq, if, call, set.");
	return false;
}

static bool LowerBlock(const TArray<TSharedPtr<FJsonValue>>& Block, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError)
{
	for (int32 Index = 0; Index < Block.Num(); ++Index)
	{
		const FString StmtPointer = FString::Printf(TEXT("%s[%d]"), *Pointer, Index);
		if (!LowerStatement(Block[Index], StmtPointer, Ctx, OutErrorPointer, OutError))
		{
			return false;
		}
	}
	return true;
}

} // anonymous

bool LowerSemanticFunctionIR(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	FSemanticLowerResult& OutResult,
	FString& OutErrorPointer,
	FString& OutError)
{
	FLowerCtx Ctx;
	Ctx.Blueprint = Blueprint;
	Ctx.ExecFrontier.Add(TEXT("entry.then"));

	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (Params.IsValid() && Params->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
		{
			const TSharedPtr<FJsonObject>* InputObj = nullptr;
			if (InputValue.IsValid() && InputValue->TryGetObject(InputObj) && InputObj != nullptr && InputObj->IsValid())
			{
				FString Name;
				if ((*InputObj)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
				{
					Ctx.Inputs.Add(Name);
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Flow = nullptr;
	if (Params.IsValid() && Params->TryGetArrayField(TEXT("flow"), Flow) && Flow != nullptr)
	{
		if (!LowerBlock(*Flow, TEXT("flow"), Ctx, OutErrorPointer, OutError))
		{
			return false;
		}
	}

	LinkFrontier(Ctx, TEXT("result.execute"));

	OutResult.Patch->SetArrayField(TEXT("nodes"), Ctx.Nodes);
	OutResult.Patch->SetArrayField(TEXT("links"), Ctx.Links);
	OutResult.Patch->SetArrayField(TEXT("defaults"), Ctx.Defaults);
	OutResult.Resolutions = Ctx.Resolutions;
	return true;
}

} // namespace BlueprintBridge
