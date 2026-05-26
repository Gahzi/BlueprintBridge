# BlueprintBridge

BlueprintBridge is an **Editor-only Unreal Engine plugin** that exposes a local JSON command bridge for inspecting and authoring Blueprint assets from external tools, scripts, or coding agents.

It runs inside `UnrealEditor.exe`, listens on a local Windows named pipe, executes requests through Unreal Editor APIs, and returns structured JSON responses. The goal is to automate Blueprint editing without direct `.uasset` mutation and without fragile screen/mouse automation.

```text
External tool / script / agent
    -> length-prefixed JSON over local named pipe
        -> BlueprintBridge editor plugin
            -> Unreal Editor APIs inspect/edit assets
        <- JSON response
    <- terminal/tool output
```

## Status

BlueprintBridge is functional and has been used to create and iterate on real Blueprint-only gameplay assets. It currently supports a broad set of Blueprint authoring primitives:

- editor/project information and ping checks
- Blueprint, graph, node, pin, variable, component, and widget-tree inspection
- Blueprint asset and Widget Blueprint asset creation
- Blueprint duplication, compile, save, and checkout helpers
- component/SCS editing, including common specialized component setters
- Blueprint variable creation, defaults, metadata, replication flags, and array/set container types
- event graph and function graph creation/editing
- node creation for variables, branches, sequences, functions, events, custom events, dynamic casts, spawn actor, macros, structs, delegates, timers, traces, arrays, math helpers, UMG runtime calls, and Create Widget
- pin linking, link movement, link breaking, pin defaults, type copying, safer pin lookup aliases, node movement, and node deletion
- declarative graph patches (`ApplyGraphPatch`/`ApplyFunctionPatch`) and a Semantic IR (`LowerSemanticFunction`/`ApplySemanticFunction`) that lowers high-level intent (`if`, `return`, pure/impure `call`, `set`, `seq`) into patches
- optional `fields` selector on every inspection command for trimmed responses (`["nodes.title"]`, `["variables.name"]`, etc.)
- `DescribeGraph` defaults to the compact summary shape (entry/result nodes, execution chains, function calls, branches, variable reads/writes); the previous per-node/per-pin dump is available as `DescribeGraphFull`
- one-shot asset description via `SummarizeBlueprint` — parent class, interfaces, variables, components, delegates, and per-graph summaries in a single response
- one-shot asset scaffolding via `CreateBlueprintFromSpec` — create a Blueprint and populate variables, components, and Semantic IR functions in one transaction with rollback on failure
- `ApplyAndFix` — apply Semantic IR, compile, and return compile diagnostics inline so the next turn has everything it needs to produce a fix
- `ResolveSymbol` — for any reflected C++ symbol, returns the module name, relative + absolute header path, doxygen comment, and a C++-style signature; falls back to `kind=blueprint` for BP-defined symbols
- `ReplaceSemanticFunction` — wipe an existing function's body and re-lower a new Semantic IR over it in one transaction, with rollback on lowering failure
- initial UMG widget-tree creation/editing and slot layout commands
- batched request execution
- command discovery through `ListCommands` and `DescribeCommand`
- descriptive input and selected output schemas for registered commands
- generated JSON/Markdown command schema documentation through `GenerateCommandDocs`
- optional request validation against command schemas
- automation coverage under the `BlueprintBridge` test namespace

The plugin is still evolving. Some Blueprint systems remain context-sensitive and may need additional purpose-built primitives.

## Platform support

Current support:

```text
Windows only
Unreal Editor only
```

The transport is a Windows named pipe. By default BlueprintBridge listens on:

```text
\\.\pipe\BlueprintBridge
```

The pipe name is configurable through editor user settings.

## Why this exists

Blueprint assets are binary editor-managed assets. Editing `.uasset` files directly from an external process is unsafe and likely to corrupt assets.

BlueprintBridge keeps Unreal in charge of its own data:

- assets are loaded through Unreal APIs
- `UBlueprint`, `UEdGraph`, `UEdGraphNode`, `UEdGraphPin`, component templates, and widget trees are edited in-editor
- transactions are used where practical
- packages are marked dirty
- Blueprints are compiled through Kismet editor utilities
- assets are saved through editor saving utilities

## Architecture

```text
BlueprintBridgeEditor module

Transport layer
    Windows named pipe server
    length-prefixed UTF-8 JSON messages

Protocol layer
    request object
    response object
    command name + params

Command registry
    stores command metadata, risk, schemas, and handlers
    dispatches command names to C++ handlers

Unreal operation layer
    asset loading
    Blueprint inspection
    graph/node/pin operations
    component/SCS operations
    widget-tree operations
    compile/save/source-control helpers

Command implementation files
    protocol
    inspection
    graph/node/pin editing
    components
    assets
    widgets
    variables
```

## Transport protocol

Messages are length-prefixed UTF-8 JSON:

```text
[4-byte little-endian uint32 payload length][UTF-8 JSON payload]
```

The plugin currently handles one connected client at a time. A client may send multiple framed requests over the same connection.

## Request and response format

### Request

```json
{
  "id": "request-id",
  "version": 1,
  "command": "DescribeBlueprint",
  "params": {
    "asset": "/Game/Example/BP_Example"
  }
}
```

### Success response

```json
{
  "id": "request-id",
  "ok": true,
  "result": {}
}
```

Some commands return a string result:

```json
{
  "id": "request-id",
  "ok": true,
  "result": "Pong"
}
```

### Error response

```json
{
  "id": "request-id",
  "ok": false,
  "error": {
    "code": "AssetNotFound",
    "message": "Could not load Blueprint '/Game/Example/BP_Missing'."
  }
}
```

## Configuration

Default editor user settings live in:

```text
Config/DefaultEditorPerProjectUserSettings.ini
```

Settings section:

```ini
[/Script/BlueprintBridgeEditor.BlueprintBridgeSettings]
bEnableServer=true
PipeName=BlueprintBridge
bStartInUnattended=false
bRequireAuthToken=false
AuthToken=
bValidateRequestsAgainstSchemas=false
MaxBatchSize=0
```

Settings:

