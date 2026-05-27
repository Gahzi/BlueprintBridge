# Semantic IR v2 — handoff for follow-up PRs

This document is a self-contained brief for an agent picking up the v2 work after the foundation PR (`let` + comparison/logical operator sugar + auto-hoist) and the subsequent `cast`/loop work. It assumes no prior session context.

## Current status snapshot

Shipped in the current stack:

- PR 2: `cast`
- PR 3: `forEach` / `for`
- PR 4: `while` / `break` / `continue`

Next queued construct: **PR 5 — `switch`**.

Latest compile validation succeeded with:

```text
D:/Odyssey/UGSBiscuit/Engine/Build/BatchFiles/Build.bat BiscuitEditor Win64 Development -Project="D:/Odyssey/UGSBiscuit/Projects/Biscuit/Biscuit.uproject" -WaitMutex
Result: Succeeded
```

Targeted automation was attempted for loop tests, but the editor commandlet hit unrelated project startup issues before useful test completion output was captured. Re-run targeted `BlueprintBridge.Blueprint.SemanticIR.*` automation once the editor/debugger environment is clean.

Known follow-ups before/while starting PR 5:

- Re-run targeted tests for `Cast`, `ForEach`, `For`, `While`, and `Loop` once commandlet startup is stable.
- Add an explicit `Loop.NestedBreak` regression test.
- Consider checking/reporting the return value of `ApplyPinContainerType(...)` when parsing semantic input pin container types.

## Quick orientation

**Files you'll touch most:**

- `Source/BlueprintBridgeEditor/Private/BlueprintBridgeSemanticLowering.cpp` — the IR lowering. All v2 work happens here.
- `Source/BlueprintBridgeEditor/Private/BlueprintBridgeSemanticLowering.h` — public surface; rarely needs changes.
- `Source/BlueprintBridgeEditor/Private/BlueprintBridgeGraphPatchCommands.cpp` — patch-surface node support. Extend when a new construct needs a K2 node type the patcher cannot yet spawn.
- `Source/BlueprintBridgeEditor/Private/BlueprintBridgeCommandRegistration.cpp` — schema descriptions for `LowerSemanticFunction` / `ApplySemanticFunction`. Update when you add a new IR construct.
- `Source/BlueprintBridgeEditor/Private/BlueprintBridgeEditorTests.cpp` — tests under `BlueprintBridge.Blueprint.SemanticIR.*`.
- `README.md` — Semantic IR section starts around line ~918. Add a subsection per new construct.

**Key types in `BlueprintBridgeSemanticLowering.cpp`:**

- `FExprResult` — the result of lowering an expression. Carries `PinRef` (a `"nodeId.PinName"` string), `bIsLiteral` + `LiteralValue`, `TypeCategory` / `TypeSubCategory` / `TypeSubCategoryObject` (for type-driven dispatch), and `TypeContainer` for array/container-aware lowering.
- `FScopeBinding` — `{Name, FExprResult}`. A single named binding in scope.
- `FScopeFrame` — a stack frame holding bindings. Push on entering a scoped construct, pop on exit.
- `FLoopContext` — loop-control state for the innermost active loop. Holds the `continue` target and collected `break` exits.
- `FLowerCtx` — the lowering state. Holds `ScopeStack`, `LoopStack`, `Inputs`, `InputTypes`, `ExecFrontier`, `bStrictPureExpressions`.

**Helpers you can reuse:**

- `FindInScope(Ctx, Name)` — walks `ScopeStack` top-down. Use this in any new resolver path.
- `FillTypeFromPinType` / `FillTypeFromUFunctionReturn` — populate `FExprResult` type fields from a UE pin type or a UFunction's return property, including `TypeContainer` when available.
- `LinkFrontier`, `LinkFrontierAndAdvance` — manage exec sequencing. Most loops/branches need these. `break` collects current frontier into the active loop context; `continue` links current frontier to the active loop's continue target.
- `AddNode`, `AddLink`, `AddDefault`, `WireExprToPin` — patch construction primitives.
- `LowerExpression`, `LowerStatement`, `LowerBlock` — the three recursive entry points. New constructs typically add cases inside these or get called from them.

