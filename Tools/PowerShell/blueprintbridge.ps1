param(
	[Parameter(Position = 0)]
	[string]$Command = "Ping",

	[Parameter(Position = 1, ValueFromRemainingArguments = $true)]
	[string[]]$RemainingArgs,

	[string]$PipeName = "BlueprintBridge",
	[string]$AuthToken = $env:BLUEPRINTBRIDGE_AUTH_TOKEN,
	[string]$RequestJson,
	[string]$RequestFile,
	[string]$Fields
)

$ErrorActionPreference = "Stop"

function Parse-RequestJson {
	param([string]$JsonText)

	try {
		return $JsonText | ConvertFrom-Json -AsHashtable
	} catch {
		return $JsonText | ConvertFrom-Json
	}
}

function Add-RequestAuthToken {
	param($Request, [string]$Token)

	if (-not $Token) {
		return $Request
	}

	if ($Request -is [hashtable]) {
		$Request.authToken = $Token
		return $Request
	}

	$Property = $Request.PSObject.Properties['authToken']
	if ($Property) {
		$Property.Value = $Token
	} else {
		$Request | Add-Member -NotePropertyName authToken -NotePropertyValue $Token
	}
	return $Request
}

function New-RequestObject {
	param([string]$Command, [string[]]$RemainingArgs)

	$id = [guid]::NewGuid().ToString()
	switch ($Command.ToLowerInvariant()) {
		"ping" { return @{ id = $id; version = 1; command = "Ping"; params = @{} } }
		"project" { return @{ id = $id; version = 1; command = "GetProjectName"; params = @{} } }
		"engine-version" { return @{ id = $id; version = 1; command = "GetEngineVersion"; params = @{} } }
		"describe-blueprint" {
			if ($RemainingArgs.Count -lt 1) { throw "describe-blueprint requires an asset path." }
			return @{ id = $id; version = 1; command = "DescribeBlueprint"; params = @{ asset = $RemainingArgs[0] } }
		}
		"describe-graph" {
			if ($RemainingArgs.Count -lt 2) { throw "describe-graph requires: asset graph." }
			return @{ id = $id; version = 1; command = "DescribeGraph"; params = @{ asset = $RemainingArgs[0]; graph = $RemainingArgs[1] } }
		}
		"describe-graph-full" {
			if ($RemainingArgs.Count -lt 2) { throw "describe-graph-full requires: asset graph." }
			return @{ id = $id; version = 1; command = "DescribeGraphFull"; params = @{ asset = $RemainingArgs[0]; graph = $RemainingArgs[1] } }
		}
		"describe-node" {
			if ($RemainingArgs.Count -lt 3) { throw "describe-node requires: asset graph nodeGuid." }
			return @{ id = $id; version = 1; command = "DescribeNode"; params = @{ asset = $RemainingArgs[0]; graph = $RemainingArgs[1]; node = $RemainingArgs[2] } }
		}
		"describe-subgraph" {
			if ($RemainingArgs.Count -lt 3) { throw "describe-subgraph requires: asset graph seedGuid [depth]." }
			$params = @{ asset = $RemainingArgs[0]; graph = $RemainingArgs[1]; seeds = @($RemainingArgs[2]) }
			if ($RemainingArgs.Count -ge 4) { $params.depth = [int]$RemainingArgs[3] }
			return @{ id = $id; version = 1; command = "DescribeSubgraph"; params = $params }
		}
		"get-connected-nodes" {
			if ($RemainingArgs.Count -lt 3) { throw "get-connected-nodes requires: asset graph nodeGuid." }
			return @{ id = $id; version = 1; command = "GetConnectedNodes"; params = @{ asset = $RemainingArgs[0]; graph = $RemainingArgs[1]; node = $RemainingArgs[2] } }
		}
		"find-nodes" {
			if ($RemainingArgs.Count -lt 2) { throw "find-nodes requires: asset graph [filterJson]." }
			$params = @{ asset = $RemainingArgs[0]; graph = $RemainingArgs[1] }
			if ($RemainingArgs.Count -ge 3) {
				$filter = Parse-RequestJson $RemainingArgs[2]
				if ($filter -is [hashtable]) {
					foreach ($k in $filter.Keys) { $params[$k] = $filter[$k] }
				} else {
					foreach ($p in $filter.PSObject.Properties) { $params[$p.Name] = $p.Value }
				}
			}
			return @{ id = $id; version = 1; command = "FindNodes"; params = $params }
		}
		"summarize-blueprint" {
			if ($RemainingArgs.Count -lt 1) { throw "summarize-blueprint requires an asset path." }
			return @{ id = $id; version = 1; command = "SummarizeBlueprint"; params = @{ asset = $RemainingArgs[0] } }
		}
		"list-commands" {
			return @{ id = $id; version = 1; command = "ListCommands"; params = @{} }
		}
		"describe-command" {
			if ($RemainingArgs.Count -lt 1) { throw "describe-command requires: commandName." }
			return @{ id = $id; version = 1; command = "DescribeCommand"; params = @{ command = $RemainingArgs[0] } }
		}
		"get-blueprint-default" {
			if ($RemainingArgs.Count -lt 2) { throw "get-blueprint-default requires: asset property." }
			return @{ id = $id; version = 1; command = "GetBlueprintDefault"; params = @{ asset = $RemainingArgs[0]; property = $RemainingArgs[1] } }
		}
		"find-variable-references" {
			if ($RemainingArgs.Count -lt 2) { throw "find-variable-references requires: asset variable." }
			return @{ id = $id; version = 1; command = "FindVariableReferences"; params = @{ asset = $RemainingArgs[0]; variable = $RemainingArgs[1] } }
		}
		"describe-components" {
			if ($RemainingArgs.Count -lt 1) { throw "describe-components requires an asset path." }
			return @{ id = $id; version = 1; command = "DescribeComponents"; params = @{ asset = $RemainingArgs[0] } }
		}
		"add-component" {
			if ($RemainingArgs.Count -lt 3) { throw "add-component requires: asset name componentClass [parent]." }
			$params = @{ asset = $RemainingArgs[0]; name = $RemainingArgs[1]; componentClass = $RemainingArgs[2] }
			if ($RemainingArgs.Count -ge 4) { $params.parent = $RemainingArgs[3] }
			return @{ id = $id; version = 1; command = "AddComponent"; params = $params }
		}
		"attach-component" {
			if ($RemainingArgs.Count -lt 3) { throw "attach-component requires: asset name parent." }
			return @{ id = $id; version = 1; command = "AttachComponent"; params = @{ asset = $RemainingArgs[0]; name = $RemainingArgs[1]; parent = $RemainingArgs[2] } }
		}
		"set-component-property" {
			if ($RemainingArgs.Count -lt 4) { throw "set-component-property requires: asset name property value." }
			return @{ id = $id; version = 1; command = "SetComponentProperty"; params = @{ asset = $RemainingArgs[0]; name = $RemainingArgs[1]; property = $RemainingArgs[2]; value = $RemainingArgs[3] } }
		}
		"create-blueprint-asset" {
			if ($RemainingArgs.Count -lt 2) { throw "create-blueprint-asset requires: asset parentClass." }
			return @{ id = $id; version = 1; command = "CreateBlueprintAsset"; params = @{ asset = $RemainingArgs[0]; parentClass = $RemainingArgs[1] } }
		}
		"duplicate-asset" {
			if ($RemainingArgs.Count -lt 2) { throw "duplicate-asset requires: sourceAsset destAsset." }
			return @{ id = $id; version = 1; command = "DuplicateAsset"; params = @{ sourceAsset = $RemainingArgs[0]; destAsset = $RemainingArgs[1] } }
		}
		"checkout-asset" {
			if ($RemainingArgs.Count -lt 1) { throw "checkout-asset requires an asset path." }
			return @{ id = $id; version = 1; command = "CheckoutAsset"; params = @{ asset = $RemainingArgs[0] } }
		}
		"compile-blueprint" {
			if ($RemainingArgs.Count -lt 1) { throw "compile-blueprint requires an asset path." }
			return @{ id = $id; version = 1; command = "CompileBlueprint"; params = @{ asset = $RemainingArgs[0] } }
		}
		"save-asset" {
			if ($RemainingArgs.Count -lt 1) { throw "save-asset requires an asset path." }
			return @{ id = $id; version = 1; command = "SaveAsset"; params = @{ asset = $RemainingArgs[0] } }
		}
		"set-blueprint-default" {
			if ($RemainingArgs.Count -lt 3) { throw "set-blueprint-default requires: asset property value." }
			return @{ id = $id; version = 1; command = "SetBlueprintDefault"; params = @{ asset = $RemainingArgs[0]; property = $RemainingArgs[1]; value = $RemainingArgs[2] } }
		}
		"describe-subobjects" {
			if ($RemainingArgs.Count -lt 1) { throw "describe-subobjects requires: asset [subobjectClass] [includeProperties]." }
			$params = @{ asset = $RemainingArgs[0] }
			if ($RemainingArgs.Count -ge 2) { $params.subobjectClass = $RemainingArgs[1] }
			if ($RemainingArgs.Count -ge 3) { $params.includeProperties = [System.Convert]::ToBoolean($RemainingArgs[2]) }
			return @{ id = $id; version = 1; command = "DescribeSubobjects"; params = $params }
		}
		"set-subobject-default" {
			if ($RemainingArgs.Count -lt 4) { throw "set-subobject-default requires: asset subobject property value [subobjectClass]." }
			$params = @{ asset = $RemainingArgs[0]; subobject = $RemainingArgs[1]; property = $RemainingArgs[2]; value = $RemainingArgs[3] }
			if ($RemainingArgs.Count -ge 5) { $params.subobjectClass = $RemainingArgs[4] }
			return @{ id = $id; version = 1; command = "SetSubobjectDefault"; params = $params }
		}
		"add-blueprint-variable" {
			if ($RemainingArgs.Count -lt 3) { throw "add-blueprint-variable requires: asset name category [defaultValue] [subCategoryObject]." }
			$params = @{ asset = $RemainingArgs[0]; name = $RemainingArgs[1]; category = $RemainingArgs[2] }
			if ($RemainingArgs.Count -ge 4) { $params.defaultValue = $RemainingArgs[3] }
			if ($RemainingArgs.Count -ge 5) { $params.subCategoryObject = $RemainingArgs[4] }
			return @{ id = $id; version = 1; command = "AddBlueprintVariable"; params = $params }
		}
		default { throw "Unknown command '$Command'. Inspection: ping, project, engine-version, list-commands, describe-command, describe-blueprint, summarize-blueprint, describe-graph, describe-graph-full, describe-node, describe-subgraph, get-connected-nodes, find-nodes, find-variable-references, describe-components, describe-subobjects, get-blueprint-default. Mutation: add-component, attach-component, set-component-property, create-blueprint-asset, duplicate-asset, checkout-asset, compile-blueprint, save-asset, set-blueprint-default, set-subobject-default, add-blueprint-variable. Escape hatch: -RequestJson, -RequestFile (see ListCommands for every supported command)." }
	}
}