- `bEnableServer` — starts the named-pipe server when the editor starts.
- `PipeName` — pipe name without the `\\.\pipe\` prefix.
- `bStartInUnattended` — allows the server to start in unattended editor runs.
- `bRequireAuthToken` — when `true`, every request must include `authToken`.
- `AuthToken` — expected token value when auth is enabled.
- `bValidateRequestsAgainstSchemas` — validates required fields and basic JSON types before running a command.
- `MaxBatchSize` — maximum requests in a `Batch`; `0` means unlimited.

If auth is enabled and a request omits `authToken` or sends the wrong token, BlueprintBridge returns an `Unauthorized` error.

Schema validation is off by default to preserve existing client behavior.

The legacy section is still read for compatibility:

```ini
[/Script/BlueprintBridgeEditor.BlueprintBridge]
```

## Installation

1. Copy the plugin folder into your Unreal project:

   ```text
   YourProject/Plugins/BlueprintBridge
   ```

2. Enable it in your `.uproject`:

   ```json
   {
     "Name": "BlueprintBridge",
     "Enabled": true,
     "TargetAllowList": ["Editor"],
     "PlatformAllowList": ["Win64"]
   }
   ```

3. Regenerate project files if needed.
4. Build your editor target.
5. Start `UnrealEditor.exe`.
6. Send a `Ping` request.

BlueprintBridge commands require a live Unreal Editor process. Headless `UnrealEditor-Cmd.exe` is useful for automation tests, but the named-pipe authoring bridge is served by the editor process.

## PowerShell client

This project includes a PowerShell helper script at:

```text
Tools/PowerShell/blueprintbridge.ps1
```

Example commands:

```powershell
blueprintbridge.ps1 ping
blueprintbridge.ps1 project
blueprintbridge.ps1 engine-version
blueprintbridge.ps1 describe-blueprint /Game/Example/BP_Example
blueprintbridge.ps1 describe-graph /Game/Example/BP_Example EventGraph
blueprintbridge.ps1 find-variable-references /Game/Example/BP_Example ExampleState
blueprintbridge.ps1 create-blueprint-asset /Game/Example/BP_NewAsset /Script/Engine.Actor
blueprintbridge.ps1 duplicate-asset /Game/Example/BP_Template /Game/Example/BP_NewAsset
```

Raw JSON requests are also supported:

```powershell
blueprintbridge.ps1 -RequestJson '{"id":"1","version":1,"command":"Ping","params":{}}'
```

If auth is enabled, pass `-AuthToken` or set:

```powershell
$env:BLUEPRINTBRIDGE_AUTH_TOKEN = "your-token"
blueprintbridge.ps1 ping
```

When calling from Git Bash/MSYS, disable path conversion so Unreal `/Game/...` paths are not rewritten:

```bash
MSYS_NO_PATHCONV=1 powershell.exe -ExecutionPolicy Bypass -File D:/Path/To/blueprintbridge.ps1 describe-blueprint /Game/Example/BP_Example
```

## Python client

A small direct named-pipe client is available at:

```text
Tools/Python/blueprintbridge_client.py
```

It is intended for scripted agents that want to avoid shell path rewriting and reuse helpers such as `describe_function`, `duplicate_function_graph`, and `batch`.

## Core command reference

Most graph commands use this common shape:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200
}
```

### Basic/editor info

- `Ping`
- `GetProjectName`
- `GetEngineVersion`

### Command discovery

#### `ListCommands`

Returns registered command names, descriptions, categories, and risk levels.

```json
{}
```

#### `DescribeCommand`

Returns metadata and schemas for one command.

```json
{
  "command": "DescribeBlueprint"
}
```

Command schemas are descriptive by default. If `bValidateRequestsAgainstSchemas` is enabled, BlueprintBridge checks required fields and basic JSON types before running a command. Unknown fields are currently allowed.

Selected stable read-only commands also expose descriptive `outputSchema` data: `Ping`, `ListCommands`, `DescribeCommand`, `DescribeBlueprint`, `DescribeGraph`, `DescribeNode`, and `FindNodes`.

#### `GenerateCommandDocs`

Generates command schema documentation from the runtime command registry. By default it writes both files under `Saved/BlueprintBridge`:

```json
{
  "format": "both",
  "directory": "D:/Optional/Output/Directory"
}
```

Supported `format` values are `both`, `json`, and `markdown`. The result includes generated file paths and `commandCount`.

### Batch execution

#### `Batch`

Executes a list of normal requests and returns per-request responses.

```json
{
  "rollbackOnFailure": true,
  "requests": [
    {
      "id": "describe",
      "version": 1,
      "command": "DescribeBlueprint",
      "params": { "asset": "/Game/Example/BP_Example" }
    }
  ]
}
```

`rollbackOnFailure` wraps the batch in an editor transaction and cancels it after the first failed child request.

### Blueprint and graph inspection

> **Breaking change (PR3):** `DescribeGraph` now returns the compact summary shape (`entryNodes`, `executionChains`, `functionCalls`, `branches`, `variables.reads/writes`, `warnings`, etc. — same shape as `SummarizeBlueprintGraph`). The previous per-node/per-pin dump is now available as **`DescribeGraphFull`**. Migration is a one-line rename for callers that need the verbose shape. `SummarizeBlueprintGraph` continues to work as an alias for the summary shape.

All inspection commands listed in this section (`DescribeBlueprint`, `DescribeGraph`, `DescribeGraphFull`, `DescribeNode`, `DescribeClass`, `DescribeFunction`, `DescribeComponents`, `DescribeWidgetTree`) accept an optional `fields` array that trims the response to just the keys you ask for. Field paths are dot-separated and walk arrays element-wise:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "fields": ["nodes.guid", "nodes.title", "nodes.links"]
}
```

Rules: missing or empty `fields` returns the full response (no behavior change for existing callers). Unknown paths are silently dropped — `"nodes.banana"` keeps the `nodes` array with empty objects inside, not an error. `"nodes"` by itself keeps the whole nodes subtree.

#### `DescribeBlueprint`

```json
{
  "asset": "/Game/Path/BP_Asset"
}
```

Returns parent class, variables, variable flags/replication metadata, variable container types, graph names, and widget-tree information for Widget Blueprints.

#### `DescribeGraph`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph"
}
```

Returns the compact semantic summary: entry/result nodes, execution chains, function calls, branches, variable reads/writes, disconnected nodes, and warnings. Same payload shape as `SummarizeBlueprintGraph` (which remains available as an alias).

#### `DescribeGraphFull`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph"
}
```

Returns the verbose dump: every node with every pin, pin types, container types, defaults, links, GUIDs, and positions. Use when you need per-node or per-pin detail (e.g. confirming a patch landed, walking node topology by hand). This is the shape `DescribeGraph` returned before PR3.

#### `DescribeNode`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "node": "NODE-GUID"
}
```

#### `FindNodes`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "OptionalGraphName",
  "class": "OptionalClassPathSubstring",
  "title": "OptionalTitleSubstring",
  "variable": "OptionalVariableName"
}
```

#### `FindVariableReferences`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "variable": "ExampleState"
}
```