## What's already shipped

The current IR supports these forms. Don't re-implement them.

### Statements
- `call` (impure), `set`, `if`/`then`/`else`, `seq`, `return`, **`let`**, **`cast`**, **`forEach`**, **`for`**, **`while`**, **`break`**, **`continue`**

### Expressions
- `var`, `in`, `self`, `lit`, `call`
- Operators: **`==` `!=` `<` `<=` `>` `>=`** (type-dispatched comparison), **`and` `or` `not`** (logical)

### Behaviors
- **Scope stack** (`FScopeFrame`) — checked by `ResolveBareString` before function inputs/member vars. Inner bindings shadow outer. `cast` result names and loop induction variables are scoped to their `then`/`body` blocks only.
- **Type tracking** on `FExprResult` — drives comparison-operator dispatch and array/container checks for `forEach`.
- **Auto-hoist** — impure calls in expression position sequence into exec automatically. Opt out per request with `"strictPureExpressions": true`.
- **Loop context stack** (`FLoopContext`) — lets `break`/`continue` target the innermost active loop without leaking control state across nested loops.
- **JSON type dispatch in `LowerExpression`** — explicit checks on `Value->Type` (`EJson::String` / `EJson::Boolean` / `EJson::Number`). Do NOT rely on `TryGetString` first — `FJsonValueNumber::TryGetString` stringifies numbers, which would misroute literals.

## Conventions every new PR should follow

1. **Add the construct to `LowerStatement` or `LowerExpression`**, branching on field name (`if (Stmt->HasField(TEXT("forEach"))) { ... }`). Keep the dispatch flat — early returns, no nested if-else trees.

2. **Push a scope frame** when introducing a new name binding (loop induction var, cast result name, switch fallthrough discriminant). Pop on block exit. Use `Ctx.ScopeStack.Add(MoveTemp(Frame));` and `Ctx.ScopeStack.Pop();`.

3. **Populate `Ctx.Resolutions`** with a meaningful entry per IR node so callers can audit. The key is the dotted JSON pointer (e.g. `"flow[0].forEach.body[2].args.X"`). The value is a short tag (`scope:Name`, `param:Name`, `var:Name`, `op:FuncName`, `hoist:impure`, etc.).

4. **Use the JSON-pointer style for error reporting**. `ConcatPtr(Pointer, "field")` for field access; `FString::Printf(TEXT("%s[%d]"), *Pointer, Index)` for arrays. The `pointer` field in errors locates the offending IR node for the caller.

5. **Type-driven dispatch** — when the lowered K2 node depends on operand types (e.g. comparison ops, switch discriminant), read `FExprResult.TypeCategory` rather than re-resolving. Populate it for any new expression form.

6. **Frontier discipline** — when a construct has multiple exit paths (cast then/else, branch then/else, loop body/completed), reset `Ctx.ExecFrontier` between paths and union the exits at the end. Pattern is in the existing `if` handler.

7. **README + schema description** — every new construct gets a subsection under "Semantic IR" in README.md and a mention in the schema descriptions in `BlueprintBridgeCommandRegistration.cpp` so `DescribeCommand` surfaces it.

8. **At least one test per new statement form, one per new expression form**. Test pattern is in `BlueprintBridgeEditorTests.cpp` under `SemanticIR.*` — use the `CountPatchNodes` helper to assert node counts in the lowered patch.

## Completed v2 follow-ups in this stack

### PR 2 — `cast` — shipped

**Goal:** Lower a `K2Node_DynamicCast` from the IR with a scoped cast-result name visible only in `then`.

**Status:** Implemented in `BlueprintBridgeSemanticLowering.cpp`, documented in `README.md`, and covered by Semantic IR tests.

**IR shape:**
```json
{"cast": <expr>, "to": "/Script/Biscuit.BiscuitCharacter", "as": "MyChar",
 "then": [<statements that can reference MyChar>],
 "else": [<statements>]}
```

