// Copyright Odyssey Interactive. All Rights Reserved.

#include "BlueprintBridgeSemanticLowering.h"

#include "BlueprintBridgeCommandsPrivate.h"

namespace BlueprintBridge
{
namespace
{
struct FExprResult
{
	bool bIsLiteral = false;
	FString LiteralValue;
	FString PinRef;
	// Type-tracking (v2 — drives comparison/logical operator dispatch).
	FString TypeCategory;            // "int", "real", "bool", "name", "string", "struct", "object"
	FString TypeSubCategory;         // "float" / "double" for real; otherwise empty
	FString TypeSubCategoryObject;   // class/struct path when applicable
	FString TypeContainer;           // "None", "Array", "Set", "Map" when known
};

struct FScopeBinding
{
	FString Name;
	FExprResult Result;
};

struct FScopeFrame
{
	TArray<FScopeBinding> Bindings;
};

struct FLoopContext
{
	FString ContinueTarget;
	TArray<FString> BreakExits;
};

struct FLowerCtx
{
	UBlueprint* Blueprint = nullptr;
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<TSharedPtr<FJsonValue>> Links;
	TArray<TSharedPtr<FJsonValue>> Defaults;
	TSharedRef<FJsonObject> Resolutions = MakeShared<FJsonObject>();
	TSet<FString> Inputs;
	TMap<FString, FEdGraphPinType> InputTypes;
	TArray<FString> ExecFrontier;
	TArray<FScopeFrame> ScopeStack;
	TArray<FLoopContext> LoopStack;
	bool bStrictPureExpressions = false;
	int32 IdCounter = 0;