#### `AnalyzeGraph`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "ScoreSlam"
}
```

Reports simple exec reachability plus disconnected/orphaned node classifications.

#### `SummarizeBlueprint`

```json
{
  "asset": "/Game/Path/BP_Asset"
}
```

Returns a one-shot description of an entire Blueprint asset: `kind`, `parentClass`, `parentBlueprint`, `interfaces`, `variables` (with flags), `components` (SCS tree), `delegates` (from the generated class), `graphs.event[]` / `graphs.functions[]` / `graphs.macros[]` (each entry has `name`, optional `signature` for functions/macros, and a `SummarizeBlueprintGraph`-shaped `summary`), plus `widgetTree` for Widget Blueprints.

Opt-out flags (all default `true`): `includeFunctionBodies`, `includeEventGraph`, `includeMacros`, `includeDelegates`, `includeWidgetTree`. Use these on large assets to trim the response further than the default summary already does.

Reserved for v2 (accepted but no-op today): `includeReflection`, `includeSubobjectProperties`, `includeParent`.

### Reflection

Reflection commands inspect Unreal metadata before graph mutation.

#### `DescribeFunction`

```json
{
  "class": "/Script/Engine.Actor",
  "function": "K2_GetActorLocation"
}
```

Returns function flags, `isPureNode`, `hasExecPins`, metadata, and reflected params with pin types/ref/const flags.

#### `FindFunctions`

```json
{
  "class": "/Script/Engine.Actor",
  "nameContains": "Location",
  "blueprintCallableOnly": true,
  "includeInherited": true
}
```

#### `DescribeClass`

```json
{
  "class": "/Script/Engine.Actor",
  "includeFunctions": true,
  "includeProperties": false,
  "includeDelegates": true
}
```

#### `DescribeProperty`

```json
{
  "class": "/Script/Engine.Actor",
  "property": "PrimaryActorTick"
}
```

#### `DescribeDelegate`

```json
{
  "class": "/Script/Engine.Actor",
  "delegate": "OnDestroyed"
}
```

Returns the delegate property and signature params for binding-compatible Blueprint function creation.

#### `CheckDelegateCompatibility`

```json
{
  "asset": "/Game/Path/BP_Enemy",
  "function": "OnTargetSelected",
  "delegateOwnerClass": "/Script/Biscuit.TargetingComponent",
  "delegate": "OnTargetSelected"
}
```

Use `asset` for Blueprint function graphs or `functionClass` for reflected C++/UFunction sources. The result reports `compatible`, expected/actual params, mismatches, and suggested `SetUserDefinedPinFlags` fixes for ref/const flag mismatches.

#### `FindReflectionSymbols`

```json
{
  "query": "GetTarget",
  "kinds": ["function", "property", "delegate", "class"],
  "blueprintCallableOnly": true,
  "includeInherited": true,
  "includeEngine": false,
  "includeProject": true,
  "maxResults": 50
}
```

Searches loaded reflection symbols so clients can discover exact owner classes and names before mutating graphs.

#### `ResolveSymbol`

Resolves a reflected C++ symbol to its source location and metadata. Useful for hybrid C++/BP investigations where the LLM needs to read the actual C++ behind a `UFUNCTION`/`UCLASS`/`UPROPERTY`/`UDELEGATE` without falling back to grep.

```json
{
  "kind": "function",
  "class": "/Script/Biscuit.TargetingComponent",
  "member": "OnTargetSelected"
}
```

`kind` is one of `class`, `function`, `property`, `delegate`. `member` is required for everything except `class`.

Response (C++-defined symbol):

```json
{
  "kind": "function",
  "owner": "/Script/Biscuit.TargetingComponent",
  "name": "OnTargetSelected",
  "module": "Biscuit",
  "headerPath": "Public/Targeting/TargetingComponent.h",
  "absoluteHeaderPath": "D:/Odyssey/UGSBiscuit/Source/Biscuit/Public/Targeting/TargetingComponent.h",
  "signature": "void OnTargetSelected(AActor* Target)",
  "comment": "/** Broadcast when a new target is selected. */",
  "metadata": { "..." : "..." }
}
```

Response (Blueprint-defined symbol — no C++ source):

```json
{
  "kind": "blueprint",
  "owner": "/Game/Path/BP_Asset",
  "name": "SomeBPFunction",
  "metadata": { "..." : "..." }
}
```

`absoluteHeaderPath` is best-effort: it's resolved for plugin and game-module sources but skipped for engine modules (their layout varies enough that resolving them reliably isn't worth the surface area). When omitted, the relative `headerPath` is still useful for grep-based follow-ups.

### Asset, source control, compile, save

#### `CreateBlueprintAsset`

```json
{
  "asset": "/Game/Path/BP_NewAsset",
  "parentClass": "/Script/Engine.Actor"
}
```

#### `CreateBlueprintFromSpec`

Creates a Blueprint and populates variables, components, and Semantic IR functions in one transactional call. Each `variables[]`/`components[]`/`functions[]` entry reuses the same shape as its single-step counterpart (`AddBlueprintVariable`, `AddComponent`, `ApplySemanticFunction`) minus the redundant `asset`/`function`-name fields.

```json
{
  "asset": "/Game/Abilities/BP_NewAbility",
  "parentClass": "/Script/Engine.Actor",
  "variables": [
    { "name": "Health", "category": "int", "defaultValue": "100" }
  ],
  "components": [
    { "name": "Hitbox", "componentClass": "/Script/Engine.BoxComponent", "root": true }
  ],
  "functions": [
    {
      "name": "Constant",
      "outputs": [{ "name": "Out", "category": "int" }],
      "flow": [ { "return": { "Out": 42 } } ]
    }
  ],
  "compile": true
}
```

`rollbackOnFailure` defaults true — if any step fails (bad variable type, missing component class, lowering error, etc.) the whole creation is canceled and the asset never materializes. Errors carry a `phase` field (`asset`, `variables[N]`, `components[N]`, `functions[N]`) plus a `rolledBack` boolean so callers can pinpoint where the spec broke down.

#### `CreateWidgetBlueprintAsset`

```json
{
  "asset": "/Game/Path/WBP_NewWidget",
  "parentClass": "/Script/UMG.UserWidget"
}
```

`parentClass` is optional and defaults to `UserWidget`.

#### `DuplicateAsset`

```json
{
  "sourceAsset": "/Game/Example/BP_Template",
  "destAsset": "/Game/Example/BP_NewAsset"
}
```

#### `CheckoutAsset`

Checks out an asset through Unreal source-control helpers.

#### `CompileBlueprint`

Compiles a Blueprint and returns structured status fields:

```json
{
  "status": "UpToDate",
  "success": true,
  "errorCount": 0,
  "warningCount": 0,
  "messages": []
}
```

`messages` contains Blueprint compiler diagnostics when available. Each diagnostic includes `severity` and `message`, plus optional graph context such as `graph`, `nodeGuid`, `nodeName`, `pinId`, and `pinName` when Unreal attaches graph tokens to the compiler message.

#### `SaveAsset`

Saves an asset package.

### Blueprint variables and defaults

#### `AddBlueprintVariable`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "name": "SomeBool",
  "category": "bool",
  "defaultValue": "false"
}
```

For object, class, enum, and struct variables, provide `subCategoryObject`:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "name": "TargetActor",
  "category": "object",
  "subCategoryObject": "/Script/Engine.Actor"
}
```

Container variables are supported with `containerType`:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "name": "Targets",
  "category": "object",
  "subCategoryObject": "/Script/Engine.Actor",
  "containerType": "Array"
}
```