**Lowering strategy:**
1. Lower the cast input expression.
2. Resolve `to` as a class path via `LoadClassByPath`. Error if missing or not a UClass.
3. Emit `DynamicCast` node (already in the patch surface — see `BlueprintBridgeGraphPatchCommands.cpp` for how the existing patch handler maps it).
4. Wire input → node's input pin.
5. Reset frontier to `<castNode>.then`; push a `FScopeFrame` with one binding: `{as, FExprResult pointing at "<castNode>.AsXxx"}`. Pop after `then` block.
6. Reset frontier to `<castNode>.CastFailed` for the `else` block.
7. Union exits.

**UE references:**
- `UK2Node_DynamicCast` — pin names are `Object` (input), `AsXxxClass` (output, where Xxx is the class name), `then` / `CastFailed` (exec).
- The pin name for the cast result is class-dependent. Either read it from the patch handler after the node is created (preferred — eliminates string-construction guesswork), or follow the K2 convention (`As<ClassName>` where `ClassName` is `Class->GetName()`).

**Tests added:**
- `Cast.Basic` — cast succeeds, `as` name resolves inside `then` only.
- `Cast.ScopePop` — `as` name not visible after the `then` block (should fall through to string literal).
- `Cast.ElseBranch` — `else` block runs when cast result name isn't referenced.

---

### PR 3 — `forEach` + `for` — shipped

**Goal:** Loop constructs with the induction variable scoped to the body.

**Status:** Implemented via standard macro patch nodes (`ForEachLoop`, `ForLoop`). `FExprResult::TypeContainer` and pin-container parsing were added so array inputs can be recognized for `forEach`.

**IR shapes:**
```json
{"forEach": "Targets", "as": "Target", "body": [<statements>]}
{"for": {"from": 0, "to": 5}, "as": "i", "body": [<statements>]}
```

**Implemented lowering strategy:**
1. Lower the iterable (`forEach`) or `from`/`to` (`for`).
2. Emit a macro patch node: `ForEachLoop` or `ForLoop`. The patch surface resolves these through `SpawnMacroPatchNode` and the standard `/Engine/EditorBlueprintResources/StandardMacros.StandardMacros` asset.
3. Wire `forEach` iterable to `Array`, or `for.from` / `for.to` to `First Index` / `Last Index`.
4. Push a scope frame with `{as → "<macroNode>.Array Element"}` and, for `forEach`, also `{"<as>_Index" → "<macroNode>.Array Index"}`. For `for`, push `{as → "<macroNode>.Index"}`. Lower `body` with the frame active, then pop.
5. Drop the body exit frontier after lowering the body and set downstream frontier to `<macroNode>.Completed`. The standard macro owns iteration; downstream statements should only continue from `Completed`.

**Implementation notes:**
- `forEach` rejects known non-array containers and rejects literal iterables. Unknown container remains allowed so legacy/partially typed expressions can still lower.
- `for.from` and `for.to` must be `int` when their type is known.
- Induction bindings reuse the iterable element type for `forEach` and use `int` for `for`.

**Tests added:**
- `ForEach.Basic` — body executes once per array element, induction var visible inside body.
- `For.Basic` — body executes (to - from) times, index var visible.
- `Loop.InductionVarScopePop` — induction var not visible after `body`.
- `Loop.NestedScopes` — inner loop's induction var shadows an outer let with the same name; outer name is restored after inner loop.

---

### PR 4 — `while` + `break` + `continue` — shipped

**Goal:** Condition-driven loops with early-exit / next-iter control. Requires loop context tracking so `break`/`continue` know which loop they target.

**Status:** Implemented via the standard `WhileLoop` macro patch node plus `FLoopContext` stack tracking in the lowering context. `BlueprintBridgeGraphPatchCommands.cpp` now recognizes `WhileLoop` patch nodes.

**IR shapes:**
```json
{"while": <bool expr>, "body": [<statements>]}
{"break": true}      // statement, valid only inside a loop body
{"continue": true}   // statement, valid only inside a loop body
```

**Lowering strategy:**