	FString NewId() { return FString::Printf(TEXT("n_%d"), ++IdCounter); }
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

static void LinkFrontierAndAdvance(FLowerCtx& Ctx, const FString& NodeId, const TCHAR* OutExecPin = TEXT("then"))
{
	LinkFrontier(Ctx, NodeId + TEXT(".execute"));
	Ctx.ExecFrontier.Add(FString::Printf(TEXT("%s.%s"), *NodeId, OutExecPin));
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

static const FScopeBinding* FindInScope(const FLowerCtx& Ctx, const FString& Name)
{
	for (int32 i = Ctx.ScopeStack.Num() - 1; i >= 0; --i)
	{
		for (const FScopeBinding& Binding : Ctx.ScopeStack[i].Bindings)
		{
			if (Binding.Name == Name)
			{
				return &Binding;
			}
		}
	}
	return nullptr;
}

static void FillTypeFromPinType(FExprResult& Out, const FEdGraphPinType& PinType)
{
	Out.TypeCategory = PinType.PinCategory.ToString();
	Out.TypeSubCategory = PinType.PinSubCategory.ToString();
	if (PinType.PinSubCategoryObject.IsValid())
	{
		Out.TypeSubCategoryObject = PinType.PinSubCategoryObject->GetPathName();
	}
	Out.TypeContainer = PinContainerTypeToString(PinType);
}

static void FillTypeFromUFunctionReturn(FExprResult& Out, UFunction* Function)
{
	if (!Function) { return; }
	FProperty* ReturnProp = Function->GetReturnProperty();
	if (!ReturnProp)
	{
		// First out-param is the conventional pseudo-return for impure UFunctions.
		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop && Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ReturnParm | CPF_ReferenceParm))
			{
				ReturnProp = Prop;
				break;
			}
		}
	}
	if (!ReturnProp) { return; }
	FEdGraphPinType PinType;
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (Schema && Schema->ConvertPropertyToPinType(ReturnProp, PinType))
	{
		FillTypeFromPinType(Out, PinType);
	}
}

static bool ResolveBareString(const FString& Name, const FString& Pointer, FLowerCtx& Ctx, FExprResult& OutExpr)
{
	// Scope stack (let bindings, future loop induction vars, cast results) is checked
	// before function inputs / member vars so inner bindings shadow outer scopes.
	if (const FScopeBinding* Bound = FindInScope(Ctx, Name))
	{
		OutExpr = Bound->Result;
		Ctx.Resolutions->SetStringField(Pointer, FString::Printf(TEXT("scope:%s"), *Name));
		return true;
	}
	if (Ctx.Inputs.Contains(Name))
	{
		OutExpr.bIsLiteral = false;
		OutExpr.PinRef = FString::Printf(TEXT("entry.%s"), *Name);
		if (const FEdGraphPinType* InputType = Ctx.InputTypes.Find(Name))
		{
			FillTypeFromPinType(OutExpr, *InputType);
		}
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
		FillTypeFromPinType(OutExpr, VarType);
		Ctx.Resolutions->SetStringField(Pointer, FString::Printf(TEXT("var:%s"), *Name));
		return true;
	}
	OutExpr.bIsLiteral = true;
	OutExpr.LiteralValue = Name;
	OutExpr.TypeCategory = TEXT("string");
	Ctx.Resolutions->SetStringField(Pointer, TEXT("literal:string"));
	return true;
}

// Pick the KismetMathLibrary function for a comparison operator on a given type pair.
// Returns empty string if the types aren't comparable with that operator.
static FString PickComparisonFunction(const FString& Op, const FString& TypeA, const FString& TypeB)
{
	auto IsNumeric = [](const FString& T) { return T == TEXT("int") || T == TEXT("real") || T == TEXT("byte"); };
	const bool bBothNumeric = IsNumeric(TypeA) && IsNumeric(TypeB);
	const bool bBothInt = TypeA == TEXT("int") && TypeB == TEXT("int");
	const bool bAnyFloat = bBothNumeric && (TypeA == TEXT("real") || TypeB == TEXT("real"));

	if (Op == TEXT("=="))
	{
		if (bBothInt) return TEXT("EqualEqual_IntInt");
		if (bAnyFloat) return TEXT("EqualEqual_FloatFloat");
		if (TypeA == TEXT("bool") && TypeB == TEXT("bool")) return TEXT("EqualEqual_BoolBool");
		if (TypeA == TEXT("name") && TypeB == TEXT("name")) return TEXT("EqualEqual_NameName");
		if (TypeA == TEXT("string") && TypeB == TEXT("string")) return TEXT("EqualEqual_StrStr");
		if (TypeA == TEXT("object") && TypeB == TEXT("object")) return TEXT("EqualEqual_ObjectObject");
	}
	else if (Op == TEXT("!="))
	{
		if (bBothInt) return TEXT("NotEqual_IntInt");
		if (bAnyFloat) return TEXT("NotEqual_FloatFloat");
		if (TypeA == TEXT("bool") && TypeB == TEXT("bool")) return TEXT("NotEqual_BoolBool");
		if (TypeA == TEXT("name") && TypeB == TEXT("name")) return TEXT("NotEqual_NameName");
		if (TypeA == TEXT("string") && TypeB == TEXT("string")) return TEXT("NotEqual_StrStr");
		if (TypeA == TEXT("object") && TypeB == TEXT("object")) return TEXT("NotEqual_ObjectObject");
	}
	else if (Op == TEXT("<"))
	{
		if (bBothInt) return TEXT("Less_IntInt");
		if (bAnyFloat) return TEXT("Less_FloatFloat");
	}
	else if (Op == TEXT("<="))
	{
		if (bBothInt) return TEXT("LessEqual_IntInt");
		if (bAnyFloat) return TEXT("LessEqual_FloatFloat");
	}
	else if (Op == TEXT(">"))
	{
		if (bBothInt) return TEXT("Greater_IntInt");
		if (bAnyFloat) return TEXT("Greater_FloatFloat");
	}
	else if (Op == TEXT(">="))
	{
		if (bBothInt) return TEXT("GreaterEqual_IntInt");
		if (bAnyFloat) return TEXT("GreaterEqual_FloatFloat");
	}
	return FString();
}

static bool LowerBinaryOperator(const FString& Op, const TArray<TSharedPtr<FJsonValue>>& Operands, const FString& Pointer, FLowerCtx& Ctx, FExprResult& OutExpr, FString& OutErrorPointer, FString& OutError)
{
	if (Operands.Num() != 2)
	{
		OutErrorPointer = Pointer;
		OutError = FString::Printf(TEXT("Operator '%s' requires exactly 2 operands, got %d."), *Op, Operands.Num());
		return false;
	}
	FExprResult A, B;
	if (!LowerExpression(Operands[0], FString::Printf(TEXT("%s[0]"), *Pointer), Ctx, false, A, OutErrorPointer, OutError)) { return false; }
	if (!LowerExpression(Operands[1], FString::Printf(TEXT("%s[1]"), *Pointer), Ctx, false, B, OutErrorPointer, OutError)) { return false; }
	// Literals on one side use the other side's type for dispatch.
	FString TypeA = A.TypeCategory.IsEmpty() ? B.TypeCategory : A.TypeCategory;
	FString TypeB = B.TypeCategory.IsEmpty() ? A.TypeCategory : B.TypeCategory;
	if (TypeA.IsEmpty() && TypeB.IsEmpty())
	{
		// Both literals — default to int.
		TypeA = TypeB = TEXT("int");
	}
	const FString FuncName = PickComparisonFunction(Op, TypeA, TypeB);
	if (FuncName.IsEmpty())
	{
		OutErrorPointer = Pointer;
		OutError = FString::Printf(TEXT("Operator '%s' does not support operand types ('%s', '%s')."), *Op, *TypeA, *TypeB);
		return false;
	}
	const FString NodeId = Ctx.NewId();
	TSharedRef<FJsonObject> Node = AddNode(Ctx, NodeId, TEXT("FunctionCall"));
	Node->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetMathLibrary"));
	Node->SetStringField(TEXT("function"), FuncName);
	WireExprToPin(Ctx, A, NodeId, TEXT("A"));
	WireExprToPin(Ctx, B, NodeId, TEXT("B"));
	OutExpr.bIsLiteral = false;
	OutExpr.PinRef = NodeId + TEXT(".ReturnValue");
	OutExpr.TypeCategory = TEXT("bool");
	Ctx.Resolutions->SetStringField(Pointer, FString::Printf(TEXT("op:%s"), *FuncName));
	return true;
}

static bool LowerLogicalOperator(const FString& Op, const TArray<TSharedPtr<FJsonValue>>& Operands, const FString& Pointer, FLowerCtx& Ctx, FExprResult& OutExpr, FString& OutErrorPointer, FString& OutError)
{
	const FString FuncName = (Op == TEXT("and")) ? TEXT("BooleanAND") : TEXT("BooleanOR");
	if (Operands.Num() < 2)
	{
		OutErrorPointer = Pointer;
		OutError = FString::Printf(TEXT("Operator '%s' requires at least 2 operands, got %d."), *Op, Operands.Num());
		return false;
	}
	// Lower all operands; chain left-to-right as (((a op b) op c) op d).
	TArray<FExprResult> Lowered;
	Lowered.Reserve(Operands.Num());
	for (int32 i = 0; i < Operands.Num(); ++i)
	{
		FExprResult R;
		if (!LowerExpression(Operands[i], FString::Printf(TEXT("%s[%d]"), *Pointer, i), Ctx, false, R, OutErrorPointer, OutError)) { return false; }
		if (!R.TypeCategory.IsEmpty() && R.TypeCategory != TEXT("bool"))
		{
			OutErrorPointer = FString::Printf(TEXT("%s[%d]"), *Pointer, i);
			OutError = FString::Printf(TEXT("Operator '%s' requires bool operands, got '%s'."), *Op, *R.TypeCategory);
			return false;
		}
		Lowered.Add(R);
	}
	FExprResult Acc = Lowered[0];
	for (int32 i = 1; i < Lowered.Num(); ++i)
	{
		const FString NodeId = Ctx.NewId();
		TSharedRef<FJsonObject> Node = AddNode(Ctx, NodeId, TEXT("FunctionCall"));
		Node->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetMathLibrary"));
		Node->SetStringField(TEXT("function"), FuncName);
		WireExprToPin(Ctx, Acc, NodeId, TEXT("A"));
		WireExprToPin(Ctx, Lowered[i], NodeId, TEXT("B"));
		Acc.bIsLiteral = false;
		Acc.PinRef = NodeId + TEXT(".ReturnValue");
		Acc.TypeCategory = TEXT("bool");
	}
	OutExpr = Acc;
	Ctx.Resolutions->SetStringField(Pointer, FString::Printf(TEXT("op:%s(x%d)"), *FuncName, Lowered.Num()));
	return true;
}

static bool LowerNotOperator(const TSharedPtr<FJsonValue>& Operand, const FString& Pointer, FLowerCtx& Ctx, FExprResult& OutExpr, FString& OutErrorPointer, FString& OutError)
{
	FExprResult Inner;
	if (!LowerExpression(Operand, Pointer, Ctx, false, Inner, OutErrorPointer, OutError)) { return false; }
	if (!Inner.TypeCategory.IsEmpty() && Inner.TypeCategory != TEXT("bool"))
	{
		OutErrorPointer = Pointer;
		OutError = FString::Printf(TEXT("Operator 'not' requires a bool operand, got '%s'."), *Inner.TypeCategory);
		return false;
	}
	const FString NodeId = Ctx.NewId();
	TSharedRef<FJsonObject> Node = AddNode(Ctx, NodeId, TEXT("FunctionCall"));
	Node->SetStringField(TEXT("functionClass"), TEXT("/Script/Engine.KismetMathLibrary"));
	Node->SetStringField(TEXT("function"), TEXT("Not_PreBool"));
	WireExprToPin(Ctx, Inner, NodeId, TEXT("A"));
	OutExpr.bIsLiteral = false;
	OutExpr.PinRef = NodeId + TEXT(".ReturnValue");
	OutExpr.TypeCategory = TEXT("bool");
	Ctx.Resolutions->SetStringField(Pointer, TEXT("op:Not_PreBool"));
	return true;
}

static FString GetDynamicCastResultPinName(UClass* TargetClass)
{
	return TargetClass ? UEdGraphSchema_K2::PN_CastedValuePrefix + TargetClass->GetDisplayNameText().ToString() : FString();
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
	// bAllowImpureCall == true means we're in statement position; false means expression position.
	// Statement-position pure calls are dead code (no exec consumer). Always reject.
	if (bPure && bAllowImpureCall)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("call"));
		OutError = FString::Printf(TEXT("'%s' is BlueprintPure; calling it in statement position is dead code. Use it inside an expression (e.g. 'if' condition or 'return' value)."), *FuncName);
		return false;
	}
	// Impure call in expression position: auto-hoist by default. The exec pump happens
	// via the frontier mechanism below — multiple references to the result pin reuse a
	// single node, so the side-effect runs once. Opt out with strictPureExpressions.
	if (!bPure && !bAllowImpureCall && Ctx.bStrictPureExpressions)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("call"));
		OutError = FString::Printf(TEXT("'%s' is not BlueprintPure; hoist it into a statement before using its return value. (strictPureExpressions is enabled — disable to auto-hoist.)"), *FuncName);
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
	FillTypeFromUFunctionReturn(OutExpr, Function);
	if (!bPure && !bAllowImpureCall)
	{
		Ctx.Resolutions->SetStringField(ConcatPtr(Pointer, TEXT("call")), TEXT("hoist:impure"));
	}
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
			FillTypeFromPinType(OutExpr, VarType);
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
			if (const FEdGraphPinType* InputType = Ctx.InputTypes.Find(Name))
			{
				FillTypeFromPinType(OutExpr, *InputType);
			}
			return true;
		}
		if (Object->HasField(TEXT("self")))
		{
			const FString NodeId = Ctx.NewId();
			AddNode(Ctx, NodeId, TEXT("Self"));
			OutExpr.bIsLiteral = false;
			OutExpr.PinRef = NodeId + TEXT(".self");
			OutExpr.TypeCategory = TEXT("object");
			return true;
		}
		if (Object->HasField(TEXT("lit")))
		{
			const TSharedPtr<FJsonValue> LitVal = Object->TryGetField(TEXT("lit"));
			OutExpr.bIsLiteral = true;
			OutExpr.LiteralValue = JsonValueToLiteral(LitVal);
			bool LB = false; double LN = 0.0;
			if (LitVal.IsValid())
			{
				if (LitVal->TryGetBool(LB)) { OutExpr.TypeCategory = TEXT("bool"); }
				else if (LitVal->TryGetNumber(LN))
				{
					OutExpr.TypeCategory = FMath::IsNearlyEqual(LN, FMath::RoundToDouble(LN)) ? TEXT("int") : TEXT("real");
				}
				else { OutExpr.TypeCategory = TEXT("string"); }
			}
			return true;
		}
		if (Object->HasField(TEXT("call")))
		{
			return LowerCallExpression(Object, Pointer, Ctx, bAllowImpureCall, OutExpr, OutErrorPointer, OutError);
		}
		// Comparison + logical operator sugar.
		for (const TCHAR* Op : { TEXT("=="), TEXT("!="), TEXT("<"), TEXT("<="), TEXT(">"), TEXT(">=") })
		{
			if (Object->HasField(Op))
			{
				const TArray<TSharedPtr<FJsonValue>>* Operands = nullptr;
				if (!Object->TryGetArrayField(Op, Operands) || !Operands)
				{
					OutErrorPointer = ConcatPtr(Pointer, Op);
					OutError = FString::Printf(TEXT("Operator '%s' must be an array of 2 operands."), Op);
					return false;
				}
				return LowerBinaryOperator(Op, *Operands, ConcatPtr(Pointer, Op), Ctx, OutExpr, OutErrorPointer, OutError);
			}
		}
		for (const TCHAR* Op : { TEXT("and"), TEXT("or") })
		{
			if (Object->HasField(Op))
			{
				const TArray<TSharedPtr<FJsonValue>>* Operands = nullptr;
				if (!Object->TryGetArrayField(Op, Operands) || !Operands)
				{
					OutErrorPointer = ConcatPtr(Pointer, Op);
					OutError = FString::Printf(TEXT("Operator '%s' must be an array of bool expressions."), Op);
					return false;
				}
				return LowerLogicalOperator(Op, *Operands, ConcatPtr(Pointer, Op), Ctx, OutExpr, OutErrorPointer, OutError);
			}
		}
		if (Object->HasField(TEXT("not")))
		{
			return LowerNotOperator(Object->TryGetField(TEXT("not")), ConcatPtr(Pointer, TEXT("not")), Ctx, OutExpr, OutErrorPointer, OutError);
		}

		OutErrorPointer = Pointer;
		OutError = TEXT("Unknown expression form. Expected one of: var, in, self, lit, call, ==, !=, <, <=, >, >=, and, or, not.");
		return false;
	}

	// Dispatch on the JSON type explicitly. FJsonValueNumber::TryGetString stringifies
	// the number, so a TryGetString-first ordering would misread the literal 5 as the
	// string "5" and resolve it through ResolveBareString (→ literal:string), which
	// breaks type-driven comparison dispatch.
	if (Value->Type == EJson::String)
	{
		FString StringValue;
		Value->TryGetString(StringValue);
		return ResolveBareString(StringValue, Pointer, Ctx, OutExpr);
	}
	if (Value->Type == EJson::Boolean)
	{
		bool BoolValue = false;
		Value->TryGetBool(BoolValue);
		OutExpr.bIsLiteral = true;
		OutExpr.LiteralValue = BoolValue ? TEXT("true") : TEXT("false");
		OutExpr.TypeCategory = TEXT("bool");
		Ctx.Resolutions->SetStringField(Pointer, TEXT("literal:bool"));
		return true;
	}
	if (Value->Type == EJson::Number)
	{
		double NumberValue = 0.0;
		Value->TryGetNumber(NumberValue);
		OutExpr.bIsLiteral = true;
		OutExpr.LiteralValue = FormatNumberLiteral(NumberValue);
		OutExpr.TypeCategory = FMath::IsNearlyEqual(NumberValue, FMath::RoundToDouble(NumberValue)) ? TEXT("int") : TEXT("real");
		Ctx.Resolutions->SetStringField(Pointer, TEXT("literal:number"));
		return true;
	}

	OutErrorPointer = Pointer;
	OutError = TEXT("Unsupported expression value type.");
	return false;
}

