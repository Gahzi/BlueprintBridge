# GitHub Packaging Checklist

This plugin currently works inside the Biscuit project, but a few cleanup steps should happen before exporting it to a standalone GitHub repository.

## Naming

Chosen public name:

1. `BlueprintBridge`

Other alternatives that were considered:

- `K2Bridge`
- `UnrealAgentBridge`
- `BlueprintAgentBridge`
- `EditorPipe`
- `GraphPipe`

## Required cleanup before public repo

- Move/copy the PowerShell client into the plugin or repo, e.g.:

  ```text
  Tools/PowerShell/blueprintbridge.ps1
  ```

- Split the large module `.cpp` into smaller files.
- Consider adding richer settings/UI exposure inside the editor.
- Consider adding compile diagnostics to command responses.
- Consider adding command allowlist/denylist settings.

## Suggested repository layout

```text
BlueprintBridge/
  BlueprintBridge.uplugin
  README.md
  GITHUB_PACKAGING.md
  LICENSE
  .gitignore
  Source/
    BlueprintBridgeEditor/
      BlueprintBridgeEditor.Build.cs
      Public/
      Private/
        BlueprintBridgeEditorModule.cpp
        Protocol/
        Transport/
        Commands/
        Blueprint/
  Tools/
    PowerShell/
      blueprintbridge.ps1
  examples/
    requests/
      ping.json
      describe-blueprint.json
      describe-graph.json
      create-function-getter.json
```

## Publishing notes

Do not paste GitHub credentials or personal access tokens into AI chat.

Safe options:

- Use `gh auth login` manually in your own terminal.
- Use browser/device-code auth.
- Use a local credential manager.
- If a token is required for automation, place it in an environment variable locally and do not print it.

Example once the repo is prepared and you are authenticated:

```powershell
git init
git add .
git commit -m "Initial BlueprintBridge plugin"
gh repo create YOUR_ORG/BlueprintBridge --private --source=. --remote=origin --push
```

or for an existing repo:

```powershell
git remote add origin https://github.com/YOUR_ORG/BlueprintBridge.git
git push -u origin main
```
