# WorldData Agent Host

`WorldData.AgentHost.App` is the only executable composition root. It exposes
WorldData Agent Host IPC v2 on stdin/stdout and keeps the version-specific Codex
app-server JSON-RPC protocol inside `WorldData.AgentHost.Codex`.

Current UE clients send the MCP credential once in the private UE-to-Host stdio
handshake, then the Host places it only in the private Host-to-Codex child
environment required by Codex's `env_http_headers` configuration. It is never
written to disk or a command line. For protocol-v1 migration only, the Host can
read (but never modify) an inherited credential from an already-built legacy UE
client until that client is rebuilt.

- `Contracts`: the only public .NET assembly surface; DTOs and interfaces only.
- `Codex`: internal app-server transport and protocol implementation, visible only to the composition root.
- `App`: internal command routing and dependency composition.
- `protocol/worlddata-agent-host-v2.schema.json`: the stable, sequenced UE-to-host wire contract.

The Unreal side follows the same boundary: implementation modules expose one
`I...Module` header from `Public/`; concrete gateways, processes, runtime
installers, diagnostics, security storage, the web bridge, and widgets remain in
`Private/`.

Build a self-contained Windows runtime with:

```powershell
dotnet publish .\WorldData.AgentHost.App\WorldData.AgentHost.App.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:PublishTrimmed=false -o ..\..\Plugins\UEBridgeMCP\Binaries\Win64\AgentHost
```

Run the transport/auth/model-list handshake probe with:

```powershell
node .\tests\protocol-probe.mjs <host-exe> <codex-exe> <project-root>
```

Run deterministic fault and module-boundary checks with:

```powershell
node .\tests\fault-probe.mjs <host-exe> <project-root>
node .\tests\verify-module-boundaries.mjs
node .\tests\verify-security-boundaries.mjs
node .\tests\verify-ui-contract.mjs
```