static FExprResult MakeLoopIndexResult(const FString& PinRef)
{
	FExprResult Result;
	Result.bIsLiteral = false;
	Result.PinRef = PinRef;
	Result.TypeCategory = TEXT("int");
	Result.TypeContainer = TEXT("None");
	return Result;
}

static bool LowerCastStatement(const TSharedPtr<FJsonObject>& Stmt, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError)
{
	FExprResult CastInput;
	if (!LowerExpression(Stmt->TryGetField(TEXT("cast")), ConcatPtr(Pointer, TEXT("cast")), Ctx, false, CastInput, OutErrorPointer, OutError))
	{
		return false;
	}
	if (CastInput.bIsLiteral)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("cast"));
		OutError = TEXT("'cast' input must resolve to an object expression, not a literal.");
		return false;
	}

	FString TargetClassPath;
	if (!Stmt->TryGetStringField(TEXT("to"), TargetClassPath) || TargetClassPath.IsEmpty())
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("to"));
		OutError = TEXT("'cast' requires non-empty 'to' class path.");
		return false;
	}
	UClass* TargetClass = LoadClassByPath(TargetClassPath);
	if (!TargetClass)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("to"));
		OutError = FString::Printf(TEXT("Could not load cast target class '%s'."), *TargetClassPath);
		return false;
	}

	FString BindingName;
	if (!Stmt->TryGetStringField(TEXT("as"), BindingName) || BindingName.IsEmpty())
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("as"));
		OutError = TEXT("'cast' requires non-empty 'as' binding name.");
		return false;
	}

	const FString CastId = Ctx.NewId();
	TSharedRef<FJsonObject> CastNode = AddNode(Ctx, CastId, TEXT("DynamicCast"));
	CastNode->SetStringField(TEXT("targetClass"), TargetClassPath);
	LinkFrontier(Ctx, CastId + TEXT(".execute"));
	WireExprToPin(Ctx, CastInput, CastId, UEdGraphSchema_K2::PN_ObjectToCast.ToString());

	FExprResult CastResult;
	CastResult.bIsLiteral = false;
	CastResult.PinRef = CastId + TEXT(".") + GetDynamicCastResultPinName(TargetClass);
	CastResult.TypeCategory = UEdGraphSchema_K2::PC_Object.ToString();
	CastResult.TypeSubCategoryObject = TargetClass->GetPathName();
	CastResult.TypeContainer = TEXT("None");

	Ctx.Resolutions->SetStringField(Pointer, FString::Printf(TEXT("cast:%s"), *TargetClassPath));

	const TArray<TSharedPtr<FJsonValue>>* ThenBlock = nullptr;
	Stmt->TryGetArrayField(TEXT("then"), ThenBlock);
	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(CastId + TEXT(".") + UEdGraphSchema_K2::PN_CastSucceeded.ToString());
	FScopeFrame ThenFrame;
	FScopeBinding Binding;
	Binding.Name = BindingName;
	Binding.Result = CastResult;
	ThenFrame.Bindings.Add(Binding);
	Ctx.ScopeStack.Add(MoveTemp(ThenFrame));
	if (ThenBlock != nullptr)
	{
		if (!LowerBlock(*ThenBlock, ConcatPtr(Pointer, TEXT("then")), Ctx, OutErrorPointer, OutError))
		{
			Ctx.ScopeStack.Pop();
			return false;
		}
	}
	Ctx.ScopeStack.Pop();
	TArray<FString> ThenExits = Ctx.ExecFrontier;

	const TArray<TSharedPtr<FJsonValue>>* ElseBlock = nullptr;
	Stmt->TryGetArrayField(TEXT("else"), ElseBlock);
	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(CastId + TEXT(".") + UEdGraphSchema_K2::PN_CastFailed.ToString());
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