For `while`:
1. Lower the condition expression and require `bool` when the type is known.
2. Emit a `WhileLoop` macro patch node, wire the condition to `Condition`, and link the incoming exec frontier to `<loop>.execute`.
3. Push a `FLoopContext` where `ContinueTarget` is `<loop>.execute` and `BreakExits` starts empty.
4. Lower the body from `<loop>.LoopBody`, then pop the loop context.
5. Link normal body exits back to `<loop>.execute`, then set downstream frontier to `<loop>.Completed` plus any collected `BreakExits`.

For `break`:
- Read the top of `LoopStack`; append the current frontier to that context's `BreakExits`; clear the current frontier so later statements in the same block are unreachable.

For `continue`:
- Link current frontier to the loop context's `ContinueTarget`; clear the current frontier.

**Gotchas:**
- A `break` or `continue` outside any loop is a lowering error with a clear pointer.
- Nested loops: `break`/`continue` always target the innermost. The `LoopStack` top is the right target.
- After a `break` or `continue` inside a `then`/`else` block, the frontier is empty for the rest of that block. The existing unreachable-statement check should kick in automatically.

**Tests added:**
- `While.Basic` — emits one `WhileLoop` and records `loop:while` resolution.
- `Loop.Break` — break inside loop body exits to statements after the loop.
- `Loop.Continue` — continue links back to the loop test/execute target.
- `Loop.BreakOutsideLoop` — break outside any loop returns a `SemanticLoweringFailed` error.

**Recommended follow-up test:**
- `Loop.NestedBreak` — break inside inner loop only terminates inner; outer continues. The implementation uses the loop stack top and should support this, but the explicit regression test is still worth adding.

---

## Queued PRs in priority order

### PR 5 — `switch`

**Goal:** Multi-way dispatch on enum, integer, or string discriminants.

**IR shape:**
```json
{"switch": "ActionType",
 "cases": [
   {"value": "EAction::Attack", "body": [<statements>]},
   {"value": "EAction::Defend", "body": [<statements>]}
 ],
 "default": [<statements>]}
```

**Lowering strategy:**
1. Lower the discriminant. Read its `TypeCategory` to pick the K2 switch variant:
   - `byte` (with `TypeSubCategoryObject` set to a UEnum) → `K2Node_SwitchEnum`
   - `int` → `K2Node_SwitchInteger`
   - `string` → `K2Node_SwitchString`
   - Anything else → lowering error with pointer to `"switch"`.
2. Emit the switch node and wire the discriminant.
3. For each case: validate `value` parses as the discriminant's type (enum literal → enum tag; int → integer; string → string). Add a case pin to the switch node with that label. Push a fresh frontier from the case pin; lower the case body; collect exit frontiers.
4. Lower the `default` body (if present) from the switch's `Default` pin.
5. Union all case exits + default exits into the new frontier.

**Gotchas:**
- The K2 switch nodes use dynamic pins per case. The patch surface may need a new helper to add case pins by label — check `BlueprintBridgeGraphPatchCommands.cpp` for `AddEnumSwitchNode` / `AddSwitchStringNode` first. If they don't exist as patch primitives, add them.
- Case-value validation: for enums, parse `EAction::Attack` form into `Tag` against the resolved UEnum. For integers, `FCString::Atoi`. For strings, accept the raw string.
- Duplicate case values should be a lowering error.

**Tests:**
- `Switch.EnumBasic` — discriminant resolves to enum, two cases + default execute correctly.
- `Switch.IntegerBasic` — integer discriminant.
- `Switch.StringBasic` — string discriminant.
- `Switch.UnsupportedType` — discriminant with no matching switch variant errors with pointer.

**Effort:** 1 session. Mostly straightforward once you confirm the patch surface has the switch primitives.

---

### PR 6 — Local variable declarations

**Goal:** User-facing function-local variable declaration (different from internal `let` scoping). These persist on the function graph — visible in `DescribeBlueprint`'s function locals.

**IR shape:**
```json
{"local": "name", "type": "int", "value": <initial expr>}
```

