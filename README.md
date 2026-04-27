# Blueprint Bridge

> Working name. This plugin exposes a local Windows IPC bridge that lets an external coding agent inspect and edit Unreal Editor assets, especially Blueprints, without driving the editor UI with mouse/screen automation.

## Naming ideas

Current internal name:

```text
BlueprintBridge
```

Possible public names:

- **K2Bridge** — short, Blueprint-focused, Unreal-flavored.
- **EditorPipe** — emphasizes local IPC into the editor.
- **BlueprintBridge** — clear and descriptive.
- **BlueprintBridge** — clear if we want the project to feel Blueprint-first.
- **GraphPipe** — concise, good if graph editing is the primary identity.
- **UAgentBridge** — Unreal-ish naming, broader than Blueprint.

My current favorite for a public GitHub repo is **K2Bridge** if the main value is Blueprint graph editing, or **BlueprintBridge** if we want it to sound broader and more discoverable.

## What this is

Blueprint Bridge is an **Editor-only Unreal Engine plugin** for Windows. It runs inside `UnrealEditor.exe`, listens on a local named pipe, accepts structured JSON commands, executes those commands using Unreal Editor APIs, and returns JSON responses.

The intended workflow is:

```text
External agent / CLI / terminal
    -> JSON request over Windows named pipe
        -> Unreal Editor plugin
            -> Unreal Editor APIs modify assets safely
        <- JSON response
    <- terminal output
```

This avoids unsafe direct `.uasset` editing and avoids fragile screen/mouse automation.

## Current status

This is early but already functional. It can:

- connect to the editor over a Windows named pipe
- inspect Blueprints and graphs
- find variable references
- create and edit common Blueprint graph nodes
- create function/event graphs
- add function inputs/outputs
- connect, move, and break pin links
- compile and save Blueprints
- perform simple Blueprint variable/default edits

It has already been used to migrate a Blueprint from a bool-backed state flow to an enum-backed state flow.

## Platform support

Current support:

```text
Windows only
Unreal Editor only
```

The transport is currently a Windows named pipe. By default it listens on:

```text
\\.\pipe\BlueprintBridge
```

The pipe name is configurable through plugin config/settings.

## Why a plugin instead of editing `.uasset` files directly?

Blueprint assets are binary, versioned, editor-managed assets. Directly modifying `.uasset` files from an external process is risky and likely to corrupt assets.

This plugin keeps Unreal in charge of its own assets:

- load assets through Unreal APIs
- modify `UBlueprint`, `UEdGraph`, `UEdGraphNode`, and `UEdGraphPin` objects
- use transactions where possible
- mark packages dirty
- compile Blueprints through Kismet editor utilities
- save packages through editor saving utilities

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

Command router
    dispatches command names to C++ handlers

Unreal operation layer
    asset loading
    Blueprint inspection
    graph/node/pin operations
    compile/save/source control helpers