static bool LowerForEachStatement(const TSharedPtr<FJsonObject>& Stmt, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError)
{
	FExprResult Iterable;
	if (!LowerExpression(Stmt->TryGetField(TEXT("forEach")), ConcatPtr(Pointer, TEXT("forEach")), Ctx, false, Iterable, OutErrorPointer, OutError))
	{
		return false;
	}
	if (Iterable.bIsLiteral)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("forEach"));
		OutError = TEXT("'forEach' iterable must resolve to an array expression, not a literal.");
		return false;
	}
	if (!Iterable.TypeContainer.IsEmpty() && Iterable.TypeContainer != TEXT("Array"))
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("forEach"));
		OutError = FString::Printf(TEXT("'forEach' iterable must be an array, got container '%s'."), *Iterable.TypeContainer);
		return false;
	}

	FString BindingName;
	if (!Stmt->TryGetStringField(TEXT("as"), BindingName) || BindingName.IsEmpty())
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("as"));
		OutError = TEXT("'forEach' requires non-empty 'as' binding name.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Body = nullptr;
	if (!Stmt->TryGetArrayField(TEXT("body"), Body) || !Body)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("body"));
		OutError = TEXT("'forEach' requires 'body' array.");
		return false;
	}

	const FString LoopId = Ctx.NewId();
	AddNode(Ctx, LoopId, TEXT("ForEachLoop"));
	LinkFrontier(Ctx, LoopId + TEXT(".execute"));
	WireExprToPin(Ctx, Iterable, LoopId, TEXT("Array"));

	FExprResult ElementResult = Iterable;
	ElementResult.bIsLiteral = false;
	ElementResult.PinRef = LoopId + TEXT(".Array Element");
	ElementResult.TypeContainer = TEXT("None");

	FScopeFrame BodyFrame;
	FScopeBinding ElementBinding;
	ElementBinding.Name = BindingName;
	ElementBinding.Result = ElementResult;
	BodyFrame.Bindings.Add(ElementBinding);
	FScopeBinding IndexBinding;
	IndexBinding.Name = BindingName + TEXT("_Index");
	IndexBinding.Result = MakeLoopIndexResult(LoopId + TEXT(".Array Index"));
	BodyFrame.Bindings.Add(IndexBinding);

	Ctx.Resolutions->SetStringField(Pointer, TEXT("loop:forEach"));
	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(LoopId + TEXT(".LoopBody"));
	Ctx.ScopeStack.Add(MoveTemp(BodyFrame));
	const bool bBodyOk = LowerBlock(*Body, ConcatPtr(Pointer, TEXT("body")), Ctx, OutErrorPointer, OutError);
	Ctx.ScopeStack.Pop();
	if (!bBodyOk)
	{
		return false;
	}

	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(LoopId + TEXT(".Completed"));
	return true;
}

