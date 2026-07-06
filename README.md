# World Data MCP Bridge

World Data MCP Bridge is an Unreal Engine editor plugin that exposes a live UE editor session through the Model Context Protocol (MCP). It is maintained as a standalone plugin repository and can be mounted into UE projects under `Plugins/UEBridgeMCP`.

This fork is tailored for WorldData/CameraEditor workflows. It includes the embedded MCP HTTP server, an in-editor conversation panel, ACP client integration for coding agents, and UE authoring tools for scene inspection, asset editing, PCG, spatial placement, gameplay systems, UI data, Sequencer, and AI Director assets.

## Features

- Embedded MCP Streamable HTTP endpoint at `/mcp`.
- Per-project server name and default port derived from the `.uproject` identity.
- Generated client configs for project-root `.mcp.json` and `.cursor/mcp.json`.
- Token-protected trusted tools through `X-WorldData-MCP-Token`.
- In-editor panel available from **Window > UEBridgeMCP Conversation Panel**.
- ACP conversation support for Codex, Cursor Agent, and Claude Code adapters.
- Tool modules split by concern:
  - `UEBridgeMCPCore`
  - `UEBridgeMCPEditor`
  - `UEBridgeMCPPCGTools`
  - `UEBridgeMCPSpatialTools`
  - `UEBridgeMCPAIDirectorTools`
- Local PCG knowledge data under `Data/`.
- Legacy Nwiro bridge sources preserved under `Legacy/` for reference, but not compiled by the active editor module.

## Requirements

- Unreal Engine 5.8 project, or a compatible UE 5.x C++ editor build.
- A C++ UE project. Blueprint-only projects need a C++ target before this plugin can compile.
- Enabled dependency plugins declared in `UEBridgeMCP.uplugin`, including:
  - `PythonScriptPlugin`
  - `ControlRig`
  - `DataValidation`
  - `EditorScriptingUtilities`
  - `EnhancedInput`
  - `GameplayAbilities`
  - `IKRig`
  - `Niagara`
  - `PCG`
  - `PoseSearch`
  - `PropertyBindingUtils`
  - `StateTree`
  - `AIDirectorCamera`

If a host project does not include `AIDirectorCamera`, either install that plugin or remove the `UEBridgeMCPAIDirectorTools` module and `AIDirectorCamera` dependency before building.

## Install Into A Project

Clone or copy this repository into the target project's plugin directory:

```text
<YourProject>/Plugins/UEBridgeMCP/
```

Enable it in the `.uproject`:

```json
{
  "Plugins": [
    {
      "Name": "UEBridgeMCP",
      "Enabled": true
    }
  ]
}
```

Regenerate project files if needed, then build the editor target.

Example:

```powershell
& "D:\EpicRe\UE_5.8\Engine\Build\BatchFiles\Build.bat" YourProjectEditor Win64 Development -Project="D:\Path\To\YourProject\YourProject.uproject" -WaitMutex
```

If Unreal reports that Live Coding is active, close the editor or disable Live Coding before running the external build.

## MCP Endpoint And Config Files

The plugin starts the MCP server during editor startup. If the project has no saved MCP config yet, the default port is generated from the project identity:

```text
5753 + (ProjectHash % 20000)
```

On successful startup the plugin writes:

- `<Project>/.mcp.json`
- `<Project>/.cursor/mcp.json`
- `<Project>/Saved/UEBridgeMCP/config.json`
- `<Project>/Saved/UEBridgeMCP/mcp.json`

The project-root files advertise the local endpoint and are safe to share. The `Saved/UEBridgeMCP/mcp.json` file includes the trusted-client header and should stay local.

Example project-root config:

```json
{
  "mcpServers": {
    "world_data_myproject_ab12cd34": {
      "type": "http",
      "url": "http://127.0.0.1:12345/mcp",
      "tool_timeout_sec": 120
    }
  }
}
```

Trusted mutating tools require a header:

```text
X-WorldData-MCP-Token: <token from Saved/UEBridgeMCP/mcp.json>
```

High-risk tools may additionally require explicit human confirmation via tool arguments:

```json
{
  "confirmDangerousAction": true,
  "confirmationReason": "User approved this exact editor action."
}
```

## Editor UI

Open the panel from:

```text
Window > UEBridgeMCP Conversation Panel
```

The panel provides:

- Server status, port controls, and registered tool visibility.
- Conversation history.
- Agent selector for Codex, Cursor, and Claude Code.
- Model selector per agent.
- File attachments in prompts.
- Permission prompts for ACP tool access.

ACP adapter binaries are detected from common locations including:

- `<Project>/Saved/UEBridgeMCP`
- `<Project>/Binaries`
- `<Plugin>/Binaries`
- `PATH`

## Smoke Test

After the editor has started, read the generated project-root `.mcp.json` to find the active server URL. Then list MCP tools:

```powershell
$url = "http://127.0.0.1:<port>/mcp"
$body = @{
  jsonrpc = "2.0"
  id = 1
  method = "tools/list"
  params = @{}
} | ConvertTo-Json -Depth 8

Invoke-RestMethod -Uri $url -Method Post -ContentType "application/json" -Body $body
```

For a trusted tool call, use the generated token from `Saved/UEBridgeMCP/mcp.json`:

```powershell
$connection = Get-Content "Saved/UEBridgeMCP/mcp.json" -Raw | ConvertFrom-Json
$headers = @{
  "X-WorldData-MCP-Token" = $connection.headers."X-WorldData-MCP-Token"
}

Invoke-RestMethod -Uri $connection.url -Method Post -ContentType "application/json" -Headers $headers -Body $body
```

## Repository Layout

```text
Data/                       Local knowledge data used by MCP tools
Legacy/                     Archived legacy bridge implementation
Source/UEBridgeMCPCore/     Shared registry and scene-brief state
Source/UEBridgeMCPEditor/   MCP server, editor panel, and base tool set
Source/UEBridgeMCPPCGTools/ PCG-specific tool module
Source/UEBridgeMCPSpatialTools/
                            Spatial perception and placement tools
Source/UEBridgeMCPAIDirectorTools/
                            AI Director asset and sequencer helpers
Tools/                      Local audit/development scripts
```

Generated folders such as `Binaries/`, `Intermediate/`, `Saved/`, and `DerivedDataCache/` are intentionally ignored.

## Development

Useful local checks:

```powershell
node Tools/audit-mcp-plugin.mjs
```

The audit script expects this plugin to live at `Plugins/UEBridgeMCP` inside a project checkout.

When adding a new tool module:

1. Add a module under `Source/`.
2. Depend on `UEBridgeMCPCore`.
3. Register definitions and dispatch through `WorldDataMCP::RegisterMCPToolModule`.
4. Add the module to `UEBridgeMCP.uplugin`.
5. Rebuild the editor target.
6. Start the editor and verify the tool appears in `tools/list`.

## Operational Notes

- The HTTP server only accepts loopback Host/Origin values.
- Mutating tools require trusted-client access.
- Some destructive or reflected-write tools also require explicit human approval in the tool arguments.
- Each editor session should use its own port. If the preferred port is occupied, startup probes the next 9 ports.
- The root `.mcp.json` is safe to commit only because it does not include the trusted token.
- Files under `Saved/UEBridgeMCP/` should not be committed.

## License

Private/internal project plugin unless a license file is added to this repository.