**Lowering strategy:**
1. Call `FBlueprintEditorUtils::AddLocalVariable(Blueprint, FunctionGraph, FName, FEdGraphPinType, DefaultValueString)`.
2. If `value` is provided, emit a `VariableSet` node consuming the lowered initial expression. Wire into the exec frontier.
3. No scope-stack involvement — local variables are referenceable globally within the function via the existing `ResolveBareString` member-variable path (treat function locals as members for resolution purposes; investigate whether `TryGetBlueprintVariableType` already finds them or needs extension).

**Gotchas:**
- Local variables are scoped to the function graph in UE. The bridge's existing `TryGetBlueprintVariableType` looks at class members; it may not see function locals. You might need a new helper `TryGetFunctionLocalVariableType(Blueprint, FunctionName, VariableName, OutPinType)`.
- Conflict with existing variable names: if a local has the same name as a class member, the local should shadow. Document the resolution order: scope stack (let) → function locals → function inputs → class members → string literal.

**Tests:**
- `Local.Basic` — declare a local, initialize it, reference it elsewhere.
- `Local.ShadowsMember` — local with same name as a class member shadows.
- `Local.PersistsInDescribeBlueprint` — `DescribeBlueprint` shows the local in its function's locals.

**Effort:** ~0.5 session.

---

### PR 7 — Struct member assignment

**Goal:** Set a single member of a struct variable in one statement.

**IR shape:**
```json
{"setMember": "MyStruct", "member": "X", "to": <expr>}
```

Or possibly the dotted form `{"set": "MyStruct.X", "to": <expr>}`. Pick one — the explicit form is less ambiguous.

**Lowering strategy:**
1. Resolve `MyStruct` to a variable. Read its struct type via `FEdGraphPinType.PinSubCategoryObject` (must be a UScriptStruct).
2. Validate `member` is a real property on that struct.
3. Emit a `K2Node_SetFieldsInStruct` node (or `K2Node_VariableSet` with struct pin breakouts — check what the patch surface supports). Wire the struct's get → set; the member's value → the member pin.
4. Frontier advances through the set node.

**Gotchas:**
- UE's "Set Members in Struct" node supports multiple member assignments per node. v1 scope: single-member only.
- Some struct types have member-write restrictions (e.g. `bRPC` flags). The compile will catch these — don't pre-validate in the lowering.

**Tests:**
- `StructSet.Basic` — set one member of a Vector or simple struct.
- `StructSet.UnknownMember` — `member` that doesn't exist on the struct returns an error with pointer.
- `StructSet.NotAStruct` — `setMember` on a non-struct variable returns an error.

**Effort:** ~1 session.

---

### PR 8 — `bind` / `broadcast`

**Goal:** Delegate operations with signature validation at lowering time.

**IR shapes:**
```json
{"bind": "OnDamaged", "target": "self", "function": "HandleDamage"}
{"broadcast": "OnAttackHit", "args": {"Damage": 50.0}}
```

**Lowering strategy:**

For `bind`:
1. Resolve `target` and `OnDamaged` to a delegate property on the target.
2. Resolve `function` to a UFunction on the target's class.
3. Validate: bound function's signature matches the delegate's `DelegateSignature` UFunction. Compare parameter counts, types, and `bIsOutParam`/`bIsConst` flags. Mismatch → error with a clear pointer to the conflicting parameter.
4. Emit `K2Node_AddDelegate` node (or `K2Node_AssignDelegate` depending on the delegate kind — multicast vs. single-cast).