Supported container values:

- `None`
- `Array`
- `Set`

`Map` is currently rejected.

Legacy `isArray: true` is also accepted.

`AddBlueprintVariable` also accepts the same editor-facing flag fields as `SetBlueprintVariableFlags` (`instanceEditable`, `blueprintReadOnly`, `exposeOnSpawn`, `private`, `categoryName`, `tooltip`, `replication`, and `repNotifyFunc`) so common variables can be created and exposed in one request.

#### `SetBlueprintVariableFlags`

Sets Blueprint variable metadata and replication flags.

```json
{
  "asset": "/Game/Path/BP_Asset",
  "variable": "SomeValue",
  "instanceEditable": true,
  "blueprintReadOnly": false,
  "exposeOnSpawn": true,
  "private": false,
  "categoryName": "Config",
  "tooltip": "Shown in editor",
  "replication": "RepNotify",
  "repNotifyFunc": "OnRep_SomeValue"
}
```

`replication` supports:

- `None`
- `Replicated`
- `RepNotify`

#### `SetBlueprintDefault`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "property": "SomeProperty",
  "value": "SomeImportedTextValue"
}
```

### Component/SCS editing

#### `DescribeComponents`

```json
{
  "asset": "/Game/Path/BP_Actor"
}
```

Returns SCS component names, classes, parent names, root status, and template paths.

#### `AddComponent`

```json
{
  "asset": "/Game/Path/BP_Actor",
  "name": "MyComponent",
  "componentClass": "/Script/Engine.SceneComponent",
  "parent": "OptionalParentComponentName"
}
```

#### `AttachComponent`

```json
{
  "asset": "/Game/Path/BP_Actor",
  "name": "ChildComponent",
  "parent": "ParentComponent"
}
```

#### `SetRootComponent`

```json
{
  "asset": "/Game/Path/BP_Actor",
  "name": "RootComponentName"
}
```

#### `SetComponentTransform`

```json
{
  "asset": "/Game/Path/BP_Actor",
  "name": "MySceneComponent",
  "location": { "x": 0, "y": 0, "z": 100 },
  "rotation": { "pitch": 0, "yaw": 90, "roll": 0 },
  "scale": { "x": 1, "y": 1, "z": 1 }
}
```

#### `SetComponentProperty`

Imports a text value into a property on a component template.

```json
{
  "asset": "/Game/Path/BP_Actor",
  "name": "MyComponent",
  "property": "bHiddenInGame",
  "value": "true"
}
```

#### Specialized component setters

- `SetStaticMesh`
- `SetCollisionProfileName`
- `SetBoxExtent`
- `SetGenerateOverlapEvents`

Example:

```json
{
  "asset": "/Game/Path/BP_Actor",
  "name": "CollisionBox",
  "extent": { "x": 100, "y": 100, "z": 50 }
}
```

`SetCollisionProfileName` uses `profile`:

```json
{
  "asset": "/Game/Path/BP_Actor",
  "name": "CollisionBox",
  "profile": "OverlapAllDynamic"
}
```

### Graph management

- `CreateFunctionGraph`
- `DuplicateFunctionGraph`
- `CreateEventGraph`
- `DeleteGraph`
- `RenameGraph`
- `AddFunctionInput`
- `AddFunctionOutput`
- `EditUserDefinedPin`
- `SetUserDefinedPinFlags`
- `RenameCustomEvent`
- `AddVariableGetterFunction`
- `ApplyGraphPatch`
- `ApplyFunctionPatch`
- `LowerSemanticFunction`
- `ApplySemanticFunction`

Function input/output pin type params use the same `category`, `subCategory`, `subCategoryObject`, and `containerType` fields as variables. Function pins also accept `byRef` and `isConst` (`const` is still accepted as a legacy alias).

`DuplicateFunctionGraph` clones an existing function graph inside a Blueprint and can optionally apply exact name maps to duplicated function pins and variable references:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "sourceGraph": "ScoreRangedAttack",
  "newGraph": "ScoreSlam",
  "renames": {
    "RangedAttackConfig": "SlamConfig",
    "bCanRangedAttack": "bCanSlam"
  }
}
```

Use `pinRenames` or `variableRenames` when you only want one side of that behavior. `renames` applies to both user-defined function pins and local/self variable references. Set `strictRenames: true` to fail on unmatched rename keys, rename collisions, or missing self-variable targets; the result includes applied/unmatched/collision reports for non-strict runs.

Use `SetUserDefinedPinFlags` for simple `byRef` / `isConst` changes without re-specifying the whole pin type.