static bool LowerWhileStatement(const TSharedPtr<FJsonObject>& Stmt, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError)
{
	FExprResult Condition;
	if (!LowerExpression(Stmt->TryGetField(TEXT("while")), ConcatPtr(Pointer, TEXT("while")), Ctx, false, Condition, OutErrorPointer, OutError))
	{
		return false;
	}
	if (!Condition.TypeCategory.IsEmpty() && Condition.TypeCategory != TEXT("bool"))
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("while"));
		OutError = FString::Printf(TEXT("'while' condition must be bool, got '%s'."), *Condition.TypeCategory);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Body = nullptr;
	if (!Stmt->TryGetArrayField(TEXT("body"), Body) || !Body)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("body"));
		OutError = TEXT("'while' requires 'body' array.");
		return false;
	}

	const FString LoopId = Ctx.NewId();
	AddNode(Ctx, LoopId, TEXT("WhileLoop"));
	LinkFrontier(Ctx, LoopId + TEXT(".execute"));
	WireExprToPin(Ctx, Condition, LoopId, TEXT("Condition"));

	FLoopContext LoopContext;
	LoopContext.ContinueTarget = LoopId + TEXT(".execute");
	Ctx.LoopStack.Add(MoveTemp(LoopContext));
	Ctx.Resolutions->SetStringField(Pointer, TEXT("loop:while"));

	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(LoopId + TEXT(".LoopBody"));
	const bool bBodyOk = LowerBlock(*Body, ConcatPtr(Pointer, TEXT("body")), Ctx, OutErrorPointer, OutError);
	FLoopContext FinishedLoop = Ctx.LoopStack.Last();
	Ctx.LoopStack.Pop();
	if (!bBodyOk)
	{
		return false;
	}

	LinkFrontier(Ctx, LoopId + TEXT(".execute"));
	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(LoopId + TEXT(".Completed"));
	Ctx.ExecFrontier.Append(FinishedLoop.BreakExits);
	return true;
}

