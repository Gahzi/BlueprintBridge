# BlueprintBridge command quick reference

This is the short, agent-oriented reference for common BlueprintBridge JSON shapes and pitfalls. For the complete canonical schema, call `DescribeCommand` for a command or run `GenerateCommandDocs`. If this file and `DescribeCommand` disagree, trust `DescribeCommand` and update this file.

## Global rules

- On Windows, use the PowerShell helper or a direct named-pipe client. Do not send `/Game/...` package paths through Git Bash; path rewriting can corrupt them.
- Use `DescribeCommand` before scripting unfamiliar commands.
- Prefer `Batch` for multi-node edits. Use `rollbackOnFailure: true` when a batch should be canceled after the first failed child request.
- Use `DescribeGraph` after node creation if pin names, directions, or pure/impure behavior are uncertain.
- C++ `BlueprintPure` functions spawn pure call nodes with no `execute` / `then` pins. Const Blueprint-callable functions are often authored or exposed as pure nodes; verify with `DescribeFunction` or `DescribeGraph` before wiring exec.
- `EditUserDefinedPin` reconstructs the node and can reset the edited pin default when the type changes. For just ref/const flags, prefer `SetUserDefinedPinFlags`.
- Prefer `DuplicateFunctionGraph` when repeating an existing function pattern inside the same Blueprint.

## Protocol

### `DescribeCommand`

```json
{ "command": "DescribeCommand", "params": { "command": "AddVariableGetNode" } }
```

### `Batch`

```json
{
  "command": "Batch",
  "params": {
    "rollbackOnFailure": true,
    "requests": [
      { "command": "AddBranchNode", "params": { "asset": "/Game/Path/BP_Asset", "graph": "EventGraph", "x": 0, "y": 0 } }
    ]
  }
}
```

## Inspection

### `DescribeBlueprint`

Required params:
- `asset`

### `DescribeGraph`

Required params:
- `asset`
- `graph`

### `DescribeNode`

Required params:
- `asset`
- `graph`
- `node` — node GUID

### `FindNodes`

Required params:
- `asset`

Optional filters:
- `graph`
- `class`
- `title`
- `variable`

### `AnalyzeGraph`

Required params:
- `asset`
- `graph`

Reports simple exec reachability and classifies disconnected/orphaned nodes.

## Reflection

Reflection commands answer questions about Unreal classes/functions/properties before you mutate a Blueprint graph.

### `DescribeFunction`

Required params:
- `class` — e.g. `/Script/Engine.Actor`, or a Blueprint asset/class path
- `function`

Returns function flags, `isPureNode`, `hasExecPins`, metadata, and param pin types.

### `FindFunctions`

Required params:
- `class`

Optional params:
- `nameContains`
- `blueprintCallableOnly`
- `includeInherited`

### `DescribeProperty`

Required params:
- `class`
- `property`

### `DescribeDelegate`

Required params:
- `class`
- `delegate`

Returns delegate property metadata plus the signature params. Use this before creating/binding delegate-compatible Blueprint functions.

## Graph management

### `CreateFunctionGraph`

Required params:
- `asset`
- `function`

### `DuplicateFunctionGraph`

Required params:
- `asset`
- `sourceGraph`
- `newGraph`

Optional params:
- `renames` — exact name map applied to user-defined function pins and local/self variable references
- `pinRenames` — exact name map applied only to user-defined function pins
- `variableRenames` — exact name map applied only to local/self variable references
- `strictRenames` — fail if a requested rename is unmatched, collides, or targets a missing self variable
- `compile` — compile after duplication

```json
{
  "command": "DuplicateFunctionGraph",
  "params": {
    "asset": "/Game/Path/BP_Enemy",
    "sourceGraph": "ScoreCharge",
    "newGraph": "ScoreSlam",
    "strictRenames": true,
    "renames": {
      "ChargeConfig": "SlamConfig",
      "bCanCharge": "bCanSlam"
    }
  }
}
```