`ApplyGraphPatch` applies node creation, defaults, and links in one request. Nodes get temporary ids that links/defaults can reference with `nodeId.pinName`; function graphs also expose virtual `entry` and `result` ids when those nodes exist.

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "ScoreSlam",
  "rollbackOnFailure": true,
  "nodes": [
    { "id": "branch", "type": "Branch", "x": 400, "y": 0 },
    { "id": "score", "type": "FunctionCall", "functionClass": "/Script/Biscuit.ScoreLibrary", "function": "ComputeSlamScore", "x": 700, "y": 0 }
  ],
  "defaults": [
    { "node": "branch", "pin": "Condition", "value": "true" }
  ],
  "links": [
    { "from": "branch.then", "to": "score.execute" }
  ]
}
```

Supported v1 node types include `Branch`, `Sequence`, `Reroute`, `Comment`, `VariableGet`, `VariableSet`, `FunctionCall`, `Self`, `DynamicCast`, `MakeStruct`, `BreakStruct`, and `CustomEvent`.

#### `LowerSemanticFunction` / `ApplySemanticFunction`

A higher-level layer above `ApplyFunctionPatch`. The caller writes a function signature plus a `flow` tree of intent — `call`, `set`, `if`/`then`/`else`, `seq`, `return` — and BlueprintBridge lowers it to the same node/link JSON `ApplyGraphPatch` accepts. The lowering threads exec pins, detects pure vs impure UFunctions via reflection, and resolves bare identifiers in order (function input → member variable → literal) with the chosen resolution reported back so the caller can audit.

`LowerSemanticFunction` is a pure dry-run: it returns the patch JSON plus a `resolutions` map without mutating the asset. `ApplySemanticFunction` does the same lowering and then applies it through `ApplyFunctionPatch` (including optional `createIfMissing` and `compile`). The `resolutions` map is included in the success response in both cases.

```json
{
  "asset": "/Game/Abilities/BP_SlamAbility",
  "function": "ScoreSlam",
  "createIfMissing": true,
  "compile": true,
  "inputs":  [{"name": "Context", "category": "struct", "subCategoryObject": "/Script/Biscuit.AbilityScoreContext", "byRef": true, "isConst": true}],
  "outputs": [{"name": "Score",   "category": "float"}],
  "flow": [
    {
      "if": {"call": "/Script/Biscuit.ScoreLibrary.CanSlam", "args": {"Context": "Context"}},
      "then": [{"return": {"Score": {"call": "/Script/Biscuit.ScoreLibrary.ComputeSlamScore", "args": {"Context": "Context", "Config": "SlamConfig"}}}}],
      "else": [{"return": {"Score": 0.0}}]
    }
  ]
}
```

v1 statement forms: `call` (impure), `set`, `if`/`then`/`else`, `seq`, `return`. v1 expression forms: `var`, `in`, `self`, `lit`, `call` (pure). Function references use the full UFunction path (`/Script/Package.Class.Function`). Calling a pure function in statement position or an impure function in expression position is a lowering error and the response carries a `pointer` field locating the offending IR node (e.g. `flow[0].if.args.Context`). Loops, `cast`, `switch`, `let` bindings, and delegate bind/broadcast are scheduled for v2.

#### `ReplaceSemanticFunction`

Rewrites an existing function body from a new Semantic IR in one transaction. Wipes every non-entry/non-result node in the function graph, then re-lowers the new IR over the preserved entry/result nodes. For whole-function rewrites — use `ApplyGraphPatch` with `existingGuid` refs for surgical edits to a subset of nodes.

```json
{
  "asset": "/Game/Path/BP_Asset",
  "function": "ScoreSlam",
  "flow": [ /* new IR — replaces the entire current body */ ],
  "compile": true
}
```

`inputs` and `outputs` are optional. If omitted, the existing function signature is preserved and only the body is replaced. If provided, they merge with the existing signature via `ApplyFunctionPatch`'s pin-merge.

The function must already exist — use `ApplySemanticFunction` with `createIfMissing: true` if you need creation. On lowering failure (bad function path, unresolved variable, etc.) the transaction cancels and the original body is restored.

#### `ApplyAndFix`

`ApplyAndFix` collapses the apply→compile→inspect-errors round-trip loop. It accepts the same params as `ApplySemanticFunction`, applies the IR, compiles the Blueprint, and returns the compile diagnostics in the same response — so when a turn produces a broken IR, the next turn already has everything it needs to write a correction.

```json
{
  "asset": "/Game/Path/BP_Asset",
  "function": "ScoreSlam",
  "createIfMissing": true,
  "inputs":  [...],
  "outputs": [...],
  "flow":    [...],
  "rollbackOnCompileError": false
}
```

On clean compile: success response with `compileStatus: "UpToDate"`, the `resolutions` map, and warning count. On compile errors: `ok: false`, `error.code: "CompileFailed"`, plus `error.messages` (the structured diagnostics — same shape as `CompileBlueprint` produces), `error.appliedPatch` (the lowered patch JSON so the caller can correlate diagnostics back to IR locations), `error.resolutions`, `error.compileStatus`, and `error.rolledBack` (`true` only when `rollbackOnCompileError: true`).

Default `rollbackOnCompileError: false` — leaving the broken state in place is usually more useful than reverting, since the caller can inspect the actual node graph to understand what failed.

### Node creation

Common graph nodes:

- `AddVariableGetNode`
- `AddVariableSetNode`
- `AddBranchNode`
- `AddSequenceNode`
- `AddRerouteNode`
- `AddCommentNode`
- `AddEnumEqualityNode`
- `AddEnumSwitchNode`
- `AddFunctionCallNode`
- `AddCustomEventNode`
- `AddEventNode`
- `AddDynamicCastNode`
- `AddSelfNode`

Gameplay/control-flow helpers:

- `AddSpawnActorNode`
- `AddComponentEventNode`
- `AddEventDispatcher`
- `AddDelegateBindNode`
- `AddDelegateBroadcastNode`
- `AddCreateDelegateNode`
- `SetCreateDelegateFunction`
- `AddForLoopNode`
- `AddForEachLoopNode`
- `AddAuthoritySwitchNode`
- `AddMakeStructNode`
- `AddBreakStructNode`
- `AddTimerNode`
- `AddLineTraceNode`
- `AddArrayFunctionNode`
- `AddMathNode`

UMG/runtime helpers:

- `AddCreateWidgetNode`
- `AddWidgetFunctionNode`

Many node creation commands support `pinDefaults`:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200,
  "functionClass": "/Script/Engine.KismetMathLibrary",
  "function": "Add_IntInt",
  "pinDefaults": {
    "B": "1"
  }
}
```

#### `AddSpawnActorNode`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200,
  "actorClass": "/Game/Path/BP_Spawned.BP_Spawned_C",
  "pinDefaults": {
    "CollisionHandlingOverride": "AlwaysSpawn"
  }
}
```

#### `AddTimerNode`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200,
  "operation": "SetByFunctionName",
  "pinDefaults": {
    "FunctionName": "TickSpawn",
    "Time": "1.0",
    "bLooping": "true"
  }
}
```

Supported timer operations:

- `SetByEvent`
- `SetByFunctionName`
- `ClearByHandle`
- `ClearAndInvalidateByHandle`

#### `AddArrayFunctionNode`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200,
  "operation": "Length"
}
```

Supported operations:

- `Add`
- `AddUnique`
- `Remove`
- `RemoveItem`
- `Clear`
- `Length`
- `Get`
- `Contains`

#### Delegate binding pattern

For context-sensitive `Create Event` nodes, create the node first, connect it to the delegate signature, then set its function:

```json
{
  "command": "AddCreateDelegateNode",
  "params": {
    "asset": "/Game/Path/BP_Asset",
    "graph": "EventGraph",
    "x": 600,
    "y": 200
  }
}
```

Connect:

```text
CreateEvent.OutputDelegate -> BindEvent.Delegate
```

Then call:

```json
{
  "command": "SetCreateDelegateFunction",
  "params": {
    "asset": "/Game/Path/BP_Asset",
    "graph": "EventGraph",
    "node": "CREATE-DELEGATE-NODE-GUID",
    "function": "HandleEvent"
  }
}
```

This order mirrors Unreal’s context-sensitive delegate validation and avoids invalid `Create Event` nodes.

### Pin and node editing

#### `ConnectPins`

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "fromNode": "SOURCE-NODE-GUID",
  "fromPin": "then",
  "toNode": "TARGET-NODE-GUID",
  "toPin": "execute"
}
```

Pin lookup is direction-aware and accepts normalized aliases for common cases such as:

- `False` -> `else`
- `True` -> `then`
- `Exec` -> `execute`
- whitespace/underscore-insensitive names such as `Other Actor` vs `OtherActor`

Other editing commands:

- `MovePinLinks`
- `BreakPinLinks`
- `SetPinDefault`
- `CopyPinType`
- `SetNodePosition`
- `DeleteNode`

### Widget Blueprints and UMG

#### `DescribeWidgetTree`

```json
{
  "asset": "/Game/Path/WBP_Widget"
}
```

#### `AddWidget`