```

### Request format

Requests are JSON objects:

```json
{
  "id": "request-id",
  "version": 1,
  "command": "DescribeBlueprint",
  "params": {
    "asset": "/Game/AI/BP_EnemyWeakpoint"
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

Some commands return a string instead of an object:

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

## Transport protocol

The named pipe uses length-prefixed UTF-8 JSON:

```text
[4-byte little-endian uint32 payload length][UTF-8 JSON payload]
```

The plugin currently handles one connected client at a time. A client may send multiple framed requests over the same connection.

## Configuration

A default config file is included at:

```text
Config/DefaultEditorPerProjectUserSettings.ini
```

Current settings section:

```ini
[/Script/BlueprintBridgeEditor.BlueprintBridge]
PipeName=BlueprintBridge
bRequireAuthToken=false
AuthToken=
```

### Settings

- `PipeName` — local pipe name without the `\\.\pipe\` prefix.
- `bRequireAuthToken` — when `true`, every request must include `authToken`.
- `AuthToken` — expected token value when auth is enabled.

If auth is enabled and a request omits `authToken` or sends the wrong token, the plugin returns:

```json
{
  "ok": false,
  "error": {
    "code": "Unauthorized",
    "message": "Missing or invalid auth token."
  }
}
```

## PowerShell client

A simple PowerShell helper lives outside the plugin in this project:

```text
Projects/Biscuit/Tools/BlueprintBridge/blueprintbridge.ps1
```

Examples:

```powershell
blueprintbridge.ps1 ping
blueprintbridge.ps1 project
blueprintbridge.ps1 engine-version
blueprintbridge.ps1 describe-blueprint /Game/AI/BP_EnemyWeakpoint
blueprintbridge.ps1 describe-graph /Game/AI/BP_EnemyWeakpoint EventGraph
blueprintbridge.ps1 find-variable-references /Game/AI/BP_EnemyWeakpoint bIsEnabled
```

The client also accepts raw JSON:

```powershell
blueprintbridge.ps1 -RequestJson '{"id":"1","version":1,"command":"Ping","params":{}}'
```

If auth is enabled, either pass `-AuthToken` explicitly or set an environment variable:

```powershell
$env:BLUEPRINTBRIDGE_AUTH_TOKEN = "your-token"
blueprintbridge.ps1 ping
```

The script automatically injects `authToken` into the request before serialization.

When calling from Git Bash/MSYS, use `MSYS_NO_PATHCONV=1` so Unreal paths such as `/Game/...` are not rewritten into Windows paths:

```bash
MSYS_NO_PATHCONV=1 powershell.exe -ExecutionPolicy Bypass -File D:/Path/To/blueprintbridge.ps1 describe-blueprint /Game/AI/BP_EnemyWeakpoint
```

## Installation in an Unreal project

1. Copy the plugin folder into your project:

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
4. Build the editor target.
5. Start the editor.
6. Send a ping request through the client.

## Current commands

### Basic/editor info

#### `Ping`

Returns `Pong`.

```json
{"id":"1","version":1,"command":"Ping","params":{}}
```

#### `GetProjectName`

Returns the current Unreal project name.

#### `GetEngineVersion`

Returns the current engine version string.

### Blueprint/graph inspection

#### `DescribeBlueprint`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset"
}
```

Returns parent class, Blueprint variables, and graph names.

#### `DescribeGraph`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph"
}
```

Returns nodes, pins, pin types, defaults, links, node GUIDs, positions, and variable node metadata.

#### `DescribeNode`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "node": "NODE-GUID"
}
```

Returns a single node description.

#### `FindNodes`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "OptionalGraphName",
  "class": "OptionalClassPathSubstring",
  "title": "OptionalTitleSubstring",
  "variable": "OptionalVariableName"
}
```

All filters except `asset` are optional.

#### `FindVariableReferences`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "variable": "WeakpointState"
}
```

Returns variable get/set nodes referencing that variable.

### Blueprint graph management

#### `CreateFunctionGraph`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "function": "GetFoo"
}
```

Creates a user function graph.

#### `CreateEventGraph`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "NewEventGraph"
}
```

Creates an event graph page.

#### `AddFunctionInput`

Params can specify a type directly:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "FunctionName",
  "name": "InputName",
  "category": "bool"
}
```

Or copy a type from an existing Blueprint variable:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "FunctionName",
  "name": "State",
  "sourceVariable": "WeakpointState"
}
```

#### `AddFunctionOutput`

Same params as `AddFunctionInput`, but creates a return/result pin.

#### `DeleteGraph`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "GraphToDelete"
}
```

#### `RenameGraph`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "OldName",
  "newName": "NewName"
}
```

### Node creation

All graph node creation commands require:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200
}
```

#### `AddVariableGetNode`

Additional params:

```json
{
  "variable": "WeakpointState"
}
```

#### `AddVariableSetNode`

Additional params:

```json
{
  "variable": "WeakpointState"
}
```

#### `AddBranchNode`

Adds a Branch node.

#### `AddSequenceNode`

Adds a Sequence node.

Optional:

```json
{
  "extraOutputs": 1
}
```

#### `AddRerouteNode`

Adds a reroute/knot node.

#### `AddCommentNode`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 100,
  "y": 100,
  "width": 400,
  "height": 200,
  "text": "Comment text"
}
```

#### `AddEnumEqualityNode`

Adds a proper Blueprint `Equal (Enum)` node.

#### `AddEnumSwitchNode`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200,
  "enum": "/Game/Path/EState.EState"
}
```

#### `AddFunctionCallNode`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "x": 400,
  "y": 200,
  "functionClass": "/Script/Engine.KismetMathLibrary",
  "function": "EqualEqual_ByteByte"
}
```

### Pin and node editing

#### `ConnectPins`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "fromNode": "NODE-GUID",
  "fromPin": "then",
  "toNode": "NODE-GUID",
  "toPin": "execute"
}
```

#### `MovePinLinks`

Moves all links from one pin to another.

#### `BreakPinLinks`

Breaks all links from one pin.

#### `SetPinDefault`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "node": "NODE-GUID",
  "pin": "B",
  "value": "NewEnumerator1"
}
```

#### `CopyPinType`

Copies the pin type from one pin to another. Useful for wildcard/enum-aware node setup.

