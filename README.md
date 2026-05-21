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
Tools/BlueprintBridge/blueprintbridge.ps1
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

### Blueprint and graph inspection

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

Returns graph nodes, pin names, pin directions, pin types, pin container types, defaults, links, node GUIDs, positions, and variable node metadata.

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

### Asset, source control, compile, save

#### `CreateBlueprintAsset`

```json
{
  "asset": "/Game/Path/BP_NewAsset",
  "parentClass": "/Script/Engine.Actor"
}
```

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
- `CreateEventGraph`
- `DeleteGraph`
- `RenameGraph`
- `AddFunctionInput`
- `AddFunctionOutput`
- `EditUserDefinedPin`
- `RenameCustomEvent`
- `AddVariableGetterFunction`

Function input/output pin type params use the same `category`, `subCategory`, `subCategoryObject`, and `containerType` fields as variables.

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

## Roadmap

High-value next work:

- richer compile diagnostics in command responses
- continue reducing shared command helper coupling if needed
- standalone cross-shell CLI client
- generated schema docs
- remove/rename Blueprint variables
- more delegate unbind/clear helpers
- richer array/map/set graph operations
- more trace/collision and math node helpers
- complete runtime UMG construction/property/brush helpers
- Behavior Tree / StateTree / Animation Blueprint asset operations
- transaction grouping with optional rollback semantics
- optional file-queue or socket transport