```json
{
  "asset": "/Game/Path/WBP_Widget",
  "name": "RootCanvas",
  "widgetClass": "/Script/UMG.CanvasPanel",
  "root": true
}
```

To add to a panel:

```json
{
  "asset": "/Game/Path/WBP_Widget",
  "name": "TitleText",
  "widgetClass": "/Script/UMG.TextBlock",
  "parent": "RootCanvas"
}
```

#### `SetRootWidget`

```json
{
  "asset": "/Game/Path/WBP_Widget",
  "widget": "RootCanvas"
}
```

#### `AddWidgetToParent`

```json
{
  "asset": "/Game/Path/WBP_Widget",
  "parent": "RootCanvas",
  "child": "TitleText"
}
```

#### `SetWidgetSlotLayout`

Supports Canvas slot position, size, anchors, alignment, and HorizontalBox/VerticalBox slot padding.

```json
{
  "asset": "/Game/Path/WBP_Widget",
  "widget": "TitleText",
  "position": { "x": 24, "y": 36 },
  "size": { "x": 320, "y": 48 },
  "alignment": { "x": 0.5, "y": 0.5 },
  "anchors": {
    "minimumX": 0.5,
    "minimumY": 0.0,
    "maximumX": 0.5,
    "maximumY": 0.0
  }
}
```

## Examples

### Create an array variable

```json
{
  "id": "1",
  "version": 1,
  "command": "AddBlueprintVariable",
  "params": {
    "asset": "/Game/Example/BP_Example",
    "name": "SpawnVolumes",
    "category": "object",
    "subCategoryObject": "/Game/Example/BP_SpawnVolume.BP_SpawnVolume_C",
    "containerType": "Array"
  }
}
```

### Create a function getter

```json
{
  "id": "1",
  "version": 1,
  "command": "CreateFunctionGraph",
  "params": {
    "asset": "/Game/BP_Test",
    "function": "GetExampleState"
  }
}
```

```json
{
  "id": "2",
  "version": 1,
  "command": "AddFunctionOutput",
  "params": {
    "asset": "/Game/BP_Test",
    "graph": "GetExampleState",
    "name": "ExampleState",
    "sourceVariable": "ExampleState"
  }
}
```

```json
{
  "id": "3",
  "version": 1,
  "command": "AddVariableGetNode",
  "params": {
    "asset": "/Game/BP_Test",
    "graph": "GetExampleState",
    "variable": "ExampleState",
    "x": 32,
    "y": 112
  }
}
```

Then use `DescribeGraph` to find the result node GUID and connect pins with `ConnectPins`.

## Tips for scripted clients

External clients (agents, scripts, CI jobs) drive BlueprintBridge through opaque JSON. The habits below keep round-trips low and avoid recurring dead ends. For exact common request shapes, see [`docs/blueprint_bridge_commands.md`](docs/blueprint_bridge_commands.md).

### Cost-aware inspection (especially for AI agents)

Inspection responses can be large — a `DescribeGraphFull` on a 30-node function easily exceeds 90 KB, and most of that bulk is per-pin metadata the caller never reads. For agents on a token budget, response size is the dominant cost, not request count. Reach for the cheapest command that answers your question and escalate only when stuck:

1. **`SummarizeBlueprint`** — start here when you don't know the asset. One round-trip for parent class, interfaces, variables, components, delegates, and per-graph summaries.
2. **`DescribeGraph`** (compact) — for "what's in this function." Returns entry/result nodes, execution chains, function calls, branches, and per-variable reads/writes. Roughly 5% the size of `DescribeGraphFull` and usually enough to identify a suspect node.
3. **`DescribeSubgraph`** — when you have a seed node (or a few) and want only its neighborhood. Pass `seeds` (node GUIDs) and an optional `hops` limit. This is the right tool for "what feeds into this `RotateAngleAxis`?" — not `DescribeGraphFull`.
4. **`GetConnectedNodes`** / **`DescribeNode`** — single-node detail. Use after `DescribeGraph` has named the suspect.
5. **`DescribeGraphFull`** — last resort. The full per-node, per-pin dump. Almost always pair with `fields` (below).

**Use the `fields` selector.** Most inspection commands — `SummarizeBlueprint`, `DescribeBlueprint`, `DescribeGraph`, `DescribeGraphFull`, `DescribeNode`, `DescribeSubgraph`, `GetConnectedNodes`, `DescribeComponents`, and several `Describe*` reflection commands — accept an optional `fields` array of JSON path projections, returning only the listed paths. A `DescribeGraphFull` with `fields: ["nodes.title", "nodes.pins.name", "nodes.pins.linkedTo"]` is typically 10–20% the size of the unfiltered response and still answers "what connects to what." This is the single largest cost win on heavy graphs. If a response comes back unfiltered, the command may not yet route through `ApplyFieldSelection` — fall back to a cheaper command or open a ticket.

**Use `ListCommands` / `DescribeCommand` for discovery.** The plugin exposes 80+ commands; do not rely on a hand-maintained list in a wrapper or a memory note. `ListCommands` enumerates everything currently registered, and `DescribeCommand <name>` returns the input and output schemas. Both are cheap.

**Don't widen scope speculatively.** When diagnosing a specific function, a `DescribeGraph` plus a targeted `DescribeSubgraph` is normally enough. Pulling the caller's full graph "just to verify what gets passed in" typically doubles the cost and rarely changes the diagnosis — trust the function-under-inspection's signature and only widen if the in-function evidence is genuinely inconclusive.

### Resolve schemas before scripting, not by guessing

The shortest path to a correct `params` shape is `DescribeCommand`, the generated reference produced by `GenerateCommandDocs`, or the checked-in quick reference in `docs/blueprint_bridge_commands.md`. Param names are not always obvious from the command name. Common mismatches that have bitten clients:

- `AddVariableGetNode` / `AddVariableSetNode` take `variable`, not `name`.
- `AddMakeStructNode` / `AddBreakStructNode` take `struct`, not `structType`.
- `SetBlueprintVariableFlags` takes `variable`, not `name`.
- Function pin params accept `byRef` and `isConst` (`const` is accepted as a legacy alias; avoid `bByRef` / `bIsConst`). These matter when the function will be bound to a `DECLARE_DYNAMIC_DELEGATE_*` that includes ref or const-ref params — getting them wrong causes the Blueprint compiler to insert a `CREATEDELEGATE_PROXYFUNCTION_N` thunk that silently drops out-params.

### Confirm pure vs impure before wiring exec

A `UFUNCTION(BlueprintCallable)` declared `const` on a C++ method is generally rendered as a pure node — no `execute` / `then` pins — even without `BlueprintPure`. Wiring exec to such a node fails with `PinNotFound`. Before assuming a `CallFunction` node sits in the exec chain, either read the C++ `UFUNCTION` declaration or call `DescribeGraph` after `AddFunctionCallNode` and inspect the produced pins.