static bool LowerForStatement(const TSharedPtr<FJsonObject>& Stmt, const FString& Pointer, FLowerCtx& Ctx, FString& OutErrorPointer, FString& OutError)
{
	const TSharedPtr<FJsonObject>* Range = nullptr;
	if (!Stmt->TryGetObjectField(TEXT("for"), Range) || !Range || !(*Range).IsValid())
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("for"));
		OutError = TEXT("'for' must be an object with 'from' and 'to' expressions.");
		return false;
	}
	const TSharedPtr<FJsonValue> FromField = (*Range)->TryGetField(TEXT("from"));
	const TSharedPtr<FJsonValue> ToField = (*Range)->TryGetField(TEXT("to"));
	if (!FromField.IsValid() || !ToField.IsValid())
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("for"));
		OutError = TEXT("'for' requires 'from' and 'to' expressions.");
		return false;
	}

	FExprResult FromExpr;
	if (!LowerExpression(FromField, ConcatPtr(Pointer, TEXT("for.from")), Ctx, false, FromExpr, OutErrorPointer, OutError))
	{
		return false;
	}
	FExprResult ToExpr;
	if (!LowerExpression(ToField, ConcatPtr(Pointer, TEXT("for.to")), Ctx, false, ToExpr, OutErrorPointer, OutError))
	{
		return false;
	}
	if (!FromExpr.TypeCategory.IsEmpty() && FromExpr.TypeCategory != TEXT("int"))
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("for.from"));
		OutError = FString::Printf(TEXT("'for.from' must be int, got '%s'."), *FromExpr.TypeCategory);
		return false;
	}
	if (!ToExpr.TypeCategory.IsEmpty() && ToExpr.TypeCategory != TEXT("int"))
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("for.to"));
		OutError = FString::Printf(TEXT("'for.to' must be int, got '%s'."), *ToExpr.TypeCategory);
		return false;
	}

	FString BindingName;
	if (!Stmt->TryGetStringField(TEXT("as"), BindingName) || BindingName.IsEmpty())
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("as"));
		OutError = TEXT("'for' requires non-empty 'as' binding name.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Body = nullptr;
	if (!Stmt->TryGetArrayField(TEXT("body"), Body) || !Body)
	{
		OutErrorPointer = ConcatPtr(Pointer, TEXT("body"));
		OutError = TEXT("'for' requires 'body' array.");
		return false;
	}

	const FString LoopId = Ctx.NewId();
	AddNode(Ctx, LoopId, TEXT("ForLoop"));
	LinkFrontier(Ctx, LoopId + TEXT(".execute"));
	WireExprToPin(Ctx, FromExpr, LoopId, TEXT("First Index"));
	WireExprToPin(Ctx, ToExpr, LoopId, TEXT("Last Index"));

	FScopeFrame BodyFrame;
	FScopeBinding IndexBinding;
	IndexBinding.Name = BindingName;
	IndexBinding.Result = MakeLoopIndexResult(LoopId + TEXT(".Index"));
	BodyFrame.Bindings.Add(IndexBinding);

	Ctx.Resolutions->SetStringField(Pointer, TEXT("loop:for"));
	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(LoopId + TEXT(".LoopBody"));
	Ctx.ScopeStack.Add(MoveTemp(BodyFrame));
	const bool bBodyOk = LowerBlock(*Body, ConcatPtr(Pointer, TEXT("body")), Ctx, OutErrorPointer, OutError);
	Ctx.ScopeStack.Pop();
	if (!bBodyOk)
	{
		return false;
	}

	Ctx.ExecFrontier.Reset();
	Ctx.ExecFrontier.Add(LoopId + TEXT(".Completed"));
	return true;
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

	if (Stmt->HasField(TEXT("cast")))
	{
		return LowerCastStatement(Stmt, Pointer, Ctx, OutErrorPointer, OutError);
	}

	if (Stmt->HasField(TEXT("forEach")))
	{
		return LowerForEachStatement(Stmt, Pointer, Ctx, OutErrorPointer, OutError);
	}

	if (Stmt->HasField(TEXT("for")))
	{
		return LowerForStatement(Stmt, Pointer, Ctx, OutErrorPointer, OutError);
	}

	if (Stmt->HasField(TEXT("while")))
	{
		return LowerWhileStatement(Stmt, Pointer, Ctx, OutErrorPointer, OutError);
	}

	if (Stmt->HasField(TEXT("break")))
	{
		if (Ctx.LoopStack.Num() == 0)
		{
			OutErrorPointer = ConcatPtr(Pointer, TEXT("break"));
			OutError = TEXT("'break' is only valid inside a loop body.");
			return false;
		}
		Ctx.LoopStack.Last().BreakExits.Append(Ctx.ExecFrontier);
		Ctx.ExecFrontier.Reset();
		Ctx.Resolutions->SetStringField(Pointer, TEXT("loop:break"));
		return true;
	}

	if (Stmt->HasField(TEXT("continue")))
	{
		if (Ctx.LoopStack.Num() == 0)
		{
			OutErrorPointer = ConcatPtr(Pointer, TEXT("continue"));
			OutError = TEXT("'continue' is only valid inside a loop body.");
			return false;
		}
		LinkFrontier(Ctx, Ctx.LoopStack.Last().ContinueTarget);
		Ctx.ExecFrontier.Reset();
		Ctx.Resolutions->SetStringField(Pointer, TEXT("loop:continue"));
		return true;
	}

	if (Stmt->HasField(TEXT("let")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!Stmt->TryGetArrayField(TEXT("let"), Bindings) || !Bindings)
		{
			OutErrorPointer = ConcatPtr(Pointer, TEXT("let"));
			OutError = TEXT("'let' must be an array of bindings.");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* InBlock = nullptr;
		if (!Stmt->TryGetArrayField(TEXT("in"), InBlock) || !InBlock)
		{
			OutErrorPointer = ConcatPtr(Pointer, TEXT("in"));
			OutError = TEXT("'let' requires 'in' (array of statements that uses the bindings).");
			return false;
		}

		FScopeFrame Frame;
		for (int32 i = 0; i < Bindings->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* BindingObj = nullptr;
			if (!(*Bindings)[i].IsValid() || !(*Bindings)[i]->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
			{
				OutErrorPointer = FString::Printf(TEXT("%s.let[%d]"), *Pointer, i);
				OutError = TEXT("Each 'let' binding must be {name, value}.");
				return false;
			}
			FString BindingName;
			if (!(*BindingObj)->TryGetStringField(TEXT("name"), BindingName) || BindingName.IsEmpty())
			{
				OutErrorPointer = FString::Printf(TEXT("%s.let[%d].name"), *Pointer, i);
				OutError = TEXT("'let' binding requires a non-empty 'name'.");
				return false;
			}
			const TSharedPtr<FJsonValue> ValueField = (*BindingObj)->TryGetField(TEXT("value"));
			if (!ValueField.IsValid())
			{
				OutErrorPointer = FString::Printf(TEXT("%s.let[%d].value"), *Pointer, i);
				OutError = FString::Printf(TEXT("'let' binding '%s' requires a 'value' expression."), *BindingName);
				return false;
			}
			FExprResult ValueExpr;
			const FString ValuePointer = FString::Printf(TEXT("%s.let[%d].value"), *Pointer, i);
			if (!LowerExpression(ValueField, ValuePointer, Ctx, false, ValueExpr, OutErrorPointer, OutError))
			{
				return false;
			}
			FScopeBinding Binding;
			Binding.Name = BindingName;
			Binding.Result = ValueExpr;
			Frame.Bindings.Add(MoveTemp(Binding));
		}
		Ctx.ScopeStack.Add(MoveTemp(Frame));
		const bool bBlockOk = LowerBlock(*InBlock, ConcatPtr(Pointer, TEXT("in")), Ctx, OutErrorPointer, OutError);
		Ctx.ScopeStack.Pop();
		return bBlockOk;
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
	OutError = TEXT("Unknown statement form. Expected one of: return, seq, if, call, cast, forEach, for, while, break, continue, set, let.");
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
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("strictPureExpressions"), Ctx.bStrictPureExpressions);
	}

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
					// Record the input's declared pin type for downstream type-driven dispatch
					// (comparison sugar, identifier resolution, etc.).
					FEdGraphPinType PinType;
					FString Category;
					if ((*InputObj)->TryGetStringField(TEXT("category"), Category) && !Category.IsEmpty())
					{
						PinType.PinCategory = *Category;
						FString SubCategory;
						if ((*InputObj)->TryGetStringField(TEXT("subCategory"), SubCategory))
						{
							PinType.PinSubCategory = *SubCategory;
						}
						FString SubCategoryObjectPath;
						if ((*InputObj)->TryGetStringField(TEXT("subCategoryObject"), SubCategoryObjectPath) && !SubCategoryObjectPath.IsEmpty())
						{
							PinType.PinSubCategoryObject = StaticLoadObject(UObject::StaticClass(), nullptr, *SubCategoryObjectPath);
						}
						FString ContainerError;
						ApplyPinContainerType(*InputObj, PinType, ContainerError);
						Ctx.InputTypes.Add(Name, PinType);
					}
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