For `broadcast`:
1. Resolve the delegate property.
2. Emit `K2Node_CallDelegate` node.
3. Lower each `args` value and wire to the corresponding pin (delegate's param pins).

**Gotchas:**
- Multicast vs. single-cast delegates use different K2 node types. Detect via the property's class.
- Signature mismatch errors are the most useful thing this PR can deliver — invest in clear error messages with pointers like `"flow[0].bind.function.Damage"`.
- Component-bound delegates (`OnDamaged` on a child component) require resolving the target's component path first.

**Tests:**
- `Bind.Basic` — bind a self function to a self delegate, signatures match.
- `Bind.SignatureMismatch` — bind a function with wrong param count/types; lowering errors with pointer.
- `Broadcast.Basic` — broadcast with matching args.
- `Broadcast.MissingArg` — broadcast missing a required arg; lowering errors.

**Effort:** 1.5 sessions. Signature validation is the time sink.

---

## Worked example: how `let` lowers

If you're new to the codebase, trace this end-to-end before writing a new construct. It's the cleanest of the v2 constructs and exercises every piece of the machinery (scope stack, frontier, type tracking, resolver fallback).

**Input IR:**

```json
{"let": [{"name": "n", "value": "Hp"}],
 "in": [
   {"if": {"==": ["n", 5]}, "then": [{"return": {"Out": "n"}}], "else": [{"return": {"Out": "n"}}]}
 ]}
```

(`Hp` is a member variable of type `int`. The function declares `Out: int` as an output.)

**What happens, step by step:**

1. `LowerStatement` sees the `let` field, enters the `let` branch.
2. The binding's value `"Hp"` lowers via `LowerExpression` → `ResolveBareString("Hp")`.
   - Scope stack is empty.
   - `Hp` isn't a function input.
   - `TryGetBlueprintVariableType` finds `Hp` as a class member of type `int`.
   - Emits a `VariableGet` node (`n_1`), captures `PinRef = "n_1.Hp"`, `TypeCategory = "int"`.
   - Resolutions: `flow[0].let[0].value` → `"var:Hp"`.
3. `FScopeFrame` is built with `{Name: "n", Result: {PinRef: "n_1.Hp", TypeCategory: "int"}}` and pushed onto `Ctx.ScopeStack`.
4. `LowerBlock` recurses into `in` with the frame active.
5. `LowerStatement` sees the `if`. Lowers the condition.
6. The condition `{"==": ["n", 5]}` enters `LowerBinaryOperator`.
   - Operand `"n"` (JSON string) → `LowerExpression` → `ResolveBareString("n")`.
     - `FindInScope(Ctx, "n")` finds the binding. Returns `FExprResult{PinRef: "n_1.Hp", TypeCategory: "int"}`.
     - Resolutions: `flow[0].in[0].if.==[0]` → `"scope:n"`.
   - Operand `5` (JSON number) → `LowerExpression` → number branch → `FExprResult{bIsLiteral: true, LiteralValue: "5", TypeCategory: "int"}`.
   - `PickComparisonFunction("==", "int", "int")` returns `"EqualEqual_IntInt"`.
   - Emits `FunctionCall` node (`n_2`) with `functionClass = /Script/Engine.KismetMathLibrary`, `function = EqualEqual_IntInt`.
   - Adds links: `n_1.Hp → n_2.A`. Adds defaults: `n_2.B = "5"`.
   - Returns `FExprResult{PinRef: "n_2.ReturnValue", TypeCategory: "bool"}`.
7. Back in the `if` handler: emits `Branch` node (`n_3`), wires `n_2.ReturnValue → n_3.Condition`, links exec frontier `entry.then → n_3.execute`.
8. Frontier resets to `n_3.then`. Lower `then` block.
9. `then`'s `return` lowers `"n"` (resolves via scope → `n_1.Hp`), adds link `n_1.Hp → result.Out`, links frontier `n_3.then → result.execute`. Frontier is now empty (return clears it).
10. Frontier resets to `n_3.else`. Lower `else` block.
11. `else`'s `return` does the same — links `n_1.Hp → result.Out` (second link from the same pin; K2 allows fan-out), and `n_3.else → result.execute`.
12. After the `if`, scope frame pops. The `in` block ends. The `let` statement returns.
13. Outer `LowerSemanticFunctionIR` links remaining frontier (empty here — both return paths cleared it) to `result.execute`.

**Final patch:** 3 nodes (`n_1` VariableGet "Hp", `n_2` FunctionCall `EqualEqual_IntInt`, `n_3` Branch), with the appropriate links and defaults. The single `VariableGet` is referenced from both `then` and `else` returns — that's why `Let.MultipleUse` asserts exactly **1** `VariableGet`, not 2.

**What to take away:**

- A binding stores the lowered `FExprResult` *once*; references reuse the same pin via the scope stack. No node duplication.
- Type tracking propagates correctly through the operator dispatch (`scope:n` → `int`; literal `5` → `int`; together → `EqualEqual_IntInt`).
- Frontier discipline is the trickiest part of any new construct. Reset before each parallel exec path; union exits after.
- The resolutions map is your debugger when something resolves to an unexpected target. Every scoped name, var, input, literal, and hoist gets a tag.

When you start a new construct, mentally trace it through this same level of detail *before* writing code. Most lowering bugs come from frontier mismanagement or premature `FExprResult` reuse.

## Common gotchas the foundation PR surfaced

1. **JSON type dispatch.** `FJsonValueNumber::TryGetString` stringifies — always check `Value->Type` against `EJson::String` / `EJson::Number` / `EJson::Boolean` explicitly in `LowerExpression`. The fix is at the bottom of `LowerExpression`.

2. **Eager binding semantics.** `let` evaluates binding values when the `let` statement runs, not on first use. Document any new construct's evaluation timing in its README subsection.

3. **`bool` initializers in tests.** When a test reads an optional response field (`unchanged`, etc.), initialize the receiving variable to the value the test wants to assert in the "field absent" case. `bool bUnchanged = false` for "should NOT short-circuit" assertions.

4. **In-memory test BPs and asset registry.** `CreateTestBlueprint` creates BPs in-memory but does not save them to disk. The asset registry sees them via `FAssetRegistryModule::AssetCreated` but `LoadBlueprint` calls that need the `_C` class form can fail because that form requires the asset to be on disk. Test against the bare package path or `.Asset.Asset` form for reliability.

5. **K2 macro-instance support in the patch surface.** `ForLoop`, `ForEachLoop`, and `WhileLoop` are now supported patch node types in `BlueprintBridgeGraphPatchCommands.cpp`. Future macro-backed constructs should follow that same `SpawnMacroPatchNode` pattern.

## Build + test workflow

After any change:

```
D:/Odyssey/UGSBiscuit/Engine/Build/BatchFiles/Build.bat BiscuitEditor Win64 Development \
  -Project="D:/Odyssey/UGSBiscuit/Projects/Biscuit/Biscuit.uproject" -WaitMutex
```

Run just the IR tests:

```
D:/Odyssey/UGSBiscuit/Engine/Binaries/Win64/UnrealEditor-Cmd.exe \
  D:/Odyssey/UGSBiscuit/Projects/Biscuit/Biscuit.uproject \
  -unattended -nop4 -nosplash -NullRHI \
  -ExecCmds="Automation RunTests BlueprintBridge.Blueprint.SemanticIR; Quit" \
  -TestExit="Automation Test Queue Empty" \
  -ReportOutputPath="D:/Odyssey/UGSBiscuit/Projects/Biscuit/Saved/Automation/SemanticIR" -log
```

Editor must be closed for the build (the `-WaitMutex` flag waits on the build mutex, not the editor — close the editor first).

## Don't forget

- **Update the README's Roadmap entry** for Semantic IR v2 as each PR ships. Move the relevant bullet from "queued" to "shipped."
- **Update `MEMORY.md`** if you add a memory file documenting any new construct's behavior.
- **Build before stacking another PR.** Lowering bugs are hard to triage retroactively across multiple unbuilt changes. The current stack builds successfully, but targeted automation should be re-run once unrelated editor commandlet startup issues are cleared.

## File layout reminder

```
Projects/Biscuit/Plugins/BlueprintBridge/
├── Source/BlueprintBridgeEditor/Private/
│   ├── BlueprintBridgeSemanticLowering.cpp    ← all v2 IR work
│   ├── BlueprintBridgeSemanticLowering.h
│   ├── BlueprintBridgeGraphPatchCommands.cpp  ← extend if you need a new K2 node type in the patch surface
│   ├── BlueprintBridgeCommandRegistration.cpp ← update schemas
│   └── BlueprintBridgeEditorTests.cpp         ← add tests under SemanticIR.*
├── README.md                                   ← add a subsection per new construct
└── docs/
    ├── blueprint_bridge_commands.md            ← regenerable via GenerateCommandDocs
    └── semantic-ir-v2-handoff.md               ← this file
```