### `AddFunctionInput` / `AddFunctionOutput`

Required params:
- `asset`
- `graph`
- `name`
- plus either `category` or `sourceVariable`

Common type params:
- `category` — e.g. `bool`, `int`, `float`, `object`, `class`, `struct`
- `sourceVariable`
- `subCategory`
- `subCategoryObject`
- `containerType`: `None`, `Array`, `Set`
- `byRef`
- `isConst` (`const` is accepted as a legacy alias)

Do not use `bByRef` or `bIsConst`.

### `EditUserDefinedPin`

Required params:
- `asset`
- `graph`
- `node`
- `pin`

Optional params:
- `newName`
- type params listed above

### `SetUserDefinedPinFlags`

Required params:
- `asset`
- `graph`
- `node`
- `pin`

Optional params:
- `byRef`
- `isConst` (`const` legacy alias)

Use this for ref/const tweaks without re-specifying the full pin type.

## Variables

### `AddBlueprintVariable`

Required params:
- `asset`
- `name`
- `category`

Optional type/default params:
- `subCategory`
- `subCategoryObject`
- `containerType`
- `isArray`
- `defaultValue`

Optional flag params:
- `instanceEditable`
- `blueprintReadOnly`
- `exposeOnSpawn`
- `private`
- `categoryName`
- `tooltip`
- `replication`: `None`, `Replicated`, `RepNotify`
- `repNotifyFunc`

### `SetBlueprintVariableFlags`

Required params:
- `asset`
- `variable`

Same optional flag params as above. Important: this uses `variable`, not `name`.

### `AddVariableGetNode` / `AddVariableSetNode`

Required params:
- `asset`
- `graph`
- `variable`
- `x`
- `y`

Important: these use `variable`, not `name`.

## Node creation

### `AddFunctionCallNode`

Required params:
- `asset`
- `graph`
- `functionClass`
- `function`
- `x`
- `y`

Optional:
- `pinDefaults`

Call `DescribeFunction` first if you need to know whether the node will have exec pins.

### `AddMakeStructNode` / `AddBreakStructNode`

Required params:
- `asset`
- `graph`
- `struct`
- `x`
- `y`

Important: these use `struct`, not `structType`.

### Other common node commands

Most use the standard graph-edit base:
- `asset`
- `graph`
- `x`
- `y`
- optional `pinDefaults`

Examples:
- `AddBranchNode`
- `AddSequenceNode`
- `AddRerouteNode`
- `AddCommentNode`
- `AddEnumEqualityNode`
- `AddDynamicCastNode`
- `AddCustomEventNode`
- `AddEventNode`

## Pin editing

### `ConnectPins`

Required params:
- `asset`
- `graph`
- `fromNode`
- `fromPin`
- `toNode`
- `toPin`

### `SetPinDefault`

Required params:
- `asset`
- `graph`
- `node`
- `pin`
- `value`

### `BreakPinLinks`

Required params:
- `asset`
- `graph`
- `node`
- `pin`

### `MovePinLinks`

Required params:
- `asset`
- `graph`
- `fromNode`
- `fromPin`
- `toNode`
- `toPin`

## Asset operations

### `CompileBlueprint`

Required params:
- `asset`

### `SaveAsset`

Required params:
- `asset`

### `DuplicateAsset`

Required params:
- `sourceAsset`
- `destAsset`

### `CheckoutAsset`

Required params:
- `asset`

## Known param-name traps

- `AddVariableGetNode` / `AddVariableSetNode`: use `variable`, not `name`.
- `SetBlueprintVariableFlags`: use `variable`, not `name`.
- `AddMakeStructNode` / `AddBreakStructNode`: use `struct`, not `structType`.
- Function pin ref/const flags: use `byRef` and `isConst`; `const` remains a legacy alias.
- `AddBlueprintVariable` can set editor-facing flags during creation; a second call is only needed for later edits.