### Do not cargo-cult orphan nodes from a reference graph

When mirroring an existing graph as a template, trace execute and data reachability before copying every node. Reference graphs sometimes contain dead branches (e.g., a `Set X` whose `execute` is unconnected, left over after a refactor). They compile cleanly but do nothing; faithfully replicating them produces a graph that *looks* right and silently misbehaves.

### Create variables with flags in one call

`AddBlueprintVariable` accepts `instanceEditable`, replication, category, tooltip, and related flag fields directly. `SetBlueprintVariableFlags` remains available for later changes to existing variables.

### Verify pin defaults after authoring

`AddFunctionOutput` creates the return-node pin but does not populate its default value. If the function relies on a non-empty default (a description string, `bOutIsValid=true`, etc.), follow up with `SetPinDefault`. Blueprint compiles fine with empty defaults — verification requires a `DescribeGraph` spot-check.

`EditUserDefinedPin` reconstructs the owning node and resets the edited pin's stored `PinDefaultValue` when the type changes. Re-apply defaults afterward, and expect nearby reroute nodes to need a `DescribeGraph` check if the pin topology changed.

### Use `Batch` for multi-node assembly

When building a graph with many nodes and connections, prefer `Batch` over a serial stream of requests. A serial failure mid-stream leaves orphan nodes in the graph that you have to find and `DeleteNode` before retrying. `Batch` plus a single retry is the cleaner default for non-trivial graph construction.

### PowerShell helper compatibility

On Windows, invoke BlueprintBridge through the PowerShell helper or a direct named-pipe client, not Bash. Git-for-Windows Bash can rewrite Unreal package paths like `/Game/...` into host file-system paths before the bridge sees them.

`-RequestJson` / `-RequestFile` use `ConvertFrom-Json -AsHashtable`, which requires PowerShell 7+. On Windows PowerShell 5.1, either use the positional command forms (`blueprintbridge.ps1 describe-blueprint /Game/...`) or write a small client that frames the request bytes directly over the named pipe.

### Reuse rather than rebuild when the pattern repeats

If you find yourself constructing a near-identical function graph for the Nth time (typical for per-ability scoring functions, per-state handlers, etc.), prefer `DuplicateFunctionGraph` with `renames` for functions within the same Blueprint, or `DuplicateAsset` on a template Blueprint when the whole asset is reusable. Rebuilding 15+ nodes via individual node and connection calls is the slowest path.

## Automation tests

BlueprintBridge includes Unreal automation tests under the `BlueprintBridge` namespace.

Recommended command:

```bash
D:/Path/To/Engine/Binaries/Win64/UnrealEditor-Cmd.exe \
  D:/Path/To/YourProject.uproject \
  -unattended \
  -nop4 \
  -nosplash \
  -NullRHI \
  -NoRestoreOpenAssetTabs \
  -ExecCmds="Automation RunTests BlueprintBridge; Quit" \
  -TestExit="Automation Test Queue Empty" \
  -ReportOutputPath="D:/Path/To/YourProject/Saved/Automation/BlueprintBridge_All" \
  -log
```

`-NoRestoreOpenAssetTabs` is recommended for commandlet runs because restoring editor asset tabs under `-NullRHI` can crash unrelated to the tests.

Current test areas include:

- protocol basics, auth, command discovery, schemas, and schema validation
- Blueprint inspection and variable references
- graph/node/pin editing
- function graph commands
- custom events and event graphs
- component editing and specialized setters
- asset lifecycle/defaults
- gameplay graph primitives
- variable flags and container types
- control-flow helpers
- extended graph helpers such as arrays, timers, traces, delegates, math, self, and batch requests
- UMG widget-tree commands
- asset command error handling

## Development workflow

When changing plugin C++:

1. Build the editor target.
2. Fully restart Unreal Editor before named-pipe authoring tests. A running editor keeps the old plugin DLL and old command registry.
3. Test `Ping`.
4. Exercise the new command with a small throwaway asset.
5. Run the full `BlueprintBridge` automation suite before considering the milestone complete.

A full editor restart is preferred over Live Coding because the plugin owns long-running named-pipe server state.

## Security notes

BlueprintBridge can modify and save editor assets. Treat access to the pipe as trusted local automation.

Recommended hardening for broader distribution:

- operation logging
- dry-run support for destructive commands
- command allowlist/denylist settings
- explicit save policy
- stronger auth if exposed beyond local trusted tooling

Do not paste GitHub credentials, tokens, or passwords into AI chats or logs. Use GitHub CLI/browser auth locally or a credential manager.

## Known limitations

- Windows-only named pipe transport.
- Editor-only; not a runtime plugin.
- One client at a time.
- Auth is simple shared-token auth, not a full security model.
- Blueprint compiler message collection currently returns a structured placeholder and can be made richer.
- Some Blueprint node families are highly context-sensitive and may still require specialized commands.
- UMG support covers initial widget-tree/layout/runtime helpers but not the full UMG authoring surface.
- Animation Blueprints, Materials, Niagara, Behavior Trees, and State Trees are not covered as separate graph systems yet.
- Command implementations are split into domain `.cpp` files, with shared private helpers declared in `BlueprintBridgeCommandsPrivate.h`.

## Open issues from real use

Concrete bugs and rough edges observed while dogfooding the bridge. Each item is self-contained — pick one up directly during a maintenance pass without needing additional context.

### `AddVariableGetNode` silently produces an unresolved node for function-scope variables

`AddVariableGetNode` returns `ok:true` with a valid GUID even when the named variable can't be resolved as a class member — e.g. when the caller passes a function input parameter or function-local. The resulting node has `pins: []`, and the failure only surfaces downstream when a `ConnectPins` against it fails with `PinNotFound`. A subsequent `CompileBlueprint` does not reconstruct the missing pins.

**Fix:** validate the variable resolves at creation time and return `VariableNotFound` if it doesn't. Optionally accept a `scope` parameter (`"member" | "local" | "input"`) so callers can target function-scope variables explicitly. Current workaround: reuse an existing in-graph getter for function parameters rather than adding a new `VariableGet`.

### `Batch` cannot reference earlier results

`Batch` runs a list of independent requests but offers no syntax for request `N+1` to use a value produced by request `N`. Graph-construction work — "add four nodes, then wire them" — therefore can't use `Batch` at all and must run serial. A typical small edit (one CDO change, three new function-call nodes, one variable get, four connections, one compile, one save) ends up being ~10 round trips instead of 1 transactional batch.

**Fix:** add a JSON-pointer-style reference syntax (e.g. `$ref:requests[2].result.node.guid`) resolved server-side at batch execution time. If `ApplyGraphPatch` / `ApplyFunctionPatch` already covers this use case via the declarative patch surface, document the patch shape more visibly in the README so callers reach for it instead of `Batch + serial`. Distinct from the existing Roadmap item on "Batch read parallelization", which solves a different problem (parallelizing reads, not chaining mutations).