if ($RequestFile) {
	$request = Parse-RequestJson (Get-Content -Raw -Path $RequestFile)
} elseif ($RequestJson) {
	$request = Parse-RequestJson $RequestJson
} else {
	$request = New-RequestObject $Command $RemainingArgs
}

if ($Fields) {
	$fieldsArray = @($Fields.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
	if ($fieldsArray.Count -gt 0) {
		$params = $null
		if ($request -is [hashtable]) {
			if (-not $request.ContainsKey('params') -or $null -eq $request['params']) { $request['params'] = @{} }
			$params = $request['params']
			if ($params -is [hashtable]) {
				$params['fields'] = $fieldsArray
			} else {
				$params | Add-Member -NotePropertyName fields -NotePropertyValue $fieldsArray -Force
			}
		} else {
			$paramsProp = $request.PSObject.Properties['params']
			if (-not $paramsProp -or $null -eq $paramsProp.Value) {
				$request | Add-Member -NotePropertyName params -NotePropertyValue ([pscustomobject]@{ fields = $fieldsArray }) -Force
			} else {
				$paramsProp.Value | Add-Member -NotePropertyName fields -NotePropertyValue $fieldsArray -Force
			}
		}
	}
}

$request = Add-RequestAuthToken $request $AuthToken

$json = $request | ConvertTo-Json -Depth 32 -Compress

$client = [System.IO.Pipes.NamedPipeClientStream]::new(".", $PipeName, [System.IO.Pipes.PipeDirection]::InOut)
$client.Connect(5000)
try {
	$utf8 = [System.Text.Encoding]::UTF8
	$payload = $utf8.GetBytes($json)
	$length = [BitConverter]::GetBytes([UInt32]$payload.Length)
	$client.Write($length, 0, $length.Length)
	$client.Write($payload, 0, $payload.Length)
	$client.Flush()

	$lengthBuffer = New-Object byte[] 4
	$read = 0
	while ($read -lt 4) {
		$n = $client.Read($lengthBuffer, $read, 4 - $read)
		if ($n -le 0) { throw "Pipe closed before response length was received." }
		$read += $n
	}

	$responseLength = [BitConverter]::ToUInt32($lengthBuffer, 0)
	$responseBuffer = New-Object byte[] $responseLength
	$read = 0
	while ($read -lt $responseLength) {
		$n = $client.Read($responseBuffer, $read, $responseLength - $read)
		if ($n -le 0) { throw "Pipe closed before response body was received." }
		$read += $n
	}

	$utf8.GetString($responseBuffer)
} finally {
	$client.Dispose()
}
