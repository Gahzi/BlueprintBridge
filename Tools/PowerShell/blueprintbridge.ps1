param(
	[Parameter(Position = 0)]
	[string]$Command = "Ping",

	[Parameter(Position = 1, ValueFromRemainingArguments = $true)]
	[string[]]$RemainingArgs,

	[string]$PipeName = "BlueprintBridge",
	[string]$AuthToken = $env:BLUEPRINTBRIDGE_AUTH_TOKEN,
	[string]$RequestJson,
	[string]$RequestFile
)

$ErrorActionPreference = "Stop"

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
		"find-variable-references" {
			if ($RemainingArgs.Count -lt 2) { throw "find-variable-references requires: asset variable." }
			return @{ id = $id; version = 1; command = "FindVariableReferences"; params = @{ asset = $RemainingArgs[0]; variable = $RemainingArgs[1] } }
		}
		"migrate-weakpoint-enabled-bool-to-state" {
			if ($RemainingArgs.Count -lt 1) { throw "migrate-weakpoint-enabled-bool-to-state requires an asset path." }
			$params = @{ asset = $RemainingArgs[0] }
			if ($RemainingArgs.Count -ge 2) { $params.enabledValue = $RemainingArgs[1] }
			if ($RemainingArgs.Count -ge 3) { $params.disabledValue = $RemainingArgs[2] }
			return @{ id = $id; version = 1; command = "MigrateWeakpointEnabledBoolToState"; params = $params }
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
		default { throw "Unknown command '$Command'. Use ping, project, engine-version, describe-blueprint, describe-graph, find-variable-references, migrate-weakpoint-enabled-bool-to-state, checkout-asset, compile-blueprint, save-asset, set-blueprint-default, describe-subobjects, set-subobject-default, add-blueprint-variable, -RequestJson, or -RequestFile." }
	}
}

if ($RequestFile) {
	$request = Get-Content -Raw -Path $RequestFile | ConvertFrom-Json -AsHashtable
} elseif ($RequestJson) {
	$request = $RequestJson | ConvertFrom-Json -AsHashtable
} else {
	$request = New-RequestObject $Command $RemainingArgs
}

if ($AuthToken) {
	$request.authToken = $AuthToken
}

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