### Param naming differs across commands and burns first-time callers

Different commands use different keys for the same concept: `ConnectPins` uses `fromNode/fromPin/toNode/toPin`, other commands use `node` / `sourceNode` / `targetNode` / `subobject` / etc. A new caller (human or agent) frequently guesses the wrong key and pays a round-trip per command before reading the error message. The error text is good — the underlying inconsistency keeps generating the mistake.

**Fix, smallest first:**
1. Accept `source`/`target` (or `sourceNode`/`targetNode`) as aliases for `from`/`to` on `ConnectPins` — one-line additive change.
2. Audit command schemas for a single canonical vocabulary, add aliases where commands diverge.
3. Mention `DescribeCommand` prominently in the cost-aware tips section as the recommended preflight, and consider a `--describe` flag on the PowerShell wrapper that emits the schema without dispatching the call.

### `CheckoutAsset` error message swallows the underlying Perforce reason

`CheckoutAsset` returns `CheckoutFailed` with the message `"Could not check out '<path>'."` — no details about *why*. Common causes (already opened for edit, not under source control, P4 server unreachable, file not in any depot) are indistinguishable, so callers can't decide whether to proceed or abort. During dogfooding, the call returned `CheckoutFailed` but the subsequent `SaveAsset` still succeeded — suggesting the file was already opened and the failure was benign.

**Fix:** include the underlying `ISourceControlProvider` error in the response payload. Consider treating "already opened for edit" as a successful no-op (`ok:true` with `state: "AlreadyCheckedOut"`) rather than an error.

### `FindFunctions` lacks an exact-match filter

`FindFunctions` accepts `nameContains` but not `nameEquals`. For common substrings (e.g. `Normal` returns 25+ matches including `MirrorVectorByNormal`, `MakePlaneFromPointAndNormal`, `LinePlaneIntersection_OriginNormal`, …), the response carries 15 KB+ of irrelevant entries when the caller already knows the exact name and just wants to confirm it exists.

**Fix:** add an optional `nameEquals` parameter, or document `ResolveSymbol` more prominently as the preferred path for confirming a known function name resolves.

### `AddVariableGetNode` (and possibly other node-creation commands) return compact responses with empty `pins` arrays

Even when `AddVariableGetNode` succeeds normally on a valid class member, the response body returns `pins: []`. The pins exist on the actual node (subsequent `DescribeNode` calls show them after a `CompileBlueprint`), but the immediate creation response omits them. Callers that want to chain a `ConnectPins` right after creation can't look up the new pin's name from the response — they have to either know the variable's pin name a priori or follow up with `DescribeNode`.

**Fix:** populate `pins` in the creation response so callers can wire the new node in the same batch without a roundtrip-and-describe.

## Roadmap

The bridge's authoring surface is wide enough for most workflows now. Future work prioritizes **making what's already here cheaper and faster** over adding new commands. Items are ordered by expected leverage.

### Token efficiency & speed (priority)

1. **Semantic IR v2** — Add `let` bindings, `forEach`/`for`, `switch`, `cast`, `bind`/`broadcast`, and auto-hoist of impure expressions to the IR. Each construct shipping in IR collapses multi-call `ApplyGraphPatch` fallback patterns into a single `ApplySemanticFunction` request. This is the highest-leverage future change because every gap in the IR forces the LLM into the lower-level patch surface.

2. **Diff-based editing (`PatchSemanticFunction`)** — `insertBefore` / `replace` / `delete` operations on existing graph nodes. `ReplaceSemanticFunction` already handles whole-function rewrites; diffs would let small edits scale with the size of the change instead of the size of the function. Design is gated on dogfooding evidence about which edit patterns dominate.

3. **Asset versioning + conditional reads** — Bridge increments a monotonic version counter on every mutation; clients can pass `ifAssetVersionDiffersFrom: <version>` to inspection commands and get a `unchanged: true` short-circuit response. Eliminates redundant describe calls when the LLM already has a fresh snapshot.

4. **Multi-asset describe** — `SummarizeAssets([paths])` returns N asset summaries in one call. For "what does this folder of related Blueprints do?" investigations, cuts N envelope overheads and the inter-turn reasoning cost of dispatching N separate `SummarizeBlueprint` calls.

5. **Response budget + truncation flag** — `maxResponseBytes` cap on inspection commands. When exceeded, responses carry `truncated: true` and a hint to use the `fields` selector. Protects against unexpected payload size on assets the LLM hasn't seen before.

6. **Better error messages with "did you mean"** — Typo correction on command names, UFunction paths, variable names, field names. Each successful correction saves a full retry round-trip.

7. **Compact response format** — Opt-in `format: "compact"` returns short field names (`n` instead of `name`, `t` instead of `type`) and omits boolean defaults. ~20–30% wire-byte reduction on the heavy inspection commands. Off by default to keep responses human-readable.

8. **Incremental compile** — `CompileBlueprint` currently does a full compile pass. UE's KismetCompiler supports incremental compilation; exposing it would cut multi-second waits on big Blueprints during apply→compile→fix loops.

9. **Reference queries** — `FindAllReferences(variable)`, `WhereIsUsed(function)`, `FindAllImplementations(interface)`. Reflection iterators have this information cheaply; surfacing it eliminates grep round-trips for "what calls this?" / "what implements this?" workflows. Borderline between capability and efficiency, but it replaces work the LLM does today via the file-search tool.

10. **Snippet/template library expansion** — Pre-built `ApplyGraphSnippet` templates for common patterns (event bind, scoring function, save handler, etc.) with binding substitution. Each snippet replaces a full IR construction.

11. **`Batch` read parallelization** — `Batch` currently serializes requests. Read-only requests in a batch could run concurrently on the editor thread (asset loads are cached after the first).

12. **Streaming responses** — Length-prefixed JSON framing supports streaming naturally. For huge `DescribeGraphFull` / `SummarizeBlueprint` responses, streaming would let the LLM start reasoning before the full payload arrives. Transport-level work; biggest single latency win on large assets but also the largest implementation effort. Worth considering only after #1–#11 are exhausted or proven insufficient.

### Capability expansion (lower priority)

Surface-area additions, deferred until dogfooding shows they unblock real workflows that can't be solved with the current commands:

- Animation Blueprint authoring (state machines, transition rules, anim nodes)
- Behavior Tree / StateTree asset operations
- Material graph authoring (currently completely out of scope)
- Niagara emitter/module operations
- Remove/rename Blueprint variables (currently can only add)
- More delegate unbind/clear helpers
- Richer array/map/set graph operations
- More trace/collision and math node helpers
- Complete runtime UMG construction/property/brush helpers

### Infrastructure (background)

- Standalone cross-shell CLI client (alternative to PowerShell helper)
- Optional file-queue or WebSocket transport (alternative to named pipes for non-Windows or multi-client scenarios)
- Continue reducing shared command helper coupling as patterns stabilize