#### `SetNodePosition`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "graph": "EventGraph",
  "node": "NODE-GUID",
  "x": 1200,
  "y": 400
}
```

#### `DeleteNode`

Deletes a node by GUID.

### Asset/source control/compile/save

#### `CheckoutAsset`

Checks out an asset file through Unreal source-control helpers.

#### `CompileBlueprint`

Compiles a Blueprint and returns status:

```json
{
  "status": "UpToDate"
}
```

#### `SaveAsset`

Saves an asset package.

### Blueprint variables/defaults

#### `AddBlueprintVariable`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "name": "SomeBool",
  "category": "bool",
  "defaultValue": "false"
}
```

For object/struct/enum/class variables, provide `subCategoryObject`.

#### `SetBlueprintDefault`

Params:

```json
{
  "asset": "/Game/Path/BP_Asset",
  "property": "SomeProperty",
  "value": "SomeImportedTextValue"
}
```

## Example: Create a variable getter function

This sequence creates a function that returns a Blueprint variable.

```json
{"id":"1","version":1,"command":"CreateFunctionGraph","params":{"asset":"/Game/BP_Test","function":"GetWeakpointState"}}
```

```json
{"id":"2","version":1,"command":"AddFunctionOutput","params":{"asset":"/Game/BP_Test","graph":"GetWeakpointState","name":"WeakpointState","sourceVariable":"WeakpointState"}}
```

```json
{"id":"3","version":1,"command":"AddVariableGetNode","params":{"asset":"/Game/BP_Test","graph":"GetWeakpointState","variable":"WeakpointState","x":32,"y":112}}
```

Then call `DescribeGraph`, find the result node GUID, and connect:

```json
{"id":"4","version":1,"command":"ConnectPins","params":{"asset":"/Game/BP_Test","graph":"GetWeakpointState","fromNode":"GET-NODE-GUID","fromPin":"WeakpointState","toNode":"RESULT-NODE-GUID","toPin":"WeakpointState"}}
```

Finally:

```json
{"id":"5","version":1,"command":"CompileBlueprint","params":{"asset":"/Game/BP_Test"}}
{"id":"6","version":1,"command":"SaveAsset","params":{"asset":"/Game/BP_Test"}}
```

## Development workflow

When changing the plugin C++:

1. Build the editor target.
2. Fully restart Unreal Editor.
3. Test `Ping`.
4. Test new commands.

A full restart is currently recommended over Live Coding because the plugin owns a long-running named pipe thread and command router state.

## Examples

Example JSON requests are included in:

```text
examples/requests/
```

Included examples:

- `ping.json`
- `describe-blueprint.json`
- `create-function-getter.json`

## Packaging for GitHub

Before publishing this as an external repository, recommended cleanup:

1. Move or include a standalone CLI client.
2. Add CI or at least documented build commands.
3. Consider splitting the large module `.cpp` into smaller files.
4. Add richer compile diagnostics in command responses.

Suggested repo layout:

```text
K2Bridge/
  K2Bridge.uplugin
  README.md
  LICENSE
  Source/
    K2BridgeEditor/
      K2BridgeEditor.Build.cs
      Private/
      Public/
  Tools/
    PowerShell/
      k2bridge.ps1
  examples/
    requests/
      ping.json
      describe-blueprint.json
      create-function-getter.json
```

## Security notes

This plugin can modify editor assets. Treat access to the pipe as trusted local automation.

Recommended before public release:

- operation logging
- dry-run support for destructive commands
- command allowlist/denylist settings
- explicit save policy

Do not paste GitHub credentials, tokens, or passwords into an AI chat. When publishing, use GitHub CLI/browser auth locally, or provide a token through a local environment variable or credential manager that is never echoed into logs.

## Known limitations

- Windows-only named pipe transport.
- Auth is simple shared-token auth, not a full security model.
- One client at a time.
- Some Blueprint editor behaviors are context-sensitive and may require new primitives.
- Function graph support is improving but still basic.
- UMG, Animation Blueprints, Materials, Niagara, Behavior Trees, and State Trees are not covered as separate graph systems yet.
- The code should be split into smaller files before public release.

## Roadmap

High-value next additions:

- richer settings/UI exposure inside the editor
- file queue fallback transport
- standalone CLI executable
- command schema docs/generated docs
- remove/rename Blueprint variables
- custom event node creation
- event node creation by function/delegate
- delegate bind/unbind helpers
- component/SCS editing
- UMG widget tree inspection/editing
- Behavior Tree / StateTree asset operations
- transaction grouping for multi-command edits
- better compile diagnostics returned directly from command responses
